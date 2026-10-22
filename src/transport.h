#ifndef XPHONE_TRANSPORT_H
#define XPHONE_TRANSPORT_H
#include "common.h"
#include <event2/event.h>
typedef struct xp_transport xp_transport;
typedef struct xp_peer xp_peer;
typedef struct xp_stream xp_stream;
typedef struct {
    int (*headers)(xp_stream *stream, void *arg);
    int (*record)(xp_stream *stream, const uint8_t *data, size_t len, void *arg);
    void (*ready)(xp_peer *peer, void *arg);
    void (*closed)(xp_peer *peer, const char *reason, void *arg);
    void (*stream_closed)(xp_stream *stream, void *arg);
} xp_transport_callbacks;
xp_transport *xp_transport_new(struct event_base *base, bool server, const char *cert,
                               const char *key, const char *ca, xp_transport_callbacks cb,
                               void *arg);
void xp_transport_free(xp_transport *t);
void xp_transport_expire_pending(xp_transport *t, uint64_t timeout_ms);
int xp_transport_listen(xp_transport *t, const char *host, const char *port, bool quic);
xp_peer *xp_transport_connect(xp_transport *t, const char *host, const char *port,
                              const char *authority, bool quic);
xp_stream *xp_peer_open(xp_peer *p, const char *path, bool media, const char *user,
                        const char *token);
void xp_peer_close(xp_peer *p, const char *reason);
bool xp_peer_quic(const xp_peer *p);
bool xp_peer_datagrams(const xp_peer *p);
void *xp_peer_user(const xp_peer *p);
void xp_peer_set_user(xp_peer *p, void *user);
xp_peer *xp_stream_peer(xp_stream *s);
void *xp_stream_user(xp_stream *s);
void xp_stream_set_user(xp_stream *s, void *user);
bool xp_stream_media(const xp_stream *s);
const char *xp_stream_path(const xp_stream *s);
const char *xp_stream_username(const xp_stream *s);
const char *xp_stream_token(const xp_stream *s);
int xp_stream_status(const xp_stream *s);
int xp_stream_send(xp_stream *s, const uint8_t *data, size_t len);
int xp_stream_json(xp_stream *s, json_t *message);
void xp_stream_finish(xp_stream *s);
#endif
