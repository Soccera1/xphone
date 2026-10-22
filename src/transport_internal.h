#ifndef XPHONE_TRANSPORT_INTERNAL_H
#define XPHONE_TRANSPORT_INTERNAL_H
#include "transport.h"
#include "wire.h"
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <nghttp2/nghttp2.h>
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
typedef struct xp_chunk {
    struct xp_chunk *next;
    size_t len, offset;
    bool offered;
    uint64_t created;
    uint8_t data[];
} xp_chunk;
struct xp_stream {
    xp_peer *peer;
    struct xp_stream *next;
    int64_t id;
    char path[128], method[16], protocol[40], user[XP_NAME], token[96];
    size_t header_bytes;
    int status;
    bool media, accepted, ending, closed;
    xp_parser parser;
    xp_chunk *head, *tail;
    size_t queued;
    size_t chunk_count;
    void *user_data;
};
struct xp_peer {
    xp_transport *transport;
    struct xp_peer *next;
    xp_stream *streams;
    void *user_data;
    bool quic, dead, ready, datagrams;
    char host[256], authority[280];
    uint64_t created;
    SSL *ssl;
    struct bufferevent *bev;
    nghttp2_session *h2;
    ngtcp2_conn *q;
    nghttp3_conn *h3;
    ngtcp2_crypto_ossl_ctx *tls;
    ngtcp2_crypto_conn_ref ref;
    int udp_fd;
    struct sockaddr_storage local, remote;
    socklen_t local_len, remote_len;
    struct event *read_event, *timer, *destroy_event, *flush_event;
    ngtcp2_cid original_dcid;
    struct {
        ngtcp2_cid cid;
    } ids[8];
    size_t id_count;
    xp_chunk *datagram_head, *datagram_tail;
    size_t datagram_queued;
};
struct xp_transport {
    struct event_base *base;
    bool server, closing;
    SSL_CTX *tls_ctx, *quic_ctx;
    xp_transport_callbacks cb;
    void *arg;
    xp_peer *peers;
    size_t peer_count;
    struct evconnlistener *listener;
    int udp_fd;
    struct event *udp_event;
    struct sockaddr_storage bind_addr;
    socklen_t bind_len;
};
xp_peer *xp_peer_alloc(xp_transport *t, bool quic);
xp_stream *xp_find_stream(xp_peer *p, int64_t id, bool create);
void xp_stream_dispose(xp_stream *s);
int xp_header(xp_stream *s, const uint8_t *name, size_t nlen, const uint8_t *value, size_t vlen);
int xp_headers_complete(xp_stream *s);
int xp_receive(xp_stream *s, const uint8_t *data, size_t len);
void xp_stream_mark_closed(xp_stream *s);
void xp_ready(xp_peer *p);
void xp_schedule(xp_peer *p);
int xp_queue(xp_stream *s, const uint8_t *data, size_t len);
void xp_pop_chunk(xp_stream *s);
int xp_resolve(const char *host, const char *port, int type, bool passive,
               struct sockaddr_storage *out, socklen_t *len);
int xp_tls_identity(SSL *ssl, const char *host);
void xp_h2_accept(struct evconnlistener *, evutil_socket_t, struct sockaddr *, int, void *);
int xp_h2_connect(xp_peer *p, const char *port);
int xp_h2_open(xp_stream *s);
void xp_h2_resume(xp_stream *s);
void xp_h2_flush(xp_peer *p);
int xp_h3_listen(xp_transport *t);
int xp_h3_connect(xp_peer *p, const char *port);
int xp_h3_open(xp_stream *s);
void xp_h3_flush(xp_peer *p);
void xp_h3_resume(xp_stream *s);
int xp_h3_send_datagram(xp_stream *s, const uint8_t *data, size_t len);
#endif
