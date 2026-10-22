#include "transport_internal.h"
#include <event2/buffer.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int alpn(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                unsigned inlen, void *arg) {
    (void)ssl;
    const unsigned char *wanted = arg;
    return SSL_select_next_proto((unsigned char **)out, outlen, wanted, 3, in, inlen) ==
                   OPENSSL_NPN_NEGOTIATED
               ? SSL_TLSEXT_ERR_OK
               : SSL_TLSEXT_ERR_ALERT_FATAL;
}
static SSL_CTX *tls_context(bool server, bool quic, const char *cert, const char *key,
                            const char *ca) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx)
        return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_max_early_data(ctx, 0);
    if (server) {
        if (!cert || !key || SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1)
            goto fail;
        SSL_CTX_set_alpn_select_cb(ctx, alpn, quic ? (void *)"\002h3" : (void *)"\002h2");
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        if (ca ? SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1
               : SSL_CTX_set_default_verify_paths(ctx) != 1)
            goto fail;
    }
    return ctx;
fail:
    SSL_CTX_free(ctx);
    return NULL;
}
xp_transport *xp_transport_new(struct event_base *base, bool server, const char *cert,
                               const char *key, const char *ca, xp_transport_callbacks cb,
                               void *arg) {
    xp_transport *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->base = base;
    t->server = server;
    t->cb = cb;
    t->arg = arg;
    t->udp_fd = -1;
    ngtcp2_crypto_ossl_init();
    t->tls_ctx = tls_context(server, false, cert, key, ca);
    t->quic_ctx = tls_context(server, true, cert, key, ca);
    if (!t->tls_ctx || !t->quic_ctx) {
        xp_log("TLS configuration failed: %s", ERR_reason_error_string(ERR_get_error()));
        xp_transport_free(t);
        return NULL;
    }
    return t;
}
int xp_tls_identity(SSL *ssl, const char *host) {
    unsigned char ip[16];
    if (inet_pton(AF_INET, host, ip) == 1 || inet_pton(AF_INET6, host, ip) == 1)
        return X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host) == 1 ? 0 : -1;
    return SSL_set1_host(ssl, host) == 1 && SSL_set_tlsext_host_name(ssl, host) == 1 ? 0 : -1;
}
int xp_resolve(const char *host, const char *port, int type, bool passive,
               struct sockaddr_storage *out, socklen_t *len) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC,
                             .ai_socktype = type,
                             .ai_flags = passive ? AI_PASSIVE : 0},
                    *list;
    int rc = getaddrinfo(host, port, &hints, &list);
    if (rc) {
        xp_log("Address lookup failed: %s", gai_strerror(rc));
        return -1;
    }
    /* Prefer IPv4 when both are advertised; explicit IPv6 literals still work. */
    struct addrinfo *pick = list;
    for (struct addrinfo *it = list; it; it = it->ai_next)
        if (it->ai_family == AF_INET) {
            pick = it;
            break;
        }
    if (!pick) {
        freeaddrinfo(list);
        return -1;
    }
    *len = (socklen_t)pick->ai_addrlen;
    memcpy(out, pick->ai_addr, *len);
    freeaddrinfo(list);
    return 0;
}
static void flush_pending(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    xp_peer *p = arg;
    if (!p->dead) {
        if (p->quic)
            xp_h3_flush(p);
        else
            xp_h2_flush(p);
    }
}
void xp_schedule(xp_peer *p) {
    if (!p->dead)
        event_active(p->flush_event, 0, 0);
}
xp_peer *xp_peer_alloc(xp_transport *t, bool quic) {
    if (t->peer_count >= XP_MAX_USERS * 2)
        return NULL;
    xp_peer *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->transport = t;
    p->quic = quic;
    p->udp_fd = -1;
    p->created = xp_now_ms();
    p->flush_event = event_new(t->base, -1, 0, flush_pending, p);
    p->next = t->peers;
    t->peers = p;
    ++t->peer_count;
    return p;
}
void xp_pop_chunk(xp_stream *s) {
    xp_chunk *c = s->head;
    if (!c)
        return;
    s->head = c->next;
    if (!s->head)
        s->tail = NULL;
    s->queued -= c->len;
    --s->chunk_count;
    free(c);
}
void xp_stream_dispose(xp_stream *s) {
    while (s->head)
        xp_pop_chunk(s);
    OPENSSL_cleanse(s->token, sizeof(s->token));
    free(s);
}
static void peer_destroy(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    xp_peer *p = arg;
    xp_transport *t = p->transport;
    xp_peer **it = &t->peers;
    while (*it && *it != p)
        it = &(*it)->next;
    if (*it) {
        *it = p->next;
        --t->peer_count;
    }
    if (p->q && p->ssl) {
        uint8_t packet[1500];
        ngtcp2_path_storage path;
        ngtcp2_path_storage_zero(&path);
        ngtcp2_pkt_info pi = {0};
        ngtcp2_ccerr error;
        ngtcp2_ccerr_default(&error);
        ngtcp2_ccerr_set_application_error(&error, NGHTTP3_H3_NO_ERROR, NULL, 0);
        ngtcp2_ssize n = ngtcp2_conn_write_connection_close(p->q, &path.path, &pi, packet,
                                                            sizeof(packet), &error, xp_now_ns());
        if (n > 0)
            (void)sendto(p->udp_fd, packet, (size_t)n, 0, path.path.remote.addr,
                         path.path.remote.addrlen);
    }
    if (p->destroy_event)
        event_free(p->destroy_event);
    if (p->flush_event)
        event_free(p->flush_event);
    if (p->timer)
        event_free(p->timer);
    if (p->read_event)
        event_free(p->read_event);
    if (p->bev) {
        bufferevent_free(p->bev);
        p->ssl = NULL;
    }
    if (p->h2)
        nghttp2_session_del(p->h2);
    if (p->h3)
        nghttp3_conn_del(p->h3);
    if (p->ssl) {
        SSL_set_app_data(p->ssl, NULL);
        SSL_free(p->ssl);
    }
    if (p->tls)
        ngtcp2_crypto_ossl_ctx_del(p->tls);
    if (p->q)
        ngtcp2_conn_del(p->q);
    if (p->udp_fd >= 0 && !t->server)
        close(p->udp_fd);
    while (p->streams) {
        xp_stream *s = p->streams;
        p->streams = s->next;
        xp_stream_dispose(s);
    }
    while (p->datagram_head) {
        xp_chunk *c = p->datagram_head;
        p->datagram_head = c->next;
        free(c);
    }
    free(p);
}
void xp_peer_close(xp_peer *p, const char *reason) {
    if (!p || p->dead)
        return;
    p->dead = true;
    if (p->bev)
        bufferevent_disable(p->bev, EV_READ | EV_WRITE);
    if (p->read_event)
        event_del(p->read_event);
    if (p->timer)
        event_del(p->timer);
    if (p->transport->cb.closed)
        p->transport->cb.closed(p, reason, p->transport->arg);
    p->destroy_event = evtimer_new(p->transport->base, peer_destroy, p);
    struct timeval zero = {0};
    evtimer_add(p->destroy_event, &zero);
}
void xp_transport_free(xp_transport *t) {
    if (!t)
        return;
    t->closing = true;
    if (t->listener)
        evconnlistener_free(t->listener);
    if (t->udp_event)
        event_free(t->udp_event);
    if (t->udp_fd >= 0)
        close(t->udp_fd);
    while (t->peers)
        peer_destroy(-1, 0, t->peers);
    SSL_CTX_free(t->tls_ctx);
    SSL_CTX_free(t->quic_ctx);
    free(t);
}
void xp_transport_expire_pending(xp_transport *t, uint64_t timeout_ms) {
    if (!t || !t->server)
        return;
    uint64_t now = xp_now_ms();
    for (xp_peer *p = t->peers; p; p = p->next)
        if (!p->dead && !p->user_data && now - p->created >= timeout_ms)
            xp_peer_close(p, "Authentication timeout");
}
int xp_transport_listen(xp_transport *t, const char *host, const char *port, bool quic) {
    if (xp_resolve(host, port, SOCK_STREAM, true, &t->bind_addr, &t->bind_len))
        return -1;
    t->listener =
        evconnlistener_new_bind(t->base, xp_h2_accept, t, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
                                64, (struct sockaddr *)&t->bind_addr, t->bind_len);
    if (!t->listener)
        return -1;
    return quic ? xp_h3_listen(t) : 0;
}
xp_peer *xp_transport_connect(xp_transport *t, const char *host, const char *port,
                              const char *authority, bool quic) {
    xp_peer *p = xp_peer_alloc(t, quic);
    if (!p)
        return NULL;
    snprintf(p->host, sizeof(p->host), "%s", host);
    snprintf(p->authority, sizeof(p->authority), "%s", authority);
    if (quic ? xp_h3_connect(p, port) : xp_h2_connect(p, port)) {
        xp_peer_close(p, "Connection setup failed");
    }
    return p;
}
xp_stream *xp_find_stream(xp_peer *p, int64_t id, bool create) {
    size_t count = 0;
    xp_stream **it = &p->streams;
    while (*it) {
        xp_stream *s = *it;
        if (s->id == id)
            return s;
        if (s->closed) {
            *it = s->next;
            xp_stream_dispose(s);
            continue;
        }
        ++count;
        it = &s->next;
    }
    if (!create || count >= 8)
        return NULL;
    xp_stream *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->peer = p;
    s->id = id;
    s->next = p->streams;
    p->streams = s;
    return s;
}
int xp_header(xp_stream *s, const uint8_t *name, size_t nlen, const uint8_t *value, size_t vlen) {
    if (!s || (s->header_bytes += nlen + vlen) > 8192)
        return -1;
    char *dest = NULL;
    size_t cap = 0;
#define FIELD(key, member)                                                                         \
    if (nlen == strlen(key) && !memcmp(name, key, nlen)) {                                         \
        dest = s->member;                                                                          \
        cap = sizeof(s->member);                                                                   \
    }
    FIELD(":path", path)
    FIELD(":method", method)
    FIELD(":protocol", protocol)
    FIELD("x-xphone-user", user)
    if (nlen == 13 && !memcmp(name, "authorization", 13)) {
        if (vlen < 7 || memcmp(value, "Bearer ", 7))
            return -1;
        value += 7;
        vlen -= 7;
        dest = s->token;
        cap = sizeof(s->token);
    }
    if (nlen == 7 && !memcmp(name, ":status", 7)) {
        if (vlen != 3)
            return -1;
        s->status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + value[2] - '0';
    }
    if (dest) {
        if (vlen >= cap || memchr(value, 0, vlen) || *dest)
            return -1;
        memcpy(dest, value, vlen);
        dest[vlen] = 0;
    }
    return 0;
#undef FIELD
}
int xp_headers_complete(xp_stream *s) {
    xp_peer *p = s->peer;
    if (p->transport->server) {
        if (strcmp(s->method, "CONNECT"))
            return 405;
        s->media = !strcmp(s->protocol, "xphone-media-v1");
        if (!s->media && strcmp(s->protocol, "xphone-control-v1"))
            return 501;
    }
    int status = p->transport->cb.headers ? p->transport->cb.headers(s, p->transport->arg) : 200;
    s->accepted = status == 200;
    return status;
}
static int record(const uint8_t *data, size_t len, void *arg) {
    xp_stream *s = arg;
    if (!s->accepted || s->peer->dead)
        return -1;
    return s->peer->transport->cb.record
               ? s->peer->transport->cb.record(s, data, len, s->peer->transport->arg)
               : 0;
}
int xp_receive(xp_stream *s, const uint8_t *data, size_t len) {
    if (!s || !s->accepted)
        return -1;
    return s->media ? xp_capsule_feed(&s->parser, data, len, record, s)
                    : xp_control_feed(&s->parser, data, len, record, s);
}
void xp_stream_mark_closed(xp_stream *s) {
    if (!s || s->closed)
        return;
    s->closed = true;
    xp_peer *p = s->peer;
    if (p->transport->cb.stream_closed)
        p->transport->cb.stream_closed(s, p->transport->arg);
    if (!s->media && s->accepted && !p->dead)
        xp_peer_close(p, "Session stream closed");
}
void xp_ready(xp_peer *p) {
    if (p->ready || p->dead)
        return;
    p->ready = true;
    if (p->transport->cb.ready)
        p->transport->cb.ready(p, p->transport->arg);
}
xp_stream *xp_peer_open(xp_peer *p, const char *path, bool media, const char *user,
                        const char *token) {
    if (!p || p->dead || !p->ready || strlen(token) >= 96 || strlen(user) >= XP_NAME ||
        strlen(path) >= 128)
        return NULL;
    xp_stream *s = xp_find_stream(p, -1, true);
    if (!s)
        return NULL;
    snprintf(s->path, sizeof(s->path), "%s", path);
    strcpy(s->method, "CONNECT");
    strcpy(s->protocol, media ? "xphone-media-v1" : "xphone-control-v1");
    strcpy(s->user, user);
    strcpy(s->token, token);
    s->media = media;
    int rc = p->quic ? xp_h3_open(s) : xp_h2_open(s);
    if (rc) {
        s->closed = true;
        return NULL;
    }
    return s;
}
int xp_queue(xp_stream *s, const uint8_t *data, size_t len) {
    xp_peer *p = s->peer;
    if (s->media) {
        while (s->head && !s->head->offered && !s->head->offset &&
               (s->chunk_count >= 10 || xp_now_ms() - s->head->created > 200))
            xp_pop_chunk(s);
        if (s->chunk_count >= 10)
            return -1;
    }
    if (s->queued + len > XP_QUEUE_MAX ||
        (p->bev && evbuffer_get_length(bufferevent_get_output(p->bev)) > XP_QUEUE_MAX))
        return -1;
    xp_chunk *c = malloc(sizeof(*c) + len);
    if (!c)
        return -1;
    *c = (xp_chunk){.len = len, .created = xp_now_ms()};
    memcpy(c->data, data, len);
    if (s->tail)
        s->tail->next = c;
    else
        s->head = c;
    s->tail = c;
    s->queued += len;
    ++s->chunk_count;
    return 0;
}
int xp_stream_send(xp_stream *s, const uint8_t *data, size_t len) {
    if (!s || s->closed || s->ending || s->peer->dead)
        return -1;
    if (s->media && s->peer->quic && s->peer->datagrams)
        return xp_h3_send_datagram(s, data, len);
    if (len > (s->media ? XP_MEDIA_MAX : XP_CONTROL_MAX))
        return -1;
    uint8_t *framed = malloc(len + 16);
    if (!framed)
        return -1;
    size_t n = s->media ? xp_capsule_pack(framed, data, len) : xp_control_pack(framed, data, len);
    int rc = n ? xp_queue(s, framed, n) : -1;
    free(framed);
    if (!rc) {
        if (s->peer->quic)
            xp_h3_resume(s);
        else
            xp_h2_resume(s);
    }
    return rc;
}
int xp_stream_json(xp_stream *s, json_t *message) {
    char *text = json_dumps(message, JSON_COMPACT);
    if (!text)
        return -1;
    int rc = xp_stream_send(s, (const uint8_t *)text, strlen(text));
    free(text);
    return rc;
}
void xp_stream_finish(xp_stream *s) {
    if (!s || s->closed)
        return;
    s->ending = true;
    if (s->peer->quic)
        xp_h3_resume(s);
    else
        xp_h2_resume(s);
}
bool xp_peer_quic(const xp_peer *p) {
    return p->quic;
}
bool xp_peer_datagrams(const xp_peer *p) {
    return p->quic && p->datagrams;
}
void *xp_peer_user(const xp_peer *p) {
    return p->user_data;
}
void xp_peer_set_user(xp_peer *p, void *user) {
    p->user_data = user;
}
xp_peer *xp_stream_peer(xp_stream *s) {
    return s->peer;
}
void *xp_stream_user(xp_stream *s) {
    return s->user_data;
}
void xp_stream_set_user(xp_stream *s, void *user) {
    s->user_data = user;
}
bool xp_stream_media(const xp_stream *s) {
    return s->media;
}
const char *xp_stream_path(const xp_stream *s) {
    return s->path;
}
const char *xp_stream_username(const xp_stream *s) {
    return s->user;
}
const char *xp_stream_token(const xp_stream *s) {
    return s->token;
}
int xp_stream_status(const xp_stream *s) {
    return s->status;
}
