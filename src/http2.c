#include "transport_internal.h"
#include <event2/buffer.h>
#include <event2/bufferevent_ssl.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static ssize_t send_bytes(nghttp2_session *session, const uint8_t *data, size_t len, int flags,
                          void *arg) {
    (void)session;
    (void)flags;
    xp_peer *p = arg;
    if (evbuffer_get_length(bufferevent_get_output(p->bev)) > 4096)
        return NGHTTP2_ERR_WOULDBLOCK;
    return bufferevent_write(p->bev, data, len) ? NGHTTP2_ERR_CALLBACK_FAILURE : (ssize_t)len;
}
static ssize_t read_body(nghttp2_session *session, int32_t id, uint8_t *buf, size_t len,
                         uint32_t *flags, nghttp2_data_source *source, void *arg) {
    (void)session;
    (void)id;
    (void)arg;
    xp_stream *s = source->ptr;
    while (s->media && s->head && !s->head->offset && xp_now_ms() - s->head->created > 200)
        xp_pop_chunk(s);
    if (!s->head) {
        if (s->ending) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        return NGHTTP2_ERR_DEFERRED;
    }
    xp_chunk *c = s->head;
    size_t n = c->len - c->offset;
    if (n > len)
        n = len;
    memcpy(buf, c->data + c->offset, n);
    c->offset += n;
    if (c->offset == c->len)
        xp_pop_chunk(s);
    return (ssize_t)n;
}
static nghttp2_nv nv(const char *name, const char *value) {
    return (nghttp2_nv){(uint8_t *)name, (uint8_t *)value, strlen(name), strlen(value),
                        NGHTTP2_NV_FLAG_NONE};
}
static int begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *arg) {
    (void)session;
    xp_peer *p = arg;
    if (frame->hd.type != NGHTTP2_HEADERS)
        return 0;
    return xp_find_stream(p, frame->hd.stream_id, p->transport->server)
               ? 0
               : NGHTTP2_ERR_CALLBACK_FAILURE;
}
static int header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name,
                  size_t nlen, const uint8_t *value, size_t vlen, uint8_t flags, void *arg) {
    (void)session;
    (void)flags;
    xp_peer *p = arg;
    xp_stream *s = xp_find_stream(p, frame->hd.stream_id, false);
    return xp_header(s, name, nlen, value, vlen) ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
}
static int frame_received(nghttp2_session *session, const nghttp2_frame *frame, void *arg) {
    xp_peer *p = arg;
    if (frame->hd.type == NGHTTP2_SETTINGS && !(frame->hd.flags & NGHTTP2_FLAG_ACK)) {
        if (!p->transport->server) {
            if (!nghttp2_session_get_remote_settings(session,
                                                     NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL))
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            xp_ready(p);
        }
    }
    if (frame->hd.type == NGHTTP2_HEADERS) {
        xp_stream *s = xp_find_stream(p, frame->hd.stream_id, false);
        if (!s || s->accepted)
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        int status = xp_headers_complete(s);
        if (p->transport->server) {
            char code[4];
            snprintf(code, sizeof(code), "%03d", status);
            nghttp2_nv headers[] = {nv(":status", code), nv("capsule-protocol", "?1")};
            nghttp2_data_provider source = {.source.ptr = s, .read_callback = read_body};
            if (nghttp2_submit_response(session, (int32_t)s->id, headers, s->media ? 2 : 1,
                                        status == 200 ? &source : NULL))
                return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }
    if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        xp_stream *s = xp_find_stream(p, frame->hd.stream_id, false);
        if (s)
            xp_stream_finish(s);
    }
    return 0;
}
static int data_received(nghttp2_session *session, uint8_t flags, int32_t id, const uint8_t *data,
                         size_t len, void *arg) {
    (void)session;
    (void)flags;
    xp_peer *p = arg;
    return xp_receive(xp_find_stream(p, id, false), data, len) ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
}
static int stream_closed(nghttp2_session *session, int32_t id, uint32_t error, void *arg) {
    (void)session;
    (void)error;
    xp_stream_mark_closed(xp_find_stream(arg, id, false));
    return 0;
}
static int setup(xp_peer *p) {
    const unsigned char *selected;
    unsigned len;
    SSL_get0_alpn_selected(p->ssl, &selected, &len);
    if (len != 2 || memcmp(selected, "h2", 2))
        return -1;
    nghttp2_session_callbacks *cb;
    if (nghttp2_session_callbacks_new(&cb))
        return -1;
    nghttp2_session_callbacks_set_send_callback(cb, send_bytes);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cb, begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cb, header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, frame_received);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, data_received);
    nghttp2_session_callbacks_set_on_stream_close_callback(cb, stream_closed);
    int rc = p->transport->server ? nghttp2_session_server_new(&p->h2, cb, p)
                                  : nghttp2_session_client_new(&p->h2, cb, p);
    nghttp2_session_callbacks_del(cb);
    if (rc)
        return -1;
    nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1},
                                         {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 8},
                                         {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, 8192},
                                         {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, XP_QUEUE_MAX}};
    if (nghttp2_submit_settings(p->h2, 0, settings, sizeof(settings) / sizeof(settings[0])))
        return -1;
    xp_h2_flush(p);
    return 0;
}
void xp_h2_flush(xp_peer *p) {
    if (!p->dead && p->h2 && nghttp2_session_send(p->h2))
        xp_peer_close(p, "HTTP/2 write failed");
}
void xp_h2_resume(xp_stream *s) {
    if (s->peer->h2)
        nghttp2_session_resume_data(s->peer->h2, (int32_t)s->id);
    xp_schedule(s->peer);
}
static void read_ready(struct bufferevent *bev, void *arg) {
    xp_peer *p = arg;
    if (!p->h2 || p->dead)
        return;
    struct evbuffer *input = bufferevent_get_input(bev);
    while (evbuffer_get_length(input) && !p->dead) {
        size_t len = evbuffer_get_length(input);
        const uint8_t *data = evbuffer_pullup(input, (ev_ssize_t)len);
        ssize_t n = nghttp2_session_mem_recv(p->h2, data, len);
        if (n <= 0) {
            xp_peer_close(p, "Invalid HTTP/2 data");
            return;
        }
        evbuffer_drain(input, (size_t)n);
    }
    xp_h2_flush(p);
}
static void write_ready(struct bufferevent *bev, void *arg) {
    (void)bev;
    xp_h2_flush(arg);
}
static void socket_event(struct bufferevent *bev, short events, void *arg) {
    (void)bev;
    xp_peer *p = arg;
    if (events & BEV_EVENT_CONNECTED) {
        if (setup(p))
            xp_peer_close(p, "HTTPS negotiation failed");
        return;
    }
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        const char *reason = "HTTPS connection closed";
        if (!p->transport->server && SSL_get_verify_result(p->ssl) != X509_V_OK)
            reason = "Certificate verification failed";
        xp_peer_close(p, reason);
    }
}
static int make_socket(xp_peer *p, int fd, bool server) {
    p->ssl = SSL_new(p->transport->tls_ctx);
    if (!p->ssl)
        return -1;
    if (!server && (xp_tls_identity(p->ssl, p->host) ||
                    SSL_set_alpn_protos(p->ssl, (const uint8_t *)"\002h2", 3)))
        return -1;
    p->bev = bufferevent_openssl_socket_new(p->transport->base, fd, p->ssl,
                                            server ? BUFFEREVENT_SSL_ACCEPTING
                                                   : BUFFEREVENT_SSL_CONNECTING,
                                            BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    if (!p->bev)
        return -1;
    bufferevent_setcb(p->bev, read_ready, write_ready, socket_event, p);
    bufferevent_setwatermark(p->bev, EV_READ, 0, XP_QUEUE_MAX);
    bufferevent_setwatermark(p->bev, EV_WRITE, 1024, 0);
    struct timeval timeout = {45, 0};
    bufferevent_set_timeouts(p->bev, &timeout, &timeout);
    bufferevent_enable(p->bev, EV_READ | EV_WRITE);
    return 0;
}
void xp_h2_accept(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *addr,
                  int len, void *arg) {
    (void)listener;
    (void)addr;
    (void)len;
    xp_transport *t = arg;
    xp_peer *p = xp_peer_alloc(t, false);
    if (!p) {
        close(fd);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef TCP_NOTSENT_LOWAT
    setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &one, sizeof(one));
#endif
    if (make_socket(p, fd, true)) {
        close(fd);
        xp_peer_close(p, "TLS initialization failed");
    }
}
int xp_h2_connect(xp_peer *p, const char *port) {
    struct sockaddr_storage addr;
    socklen_t len;
    if (xp_resolve(p->host, port, SOCK_STREAM, false, &addr, &len) || make_socket(p, -1, false))
        return -1;
    if (bufferevent_socket_connect(p->bev, (struct sockaddr *)&addr, len))
        return -1;
    int one = 1;
    setsockopt(bufferevent_getfd(p->bev), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef TCP_NOTSENT_LOWAT
    setsockopt(bufferevent_getfd(p->bev), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &one, sizeof(one));
#endif
    return 0;
}
int xp_h2_open(xp_stream *s) {
    xp_peer *p = s->peer;
    char auth[104];
    snprintf(auth, sizeof(auth), "Bearer %s", s->token);
    nghttp2_nv headers[] = {nv(":method", "CONNECT"),       nv(":scheme", "https"),
                            nv(":authority", p->authority), nv(":path", s->path),
                            nv(":protocol", s->protocol),   nv("authorization", auth),
                            nv("x-xphone-user", s->user),   nv("capsule-protocol", "?1")};
    headers[5].flags = NGHTTP2_NV_FLAG_NO_INDEX;
    nghttp2_data_provider source = {.source.ptr = s, .read_callback = read_body};
    int32_t id = nghttp2_submit_request(p->h2, NULL, headers, s->media ? 8 : 7, &source, s);
    if (id < 0)
        return -1;
    s->id = id;
    xp_schedule(p);
    return 0;
}
