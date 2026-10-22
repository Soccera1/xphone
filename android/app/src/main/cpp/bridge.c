#include "transport.h"
#include <jni.h>
#include <opus/opus.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* All transport operations and callbacks run on one Java HandlerThread. No JNI
 * reference or libevent object crosses threads. Audio uses independent codecs. */
typedef struct {
    JNIEnv *env;
    jobject listener;
    jmethodID control_cb, media_cb;
    struct event_base *base;
    xp_transport *transport;
    xp_peer *peer;
    xp_stream *control, *media;
    char host[256], port[8], authority[280], user[XP_NAME], token[96];
    uint64_t deadline;
    bool force_h2, switching, fallback, closed, dialing;
} bridge;

static void notify(bridge *b, json_t *object) {
    char *text = json_dumps(object, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(object);
    if (!text || (*b->env)->ExceptionCheck(b->env)) { free(text); return; }
    jstring value = (*b->env)->NewStringUTF(b->env, text);
    free(text);
    if (!value) return;
    (*b->env)->CallVoidMethod(b->env, b->listener, b->control_cb, value);
    (*b->env)->DeleteLocalRef(b->env, value);
}

static void fail(bridge *b, const char *reason) {
    if (b->closed) return;
    b->closed = true;
    notify(b, json_pack("{s:s,s:s}", "type", "_closed", "reason", reason));
}

static int headers(xp_stream *stream, void *arg) {
    bridge *b = arg;
    int status = xp_stream_status(stream);
    if (status != 200) {
        const char *reason = status == 401 ? "Authentication failed" :
                             status == 409 ? "Username already connected" : "Server rejected stream";
        fail(b, reason);
        return status;
    }
    if (xp_stream_media(stream)) notify(b, json_pack("{s:s}", "type", "_media"));
    return 200;
}

static int record(xp_stream *stream, const uint8_t *data, size_t len, void *arg) {
    bridge *b = arg;
    if (b->closed || (*b->env)->ExceptionCheck(b->env)) return -1;
    if (xp_stream_media(stream)) {
        jbyteArray bytes = (*b->env)->NewByteArray(b->env, (jsize)len);
        if (!bytes) return -1;
        (*b->env)->SetByteArrayRegion(b->env, bytes, 0, (jsize)len, (const jbyte *)data);
        (*b->env)->CallVoidMethod(b->env, b->listener, b->media_cb, bytes);
        (*b->env)->DeleteLocalRef(b->env, bytes);
    } else {
        json_error_t error;
        json_t *object = json_loadb((const char *)data, len, JSON_REJECT_DUPLICATES, &error);
        if (!json_is_object(object)) { json_decref(object); return -1; }
        notify(b, object);
    }
    return (*b->env)->ExceptionCheck(b->env) ? -1 : 0;
}

static void ready(xp_peer *peer, void *arg) {
    bridge *b = arg;
    if (peer != b->peer || b->closed) return;
    b->deadline = 0;
    b->control = xp_peer_open(peer, "/v1/session", false, b->user, b->token);
    if (!b->control) { fail(b, "Cannot open session"); return; }
    notify(b, json_pack("{s:s,s:s,s:b}", "type", "_transport", "http",
                        xp_peer_quic(peer) ? "HTTP/3" : "HTTP/2", "datagrams", xp_peer_datagrams(peer)));
}

static void closed(xp_peer *peer, const char *reason, void *arg) {
    bridge *b = arg;
    if (peer != b->peer && !b->dialing) return;
    bool fallback = !b->force_h2 && xp_peer_quic(peer) && !b->control &&
                    !strstr(reason, "Certificate");
    b->peer = NULL;
    b->media = b->control = NULL;
    b->deadline = 0;
    if (b->switching) return;
    if (fallback && !b->closed) b->fallback = true;
    else fail(b, reason);
}

static void stream_closed(xp_stream *stream, void *arg) {
    bridge *b = arg;
    if (stream == b->media) {
        b->media = NULL;
        if (!b->switching) notify(b, json_pack("{s:s}", "type", "_media_closed"));
    }
    if (stream == b->control) {
        b->control = NULL;
        if (!b->switching) fail(b, "Control stream closed");
    }
}

static void dial(bridge *b, bool quic) {
    b->dialing = true;
    xp_peer *peer = xp_transport_connect(b->transport, b->host, b->port, b->authority, quic);
    b->dialing = false;
    b->peer = b->closed || b->fallback ? NULL : peer;
    if (!peer) fail(b, "Cannot connect over HTTPS");
}

static void dispose(JNIEnv *env, bridge *b) {
    if (!b) return;
    b->switching = true;
    b->closed = true;
    if (b->transport) xp_transport_free(b->transport);
    if (b->base) event_base_free(b->base);
    if (b->listener) (*env)->DeleteGlobalRef(env, b->listener);
    OPENSSL_cleanse(b->token, sizeof(b->token));
    free(b);
}

static bool copy_string(JNIEnv *env, jstring input, char *output, size_t cap) {
    if (!input) return false;
    const char *s = (*env)->GetStringUTFChars(env, input, NULL);
    if (!s) return false;
    bool valid = strlen(s) < cap;
    if (valid) memcpy(output, s, strlen(s) + 1);
    (*env)->ReleaseStringUTFChars(env, input, s);
    return valid;
}

JNIEXPORT jlong JNICALL Java_org_xphone_client_NativeBridge_open(
    JNIEnv *env, jclass clazz, jstring url, jstring user, jstring token,
    jstring ca, jboolean http2, jobject listener) {
    bridge *b = calloc(1, sizeof(*b));
    if (!b) return 0;
    char address[512], ca_path[4096];
    b->env = env;
    b->force_h2 = http2;
    if (!copy_string(env, url, address, sizeof(address)) ||
        !copy_string(env, user, b->user, sizeof(b->user)) || !xp_name_valid(b->user) ||
        !copy_string(env, token, b->token, sizeof(b->token)) || !*b->token ||
        !copy_string(env, ca, ca_path, sizeof(ca_path)) || !*ca_path ||
        xp_parse_url(address, b->host, b->port, b->authority) || !listener) goto bad;
    b->listener = (*env)->NewGlobalRef(env, listener);
    jclass cls = (*env)->GetObjectClass(env, listener);
    b->control_cb = (*env)->GetMethodID(env, cls, "onControl", "(Ljava/lang/String;)V");
    b->media_cb = (*env)->GetMethodID(env, cls, "onMedia", "([B)V");
    (*env)->DeleteLocalRef(env, cls);
    if (!b->listener || !b->control_cb || !b->media_cb) goto bad;
    b->base = event_base_new();
    if (!b->base) goto bad;
    xp_transport_callbacks cb = {headers, record, ready, closed, stream_closed};
    b->transport = xp_transport_new(b->base, false, NULL, NULL, ca_path, cb, b);
    if (!b->transport) goto bad;
    dial(b, !http2);
    b->deadline = http2 ? 0 : xp_now_ms() + 2000;
    return (jlong)(intptr_t)b;
bad:
    dispose(env, b);
    return 0;
}

JNIEXPORT void JNICALL Java_org_xphone_client_NativeBridge_pump(JNIEnv *env, jclass cls, jlong handle) {
    bridge *b = (bridge *)(intptr_t)handle;
    if (!b || b->closed) return;
    b->env = env;
    struct timeval interval = {0, 20000};
    event_base_loopexit(b->base, &interval);
    event_base_loop(b->base, EVLOOP_ONCE);
    if (b->closed || (*env)->ExceptionCheck(env)) return;
    if (b->fallback || (b->deadline && xp_now_ms() >= b->deadline)) {
        b->switching = true;
        if (b->peer) xp_peer_close(b->peer, "Trying HTTP/2");
        b->switching = false;
        b->fallback = false;
        b->deadline = 0;
        dial(b, false);
    }
}

JNIEXPORT jboolean JNICALL Java_org_xphone_client_NativeBridge_send(JNIEnv *env, jclass cls,
                                                                    jlong handle, jstring text) {
    bridge *b = (bridge *)(intptr_t)handle;
    if (!b || !b->control || b->closed || !text) return JNI_FALSE;
    const char *s = (*env)->GetStringUTFChars(env, text, NULL);
    if (!s) return JNI_FALSE;
    json_error_t error;
    json_t *object = strlen(s) <= XP_CONTROL_MAX ? json_loads(s, JSON_REJECT_DUPLICATES, &error) : NULL;
    (*env)->ReleaseStringUTFChars(env, text, s);
    int rc = json_is_object(object) ? xp_stream_json(b->control, object) : -1;
    json_decref(object);
    return rc == 0;
}

JNIEXPORT void JNICALL Java_org_xphone_client_NativeBridge_mediaClose(JNIEnv *env, jclass cls, jlong handle) {
    bridge *b = (bridge *)(intptr_t)handle;
    if (b && b->media) { xp_stream_finish(b->media); b->media = NULL; }
}

JNIEXPORT jboolean JNICALL Java_org_xphone_client_NativeBridge_mediaOpen(JNIEnv *env, jclass cls,
                                                                       jlong handle, jlong call) {
    bridge *b = (bridge *)(intptr_t)handle;
    if (!b || !b->peer || b->closed || call <= 0) return JNI_FALSE;
    if (b->media) return JNI_TRUE;
    char path[128];
    snprintf(path, sizeof(path), "/v1/calls/%lld/media", (long long)call);
    b->media = xp_peer_open(b->peer, path, true, b->user, b->token);
    return b->media != NULL;
}

JNIEXPORT void JNICALL Java_org_xphone_client_NativeBridge_mediaSend(JNIEnv *env, jclass cls,
                                                                   jlong handle, jbyteArray bytes) {
    bridge *b = (bridge *)(intptr_t)handle;
    if (!b || !b->media || b->closed || !bytes) return;
    jsize n = (*env)->GetArrayLength(env, bytes);
    if (n < 25 || n > XP_MEDIA_MAX) return;
    uint8_t record[XP_MEDIA_MAX];
    (*env)->GetByteArrayRegion(env, bytes, 0, n, (jbyte *)record);
    if (!(*env)->ExceptionCheck(env)) (void)xp_stream_send(b->media, record, (size_t)n);
}

JNIEXPORT void JNICALL Java_org_xphone_client_NativeBridge_close(JNIEnv *env, jclass cls, jlong handle) {
    dispose(env, (bridge *)(intptr_t)handle);
}

typedef struct { OpusEncoder *encoder; OpusDecoder *decoder; int tx, rx; } codec;

JNIEXPORT jlong JNICALL Java_org_xphone_client_NativeBridge_codecCreate(JNIEnv *env, jclass cls, jint tx, jint rx) {
    if ((tx != 1 && tx != 2) || (rx != 1 && rx != 2)) return 0;
    codec *c = calloc(1, sizeof(*c));
    if (!c) return 0;
    int error;
    c->tx = tx; c->rx = rx;
    c->encoder = opus_encoder_create(XP_RATE, tx, OPUS_APPLICATION_VOIP, &error);
    c->decoder = opus_decoder_create(XP_RATE, rx, &error);
    if (!c->encoder || !c->decoder) {
        if (c->encoder) opus_encoder_destroy(c->encoder);
        if (c->decoder) opus_decoder_destroy(c->decoder);
        free(c); return 0;
    }
    opus_encoder_ctl(c->encoder, OPUS_SET_BITRATE(tx == 2 ? 64000 : 32000));
    opus_encoder_ctl(c->encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(c->encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(c->encoder, OPUS_SET_PACKET_LOSS_PERC(5));
    return (jlong)(intptr_t)c;
}

JNIEXPORT jbyteArray JNICALL Java_org_xphone_client_NativeBridge_encode(JNIEnv *env, jclass cls,
                                                                      jlong handle, jshortArray pcm) {
    codec *c = (codec *)(intptr_t)handle;
    if (!c || !pcm || (*env)->GetArrayLength(env, pcm) != XP_FRAME * c->tx) return NULL;
    short input[XP_FRAME * 2];
    uint8_t output[XP_MEDIA_MAX - 24];
    (*env)->GetShortArrayRegion(env, pcm, 0, XP_FRAME * c->tx, input);
    int n = opus_encode(c->encoder, input, XP_FRAME, output, sizeof(output));
    if (n < 1) return NULL;
    jbyteArray result = (*env)->NewByteArray(env, n);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, n, (const jbyte *)output);
    return result;
}

JNIEXPORT jshortArray JNICALL Java_org_xphone_client_NativeBridge_decode(JNIEnv *env, jclass cls,
                                                                        jlong handle, jbyteArray packet) {
    codec *c = (codec *)(intptr_t)handle;
    if (!c) return NULL;
    uint8_t input[XP_MEDIA_MAX - 24];
    short output[XP_FRAME * 2];
    jsize n = packet ? (*env)->GetArrayLength(env, packet) : 0;
    if (n < 0 || n > (jsize)sizeof(input)) return NULL;
    if (n) (*env)->GetByteArrayRegion(env, packet, 0, n, (jbyte *)input);
    int count = opus_decode(c->decoder, n ? input : NULL, n, output, XP_FRAME, 0);
    if (count < 0) count = opus_decode(c->decoder, NULL, 0, output, XP_FRAME, 0);
    if (count != XP_FRAME) return NULL;
    jshortArray result = (*env)->NewShortArray(env, count * c->rx);
    if (result) (*env)->SetShortArrayRegion(env, result, 0, count * c->rx, output);
    return result;
}

JNIEXPORT void JNICALL Java_org_xphone_client_NativeBridge_codecDestroy(JNIEnv *env, jclass cls, jlong handle) {
    codec *c = (codec *)(intptr_t)handle;
    if (!c) return;
    opus_encoder_destroy(c->encoder); opus_decoder_destroy(c->decoder); free(c);
}
