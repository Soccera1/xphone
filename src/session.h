#ifndef XPHONE_SESSION_H
#define XPHONE_SESSION_H
#include "common.h"
typedef struct xp_session xp_session;
typedef enum { XP_OFFLINE, XP_CONNECTING, XP_ONLINE, XP_OUTGOING, XP_INCOMING, XP_ACTIVE } xp_state;
typedef struct {
    xp_state state;
    char username[XP_NAME], peer[XP_NAME], status[160], transport[16];
    struct {
        char name[XP_NAME];
        bool busy;
    } users[XP_MAX_USERS];
    size_t user_count;
    uint64_t call, started;
    uint32_t generation;
    int tx, rx, volume;
    bool muted;
} xp_snapshot;
xp_session *xp_session_new(json_t *config, bool synthetic);
void xp_session_free(xp_session *session);
int xp_session_fd(xp_session *session);
void xp_session_snapshot(xp_session *session, xp_snapshot *snapshot);
json_t *xp_session_event(xp_session *session);
void xp_session_command(xp_session *session, const char *action, const char *target, int value);
#endif
