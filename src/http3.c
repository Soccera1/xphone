#include "transport_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* QUIC owns retransmission state; application buffers remain alive until the
 * corresponding HTTP/3 payload acknowledgment callback releases them. */
static void random_bytes(uint8_t *out, size_t len) {
    if (RAND_bytes(out, (int)len) != 1)
        abort();
}
static void quic_random(uint8_t *out, size_t len, const ngtcp2_rand_ctx *ctx) {
    (void)ctx;
    random_bytes(out, len);
}
static ngtcp2_conn *get_conn(ngtcp2_crypto_conn_ref *ref) {
    return ((xp_peer *)ref->user_data)->q;
}
static int new_cid(ngtcp2_conn *conn, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token,
                   size_t len, void *arg) {
    (void)conn;
    xp_peer *p = arg;
    if (p->id_count == 8)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = len;
    random_bytes(cid->data, len);
    random_bytes(token->data, sizeof(token->data));
    p->ids[p->id_count++].cid = *cid;
    return 0;
}
static int remove_cid(ngtcp2_conn *conn, const ngtcp2_cid *cid, void *arg) {
    (void)conn;
    xp_peer *p = arg;
    for (size_t i = 0; i < p->id_count; ++i) {
        if (ngtcp2_cid_eq(&p->ids[i].cid, cid)) {
            p->ids[i] = p->ids[--p->id_count];
            break;
        }
    }
    return 0;
}
static int h3_credit(nghttp3_conn *conn, int64_t id, size_t len, void *arg, void *stream_arg) {
    (void)conn;
    (void)stream_arg;
    xp_peer *p = arg;
    ngtcp2_conn_extend_max_stream_offset(p->q, id, len);
    ngtcp2_conn_extend_max_offset(p->q, len);
    return 0;
}
static int h3_data(nghttp3_conn *conn, int64_t id, const uint8_t *data, size_t len, void *arg,
                   void *stream_arg) {
    xp_peer *p = arg;
    if (xp_receive(xp_find_stream(p, id, false), data, len))
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    return h3_credit(conn, id, len, arg, stream_arg);
}
static int h3_begin(nghttp3_conn *conn, int64_t id, void *arg, void *stream_arg) {
    (void)stream_arg;
    xp_peer *p = arg;
    xp_stream *s = xp_find_stream(p, id, p->transport->server);
    if (!s)
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    nghttp3_conn_set_stream_user_data(conn, id, s);
    return 0;
}
static int h3_header(nghttp3_conn *conn, int64_t id, int32_t token, nghttp3_rcbuf *name,
                     nghttp3_rcbuf *value, uint8_t flags, void *arg, void *stream_arg) {
    (void)conn;
    (void)token;
    (void)flags;
    (void)stream_arg;
    nghttp3_vec n = nghttp3_rcbuf_get_buf(name), v = nghttp3_rcbuf_get_buf(value);
    return xp_header(xp_find_stream(arg, id, false), n.base, n.len, v.base, v.len)
               ? NGHTTP3_ERR_CALLBACK_FAILURE
               : 0;
}
static nghttp3_nv nv(const char *name, const char *value) {
    return (nghttp3_nv){(uint8_t *)name, (uint8_t *)value, strlen(name), strlen(value),
                        NGHTTP3_NV_FLAG_NONE};
}
static nghttp3_ssize h3_read(nghttp3_conn *conn, int64_t id, nghttp3_vec *vec, size_t count,
                             uint32_t *flags, void *arg, void *stream_arg) {
    (void)conn;
    (void)count;
    (void)stream_arg;
    xp_stream *s = xp_find_stream(arg, id, false);
    if (!s)
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    xp_chunk *c = s->head;
    while (c && c->offered)
        c = c->next;
    if (!c) {
        if (s->ending) {
            *flags |= NGHTTP3_DATA_FLAG_EOF;
            return 0;
        }
        return NGHTTP3_ERR_WOULDBLOCK;
    }
    c->offered = true;
    vec[0] = (nghttp3_vec){c->data, c->len};
    return 1;
}
static int h3_acked(nghttp3_conn *conn, int64_t id, uint64_t len, void *arg, void *stream_arg) {
    (void)conn;
    (void)stream_arg;
    xp_stream *s = xp_find_stream(arg, id, false);
    if (!s)
        return 0;
    while (len && s->head) {
        xp_chunk *c = s->head;
        size_t n = c->len - c->offset;
        if (n > len)
            n = (size_t)len;
        c->offset += n;
        len -= n;
        if (c->offset == c->len)
            xp_pop_chunk(s);
    }
    return 0;
}
static int h3_headers_done(nghttp3_conn *conn, int64_t id, int fin, void *arg, void *stream_arg) {
    (void)fin;
    (void)stream_arg;
    xp_peer *p = arg;
    xp_stream *s = xp_find_stream(p, id, false);
    if (!s || s->accepted)
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    int status = xp_headers_complete(s);
    if (p->transport->server) {
        char code[4];
        snprintf(code, sizeof(code), "%03d", status);
        nghttp3_nv headers[] = {nv(":status", code), nv("capsule-protocol", "?1")};
        nghttp3_data_reader source = {.read_data = h3_read};
        if (nghttp3_conn_submit_response(conn, id, headers, s->media ? 2 : 1,
                                         status == 200 ? &source : NULL))
            return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
static int h3_end(nghttp3_conn *conn, int64_t id, void *arg, void *stream_arg) {
    (void)conn;
    (void)stream_arg;
    xp_stream *s = xp_find_stream(arg, id, false);
    if (s)
        xp_stream_finish(s);
    return 0;
}
static int h3_closed(nghttp3_conn *conn, int64_t id, uint64_t code, void *arg, void *stream_arg) {
    (void)conn;
    (void)code;
    (void)stream_arg;
    xp_stream_mark_closed(xp_find_stream(arg, id, false));
    return 0;
}
static int h3_stop(nghttp3_conn *conn, int64_t id, uint64_t code, void *arg, void *stream_arg) {
    (void)conn;
    (void)stream_arg;
    return ngtcp2_conn_shutdown_stream_read(((xp_peer *)arg)->q, 0, id, code)
               ? NGHTTP3_ERR_CALLBACK_FAILURE
               : 0;
}
static int h3_reset(nghttp3_conn *conn, int64_t id, uint64_t code, void *arg, void *stream_arg) {
    (void)conn;
    (void)stream_arg;
    return ngtcp2_conn_shutdown_stream_write(((xp_peer *)arg)->q, 0, id, code)
               ? NGHTTP3_ERR_CALLBACK_FAILURE
               : 0;
}
static int h3_settings(nghttp3_conn *conn, const nghttp3_proto_settings *settings, void *arg) {
    (void)conn;
    xp_peer *p = arg;
    const ngtcp2_transport_params *tp = ngtcp2_conn_get_remote_transport_params2(p->q);
    p->datagrams = settings->h3_datagram && tp && tp->max_datagram_frame_size >= XP_MEDIA_MAX + 8;
    if (!p->transport->server) {
        if (!settings->enable_connect_protocol)
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        xp_ready(p);
    }
    return 0;
}
static int setup_h3(xp_peer *p) {
    if (p->h3)
        return 0;
    nghttp3_callbacks cb = {.acked_stream_data = h3_acked,
                            .stream_close = h3_closed,
                            .recv_data = h3_data,
                            .deferred_consume = h3_credit,
                            .begin_headers = h3_begin,
                            .recv_header = h3_header,
                            .end_headers = h3_headers_done,
                            .end_stream = h3_end,
                            .stop_sending = h3_stop,
                            .reset_stream = h3_reset,
                            .rand = random_bytes,
                            .recv_settings2 = h3_settings};
    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    settings.enable_connect_protocol = 1;
    settings.h3_datagram = 1;
    settings.max_field_section_size = 8192;
    settings.qpack_max_dtable_capacity = 0;
    settings.qpack_blocked_streams = 0;
    int rc = p->transport->server ? nghttp3_conn_server_new(&p->h3, &cb, &settings, NULL, p)
                                  : nghttp3_conn_client_new(&p->h3, &cb, &settings, NULL, p);
    if (rc)
        return -1;
    if (p->transport->server)
        nghttp3_conn_set_max_client_streams_bidi(p->h3, 8);
    int64_t ctrl, enc, dec;
    if (ngtcp2_conn_open_uni_stream(p->q, &ctrl, NULL) ||
        ngtcp2_conn_open_uni_stream(p->q, &enc, NULL) ||
        ngtcp2_conn_open_uni_stream(p->q, &dec, NULL) ||
        nghttp3_conn_bind_control_stream(p->h3, ctrl) ||
        nghttp3_conn_bind_qpack_streams(p->h3, enc, dec))
        return -1;
    return 0;
}
static int key_ready(ngtcp2_conn *conn, ngtcp2_encryption_level level, void *arg) {
    (void)conn;
    return level == NGTCP2_ENCRYPTION_LEVEL_1RTT && setup_h3(arg) ? NGTCP2_ERR_CALLBACK_FAILURE : 0;
}
static int stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t id, uint64_t offset,
                       const uint8_t *data, size_t len, void *arg, void *stream_arg) {
    (void)offset;
    (void)stream_arg;
    xp_peer *p = arg;
    if (!p->h3)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    nghttp3_ssize n = nghttp3_conn_read_stream2(
        p->h3, id, data, len, !!(flags & NGTCP2_STREAM_DATA_FLAG_FIN), xp_now_ns());
    if (n < 0)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    ngtcp2_conn_extend_max_stream_offset(conn, id, (uint64_t)n);
    ngtcp2_conn_extend_max_offset(conn, (uint64_t)n);
    return 0;
}
static int stream_acked(ngtcp2_conn *conn, int64_t id, uint64_t offset, uint64_t len, void *arg,
                        void *stream_arg) {
    (void)conn;
    (void)offset;
    (void)stream_arg;
    xp_peer *p = arg;
    return p->h3 && nghttp3_conn_add_ack_offset(p->h3, id, len) ? NGTCP2_ERR_CALLBACK_FAILURE : 0;
}
static int stream_unblocked(ngtcp2_conn *conn, int64_t id, uint64_t max, void *arg,
                            void *stream_arg) {
    (void)conn;
    (void)max;
    (void)stream_arg;
    xp_peer *p = arg;
    return p->h3 && nghttp3_conn_unblock_stream(p->h3, id) ? NGTCP2_ERR_CALLBACK_FAILURE : 0;
}
static int remote_stream_credit(ngtcp2_conn *conn, uint64_t max_streams, void *arg) {
    (void)conn;
    xp_peer *p = arg;
    if (p->h3 && p->transport->server)
        nghttp3_conn_set_max_client_streams_bidi(p->h3, max_streams);
    return 0;
}
static int quic_stream_closed(ngtcp2_conn *conn, uint32_t flags, int64_t id, uint64_t rx_code,
                              uint64_t tx_code, void *arg, void *stream_arg) {
    (void)flags;
    (void)tx_code;
    (void)stream_arg;
    xp_peer *p = arg;
    if (p->h3)
        nghttp3_conn_close_stream(p->h3, id, rx_code);
    if ((id & 3) == 0 && p->transport->server)
        ngtcp2_conn_extend_max_streams_bidi(conn, 1);
    return 0;
}
static int datagram_received(ngtcp2_conn *conn, uint32_t flags, const uint8_t *data, size_t len,
                             void *arg) {
    (void)conn;
    xp_peer *p = arg;
    if (flags & NGTCP2_DATAGRAM_FLAG_0RTT)
        return 0;
    uint64_t quarter;
    int prefix = xp_varint_read(data, len, &quarter);
    if (!prefix || quarter >= (1ULL << 60) || len - (size_t)prefix > XP_MEDIA_MAX)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    xp_stream *s = xp_find_stream(p, (int64_t)(quarter * 4), false);
    /* Datagram and header packets may arrive out of order. Ignore until accepted. */
    if (!s || !s->media || !s->accepted || s->ending || s->closed)
        return 0;
    return p->transport->cb.record && p->transport->cb.record(
                                          s, data + prefix, len - (size_t)prefix, p->transport->arg)
               ? NGTCP2_ERR_CALLBACK_FAILURE
               : 0;
}
static int make_quic(xp_peer *p, const ngtcp2_pkt_hd *hd) {
    bool server = p->transport->server;
    if (server && !hd)
        return -1;
    ngtcp2_callbacks cb = {.client_initial = server ? NULL : ngtcp2_crypto_client_initial_cb,
                           .recv_client_initial =
                               server ? ngtcp2_crypto_recv_client_initial_cb : NULL,
                           .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
                           .encrypt = ngtcp2_crypto_encrypt_cb,
                           .decrypt = ngtcp2_crypto_decrypt_cb,
                           .hp_mask = ngtcp2_crypto_hp_mask_cb,
                           .recv_stream_data = stream_data,
                           .acked_stream_data_offset = stream_acked,
                           .recv_retry = server ? NULL : ngtcp2_crypto_recv_retry_cb,
                           .rand = quic_random,
                           .get_new_connection_id2 = new_cid,
                           .remove_connection_id = remove_cid,
                           .update_key = ngtcp2_crypto_update_key_cb,
                           .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
                           .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
                           .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
                           .get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb,
                           .recv_rx_key = server ? NULL : key_ready,
                           .recv_tx_key = server ? key_ready : NULL,
                           .extend_max_stream_data = stream_unblocked,
                           .extend_max_remote_streams_bidi = remote_stream_credit,
                           .stream_close2 = quic_stream_closed,
                           .recv_datagram = datagram_received};
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = xp_now_ns();
    settings.max_tx_udp_payload_size = 1200;
    settings.handshake_timeout = 5 * NGTCP2_SECONDS;
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = XP_QUEUE_MAX;
    params.initial_max_stream_data_bidi_remote = XP_QUEUE_MAX;
    params.initial_max_stream_data_uni = XP_QUEUE_MAX;
    params.initial_max_data = XP_QUEUE_MAX * 4;
    params.initial_max_streams_bidi = server ? 8 : 0;
    params.initial_max_streams_uni = 3;
    params.max_idle_timeout = 45 * NGTCP2_SECONDS;
    params.max_datagram_frame_size = 1200;
    params.active_connection_id_limit = 4;
    ngtcp2_cid scid = {.datalen = 16}, dcid = {.datalen = 16};
    random_bytes(scid.data, 16);
    random_bytes(dcid.data, 16);
    p->ids[p->id_count++].cid = scid;
    ngtcp2_path path = {.local = {(struct sockaddr *)&p->local, p->local_len},
                        .remote = {(struct sockaddr *)&p->remote, p->remote_len}};
    int rc;
    if (server) {
        params.original_dcid = hd->dcid;
        params.original_dcid_present = 1;
        p->original_dcid = hd->dcid;
        rc = ngtcp2_conn_server_new(&p->q, &hd->scid, &scid, &path, hd->version, &cb, &settings,
                                    &params, NULL, p);
    } else {
        rc = ngtcp2_conn_client_new(&p->q, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cb, &settings,
                                    &params, NULL, p);
    }
    if (rc)
        return -1;
    p->ssl = SSL_new(p->transport->quic_ctx);
    if (!p->ssl || ngtcp2_crypto_ossl_ctx_new(&p->tls, p->ssl))
        return -1;
    p->ref = (ngtcp2_crypto_conn_ref){.get_conn = get_conn, .user_data = p};
    SSL_set_app_data(p->ssl, &p->ref);
    if (server) {
        if (ngtcp2_crypto_ossl_configure_server_session(p->ssl))
            return -1;
        SSL_set_accept_state(p->ssl);
    } else {
        if (ngtcp2_crypto_ossl_configure_client_session(p->ssl) || xp_tls_identity(p->ssl, p->host))
            return -1;
        SSL_set_connect_state(p->ssl);
        SSL_set_alpn_protos(p->ssl, (const uint8_t *)"\002h3", 3);
    }
    ngtcp2_conn_set_tls_native_handle(p->q, p->tls);
    return 0;
}
static void arm_timer(xp_peer *p) {
    if (p->dead || !p->timer || !p->q)
        return;
    uint64_t expiry = ngtcp2_conn_get_expiry2(p->q), now = xp_now_ns();
    uint64_t us = expiry > now ? (expiry - now + 999) / 1000 : 1;
    struct timeval tv = {(time_t)(us / 1000000), (suseconds_t)(us % 1000000)};
    evtimer_add(p->timer, &tv);
}
static void quic_error(xp_peer *p, int code) {
    if (code == NGTCP2_ERR_DRAINING || code == NGTCP2_ERR_CLOSING) {
        xp_peer_close(p, "Peer disconnected");
        return;
    }
    const char *reason = "QUIC connection failed";
    if (!p->transport->server && SSL_get_verify_result(p->ssl) != X509_V_OK)
        reason = "Certificate verification failed";
    xp_log("HTTP/3: %s", ngtcp2_strerror(code));
    xp_peer_close(p, reason);
}
static bool transmit(xp_peer *p, ngtcp2_path *path, const uint8_t *packet, size_t len) {
    ssize_t n = sendto(p->udp_fd, packet, len, 0, path->remote.addr, path->remote.addrlen);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
        xp_peer_close(p, "QUIC socket write failed");
        return false;
    }
    return true;
}
static void pop_datagram(xp_peer *p) {
    xp_chunk *c = p->datagram_head;
    p->datagram_head = c->next;
    if (!p->datagram_head)
        p->datagram_tail = NULL;
    p->datagram_queued -= c->len;
    free(c);
}
void xp_h3_flush(xp_peer *p) {
    if (p->dead || !p->q)
        return;
    uint8_t packet[1500];
    ngtcp2_path_storage path;
    ngtcp2_path_storage_zero(&path);
    ngtcp2_pkt_info pi = {0};
    /* Service reliable control first, then real-time media. All work is bounded. */
    for (int round = 0; round < 64 && !p->dead; ++round) {
        nghttp3_vec vec[8];
        int64_t id = -1;
        int fin = 0;
        nghttp3_ssize count = p->h3 ? nghttp3_conn_writev_stream(p->h3, &id, &fin, vec, 8) : 0;
        if (count < 0) {
            xp_peer_close(p, "HTTP/3 stream write failed");
            return;
        }
        ngtcp2_ssize consumed = -1;
        ngtcp2_ssize n =
            ngtcp2_conn_writev_stream(p->q, &path.path, &pi, packet, sizeof(packet), &consumed,
                                      fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0, id,
                                      (const ngtcp2_vec *)vec, (size_t)count, xp_now_ns());
        if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
            nghttp3_conn_block_stream(p->h3, id);
            continue;
        }
        if (n == NGTCP2_ERR_STREAM_SHUT_WR) {
            nghttp3_conn_shutdown_stream_write(p->h3, id);
            continue;
        }
        if (n < 0) {
            quic_error(p, (int)n);
            return;
        }
        if (consumed >= 0 && p->h3 && id >= 0 &&
            nghttp3_conn_add_write_offset(p->h3, id, (uint64_t)consumed)) {
            xp_peer_close(p, "HTTP/3 stream offset failed");
            return;
        }
        if (n > 0 && !transmit(p, &path.path, packet, (size_t)n))
            return;
        if (!n || id == -1)
            break;
    }
    for (int round = 0; round < 16 && p->datagram_head && !p->dead; ++round) {
        xp_chunk *c = p->datagram_head;
        if (xp_now_ms() - c->created > 200) {
            pop_datagram(p);
            continue;
        }
        ngtcp2_vec v = {c->data, c->len};
        int accepted = 0;
        ngtcp2_ssize n = ngtcp2_conn_writev_datagram(p->q, &path.path, &pi, packet, sizeof(packet),
                                                     &accepted, 0, 0, &v, 1, xp_now_ns());
        if (n < 0) {
            quic_error(p, (int)n);
            return;
        }
        if (accepted)
            pop_datagram(p);
        if (n > 0 && !transmit(p, &path.path, packet, (size_t)n))
            return;
        if (!n)
            break;
    }
    ngtcp2_conn_update_pkt_tx_time(p->q, xp_now_ns());
    arm_timer(p);
}
void xp_h3_resume(xp_stream *s) {
    if (s->peer->h3)
        nghttp3_conn_resume_stream(s->peer->h3, s->id);
    xp_schedule(s->peer);
}
int xp_h3_send_datagram(xp_stream *s, const uint8_t *data, size_t len) {
    xp_peer *p = s->peer;
    if (len > XP_MEDIA_MAX || s->id < 0)
        return -1;
    while (p->datagram_head && p->datagram_queued + len > XP_MEDIA_MAX * 10)
        pop_datagram(p);
    xp_chunk *c = malloc(sizeof(*c) + len + 8);
    if (!c)
        return -1;
    *c = (xp_chunk){.created = xp_now_ms()};
    size_t prefix = xp_varint_write(c->data, (uint64_t)s->id / 4);
    memcpy(c->data + prefix, data, len);
    c->len = prefix + len;
    if (p->datagram_tail)
        p->datagram_tail->next = c;
    else
        p->datagram_head = c;
    p->datagram_tail = c;
    p->datagram_queued += c->len;
    xp_schedule(p);
    return 0;
}
static void timer_ready(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    xp_peer *p = arg;
    if (p->dead)
        return;
    int rc = ngtcp2_conn_handle_expiry(p->q, xp_now_ns());
    if (rc) {
        quic_error(p, rc);
        return;
    }
    xp_h3_flush(p);
}
static bool cid_match(xp_peer *p, const uint8_t *data, size_t len) {
    if (p->original_dcid.datalen == len && !memcmp(p->original_dcid.data, data, len))
        return true;
    for (size_t i = 0; i < p->id_count; ++i)
        if (p->ids[i].cid.datalen == len && !memcmp(p->ids[i].cid.data, data, len))
            return true;
    return false;
}
static void receive_packet(xp_peer *p, struct sockaddr_storage *remote, socklen_t remote_len,
                           const uint8_t *data, size_t len) {
    ngtcp2_path path = {.local = {(struct sockaddr *)&p->local, p->local_len},
                        .remote = {(struct sockaddr *)remote, remote_len}};
    ngtcp2_pkt_info pi = {0};
    int rc = ngtcp2_conn_read_pkt(p->q, &path, &pi, data, len, xp_now_ns());
    if (rc == NGTCP2_ERR_DISCARD_PKT || rc == NGTCP2_ERR_DECRYPT)
        return;
    if (rc) {
        quic_error(p, rc);
        return;
    }
    xp_h3_flush(p);
}
static void read_client(evutil_socket_t fd, short events, void *arg) {
    (void)events;
    xp_peer *p = arg;
    uint8_t packet[65536];
    for (int i = 0; i < 64 && !p->dead; ++i) {
        struct sockaddr_storage remote;
        socklen_t len = sizeof(remote);
        ssize_t n = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&remote, &len);
        if (n < 0)
            break;
        receive_packet(p, &remote, len, packet, (size_t)n);
    }
}
static void read_server(evutil_socket_t fd, short events, void *arg) {
    (void)events;
    xp_transport *t = arg;
    uint8_t packet[65536];
    for (int i = 0; i < 128; ++i) {
        struct sockaddr_storage remote;
        socklen_t len = sizeof(remote);
        ssize_t n = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&remote, &len);
        if (n < 0)
            break;
        ngtcp2_version_cid vc;
        if (ngtcp2_pkt_decode_version_cid(&vc, packet, (size_t)n, 16))
            continue;
        xp_peer *p;
        for (p = t->peers; p; p = p->next)
            if (p->quic && !p->dead && cid_match(p, vc.dcid, vc.dcidlen))
                break;
        if (!p) {
            ngtcp2_pkt_hd hd;
            if (ngtcp2_accept(&hd, packet, (size_t)n) || hd.version != NGTCP2_PROTO_VER_V1)
                continue;
            p = xp_peer_alloc(t, true);
            if (!p)
                continue;
            p->udp_fd = t->udp_fd;
            p->local = t->bind_addr;
            p->local_len = t->bind_len;
            p->remote = remote;
            p->remote_len = len;
            if (make_quic(p, &hd)) {
                xp_peer_close(p, "QUIC setup failed");
                continue;
            }
            p->timer = evtimer_new(t->base, timer_ready, p);
        }
        receive_packet(p, &remote, len, packet, (size_t)n);
    }
}
int xp_h3_listen(xp_transport *t) {
    t->udp_fd = socket(t->bind_addr.ss_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (t->udp_fd < 0)
        return -1;
    int one = 1;
    setsockopt(t->udp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(t->udp_fd, (struct sockaddr *)&t->bind_addr, t->bind_len))
        return -1;
    t->udp_event = event_new(t->base, t->udp_fd, EV_READ | EV_PERSIST, read_server, t);
    return event_add(t->udp_event, NULL);
}
int xp_h3_connect(xp_peer *p, const char *port) {
    if (xp_resolve(p->host, port, SOCK_DGRAM, false, &p->remote, &p->remote_len))
        return -1;
    p->udp_fd = socket(p->remote.ss_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (p->udp_fd < 0)
        return -1;
    /* Route selection provides a concrete local address for QUIC path validation. */
    if (connect(p->udp_fd, (struct sockaddr *)&p->remote, p->remote_len))
        return -1;
    p->local_len = sizeof(p->local);
    if (getsockname(p->udp_fd, (struct sockaddr *)&p->local, &p->local_len) || make_quic(p, NULL))
        return -1;
    p->read_event = event_new(p->transport->base, p->udp_fd, EV_READ | EV_PERSIST, read_client, p);
    p->timer = evtimer_new(p->transport->base, timer_ready, p);
    event_add(p->read_event, NULL);
    xp_schedule(p);
    return 0;
}
int xp_h3_open(xp_stream *s) {
    xp_peer *p = s->peer;
    if (ngtcp2_conn_open_bidi_stream(p->q, &s->id, NULL))
        return -1;
    char auth[104];
    snprintf(auth, sizeof(auth), "Bearer %s", s->token);
    nghttp3_nv headers[] = {nv(":method", "CONNECT"),       nv(":scheme", "https"),
                            nv(":authority", p->authority), nv(":path", s->path),
                            nv(":protocol", s->protocol),   nv("authorization", auth),
                            nv("x-xphone-user", s->user),   nv("capsule-protocol", "?1")};
    headers[5].flags = NGHTTP3_NV_FLAG_NEVER_INDEX;
    nghttp3_data_reader source = {.read_data = h3_read};
    if (nghttp3_conn_submit_request(p->h3, s->id, headers, s->media ? 8 : 7, &source, s))
        return -1;
    xp_schedule(p);
    return 0;
}
