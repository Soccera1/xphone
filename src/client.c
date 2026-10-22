#include "common.h"
#include "session.h"
#include "ui.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t xp_interrupted;
static volatile sig_atomic_t signal_fd = -1;
static void stop(int sig) {
    (void)sig;
    xp_interrupted = 1;
    if (signal_fd >= 0) {
        uint64_t one = 1;
        ssize_t written = write((int)signal_fd, &one, sizeof(one));
        (void)written;
    }
}
static int headless(xp_session *s, const char *target, bool answer, int duration, int quit_after) {
    uint64_t begin = xp_now_ms(), started = 0;
    bool called = false, hungup = false, stdin_open = true;
    char input[4096];
    size_t used = 0;
    while (!xp_interrupted && (!quit_after || xp_now_ms() - begin < (uint64_t)quit_after * 1000)) {
        struct pollfd fds[] = {{xp_session_fd(s), POLLIN, 0},
                               {stdin_open ? STDIN_FILENO : -1, POLLIN, 0}};
        poll(fds, 2, 100);
        if (fds[0].revents & POLLIN) {
            uint64_t count;
            while (read(fds[0].fd, &count, sizeof(count)) > 0) {
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(STDIN_FILENO, input + used, sizeof(input) - used - 1);
            if (n <= 0)
                stdin_open = false;
            else {
                used += (size_t)n;
                input[used] = 0;
                char *newline;
                while ((newline = memchr(input, '\n', used))) {
                    size_t len = (size_t)(newline - input);
                    json_error_t error;
                    json_t *command = json_loadb(input, len, JSON_REJECT_DUPLICATES, &error);
                    if (json_is_object(command)) {
                        if (!strcmp(xp_str(command, "action", ""), "quit"))
                            xp_interrupted = 1;
                        else
                            xp_session_command(s, xp_str(command, "action", ""),
                                               xp_str(command, "target", ""),
                                               xp_int(command, "value", 0, 0, 100));
                    }
                    json_decref(command);
                    used -= len + 1;
                    memmove(input, newline + 1, used);
                }
                if (used == sizeof(input) - 1)
                    used = 0;
            }
        }
        json_t *event;
        while ((event = xp_session_event(s))) {
            json_dumpf(event, stdout, JSON_COMPACT);
            putchar('\n');
            fflush(stdout);
            const char *type = xp_str(event, "type", "");
            if (answer && !strcmp(type, "incoming"))
                xp_session_command(s, "accept", NULL, 0);
            if (!strcmp(type, "active") && !started)
                started = xp_now_ms();
            if (!strcmp(type, "ended") && hungup) {
                json_decref(event);
                return 0;
            }
            json_decref(event);
        }
        xp_snapshot v;
        xp_session_snapshot(s, &v);
        if (target && !called && v.state == XP_ONLINE) {
            for (size_t i = 0; i < v.user_count; ++i) {
                if (!strcmp(v.users[i].name, target) && !v.users[i].busy) {
                    called = true;
                    xp_session_command(s, "call", target, 0);
                    break;
                }
            }
        }
        if (duration && started && !hungup && xp_now_ms() - started >= (uint64_t)duration * 1000) {
            hungup = true;
            xp_session_command(s, "hangup", NULL, 0);
        }
    }
    return 0;
}
int main(int argc, char **argv) {
    const char *path = NULL, *target = NULL;
    char default_path[4096];
    bool no_ui = false, synthetic = false, answer = false;
    int duration = 0, quit_after = 0, width = 0, height = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            path = argv[++i];
        else if (!strcmp(argv[i], "--headless"))
            no_ui = true;
        else if (!strcmp(argv[i], "--synthetic"))
            synthetic = true;
        else if (!strcmp(argv[i], "--auto-answer"))
            answer = true;
        else if (!strcmp(argv[i], "--call") && i + 1 < argc)
            target = argv[++i];
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
            duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quit-after") && i + 1 < argc)
            quit_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--geometry") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &width, &height) != 2 || width < 200 || height < 200 ||
                width > 8192 || height > 8192)
                return 1;
        } else {
            fprintf(stderr, "Usage: xphone --config FILE [--headless] [--synthetic]\n"
                            "              [--call USER] [--auto-answer] [--duration SECONDS]\n"
                            "              [--geometry WIDTHxHEIGHT] [--quit-after SECONDS]\n");
            return 1;
        }
    }
    if (!path) {
        const char *config_dir = getenv("XDG_CONFIG_HOME"), *user_home = getenv("HOME");
        int n;
        if (config_dir && *config_dir)
            n = snprintf(default_path, sizeof(default_path), "%s/xphone/client.json", config_dir);
        else if (user_home && *user_home)
            n = snprintf(default_path, sizeof(default_path), "%s/.config/xphone/client.json",
                         user_home);
        else
            return 1;
        if (n < 0 || (size_t)n >= sizeof(default_path))
            return 1;
        path = default_path;
    }
    if (duration < 0 || quit_after < 0 || (target && !xp_name_valid(target)))
        return 1;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    json_t *config = xp_load_json(path, true);
    if (!config)
        return 1;
    xp_session *session = xp_session_new(config, synthetic);
    if (!session) {
        json_decref(config);
        return 1;
    }
    signal_fd = xp_session_fd(session);
    int result = no_ui ? headless(session, target, answer, duration, quit_after)
                       : xp_ui_run(session, config, width, height, quit_after);
    signal_fd = -1;
    xp_session_free(session);
    json_decref(config);
    return result;
}
