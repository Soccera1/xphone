#include "session.h"
#include "audio.h"
#include "transport.h"
#include "wire.h"
#include <errno.h>
#include <event2/event.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

typedef struct message {
    struct message *next;
    json_t *json;
} message;
typedef struct outgoing {
    uint8_t data[XP_MEDIA_MAX];
    size_t len;
    uint64_t created;
} outgoing;
typedef struct {
    xp_session *session;
    uint64_t epoch;
} audio_context;
struct xp_session {
    pthread_t thread;
    pthread_mutex_t lock;
    xp_snapshot view;
    message *commands, *command_tail, *events, *event_tail;
    size_t command_count, event_count;
    outgoing frames[10];
    size_t frame_begin, frame_count;
    uint32_t tx_sequence;
    int notify_fd, command_fd;
    struct event_base *base;
    struct event *command_event, *heartbeat, *fallback, *reconnect;
    xp_transport *transport;
    xp_peer *peer;
    xp_stream *control, *media;
    xp_audio *audio;
    audio_context *audio_context;
    uint64_t audio_epoch;
    xp_ringer *ringer;
    char host[256], port[8], authority[280], token[96], username[XP_NAME];
    char *ca;
    bool synthetic, force_h2, quitting, permanent_error, switching, authenticated;
    int capture, playback, complexity, channel_mode;
    uint64_t last_rx, last_ping, sleep_offset;
};
static void wake_fd(int fd) {
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    /* A full eventfd is already readable, so that wakeup is not lost. */
    if (n < 0 && errno != EAGAIN)
        xp_log("Client event notification failed");
}
static void emit(xp_session *s, json_t *obj) {
    if (!obj)
        return;
    message *m = calloc(1, sizeof(*m));
    if (!m) {
        json_decref(obj);
        return;
    }
    m->json = obj;
    pthread_mutex_lock(&s->lock);
    while (s->event_count >= 256 && s->events) {
        message *old = s->events;
        s->events = old->next;
        json_decref(old->json);
        free(old);
        --s->event_count;
        if (!s->events)
            s->event_tail = NULL;
    }
    if (s->event_tail)
        s->event_tail->next = m;
    else
        s->events = m;
    s->event_tail = m;
    ++s->event_count;
    pthread_mutex_unlock(&s->lock);
    wake_fd(s->notify_fd);
}
static void status(xp_session *s, xp_state state, const char *text) {
    pthread_mutex_lock(&s->lock);
    s->view.state = state;
    snprintf(s->view.status, sizeof(s->view.status), "%s", text);
    uint64_t call = s->view.call;
    pthread_mutex_unlock(&s->lock);
    emit(s, json_pack("{s:s,s:i,s:s,s:I}", "type", "status", "state", state, "message", text,
                      "call", (json_int_t)call));
}
void xp_session_snapshot(xp_session *s, xp_snapshot *snapshot) {
    pthread_mutex_lock(&s->lock);
    *snapshot = s->view;
    pthread_mutex_unlock(&s->lock);
}
json_t *xp_session_event(xp_session *s) {
    pthread_mutex_lock(&s->lock);
    message *m = s->events;
    if (m) {
        s->events = m->next;
        --s->event_count;
        if (!s->events)
            s->event_tail = NULL;
    }
    pthread_mutex_unlock(&s->lock);
    if (!m)
        return NULL;
    json_t *obj = m->json;
    free(m);
    return obj;
}
static void enqueue(xp_session *s, json_t *obj) {
    message *m = calloc(1, sizeof(*m));
    if (!m) {
        json_decref(obj);
        return;
    }
    m->json = obj;
    pthread_mutex_lock(&s->lock);
    if (s->command_count >= 64) {
        pthread_mutex_unlock(&s->lock);
        json_decref(obj);
        free(m);
        return;
    }
    if (s->command_tail)
        s->command_tail->next = m;
    else
        s->commands = m;
    s->command_tail = m;
    ++s->command_count;
    pthread_mutex_unlock(&s->lock);
    wake_fd(s->command_fd);
}
void xp_session_command(xp_session *s, const char *action, const char *target, int value) {
    enqueue(s, json_pack("{s:s,s:s,s:i}", "action", action, "target", target ? target : "", "value",
                         value));
}
static int send_control(xp_session *s, json_t *obj) {
    int rc = s->control ? xp_stream_json(s->control, obj) : -1;
    json_decref(obj);
    return rc;
}
static void stop_audio(xp_session *s) {
    if (!s->audio)
        return;
    xp_audio_stats stats;
    xp_audio_stop(s->audio, &stats);
    s->audio = NULL;
    free(s->audio_context);
    s->audio_context = NULL;
    pthread_mutex_lock(&s->lock);
    s->frame_count = s->frame_begin = 0;
    pthread_mutex_unlock(&s->lock);
    emit(s, json_pack("{s:s,s:I,s:I,s:I,s:I,s:f,s:f}", "type", "audio_stats", "sent",
                      (json_int_t)stats.sent, "received", (json_int_t)stats.received, "concealed",
                      (json_int_t)stats.concealed, "underruns", (json_int_t)stats.underruns,
                      "left_energy", stats.left_energy, "right_energy", stats.right_energy));
}
static void encoded(const uint8_t *data, size_t len, void *arg) {
    audio_context *context = arg;
    xp_session *s = context->session;
    pthread_mutex_lock(&s->lock);
    if (s->view.state != XP_ACTIVE || context->epoch != s->audio_epoch) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    if (s->frame_count == 10) {
        s->frame_begin = (s->frame_begin + 1) % 10;
        --s->frame_count;
    }
    outgoing *frame = &s->frames[(s->frame_begin + s->frame_count++) % 10];
    uint32_t seq = s->tx_sequence++;
    xp_media m = {.call = s->view.call,
                  .generation = s->view.generation,
                  .sequence = seq,
                  .timestamp = seq * XP_FRAME,
                  .channels = (uint8_t)s->view.tx,
                  .opus = data,
                  .opus_len = len};
    frame->len = xp_media_pack(frame->data, &m);
    frame->created = xp_now_ms();
    pthread_mutex_unlock(&s->lock);
    wake_fd(s->command_fd);
}
static void audio_event(const char *event, void *arg) {
    audio_context *context = arg;
    enqueue(context->session,
            json_pack("{s:s,s:I}", "action", event, "epoch", (json_int_t)context->epoch));
}
static void start_audio(xp_session *s) {
    if (!s->media || s->audio)
        return;
    xp_snapshot v;
    xp_session_snapshot(s, &v);
    if (v.state != XP_ACTIVE)
        return;
    s->audio_context = calloc(1, sizeof(*s->audio_context));
    if (!s->audio_context) {
        xp_session_command(s, "audio_error", NULL, 0);
        return;
    }
    pthread_mutex_lock(&s->lock);
    ++s->audio_epoch;
    *s->audio_context = (audio_context){s, s->audio_epoch};
    pthread_mutex_unlock(&s->lock);
    s->audio = xp_audio_start(s->synthetic, v.tx, v.rx, s->complexity, encoded, audio_event,
                              s->audio_context);
    if (s->audio)
        xp_audio_controls(s->audio, v.muted, v.volume);
    else {
        free(s->audio_context);
        s->audio_context = NULL;
        xp_session_command(s, "audio_error", NULL, 0);
    }
}
static void hello(xp_session *s) {
    send_control(s, json_pack("{s:s,s:i,s:i,s:i}", "type", "hello", "version", 1, "capture",
                              s->channel_mode == 1 ? 1 : s->capture, "playback",
                              s->channel_mode == 1 ? 1 : s->playback));
}
static bool refresh_devices(xp_session *s) {
    if (!s->synthetic)
        xp_audio_probe(&s->capture, &s->playback);
    hello(s);
    if (s->channel_mode == 2 && (s->capture < 2 || s->playback < 2)) {
        xp_snapshot v;
        xp_session_snapshot(s, &v);
        status(s, v.state, "Stereo unavailable on the selected devices");
        return false;
    }
    return true;
}
static int headers(xp_stream *stream, void *arg) {
    xp_session *s = arg;
    int code = xp_stream_status(stream);
    if (code != 200) {
        const char *reason = code == 401   ? "Authentication failed"
                             : code == 409 ? "Username already connected"
                                           : "Server rejected the session";
        s->permanent_error = true;
        xp_peer_close(xp_stream_peer(stream), reason);
        return code;
    }
    if (xp_stream_media(stream))
        start_audio(s);
    return 200;
}
static void reset_call(xp_session *s) {
    if (s->ringer) {
        xp_ringer_stop(s->ringer);
        s->ringer = NULL;
    }
    stop_audio(s);
    if (s->media) {
        xp_stream_finish(s->media);
        s->media = NULL;
    }
    pthread_mutex_lock(&s->lock);
    s->view.call = s->view.started = 0;
    s->view.generation = 0;
    s->view.peer[0] = 0;
    pthread_mutex_unlock(&s->lock);
}
static int record(xp_stream *stream, const uint8_t *data, size_t len, void *arg) {
    xp_session *s = arg;
    s->last_rx = xp_now_ms();
    if (xp_stream_media(stream)) {
        xp_media m;
        if (xp_media_unpack(&m, data, len))
            return -1;
        xp_snapshot v;
        xp_session_snapshot(s, &v);
        if (s->audio && v.state == XP_ACTIVE && m.call == v.call && m.generation == v.generation &&
            m.channels == v.rx)
            xp_audio_receive(s->audio, m.sequence, m.opus, m.opus_len);
        return 0;
    }
    json_error_t error;
    json_t *obj = json_loadb((const char *)data, len, JSON_REJECT_DUPLICATES, &error);
    if (!json_is_object(obj)) {
        json_decref(obj);
        return -1;
    }
    const char *type = xp_str(obj, "type", "");
    if (!strcmp(type, "welcome")) {
        if (xp_int(obj, "version", -1, 1, 1) != 1) {
            json_decref(obj);
            return -1;
        }
        status(s, XP_ONLINE, "Ready to call");
        s->authenticated = true;
        emit(s, json_pack("{s:s,s:s,s:s}", "type", "connection", "http",
                          xp_peer_quic(s->peer) ? "HTTP/3" : "HTTP/2", "media",
                          xp_peer_datagrams(s->peer) ? "datagrams" : "capsules"));
        hello(s);
    } else if (!strcmp(type, "presence")) {
        json_t *list = json_object_get(obj, "users");
        if (!json_is_array(list) || json_array_size(list) > XP_MAX_USERS) {
            json_decref(obj);
            return -1;
        }
        pthread_mutex_lock(&s->lock);
        s->view.user_count = 0;
        for (size_t i = 0; i < json_array_size(list); ++i) {
            json_t *entry = json_array_get(list, i);
            const char *name = xp_str(entry, "name", "");
            if (!xp_name_valid(name) || !strcmp(name, s->username))
                continue;
            size_t n = s->view.user_count++;
            snprintf(s->view.users[n].name, XP_NAME, "%s", name);
            s->view.users[n].busy = json_is_true(json_object_get(entry, "busy"));
        }
        pthread_mutex_unlock(&s->lock);
    } else if (!strcmp(type, "incoming") || !strcmp(type, "ringing")) {
        const char *peer = xp_str(obj, "peer", "");
        json_int_t call = json_integer_value(json_object_get(obj, "call"));
        if (call <= 0 || !xp_name_valid(peer)) {
            json_decref(obj);
            return -1;
        }
        pthread_mutex_lock(&s->lock);
        s->view.call = (uint64_t)call;
        snprintf(s->view.peer, XP_NAME, "%s", peer);
        pthread_mutex_unlock(&s->lock);
        status(s, !strcmp(type, "incoming") ? XP_INCOMING : XP_OUTGOING,
               !strcmp(type, "incoming") ? "Incoming call" : "Ringing...");
        if (!strcmp(type, "incoming") && !s->synthetic && !s->ringer)
            s->ringer = xp_ringer_start();
    } else if (!strcmp(type, "active")) {
        if (s->ringer) {
            xp_ringer_stop(s->ringer);
            s->ringer = NULL;
        }
        int tx = xp_int(obj, "tx_channels", -1, 1, 2), rx = xp_int(obj, "rx_channels", -1, 1, 2);
        int generation = xp_int(obj, "generation", -1, 1, INT32_MAX);
        json_int_t call = json_integer_value(json_object_get(obj, "call"));
        xp_snapshot v;
        xp_session_snapshot(s, &v);
        if (tx < 1 || rx < 1 || generation < 1 || call <= 0 || v.call != (uint64_t)call) {
            json_decref(obj);
            return -1;
        }
        stop_audio(s);
        int previous_capture = s->capture, previous_playback = s->playback;
        if (!s->synthetic)
            xp_audio_probe(&s->capture, &s->playback);
        bool capabilities_changed = s->channel_mode != 1 && (previous_capture != s->capture ||
                                                             previous_playback != s->playback);
        pthread_mutex_lock(&s->lock);
        s->view.tx = tx;
        s->view.rx = rx;
        s->view.generation = (uint32_t)generation;
        s->view.state = XP_ACTIVE;
        if (!s->view.started)
            s->view.started = xp_now_ms();
        s->tx_sequence = 0;
        pthread_mutex_unlock(&s->lock);
        status(s, XP_ACTIVE, "Connecting audio...");
        /* A peer's format update may arrive before PulseAudio dispatches our
         * device-move callback. Discover defaults here too, before reopening
         * streams, so a replaced worker cannot swallow that notification. */
        if (capabilities_changed) {
            hello(s);
        } else if (!s->media) {
            char path[128];
            snprintf(path, sizeof(path), "/v1/calls/%llu/media", (unsigned long long)v.call);
            s->media = xp_peer_open(s->peer, path, true, s->username, s->token);
            if (!s->media)
                xp_session_command(s, "audio_error", NULL, 0);
        } else
            start_audio(s);
    } else if (!strcmp(type, "ended")) {
        reset_call(s);
        status(s, XP_ONLINE, xp_str(obj, "reason", "Call ended"));
    } else if (!strcmp(type, "error")) {
        xp_snapshot v;
        xp_session_snapshot(s, &v);
        status(s, v.state, xp_str(obj, "message", "Server error"));
    }
    emit(s, obj);
    return 0;
}
static void connect_session(xp_session *s, bool h2);
static void reconnect_ready(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    xp_session *s = arg;
    if (!s->quitting && !s->peer && !s->permanent_error)
        connect_session(s, s->force_h2);
}
static void closed(xp_peer *peer, const char *reason, void *arg) {
    xp_session *s = arg;
    if (peer != s->peer)
        return;
    bool fallback = xp_peer_quic(peer) && !s->authenticated && !s->control && !s->force_h2 &&
                    !s->permanent_error && !strstr(reason, "Certificate") && !s->switching;
    evtimer_del(s->fallback);
    reset_call(s);
    s->peer = NULL;
    s->control = NULL;
    pthread_mutex_lock(&s->lock);
    s->view.user_count = 0;
    pthread_mutex_unlock(&s->lock);
    if (s->switching || s->quitting)
        return;
    if (strstr(reason, "Certificate"))
        s->permanent_error = true;
    if (fallback) {
        connect_session(s, true);
        return;
    }
    status(s, XP_OFFLINE, reason);
    if (!s->permanent_error) {
        struct timeval wait = {3, 0};
        evtimer_add(s->reconnect, &wait);
    }
}
static void stream_closed(xp_stream *stream, void *arg) {
    xp_session *s = arg;
    if (s->media == stream) {
        s->media = NULL;
        stop_audio(s);
        xp_snapshot v;
        xp_session_snapshot(s, &v);
        if (v.call)
            send_control(s, json_pack("{s:s,s:I}", "type", "hangup", "call", (json_int_t)v.call));
    }
    if (s->control == stream)
        s->control = NULL;
}
static void ready(xp_peer *peer, void *arg) {
    xp_session *s = arg;
    if (s->peer != peer)
        return;
    evtimer_del(s->fallback);
    pthread_mutex_lock(&s->lock);
    strcpy(s->view.transport, xp_peer_quic(peer) ? "HTTP/3" : "HTTP/2");
    pthread_mutex_unlock(&s->lock);
    s->control = xp_peer_open(peer, "/v1/session", false, s->username, s->token);
    if (!s->control)
        xp_peer_close(peer, "Could not open session");
}
static void fallback_ready(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    xp_session *s = arg;
    if (!s->peer || s->control)
        return;
    s->switching = true;
    xp_peer_close(s->peer, "Trying HTTP/2");
    s->switching = false;
    connect_session(s, true);
}
static void connect_session(xp_session *s, bool h2) {
    status(s, XP_CONNECTING, h2 ? "Connecting over HTTP/2..." : "Connecting over HTTP/3...");
    s->last_rx = s->last_ping = xp_now_ms();
    s->authenticated = false;
    s->peer = xp_transport_connect(s->transport, s->host, s->port, s->authority, !h2);
    if (!s->peer) {
        status(s, XP_OFFLINE, "Connection setup failed");
        return;
    }
    if (!h2) {
        struct timeval timeout = {2, 0};
        evtimer_add(s->fallback, &timeout);
    }
}
static void commands_ready(evutil_socket_t fd, short events, void *arg) {
    (void)events;
    xp_session *s = arg;
    uint64_t counter;
    while (read(fd, &counter, sizeof(counter)) > 0) {
    }
    bool changed = false;
    while (true) {
        pthread_mutex_lock(&s->lock);
        message *m = s->commands;
        if (m) {
            s->commands = m->next;
            --s->command_count;
            if (!s->commands)
                s->command_tail = NULL;
        }
        pthread_mutex_unlock(&s->lock);
        if (!m)
            break;
        changed = true;
        const char *action = xp_str(m->json, "action", "");
        json_t *epoch = json_object_get(m->json, "epoch");
        /* Device changes describe global defaults, and remain relevant even
         * if the peer's format update already replaced this audio worker. */
        if (epoch && strcmp(action, "devices_changed") &&
            ((uint64_t)json_integer_value(epoch) != s->audio_epoch || !s->audio)) {
            json_decref(m->json);
            free(m);
            continue;
        }
        xp_snapshot v;
        xp_session_snapshot(s, &v);
        if (!strcmp(action, "quit")) {
            s->quitting = true;
            event_base_loopbreak(s->base);
        } else if (!strcmp(action, "mute")) {
            pthread_mutex_lock(&s->lock);
            s->view.muted = !s->view.muted;
            bool muted = s->view.muted;
            pthread_mutex_unlock(&s->lock);
            xp_audio_controls(s->audio, muted, v.volume);
        } else if (!strcmp(action, "volume")) {
            int volume = xp_int(m->json, "value", v.volume, 0, 100);
            if (volume >= 0) {
                pthread_mutex_lock(&s->lock);
                s->view.volume = volume;
                pthread_mutex_unlock(&s->lock);
                xp_audio_controls(s->audio, v.muted, volume);
            }
        } else if (!strcmp(action, "audio_ready")) {
            if (s->audio && v.state == XP_ACTIVE)
                status(s, XP_ACTIVE, "Connected");
        } else if (!strcmp(action, "audio_error")) {
            if (v.call)
                send_control(s,
                             json_pack("{s:s,s:I}", "type", "hangup", "call", (json_int_t)v.call));
            reset_call(s);
            status(s, s->control ? XP_ONLINE : XP_OFFLINE,
                   "Audio unavailable: check microphone and speakers");
        } else if (!strcmp(action, "suspended")) {
            if (s->peer)
                xp_peer_close(s->peer, "Call ended after system suspend");
        } else if (!strcmp(action, "devices_changed")) {
            stop_audio(s);
            int old_capture = s->capture, old_playback = s->playback;
            xp_audio_probe(&s->capture, &s->playback);
            hello(s);
            if (old_capture == s->capture && old_playback == s->playback)
                start_audio(s);
        } else if (!strcmp(action, "reconnect")) {
            s->permanent_error = false;
            if (s->peer)
                xp_peer_close(s->peer, "Reconnecting");
            evtimer_del(s->reconnect);
            connect_session(s, s->force_h2);
        } else if (s->control && !strcmp(action, "call") && v.state == XP_ONLINE) {
            const char *target = xp_str(m->json, "target", "");
            if (xp_name_valid(target) && refresh_devices(s))
                send_control(s, json_pack("{s:s,s:s}", "type", "call", "to", target));
        } else if (s->control && v.call &&
                   (!strcmp(action, "accept") || !strcmp(action, "reject") ||
                    !strcmp(action, "cancel") || !strcmp(action, "hangup"))) {
            if (!strcmp(action, "accept") && !refresh_devices(s)) {
                json_decref(m->json);
                free(m);
                continue;
            }
            if (strcmp(action, "accept"))
                stop_audio(s);
            if (s->ringer) {
                xp_ringer_stop(s->ringer);
                s->ringer = NULL;
            }
            send_control(s, json_pack("{s:s,s:I}", "type", action, "call", (json_int_t)v.call));
        }
        json_decref(m->json);
        free(m);
        if (s->quitting)
            return;
    }
    while (true) {
        outgoing frame;
        pthread_mutex_lock(&s->lock);
        bool have = s->frame_count != 0;
        if (have) {
            frame = s->frames[s->frame_begin];
            s->frame_begin = (s->frame_begin + 1) % 10;
            --s->frame_count;
        }
        pthread_mutex_unlock(&s->lock);
        if (!have)
            break;
        if (s->media && xp_now_ms() - frame.created < 200 &&
            xp_stream_send(s->media, frame.data, frame.len)) {
            xp_snapshot v;
            xp_session_snapshot(s, &v);
            send_control(s, json_pack("{s:s,s:I}", "type", "hangup", "call", (json_int_t)v.call));
            stop_audio(s);
            status(s, XP_ONLINE, "Connection too congested for audio");
        }
    }
    if (changed)
        emit(s, json_pack("{s:s}", "type", "changed"));
}
static void heartbeat(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    xp_session *s = arg;
    uint64_t now = xp_now_ms();
    uint64_t sleep_offset = xp_sleep_offset_ms();
    if (sleep_offset > s->sleep_offset + 500) {
        s->sleep_offset = sleep_offset;
        if (s->peer)
            xp_peer_close(s->peer, "Session ended after system suspend");
        return;
    }
    if (s->peer && now - s->last_rx > 45000) {
        xp_peer_close(s->peer, "Heartbeat timeout");
        return;
    }
    if (s->control && now - s->last_ping >= 15000) {
        s->last_ping = now;
        send_control(s, json_pack("{s:s}", "type", "ping"));
    }
}
static void *network_thread(void *arg) {
    xp_session *s = arg;
    connect_session(s, s->force_h2);
    event_base_dispatch(s->base);
    reset_call(s);
    xp_transport_free(s->transport);
    s->transport = NULL;
    return NULL;
}
xp_session *xp_session_new(json_t *config, bool synthetic) {
    xp_session *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->notify_fd = s->command_fd = -1;
    pthread_mutex_init(&s->lock, NULL);
    const char *username = xp_str(config, "username", ""), *token = xp_str(config, "token", "");
    const char *channels = xp_str(config, "audio_channels", "auto"),
               *transport = xp_str(config, "transport", "auto");
    s->complexity = xp_int(config, "opus_complexity", 5, 0, 10);
    if (xp_parse_url(xp_str(config, "server", ""), s->host, s->port, s->authority) ||
        !xp_name_valid(username) || strlen(token) != 64 ||
        strspn(token, "0123456789abcdef") != 64 || s->complexity < 0 ||
        (strcmp(channels, "auto") && strcmp(channels, "mono") && strcmp(channels, "stereo")) ||
        (strcmp(transport, "auto") && strcmp(transport, "http2")))
        goto fail;
    strcpy(s->username, username);
    strcpy(s->token, token);
    strcpy(s->view.username, username);
    s->synthetic = synthetic;
    s->force_h2 = !strcmp(transport, "http2");
    s->channel_mode = !strcmp(channels, "mono") ? 1 : !strcmp(channels, "stereo") ? 2 : 0;
    s->view.volume = 80;
    s->sleep_offset = xp_sleep_offset_ms();
    if (synthetic)
        s->capture = s->playback = 2;
    else
        xp_audio_probe(&s->capture, &s->playback);
    if (s->channel_mode == 2 && (s->capture < 2 || s->playback < 2)) {
        xp_log("Stereo was requested but the default devices do not both support stereo");
        goto fail;
    }
    s->notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    s->command_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    s->base = event_base_new();
    if (s->notify_fd < 0 || s->command_fd < 0 || !s->base)
        goto fail;
    const char *ca = xp_str(config, "ca_file", NULL);
    if (ca)
        s->ca = strdup(ca);
    xp_transport_callbacks cb = {.headers = headers,
                                 .record = record,
                                 .ready = ready,
                                 .closed = closed,
                                 .stream_closed = stream_closed};
    s->transport = xp_transport_new(s->base, false, NULL, NULL, s->ca, cb, s);
    if (!s->transport)
        goto fail;
    s->command_event = event_new(s->base, s->command_fd, EV_READ | EV_PERSIST, commands_ready, s);
    s->heartbeat = event_new(s->base, -1, EV_PERSIST, heartbeat, s);
    s->fallback = evtimer_new(s->base, fallback_ready, s);
    s->reconnect = evtimer_new(s->base, reconnect_ready, s);
    event_add(s->command_event, NULL);
    struct timeval interval = {1, 0};
    event_add(s->heartbeat, &interval);
    if (pthread_create(&s->thread, NULL, network_thread, s))
        goto fail;
    return s;
fail:
    xp_log("Invalid client configuration or initialization failed");
    if (s->command_event)
        event_free(s->command_event);
    if (s->heartbeat)
        event_free(s->heartbeat);
    if (s->fallback)
        event_free(s->fallback);
    if (s->reconnect)
        event_free(s->reconnect);
    xp_transport_free(s->transport);
    if (s->base)
        event_base_free(s->base);
    if (s->notify_fd >= 0)
        close(s->notify_fd);
    if (s->command_fd >= 0)
        close(s->command_fd);
    free(s->ca);
    pthread_mutex_destroy(&s->lock);
    free(s);
    return NULL;
}
int xp_session_fd(xp_session *s) {
    return s->notify_fd;
}
void xp_session_free(xp_session *s) {
    if (!s)
        return;
    xp_session_command(s, "quit", NULL, 0);
    pthread_join(s->thread, NULL);
    event_free(s->command_event);
    event_free(s->heartbeat);
    event_free(s->fallback);
    event_free(s->reconnect);
    event_base_free(s->base);
    close(s->notify_fd);
    close(s->command_fd);
    message *queues[] = {s->commands, s->events};
    for (size_t i = 0; i < 2; ++i)
        while (queues[i]) {
            message *m = queues[i];
            queues[i] = m->next;
            json_decref(m->json);
            free(m);
        }
    OPENSSL_cleanse(s->token, sizeof(s->token));
    free(s->ca);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
