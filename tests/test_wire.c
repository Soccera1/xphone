#include "wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    const uint8_t *expected;
    size_t len, calls;
} check;
static int received(const uint8_t *data, size_t len, void *arg) {
    check *c = arg;
    assert(len == c->len && !memcmp(data, c->expected, len));
    ++c->calls;
    return 0;
}
int main(void) {
    uint64_t values[] = {0, 63, 64, 16383, 16384, (1ULL << 30) - 1, 1ULL << 30, (1ULL << 62) - 1};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint8_t buf[8];
        uint64_t n;
        size_t bytes = xp_varint_write(buf, values[i]);
        assert(xp_varint_read(buf, bytes, &n) == (int)bytes && n == values[i]);
        for (size_t j = 0; j < bytes; ++j)
            assert(xp_varint_read(buf, j, &n) == 0);
    }
    uint8_t encoded[XP_CONTROL_MAX + 16], payload[XP_CONTROL_MAX];
    memset(payload, 'x', sizeof(payload));
    for (int capsule = 0; capsule < 2; ++capsule) {
        size_t sizes[] = {1, 63, 64, 1024, capsule ? XP_MEDIA_MAX : XP_CONTROL_MAX};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            size_t n = capsule ? xp_capsule_pack(encoded, payload, sizes[i])
                               : xp_control_pack(encoded, payload, sizes[i]);
            xp_parser *parser = calloc(1, sizeof(*parser));
            check c = {payload, sizes[i], 0};
            for (size_t j = 0; j < n; ++j)
                assert(!(capsule ? xp_capsule_feed(parser, encoded + j, 1, received, &c)
                                 : xp_control_feed(parser, encoded + j, 1, received, &c)));
            assert(c.calls == 1 && !parser->used);
            free(parser);
        }
    }
    xp_parser parser = {0};
    check c = {payload, 1, 0};
    const uint8_t oversized[] = {0, 1, 0, 1};
    assert(xp_control_feed(&parser, oversized, sizeof(oversized), received, &c) == -1);
    memset(&parser, 0, sizeof(parser));
    const uint8_t unknown[] = {0x17, 1, 42};
    assert(!xp_capsule_feed(&parser, unknown, sizeof(unknown), received, &c) && !c.calls);
    uint8_t media[XP_MEDIA_MAX];
    xp_media frame = {.call = 0x102030405060708ULL,
                      .generation = 9,
                      .sequence = UINT32_MAX,
                      .timestamp = 12345,
                      .channels = 2,
                      .opus = payload,
                      .opus_len = 100},
             decoded;
    size_t len = xp_media_pack(media, &frame);
    assert(len == 124 && !xp_media_unpack(&decoded, media, len));
    assert(decoded.call == frame.call && decoded.sequence == UINT32_MAX &&
           decoded.generation == 9 && decoded.channels == 2 && decoded.timestamp == 12345);
    media[1] = 3;
    assert(xp_media_unpack(&decoded, media, len) == -1);
    puts("wire: fragmented records, bounds, varints, and stereo metadata passed");
    return 0;
}
