#ifndef XPHONE_COMMON_H
#define XPHONE_COMMON_H
#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define XP_NAME 32
#define XP_MAX_USERS 128
#define XP_CONTROL_MAX 65536
#define XP_MEDIA_MAX 1100
#define XP_FRAME 960
#define XP_RATE 48000
#define XP_QUEUE_MAX (256 * 1024)
uint64_t xp_now_ms(void);
uint64_t xp_boot_ms(void);
uint64_t xp_sleep_offset_ms(void);
uint64_t xp_now_ns(void);
void xp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool xp_name_valid(const char *name);
int xp_random_hex(char *out, size_t bytes);
void xp_sha256(const char *text, char out[65]);
json_t *xp_load_json(const char *path, bool secret);
int xp_save_json(const char *path, json_t *value);
const char *xp_str(json_t *obj, const char *key, const char *fallback);
int xp_int(json_t *obj, const char *key, int fallback, int lo, int hi);
int xp_parse_url(const char *url, char host[256], char port[8], char authority[280]);
#endif
