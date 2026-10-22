#include "ui.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { BG, PANEL, LINE, TEXT, SUBTLE, ACCENT, DANGER, WHITE, COLOR_COUNT };
extern volatile sig_atomic_t xp_interrupted;
typedef enum { CALL, ACCEPT, REJECT, HANGUP, MUTE, VOL_DOWN, VOL_UP, RECONNECT, CONTACT } action;
typedef struct {
    int x, y, w, h;
    action action;
    int index;
    bool enabled;
} hit;
typedef struct {
    Display *display;
    int screen, width, height, xi_opcode;
    Window window;
    Atom delete_window;
    GC gc;
    Pixmap buffer;
    XftDraw *draw;
    XftFont *font, *large, *small;
    XftColor colors[COLOR_COUNT];
    double scale;
    bool xi, dirty, running, dragging, scrolled;
    int touch, press_x, press_y, scroll_start, scroll, selected, focus;
    hit hits[XP_MAX_USERS + 16];
    size_t hit_count;
    int list_x, list_y, list_w, list_h;
    char selected_name[XP_NAME];
    xp_session *session;
    xp_snapshot view;
} ui;
static int px(ui *u, int n) {
    return (int)lround(n * u->scale);
}
static void rect(ui *u, int color, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    XSetForeground(u->display, u->gc, u->colors[color].pixel);
    XFillRectangle(u->display, u->buffer, u->gc, px(u, x), px(u, y), (unsigned)px(u, w),
                   (unsigned)px(u, h));
}
static void text(ui *u, XftFont *font, int color, int x, int baseline, int max_width,
                 const char *value) {
    if (max_width <= 0)
        return;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", value);
    XGlyphInfo extents;
    size_t len = strlen(buf);
    XftTextExtentsUtf8(u->display, font, (const FcChar8 *)buf, (int)len, &extents);
    if (extents.xOff > px(u, max_width)) {
        while (len > 0) {
            --len;
            if (((unsigned char)buf[len] & 0xc0) == 0x80)
                continue;
            memcpy(buf + len, "...", 4);
            XftTextExtentsUtf8(u->display, font, (const FcChar8 *)buf, (int)len + 3, &extents);
            if (extents.xOff <= px(u, max_width))
                break;
        }
    }
    XftDrawStringUtf8(u->draw, &u->colors[color], font, px(u, x), px(u, baseline),
                      (const FcChar8 *)buf, (int)strlen(buf));
}
static void add_hit(ui *u, int x, int y, int w, int h, action a, int index, bool enabled) {
    if (u->hit_count < sizeof(u->hits) / sizeof(u->hits[0]))
        u->hits[u->hit_count++] = (hit){x, y, w, h, a, index, enabled};
}
static void button(ui *u, int x, int y, int w, int color, const char *label, action a,
                   bool enabled) {
    rect(u, enabled ? color : LINE, x, y, w, 52);
    if ((int)u->hit_count == u->focus) {
        rect(u, WHITE, x, y, w, 2);
        rect(u, WHITE, x, y + 50, w, 2);
        rect(u, WHITE, x, y, 2, 52);
        rect(u, WHITE, x + w - 2, y, 2, 52);
    }
    text(u, u->font, enabled && color == ACCENT ? BG : WHITE, x + 14, y + 32, w - 28, label);
    add_hit(u, x, y, w, 52, a, 0, enabled);
}
static bool call_view(ui *u) {
    return u->view.state == XP_ACTIVE || u->view.state == XP_INCOMING ||
           u->view.state == XP_OUTGOING;
}
static void contacts(ui *u, int x, int y, int w, int h) {
    text(u, u->font, SUBTLE, x, y + 20, w, "PEOPLE ONLINE");
    int bottom = y + h - 68;
    u->list_x = x;
    u->list_y = y + 38;
    u->list_w = w;
    u->list_h = bottom - u->list_y;
    if (u->list_h < 0)
        u->list_h = 0;
    int max_scroll = (int)u->view.user_count * 64 - u->list_h;
    if (max_scroll < 0)
        max_scroll = 0;
    if (u->scroll > max_scroll)
        u->scroll = max_scroll;
    if (u->scroll < 0)
        u->scroll = 0;
    if (!u->view.user_count)
        text(u, u->font, SUBTLE, x + 12, y + 80, w - 24,
             u->view.state == XP_ONLINE ? "No one else is online yet" : "Connect to see people");
    XRectangle clip = {(short)px(u, x), (short)px(u, u->list_y), (unsigned short)px(u, w),
                       (unsigned short)px(u, u->list_h)};
    XSetClipRectangles(u->display, u->gc, 0, 0, &clip, 1, Unsorted);
    XftDrawSetClipRectangles(u->draw, 0, 0, &clip, 1);
    for (size_t i = 0; i < u->view.user_count; ++i) {
        int row = u->list_y + (int)i * 64 - u->scroll;
        if (row + 56 <= u->list_y || row >= bottom)
            continue;
        rect(u, (int)i == u->selected ? LINE : PANEL, x, row, w, 56);
        if ((int)u->hit_count == u->focus) {
            rect(u, WHITE, x, row, 2, 56);
            rect(u, WHITE, x + w - 2, row, 2, 56);
        }
        rect(u, u->view.users[i].busy ? SUBTLE : ACCENT, x + 14, row + 24, 8, 8);
        text(u, u->font, TEXT, x + 36, row + 25, w - 52, u->view.users[i].name);
        text(u, u->small, SUBTLE, x + 36, row + 44, w - 52,
             u->view.users[i].busy ? "In a call" : "Available");
        int hit_top = row > u->list_y ? row : u->list_y;
        int hit_bottom = row + 56 < bottom ? row + 56 : bottom;
        if (hit_bottom - hit_top >= 48)
            add_hit(u, x, hit_top, w, hit_bottom - hit_top, CONTACT, (int)i, true);
    }
    XSetClipMask(u->display, u->gc, None);
    XftDrawSetClip(u->draw, NULL);
    if (max_scroll > 0 && u->view.user_count) {
        int track = u->list_h, thumb = track * track / ((int)u->view.user_count * 64);
        if (thumb < 24)
            thumb = 24;
        rect(u, SUBTLE, x + w - 4, u->list_y + u->scroll * (track - thumb) / max_scroll, 3, thumb);
    }
    bool can_call = u->view.state == XP_ONLINE && u->selected >= 0 &&
                    (size_t)u->selected < u->view.user_count && !u->view.users[u->selected].busy;
    button(u, x, y + h - 52, w, ACCENT, "Call selected person", CALL, can_call);
}
static void phone(ui *u, int x, int y, int w, int h) {
    bool active = u->view.state == XP_ACTIVE, incoming = u->view.state == XP_INCOMING;
    if (!call_view(u)) {
        text(u, u->large, TEXT, x + 12, y + h / 2 - 24, w - 24, "A familiar voice.");
        text(u, u->font, SUBTLE, x + 12, y + h / 2 + 12, w - 24, "Choose someone to call.");
        if (u->view.state == XP_OFFLINE)
            button(u, x + 12, y + h / 2 + 40, w - 24, ACCENT, "Reconnect", RECONNECT, true);
        return;
    }
    int top = y + (h > 350 ? 48 : 12);
    text(u, u->large, TEXT, x + 12, top + 28, w - 24, u->view.peer);
    char detail[80];
    if (active) {
        uint64_t seconds = (xp_now_ms() - u->view.started) / 1000;
        snprintf(detail, sizeof(detail), "%02llu:%02llu", (unsigned long long)(seconds / 60),
                 (unsigned long long)(seconds % 60));
    } else
        snprintf(detail, sizeof(detail), "%s",
                 incoming ? "Wants to talk to you" : "Waiting for an answer");
    if (h >= 240 || !active)
        text(u, u->font, SUBTLE, x + 12, top + 58, w - 24, detail);
    if (active && h >= 280) {
        snprintf(detail, sizeof(detail), "Sending %s  /  Hearing %s",
                 u->view.tx == 2 ? "stereo" : "mono", u->view.rx == 2 ? "stereo" : "mono");
        text(u, u->small, ACCENT, x + 12, top + 86, w - 24, detail);
    }
    int bottom = y + h - 52;
    if (active) {
        button(u, x, bottom - 68, w - 124, u->view.muted ? DANGER : LINE,
               u->view.muted ? "Unmute mic" : "Mute mic", MUTE, true);
        button(u, x + w - 112, bottom - 68, 50, LINE, "-", VOL_DOWN, true);
        button(u, x + w - 50, bottom - 68, 50, LINE, "+", VOL_UP, true);
        button(u, x, bottom, w, DANGER, "Hang up", HANGUP, true);
    } else if (incoming) {
        button(u, x, bottom, (w - 12) / 2, ACCENT, "Answer", ACCEPT, true);
        button(u, x + (w + 12) / 2, bottom, (w - 12) / 2, DANGER, "Decline", REJECT, true);
    } else
        button(u, x, bottom, w, DANGER, "Cancel call", HANGUP, true);
}
static void render(ui *u) {
    xp_session_snapshot(u->session, &u->view);
    if (u->selected_name[0]) {
        u->selected = 0;
        for (size_t i = 0; i < u->view.user_count; ++i)
            if (!strcmp(u->view.users[i].name, u->selected_name)) {
                u->selected = (int)i;
                break;
            }
    }
    if (u->view.user_count && u->selected >= 0 && (size_t)u->selected < u->view.user_count)
        snprintf(u->selected_name, sizeof(u->selected_name), "%s", u->view.users[u->selected].name);
    int w = (int)(u->width / u->scale), h = (int)(u->height / u->scale);
    bool compact = w < 600;
    u->hit_count = 0;
    u->list_h = 0;
    rect(u, BG, 0, 0, w + 1, h + 1);
    text(u, u->large, TEXT, 20, 36, w - 40, "XPhone");
    char subtitle[100];
    snprintf(subtitle, sizeof(subtitle), "%s  %s", u->view.username, u->view.transport);
    text(u, u->small, SUBTLE, 20, 61, w - 40, subtitle);
    rect(u, PANEL, 16, 76, w - 32, 40);
    rect(u, u->view.state >= XP_ONLINE ? ACCENT : SUBTLE, 28, 92, 8, 8);
    text(u, u->small, TEXT, 48, 101, w - 76, u->view.status);
    int y = 132, content_h = h - y - 16;
    if (compact) {
        if (call_view(u))
            phone(u, 16, y, w - 32, content_h);
        else
            contacts(u, 16, y, w - 32, content_h);
        if (u->view.state == XP_OFFLINE) {
            /* Reconnect replaces the otherwise disabled call button on phones. */
            u->hit_count = u->hit_count ? u->hit_count - 1 : 0;
            button(u, 16, h - 68, w - 32, ACCENT, "Reconnect", RECONNECT, true);
        }
    } else {
        int left = w / 3;
        if (left < 240)
            left = 240;
        contacts(u, 20, y, left - 40, content_h);
        rect(u, LINE, left, y, 1, content_h);
        phone(u, left + 24, y, w - left - 48, content_h);
    }
    XCopyArea(u->display, u->buffer, u->window, u->gc, 0, 0, (unsigned)u->width,
              (unsigned)u->height, 0, 0);
    XFlush(u->display);
    u->dirty = false;
}
static void invoke(ui *u, hit *h) {
    if (!h || !h->enabled)
        return;
    switch (h->action) {
    case CONTACT:
        u->selected = h->index;
        snprintf(u->selected_name, sizeof(u->selected_name), "%s", u->view.users[h->index].name);
        break;
    case CALL:
        if (u->selected >= 0 && (size_t)u->selected < u->view.user_count)
            xp_session_command(u->session, "call", u->view.users[u->selected].name, 0);
        break;
    case ACCEPT:
        xp_session_command(u->session, "accept", NULL, 0);
        break;
    case REJECT:
        xp_session_command(u->session, "reject", NULL, 0);
        break;
    case HANGUP:
        xp_session_command(u->session, "hangup", NULL, 0);
        break;
    case MUTE:
        xp_session_command(u->session, "mute", NULL, 0);
        break;
    case VOL_DOWN:
        xp_session_command(u->session, "volume", NULL,
                           u->view.volume >= 10 ? u->view.volume - 10 : 0);
        break;
    case VOL_UP:
        xp_session_command(u->session, "volume", NULL,
                           u->view.volume <= 90 ? u->view.volume + 10 : 100);
        break;
    case RECONNECT:
        xp_session_command(u->session, "reconnect", NULL, 0);
        break;
    }
    u->dirty = true;
}
static bool inside(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && y >= by && x < bx + bw && y < by + bh;
}
static void handle_pointer(ui *u, int kind, double physical_x, double physical_y) {
    int x = (int)(physical_x / u->scale), y = (int)(physical_y / u->scale);
    if (kind == 0) {
        u->dragging = true;
        u->scrolled = false;
        u->press_x = x;
        u->press_y = y;
        u->scroll_start = u->scroll;
    } else if (kind == 1 && u->dragging) {
        if (abs(y - u->press_y) > 8 || abs(x - u->press_x) > 8)
            u->scrolled = true;
        if (u->scrolled &&
            inside(u->press_x, u->press_y, u->list_x, u->list_y, u->list_w, u->list_h)) {
            u->scroll = u->scroll_start + u->press_y - y;
            u->dirty = true;
        }
    } else if (kind == 2 && u->dragging) {
        u->dragging = false;
        if (!u->scrolled)
            for (size_t i = 0; i < u->hit_count; ++i) {
                hit *h = &u->hits[i];
                if (inside(x, y, h->x, h->y, h->w, h->h) &&
                    inside(u->press_x, u->press_y, h->x, h->y, h->w, h->h)) {
                    u->focus = (int)i;
                    invoke(u, h);
                    break;
                }
            }
    }
}
static void key(ui *u, XKeyEvent *event) {
    KeySym sym = XLookupKeysym(event, 0);
    if (sym == XK_Tab) {
        int step = (event->state & ShiftMask) ? -1 : 1;
        if (u->hit_count)
            u->focus = (u->focus + step + (int)u->hit_count) % (int)u->hit_count;
    } else if (sym == XK_Return || sym == XK_KP_Enter || sym == XK_space) {
        if (u->focus >= 0 && (size_t)u->focus < u->hit_count)
            invoke(u, &u->hits[u->focus]);
        else
            for (size_t i = 0; i < u->hit_count; ++i)
                if (u->hits[i].action == CALL) {
                    invoke(u, &u->hits[i]);
                    break;
                }
    } else if (sym == XK_Up || sym == XK_Down) {
        if (u->view.user_count) {
            u->selected += sym == XK_Up ? -1 : 1;
            if (u->selected < 0)
                u->selected = 0;
            if ((size_t)u->selected >= u->view.user_count)
                u->selected = (int)u->view.user_count - 1;
            snprintf(u->selected_name, sizeof(u->selected_name), "%s",
                     u->view.users[u->selected].name);
            if (u->selected * 64 < u->scroll)
                u->scroll = u->selected * 64;
            if ((u->selected + 1) * 64 > u->scroll + u->list_h)
                u->scroll = (u->selected + 1) * 64 - u->list_h;
        }
    } else if ((sym == XK_m || sym == XK_M) && u->view.state == XP_ACTIVE)
        xp_session_command(u->session, "mute", NULL, 0);
    else if (sym == XK_Escape && (u->view.state == XP_INCOMING || u->view.state == XP_OUTGOING))
        xp_session_command(u->session, u->view.state == XP_INCOMING ? "reject" : "cancel", NULL, 0);
    u->dirty = true;
}
static double get_scale(Display *display, json_t *config) {
    json_t *override = json_object_get(config, "ui_scale");
    double scale = json_is_number(override) ? json_number_value(override) : 0;
    if (scale <= 0) {
        char *resources = XResourceManagerString(display);
        if (resources) {
            XrmDatabase db = XrmGetStringDatabase(resources);
            XrmValue value;
            char *type;
            if (db && XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value))
                scale = strtod(value.addr, NULL) / 96.0;
            if (db)
                XrmDestroyDatabase(db);
        }
    }
    return isfinite(scale) && scale >= 0.5 && scale <= 4 ? scale : 1;
}
static XftFont *open_font(ui *u, int size, bool bold) {
    char pattern[100];
    snprintf(pattern, sizeof(pattern), "sans:pixelsize=%d:weight=%s", px(u, size),
             bold ? "bold" : "regular");
    return XftFontOpenName(u->display, u->screen, pattern);
}
static void resize(ui *u, int width, int height) {
    u->width = width;
    u->height = height;
    if (u->draw)
        XftDrawDestroy(u->draw);
    if (u->buffer)
        XFreePixmap(u->display, u->buffer);
    u->buffer = XCreatePixmap(u->display, u->window, (unsigned)width, (unsigned)height,
                              (unsigned)DefaultDepth(u->display, u->screen));
    u->draw = XftDrawCreate(u->display, u->buffer, DefaultVisual(u->display, u->screen),
                            DefaultColormap(u->display, u->screen));
    u->dirty = true;
}
int xp_ui_run(xp_session *session, json_t *config, int width, int height, int quit_after) {
    ui u = {.session = session,
            .running = true,
            .dirty = true,
            .touch = -1,
            .selected = 0,
            .focus = -1};
    u.display = XOpenDisplay(NULL);
    if (!u.display) {
        xp_log("Cannot open X11 display; use --headless for terminal operation");
        return 1;
    }
    u.screen = DefaultScreen(u.display);
    u.scale = get_scale(u.display, config);
    if (!width)
        width = px(&u, 900) < DisplayWidth(u.display, u.screen) - 32
                    ? px(&u, 900)
                    : DisplayWidth(u.display, u.screen) - 32;
    if (!height)
        height = px(&u, 640) < DisplayHeight(u.display, u.screen) - 48
                     ? px(&u, 640)
                     : DisplayHeight(u.display, u.screen) - 48;
    u.window = XCreateSimpleWindow(u.display, RootWindow(u.display, u.screen), 0, 0,
                                   (unsigned)width, (unsigned)height, 0, 0, 0);
    XStoreName(u.display, u.window, "XPhone");
    XClassHint class_hint = {.res_name = "xphone", .res_class = "XPhone"};
    XSetClassHint(u.display, u.window, &class_hint);
    XSizeHints hints = {.flags = PMinSize, .min_width = px(&u, 320), .min_height = px(&u, 320)};
    XSetWMNormalHints(u.display, u.window, &hints);
    u.delete_window = XInternAtom(u.display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(u.display, u.window, &u.delete_window, 1);
    u.gc = XCreateGC(u.display, u.window, 0, NULL);
    const char *palette[] = {"#101b24", "#172631", "#2b4050", "#f0f5f7",
                             "#a2b5c2", "#6fe0bd", "#bb4054", "#ffffff"};
    for (int i = 0; i < COLOR_COUNT; ++i)
        XftColorAllocName(u.display, DefaultVisual(u.display, u.screen),
                          DefaultColormap(u.display, u.screen), palette[i], &u.colors[i]);
    u.font = open_font(&u, 16, false);
    u.large = open_font(&u, 25, true);
    u.small = open_font(&u, 13, false);
    if (!u.font || !u.large || !u.small) {
        xp_log("Cannot load UI fonts");
        XCloseDisplay(u.display);
        return 1;
    }
    int event, error, major = 2, minor = 2;
    u.xi = XQueryExtension(u.display, "XInputExtension", &u.xi_opcode, &event, &error) &&
           XIQueryVersion(u.display, &major, &minor) == Success;
    long mask = ExposureMask | StructureNotifyMask | KeyPressMask;
    if (u.xi) {
        unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
        XISetMask(bits, XI_TouchBegin);
        XISetMask(bits, XI_TouchUpdate);
        XISetMask(bits, XI_TouchEnd);
        XISetMask(bits, XI_ButtonPress);
        XISetMask(bits, XI_ButtonRelease);
        XISetMask(bits, XI_Motion);
        XIEventMask xi_mask = {
            .deviceid = XIAllMasterDevices, .mask_len = sizeof(bits), .mask = bits};
        XISelectEvents(u.display, u.window, &xi_mask, 1);
    } else
        mask |= ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    XSelectInput(u.display, u.window, mask);
    resize(&u, width, height);
    XMapWindow(u.display, u.window);
    uint64_t start = xp_now_ms(), last_second = 0;
    while (u.running && !xp_interrupted &&
           (!quit_after || xp_now_ms() - start < (uint64_t)quit_after * 1000)) {
        while (XPending(u.display)) {
            XEvent ev;
            XNextEvent(u.display, &ev);
            if (ev.type == Expose)
                u.dirty = true;
            else if (ev.type == ConfigureNotify &&
                     (ev.xconfigure.width != u.width || ev.xconfigure.height != u.height))
                resize(&u, ev.xconfigure.width, ev.xconfigure.height);
            else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == u.delete_window)
                u.running = false;
            else if (ev.type == KeyPress)
                key(&u, &ev.xkey);
            else if (ev.type == GenericEvent && ev.xcookie.extension == u.xi_opcode &&
                     XGetEventData(u.display, &ev.xcookie)) {
                XIDeviceEvent *d = ev.xcookie.data;
                int type = ev.xcookie.evtype;
                if (type == XI_TouchBegin && u.touch < 0) {
                    u.touch = d->detail;
                    handle_pointer(&u, 0, d->event_x, d->event_y);
                } else if (type == XI_TouchUpdate && d->detail == u.touch)
                    handle_pointer(&u, 1, d->event_x, d->event_y);
                else if (type == XI_TouchEnd && d->detail == u.touch) {
                    handle_pointer(&u, 2, d->event_x, d->event_y);
                    u.touch = -1;
                } else if (!(d->flags & XIPointerEmulated) && u.touch < 0) {
                    if (type == XI_ButtonPress && (d->detail == 4 || d->detail == 5)) {
                        u.scroll += d->detail == 4 ? -64 : 64;
                        u.dirty = true;
                    } else if (type == XI_ButtonPress && d->detail == 1)
                        handle_pointer(&u, 0, d->event_x, d->event_y);
                    else if (type == XI_ButtonRelease && d->detail == 1)
                        handle_pointer(&u, 2, d->event_x, d->event_y);
                    else if (type == XI_Motion)
                        handle_pointer(&u, 1, d->event_x, d->event_y);
                }
                XFreeEventData(u.display, &ev.xcookie);
            } else if (!u.xi) {
                if (ev.type == ButtonPress && (ev.xbutton.button == 4 || ev.xbutton.button == 5)) {
                    u.scroll += ev.xbutton.button == 4 ? -64 : 64;
                    u.dirty = true;
                } else if (ev.type == ButtonPress && ev.xbutton.button == 1)
                    handle_pointer(&u, 0, ev.xbutton.x, ev.xbutton.y);
                else if (ev.type == ButtonRelease && ev.xbutton.button == 1)
                    handle_pointer(&u, 2, ev.xbutton.x, ev.xbutton.y);
                else if (ev.type == MotionNotify)
                    handle_pointer(&u, 1, ev.xmotion.x, ev.xmotion.y);
            }
        }
        json_t *obj;
        while ((obj = xp_session_event(session))) {
            json_decref(obj);
            u.dirty = true;
        }
        xp_session_snapshot(session, &u.view);
        uint64_t now = xp_now_ms();
        if (u.view.state == XP_ACTIVE && now / 1000 != last_second) {
            last_second = now / 1000;
            u.dirty = true;
        }
        if (u.dirty)
            render(&u);
        struct pollfd fds[] = {{ConnectionNumber(u.display), POLLIN, 0},
                               {xp_session_fd(session), POLLIN, 0}};
        int timeout = (call_view(&u) || quit_after) ? 250 : -1;
        poll(fds, 2, timeout);
        if (fds[1].revents & POLLIN) {
            uint64_t count;
            while (read(fds[1].fd, &count, sizeof(count)) > 0) {
            }
        }
    }
    XftDrawDestroy(u.draw);
    XFreePixmap(u.display, u.buffer);
    XFreeGC(u.display, u.gc);
    XftFontClose(u.display, u.font);
    XftFontClose(u.display, u.large);
    XftFontClose(u.display, u.small);
    for (int i = 0; i < COLOR_COUNT; ++i)
        XftColorFree(u.display, DefaultVisual(u.display, u.screen),
                     DefaultColormap(u.display, u.screen), &u.colors[i]);
    XDestroyWindow(u.display, u.window);
    XCloseDisplay(u.display);
    FcFini();
    return 0;
}
