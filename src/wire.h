#ifndef XPHONE_WIRE_H
#define XPHONE_WIRE_H
#include "common.h"
typedef struct {
    uint8_t data[XP_CONTROL_MAX + 16];
    size_t used;
} xp_parser;
typedef int (*xp_record_cb)(const uint8_t *, size_t, void *);
int xp_control_feed(xp_parser *p, const uint8_t *data, size_t len, xp_record_cb cb, void *arg);
int xp_capsule_feed(xp_parser *p, const uint8_t *data, size_t len, xp_record_cb cb, void *arg);
size_t xp_varint_write(uint8_t *out, uint64_t n);
int xp_varint_read(const uint8_t *in, size_t len, uint64_t *n);
size_t xp_control_pack(uint8_t *out, const uint8_t *data, size_t len);
size_t xp_capsule_pack(uint8_t *out, const uint8_t *data, size_t len);
typedef struct {
    uint64_t call;
    uint32_t generation, sequence, timestamp;
    uint8_t channels;
    const uint8_t *opus;
    size_t opus_len;
} xp_media;
size_t xp_media_pack(uint8_t out[XP_MEDIA_MAX], const xp_media *m);
int xp_media_unpack(xp_media *m, const uint8_t *data, size_t len);
#endif
