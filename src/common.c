#include "common.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t xp_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
uint64_t xp_now_ms(void) {
    return xp_now_ns() / 1000000;
}
uint64_t xp_boot_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
uint64_t xp_sleep_offset_ms(void) {
    uint64_t boot = xp_boot_ms(), monotonic = xp_now_ms();
    return boot > monotonic ? boot - monotonic : 0;
}
void xp_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
bool xp_name_valid(const char *name) {
    if (!name || !*name || strlen(name) >= XP_NAME)
        return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-')
            return false;
    return true;
}
int xp_random_hex(char *out, size_t bytes) {
    unsigned char data[64];
    if (bytes > sizeof(data) || RAND_bytes(data, (int)bytes) != 1)
        return -1;
    for (size_t i = 0; i < bytes; ++i)
        snprintf(out + i * 2, 3, "%02x", data[i]);
    return 0;
}
void xp_sha256(const char *text, char out[65]) {
    unsigned char hash[32];
    unsigned len = 0;
    if (!EVP_Digest(text, strlen(text), hash, &len, EVP_sha256(), NULL) || len != 32)
        abort();
    for (size_t i = 0; i < sizeof(hash); ++i)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
}
json_t *xp_load_json(const char *path, bool secret) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 1024 * 1024 ||
        (secret && ((st.st_mode & 0077) || st.st_uid != geteuid()))) {
        xp_log("Cannot read %s (credential files must be owned by this user and mode 0600)", path);
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    json_error_t error;
    json_t *obj = json_loadfd(fd, JSON_REJECT_DUPLICATES, &error);
    close(fd);
    if (!json_is_object(obj)) {
        xp_log("Invalid JSON object in %s", path);
        json_decref(obj);
        return NULL;
    }
    return obj;
}
int xp_save_json(const char *path, json_t *value) {
    char *temp;
    if (asprintf(&temp, "%s.XXXXXX", path) < 0)
        return -1;
    int fd = mkstemp(temp);
    int result = -1;
    if (fd >= 0) {
        if (!fchmod(fd, 0600) && !json_dumpfd(value, fd, JSON_INDENT(2) | JSON_SORT_KEYS) &&
            !fsync(fd) && !rename(temp, path))
            result = 0;
        close(fd);
    }
    if (result)
        unlink(temp);
    free(temp);
    return result;
}
const char *xp_str(json_t *obj, const char *key, const char *fallback) {
    const char *s = json_string_value(json_object_get(obj, key));
    return s ? s : fallback;
}
int xp_int(json_t *obj, const char *key, int fallback, int lo, int hi) {
    json_t *v = json_object_get(obj, key);
    if (!v)
        return fallback;
    if (!json_is_integer(v) || json_integer_value(v) < lo || json_integer_value(v) > hi)
        return -1;
    return (int)json_integer_value(v);
}
int xp_parse_url(const char *url, char host[256], char port[8], char authority[280]) {
    if (strncmp(url, "https://", 8))
        return -1;
    const char *p = url + 8, *end = p + strcspn(p, "/?#");
    if (!*p || (size_t)(end - p) >= 280 || (*end && strcmp(end, "/")))
        return -1;
    memcpy(authority, p, (size_t)(end - p));
    authority[end - p] = 0;
    strcpy(port, "443");
    const char *h = p, *he;
    if (*p == '[') {
        h = p + 1;
        he = memchr(h, ']', (size_t)(end - h));
        if (!he)
            return -1;
        p = he + 1;
    } else {
        he = memchr(p, ':', (size_t)(end - p));
        if (!he)
            he = end;
        p = he;
    }
    if (he == h || he - h >= 256)
        return -1;
    memcpy(host, h, (size_t)(he - h));
    host[he - h] = 0;
    for (const char *s = h; s < he; ++s)
        if (!(isalnum((unsigned char)*s) || strchr(".-:_", *s)))
            return -1;
    if (p < end) {
        if (*p++ != ':' || p == end || end - p >= 8)
            return -1;
        for (const char *s = p; s < end; ++s)
            if (!isdigit((unsigned char)*s))
                return -1;
        memcpy(port, p, (size_t)(end - p));
        port[end - p] = 0;
        long n = strtol(port, NULL, 10);
        if (n < 1 || n > 65535)
            return -1;
    }
    return 0;
}
