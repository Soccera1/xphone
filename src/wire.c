#include "wire.h"
#include <arpa/inet.h>
#include <string.h>

size_t xp_varint_write(uint8_t *out, uint64_t n) {
    size_t bytes = n < 64 ? 1 : n < 16384 ? 2 : n < (1ULL << 30) ? 4 : 8;
    if (n >= (1ULL << 62))
        return 0;
    for (size_t i = 0; i < bytes; ++i)
        out[bytes - i - 1] = (uint8_t)(n >> (8 * i));
    out[0] |= bytes == 8 ? 0xc0 : bytes == 4 ? 0x80 : bytes == 2 ? 0x40 : 0;
    return bytes;
}
int xp_varint_read(const uint8_t *in, size_t len, uint64_t *n) {
    if (!len)
        return 0;
    size_t bytes = 1U << (in[0] >> 6);
    if (len < bytes)
        return 0;
    *n = in[0] & 0x3f;
    for (size_t i = 1; i < bytes; ++i)
        *n = (*n << 8) | in[i];
    return (int)bytes;
}
size_t xp_control_pack(uint8_t *out, const uint8_t *data, size_t len) {
    if (!len || len > XP_CONTROL_MAX)
        return 0;
    uint32_t n = htonl((uint32_t)len);
    memcpy(out, &n, 4);
    memcpy(out + 4, data, len);
    return len + 4;
}
size_t xp_capsule_pack(uint8_t *out, const uint8_t *data, size_t len) {
    if (len > XP_MEDIA_MAX)
        return 0;
    out[0] = 0;
    size_t n = xp_varint_write(out + 1, len);
    memcpy(out + 1 + n, data, len);
    return 1 + n + len;
}
static int parse(xp_parser *p, xp_record_cb cb, void *arg, bool capsule) {
    while (p->used) {
        uint64_t length, type = 0;
        size_t header;
        if (capsule) {
            int a = xp_varint_read(p->data, p->used, &type);
            if (!a)
                return 0;
            int b = xp_varint_read(p->data + a, p->used - (size_t)a, &length);
            if (!b)
                return 0;
            header = (size_t)(a + b);
            if (length > XP_MEDIA_MAX)
                return -1;
        } else {
            if (p->used < 4)
                return 0;
            uint32_t n;
            memcpy(&n, p->data, 4);
            length = ntohl(n);
            header = 4;
            if (!length || length > XP_CONTROL_MAX)
                return -1;
        }
        if (p->used < header + length)
            return 0;
        if ((!capsule || type == 0) && cb(p->data + header, (size_t)length, arg))
            return -1;
        p->used -= header + (size_t)length;
        memmove(p->data, p->data + header + length, p->used);
    }
    return 0;
}
static int feed(xp_parser *p, const uint8_t *data, size_t len, xp_record_cb cb, void *arg,
                bool capsule) {
    while (len) {
        size_t space = sizeof(p->data) - p->used;
        if (!space)
            return -1;
        size_t n = len < space ? len : space;
        memcpy(p->data + p->used, data, n);
        p->used += n;
        data += n;
        len -= n;
        if (parse(p, cb, arg, capsule))
            return -1;
    }
    return 0;
}
int xp_control_feed(xp_parser *p, const uint8_t *d, size_t n, xp_record_cb cb, void *a) {
    return feed(p, d, n, cb, a, false);
}
int xp_capsule_feed(xp_parser *p, const uint8_t *d, size_t n, xp_record_cb cb, void *a) {
    return feed(p, d, n, cb, a, true);
}
static void put32(uint8_t *p, uint32_t n) {
    n = htonl(n);
    memcpy(p, &n, 4);
}
static uint32_t get32(const uint8_t *p) {
    uint32_t n;
    memcpy(&n, p, 4);
    return ntohl(n);
}
size_t xp_media_pack(uint8_t out[XP_MEDIA_MAX], const xp_media *m) {
    if (!m->opus_len || m->opus_len > XP_MEDIA_MAX - 24 || (m->channels != 1 && m->channels != 2))
        return 0;
    out[0] = 1;
    out[1] = m->channels;
    out[2] = out[3] = 0;
    put32(out + 4, (uint32_t)(m->call >> 32));
    put32(out + 8, (uint32_t)m->call);
    put32(out + 12, m->generation);
    put32(out + 16, m->sequence);
    put32(out + 20, m->timestamp);
    memcpy(out + 24, m->opus, m->opus_len);
    return 24 + m->opus_len;
}
int xp_media_unpack(xp_media *m, const uint8_t *d, size_t n) {
    if (n < 25 || n > XP_MEDIA_MAX || d[0] != 1 || (d[1] != 1 && d[1] != 2) || d[2] || d[3])
        return -1;
    *m = (xp_media){.call = ((uint64_t)get32(d + 4) << 32) | get32(d + 8),
                    .generation = get32(d + 12),
                    .sequence = get32(d + 16),
                    .timestamp = get32(d + 20),
                    .channels = d[1],
                    .opus = d + 24,
                    .opus_len = n - 24};
    return 0;
}
