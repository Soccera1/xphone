#include "common.h"
#include "transport.h"
#include "wire.h"
#include <event2/event.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

typedef struct user user;
typedef struct {
    struct event_base *base;
    xp_transport *transport;
    user *users;
    size_t count;
} server;
struct user {
    char name[XP_NAME], hash[65];
    xp_peer *peer;
    xp_stream *control, *media;
    user *other;
    uint64_t last_seen, call, deadline, rate_start;
    unsigned controls, frames;
    uint32_t generation;
    int capture, playback, tx;
    bool active, outgoing, hello;
};
static int send_json(user *u, json_t *obj) {
    int rc = u->control ? xp_stream_json(u->control, obj) : -1;
    json_decref(obj);
    return rc;
}
static void error_message(user *u, const char *message) {
    send_json(u, json_pack("{s:s,s:s}", "type", "error", "message", message));
}
static void presence(server *s) {
    json_t *list = json_array();
    for (size_t i = 0; i < s->count; ++i) {
        user *u = &s->users[i];
        if (u->peer && u->hello)
            json_array_append_new(list,
                                  json_pack("{s:s,s:b}", "name", u->name, "busy", u->call != 0));
    }
    json_t *event = json_pack("{s:s,s:O}", "type", "presence", "users", list);
    for (size_t i = 0; i < s->count; ++i)
        if (s->users[i].control)
            xp_stream_json(s->users[i].control, event);
    json_decref(event);
    json_decref(list);
}
static void end_call(user *u, const char *reason) {
    user *other = u->other;
    if (!u->call)
        return;
    user *ends[] = {u, other};
    for (size_t i = 0; i < 2; ++i) {
        user *v = ends[i];
        if (!v)
            continue;
        uint64_t call = v->call;
        v->call = 0;
        v->other = NULL;
        v->active = v->outgoing = false;
        if (v->media) {
            xp_stream_finish(v->media);
            v->media = NULL;
        }
        send_json(v, json_pack("{s:s,s:I,s:s}", "type", "ended", "call", (json_int_t)call, "reason",
                               reason));
    }
}
static int min_channels(int a, int b) {
    return a < b ? a : b;
}
static void call_format(user *u) {
    user *other = u->other;
    uint32_t generation = u->generation > other->generation ? u->generation : other->generation;
    u->generation = other->generation = generation + 1;
    u->tx = min_channels(u->capture, other->playback);
    other->tx = min_channels(other->capture, u->playback);
    user *ends[] = {u, other};
    for (size_t i = 0; i < 2; ++i) {
        user *v = ends[i];
        send_json(v, json_pack("{s:s,s:I,s:i,s:i,s:i,s:s}", "type", "active", "call",
                               (json_int_t)v->call, "generation", (int)v->generation, "tx_channels",
                               v->tx, "rx_channels", v->other->tx, "peer", v->other->name));
    }
}
static user *find_user(server *s, const char *name) {
    for (size_t i = 0; i < s->count; ++i)
        if (!strcmp(s->users[i].name, name))
            return &s->users[i];
    return NULL;
}
static int headers(xp_stream *stream, void *arg) {
    server *s = arg;
    xp_peer *peer = xp_stream_peer(stream);
    user *u = find_user(s, xp_stream_username(stream));
    char hash[65];
    xp_sha256(xp_stream_token(stream), hash);
    if (!u || CRYPTO_memcmp(hash, u->hash, 64))
        return 401;
    if (!xp_stream_media(stream)) {
        if (strcmp(xp_stream_path(stream), "/v1/session"))
            return 404;
        if (u->peer)
            return 409;
        u->peer = peer;
        u->control = stream;
        u->last_seen = xp_now_ms();
        u->capture = u->playback = 1;
        u->hello = false;
        xp_peer_set_user(peer, u);
        xp_stream_set_user(stream, u);
        send_json(u, json_pack("{s:s,s:i,s:s}", "type", "welcome", "version", 1, "user", u->name));
        xp_log("Connected %s over HTTP/%s", u->name, xp_peer_quic(peer) ? "3" : "2");
        return 200;
    }
    char path[128];
    snprintf(path, sizeof(path), "/v1/calls/%llu/media", (unsigned long long)u->call);
    if (peer != u->peer || !u->active || !u->call || strcmp(path, xp_stream_path(stream)))
        return 403;
    if (u->media)
        return 409;
    u->media = stream;
    xp_stream_set_user(stream, u);
    return 200;
}
static int record(xp_stream *stream, const uint8_t *data, size_t len, void *arg) {
    server *s = arg;
    user *u = xp_stream_user(stream);
    if (!u || u->peer != xp_stream_peer(stream))
        return -1;
    uint64_t now = xp_now_ms();
    if (now - u->rate_start >= 1000) {
        u->rate_start = now;
        u->controls = u->frames = 0;
    }
    if (xp_stream_media(stream)) {
        if (++u->frames > 100)
            return 0;
        xp_media frame;
        if (xp_media_unpack(&frame, data, len))
            return -1;
        if (!u->active || !u->other || frame.call != u->call || frame.generation != u->generation ||
            frame.channels != u->tx)
            return 0;
        if (u->other->media && xp_stream_send(u->other->media, data, len)) {
            end_call(u, "Media connection congested");
            presence(s);
        }
        return 0;
    }
    if (++u->controls > 50)
        return -1;
    json_error_t error;
    json_t *obj = json_loadb((const char *)data, len, JSON_REJECT_DUPLICATES, &error);
    if (!json_is_object(obj)) {
        json_decref(obj);
        return -1;
    }
    const char *type = xp_str(obj, "type", "");
    u->last_seen = now;
    if (!strcmp(type, "hello")) {
        int capture = xp_int(obj, "capture", -1, 1, 2),
            playback = xp_int(obj, "playback", -1, 1, 2);
        if (xp_int(obj, "version", -1, 1, 1) != 1 || capture < 1 || playback < 1) {
            json_decref(obj);
            return -1;
        }
        bool changed = capture != u->capture || playback != u->playback;
        u->capture = capture;
        u->playback = playback;
        u->hello = true;
        if (changed && u->active)
            call_format(u);
        presence(s);
    } else if (!strcmp(type, "ping")) {
        send_json(u, json_pack("{s:s}", "type", "pong"));
    } else if (!u->hello) {
        error_message(u, "Send hello before calling");
    } else if (!strcmp(type, "call")) {
        user *other = find_user(s, xp_str(obj, "to", ""));
        if (u->call)
            error_message(u, "Already in a call");
        else if (!other || !other->peer || !other->hello)
            error_message(u, "User is offline");
        else if (other == u)
            error_message(u, "Cannot call yourself");
        else if (other->call)
            error_message(u, "User is busy");
        else {
            uint64_t id;
            if (RAND_bytes((uint8_t *)&id, sizeof(id)) != 1)
                abort();
            id = (id & INT64_MAX) | 1;
            u->call = other->call = id;
            u->other = other;
            other->other = u;
            u->outgoing = true;
            other->outgoing = false;
            u->deadline = other->deadline = now + 30000;
            send_json(u, json_pack("{s:s,s:I,s:s}", "type", "ringing", "call", (json_int_t)id,
                                   "peer", other->name));
            send_json(other, json_pack("{s:s,s:I,s:s}", "type", "incoming", "call", (json_int_t)id,
                                       "peer", u->name));
            presence(s);
        }
    } else if (!strcmp(type, "accept") || !strcmp(type, "reject") || !strcmp(type, "cancel") ||
               !strcmp(type, "hangup")) {
        json_int_t id = json_integer_value(json_object_get(obj, "call"));
        if (!u->call || id <= 0 || (uint64_t)id != u->call) {
            /* A media FIN can race the reliable ended event. Closing is
             * idempotent, and an old close must never affect a new call. */
            if (strcmp(type, "hangup") && strcmp(type, "cancel"))
                error_message(u, "Call no longer exists");
        } else if (!strcmp(type, "accept")) {
            if (u->active || u->outgoing)
                error_message(u, "Call cannot be accepted");
            else {
                u->active = u->other->active = true;
                call_format(u);
                presence(s);
            }
        } else {
            end_call(u, !strcmp(type, "reject") ? "Call rejected" : "Call ended");
            presence(s);
        }
    } else
        error_message(u, "Unknown message type");
    json_decref(obj);
    return 0;
}
static void closed(xp_peer *peer, const char *reason, void *arg) {
    server *s = arg;
    user *u = xp_peer_user(peer);
    if (!u || u->peer != peer)
        return;
    xp_log("Disconnected %s: %s", u->name, reason);
    end_call(u, "Peer disconnected");
    u->peer = NULL;
    u->control = u->media = NULL;
    u->hello = false;
    xp_peer_set_user(peer, NULL);
    presence(s);
}
static void stream_closed(xp_stream *stream, void *arg) {
    server *s = arg;
    user *u = xp_stream_user(stream);
    if (!u)
        return;
    if (u->media == stream) {
        u->media = NULL;
        end_call(u, "Media stream closed");
        presence(s);
    }
    if (u->control == stream)
        u->control = NULL;
}
static void tick(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    server *s = arg;
    uint64_t now = xp_now_ms();
    xp_transport_expire_pending(s->transport, 10000);
    for (size_t i = 0; i < s->count; ++i) {
        user *u = &s->users[i];
        if (u->peer && now - u->last_seen > 45000)
            xp_peer_close(u->peer, "Heartbeat timeout");
        if (u->call && !u->active && now >= u->deadline) {
            end_call(u, "No answer");
            presence(s);
        }
    }
}
static void stop(evutil_socket_t sig, short events, void *arg) {
    (void)sig;
    (void)events;
    event_base_loopbreak(arg);
}
static int load_users(server *s, const char *path) {
    json_t *db = xp_load_json(path, true);
    if (!db)
        return -1;
    json_t *users = json_object_get(db, "users");
    if (!json_is_object(users) || json_object_size(users) > XP_MAX_USERS) {
        json_decref(db);
        return -1;
    }
    s->users = calloc(XP_MAX_USERS, sizeof(user));
    const char *name;
    json_t *value;
    json_object_foreach(users, name, value) {
        const char *hash = json_string_value(value);
        if (!xp_name_valid(name) || !hash || strlen(hash) != 64 ||
            strspn(hash, "0123456789abcdef") != 64) {
            json_decref(db);
            return -1;
        }
        user *u = &s->users[s->count++];
        strcpy(u->name, name);
        strcpy(u->hash, hash);
    }
    json_decref(db);
    return 0;
}
static int add_user(const char *name, const char *path) {
    if (!xp_name_valid(name)) {
        xp_log("Username must be 1–31 ASCII letters, digits, underscores or hyphens");
        return 1;
    }
    char *lock_path;
    if (asprintf(&lock_path, "%s.lock", path) < 0)
        return 1;
    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(lock_path);
    if (lock < 0 || flock(lock, LOCK_EX)) {
        if (lock >= 0)
            close(lock);
        return 1;
    }
    json_t *db;
    if (access(path, F_OK) == 0)
        db = xp_load_json(path, true);
    else if (errno == ENOENT)
        db = json_pack("{s:{}}", "users");
    else
        db = NULL;
    json_t *users = db ? json_object_get(db, "users") : NULL;
    int rc = 1;
    if (!json_is_object(users) || json_object_get(users, name) ||
        json_object_size(users) >= XP_MAX_USERS) {
        xp_log("Invalid users file, duplicate username, or user limit reached");
        goto done;
    }
    char token[65], hash[65];
    if (xp_random_hex(token, 32))
        goto done;
    xp_sha256(token, hash);
    json_object_set_new(users, name, json_string(hash));
    if (xp_save_json(path, db)) {
        xp_log("Cannot save users file");
        goto done;
    }
    json_t *credential = json_pack("{s:s,s:s}", "username", name, "token", token);
    json_dumpf(credential, stdout, JSON_COMPACT);
    putchar('\n');
    json_decref(credential);
    OPENSSL_cleanse(token, sizeof(token));
    rc = 0;
done:
    json_decref(db);
    close(lock);
    return rc;
}
int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "add-user") && !strcmp(argv[3], "--users"))
        return add_user(argv[2], argv[4]);
    if (argc != 3 || strcmp(argv[1], "--config")) {
        fprintf(stderr,
                "Usage: xphoned --config FILE\n       xphoned add-user NAME --users FILE\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    json_t *config = xp_load_json(argv[2], false);
    if (!config)
        return 1;
    server s = {0};
    const char *users = xp_str(config, "users_file", NULL);
    if (!users || load_users(&s, users)) {
        xp_log("Cannot load users");
        free(s.users);
        json_decref(config);
        return 1;
    }
    s.base = event_base_new();
    xp_transport_callbacks callbacks = {
        .headers = headers, .record = record, .closed = closed, .stream_closed = stream_closed};
    s.transport = xp_transport_new(s.base, true, xp_str(config, "certificate", NULL),
                                   xp_str(config, "private_key", NULL), NULL, callbacks, &s);
    int port = xp_int(config, "port", 443, 1, 65535);
    char service[8];
    snprintf(service, sizeof(service), "%d", port);
    bool quic = !json_is_false(json_object_get(config, "http3"));
    int result = 1;
    if (port < 0 || !s.transport ||
        xp_transport_listen(s.transport, xp_str(config, "bind", "0.0.0.0"), service, quic)) {
        xp_log("Cannot listen (check certificates, address, port, and bind permissions)");
        goto done;
    }
    struct event *timer = event_new(s.base, -1, EV_PERSIST, tick, &s);
    struct timeval interval = {1, 0};
    event_add(timer, &interval);
    struct event *term = evsignal_new(s.base, SIGTERM, stop, s.base),
                 *interrupt = evsignal_new(s.base, SIGINT, stop, s.base);
    event_add(term, NULL);
    event_add(interrupt, NULL);
    xp_log("xphoned listening on %s:%d (HTTP/2%s), %zu invited users",
           xp_str(config, "bind", "0.0.0.0"), port, quic ? " and HTTP/3" : "", s.count);
    event_base_dispatch(s.base);
    event_free(timer);
    event_free(term);
    event_free(interrupt);
    result = 0;
done:
    xp_transport_free(s.transport);
    event_base_free(s.base);
    free(s.users);
    json_decref(config);
    return result;
}
