/*
 * winvm-viewer — X11 display client for QEMU guests
 *
 * Speaks the SBD protocol with the patched QEMU:
 *   - receive "SBD1" + 4 fds (fb memfd, shared memfd, eventfd, dmabuf)
 *   - wait on eventfd for frame updates, read mode from the shared block
 *   - map the dmabuf and present it through X11
 *   - forward mouse/keyboard via QEMU QMP input-send-event
 *   - clipboard sync (X11 CLIPBOARD <-> guest) over the guest agent
 *   - resolution follow: window resize -> guest mode; guest mode -> window size
 *
 * Usage:
 *   winvm-viewer --dbus-display unix:path=/run/winvm-dbus/bus \
 *                [--width 1280] [--height 800] [--window-title "..."] \
 *                [--agent 127.0.0.1:8790]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>


static int qmp_fd = -1;
static const char *qmp_path = "/tmp/qemudisplay.sock";

static Display *dpy;
static Window win;
static GC gc;
static XImage *img;
static int win_w = 1280, win_h = 800;
static int dirty = 1;
static int have_frame = 0;
/* ---- presentation ---------------------------------------------------
 * Frames used to leave through XPutImage on every update, which pushes the
 * whole window over the X11 socket each time.  MIT-SHM shares the pixels with
 * the server instead, and damage tracking lets us push only what QEMU actually
 * changed -- on a 2560x1600 guest a full blit is ~16 MB per update.        */
static int shm_ok = 0;
/* set when the server reports BadShmSeg: the segment is gone for good, so stop
 * using XShmPutImage and present with XPutImage instead */
static volatile int shm_broken = 0;
static XShmSegmentInfo shm_info;
/* damage rect, in window coordinates; empty when x1 <= x0 */
static int dmg_x0, dmg_y0, dmg_x1, dmg_y1;
static int vnc_enabled = 0;
static int vnc_fd = -1;
static int dbus_display_enabled = 0;
static const char *dbus_display_addr = NULL;

/* ---- boot splash ---------------------------------------------------- */
/* Shown until the guest produces its first frame (and again if it ever
 * drops), so the window is never a blank or stale rectangle while
 * Windows boots.  Format: "SPL1" + u32 w + u32 h + RGBA rows.          */
typedef struct { uint32_t *px; int w, h; } spl_img;

/* status line shown under the logo, one per power state */
enum { SP_BOOT = 0, SP_SHUTDOWN, SP_SUSPEND, SP_OFF, SP_COUNT };
static spl_img spl_logo = { NULL, 0, 0 };
static spl_img spl_text[SP_COUNT];
static spl_img dlg_ask = { NULL, 0, 0 };      /* "close = power off?"        */
static spl_img dlg_force = { NULL, 0, 0 };    /* "force power off?"          */
static spl_img dlg_btn_yes = { NULL, 0, 0 };
static spl_img dlg_btn_no = { NULL, 0, 0 };
static spl_img dlg_btn_force = { NULL, 0, 0 };
static const char *splash_path = "/opt/mtvmm/share/winvm-splash.rgba";
static const char *splash_dir = "/opt/mtvmm/share/winvm";
static int splash_phase = 0;
static long splash_next_ms = 0;
static long splash_start_ms = 0;
#define SPLASH_MIN_MS 12000
#define SPLASH_GIVEUP_MS 90000
static void splash_load(const char *path);
static void render_splash(void);
static void hide_local_cursor(void);
static void show_local_cursor(void);
static long now_ms(void);
static void shm_try_enable(void);
static void shm_detach(void);
static void damage_all(void);
static void damage_add(int x, int y, int w, int h);
static int damage_empty(void);
static void present_damage(void);

/* ---- guest power state ---------------------------------------------- */
/* V_DISPLAY_OFF: the guest is still running but its console is disabled --
 * a blanked screen, a driver reset, or the instant between the OS tearing
 * the scanout down and QEMU reporting the shutdown.  It is NOT "booting",
 * which is what a plain V_RUNNING fallback used to claim.               */
enum { V_BOOTING = 0, V_RUNNING, V_SHUTTING, V_SUSPENDED, V_OFF, V_DISPLAY_OFF };
static int vstate = V_BOOTING;
static int qmp_dead = 0;
/* last runstate QMP reported (query-status); used to tell a real power-off
 * from a display that merely went away while the guest keeps running */
static char qmp_runstate[32] = "";
static long qmp_status_next = 0;

/* ---- modal dialog ---------------------------------------------------- */
enum { DLG_NONE = 0, DLG_ASK_SHUTDOWN, DLG_ASK_FORCE };
static int dlg = DLG_NONE;
static int dlg_hover = -1;
/* the dialog image is static except for hover: redrawing it on every guest
 * frame costs a 16 MB memcpy plus a per-byte dim and starves the event loop */
static int dlg_dirty = 1;
static long shut_deadline = 0;   /* next escalation step while shutting down */
static int shut_stage = 0;       /* 0 = agent, 1 = ACPI, 2 = force prompt    */
static void dialog_draw(void);
static void qmp_handle_event(const char *buf);
static void dialog_click(int x, int y, int button);

static int qmp_id = 1;

/* ---- guest agent ---- */
static char agent_host[64] = "127.0.0.1";
static int audio_tcp_wanted = 1;
static int audio_tcp_port = 8791;
static int agent_port = 8790;

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* guest agent client                                                  */
/* ------------------------------------------------------------------ */

/* one-shot request/response; returns 0 on success, fills resp */
/* Cheap liveness probe.  A bare connect() is not enough: agent_port is QEMU's
 * hostfwd listener, which accepts even when nothing is behind it.  Ask for a
 * reply instead, with a short timeout so the main loop is not stalled. */
static int agent_port_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)agent_port);
    if (inet_pton(AF_INET, agent_host, &sa.sin_addr) != 1) { close(fd); return 0; }
    int ok = 0;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        static const char probe[] = "{\"cmd\":\"getmode\"}\n";
        if (send_all(fd, probe, sizeof probe - 1) >= 0) {
            char b[256];
            ssize_t n = recv(fd, b, sizeof b, 0);
            ok = n > 0;
        }
    }
    close(fd);
    return ok;
}

static int agent_call(const char *req, char *resp, size_t respsz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)agent_port);
    if (inet_pton(AF_INET, agent_host, &sa.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (send_all(fd, req, strlen(req)) < 0 || send_all(fd, "\n", 1) < 0) { close(fd); return -1; }
    size_t got = 0;
    while (got + 1 < respsz) {
        ssize_t n = recv(fd, resp + got, respsz - 1 - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
        if (memchr(resp, '\n', got)) break;
    }
    close(fd);
    resp[got] = 0;
    /* strip UTF-8 BOM that PowerShell's StreamWriter emits */
    if (got >= 3 && (unsigned char)resp[0] == 0xEF &&
        (unsigned char)resp[1] == 0xBB && (unsigned char)resp[2] == 0xBF) {
        memmove(resp, resp + 3, got - 3 + 1);
    }
    return got ? 0 : -1;
}

static void json_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 8 < outsz; p++) {
        switch (*p) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (*p < 0x20) { o += snprintf(out + o, outsz - o, "\\u%04x", *p); }
            else out[o++] = (char)*p;
        }
    }
    out[o] = 0;
}

/* unescape a JSON string body in place-ish (writes into out) */
static void json_unescape(const char *in, size_t inlen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < inlen && o + 4 < outsz; i++) {
        if (in[i] != '\\') { out[o++] = in[i]; continue; }
        if (++i >= inlen) break;
        switch (in[i]) {
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case '"': out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/';  break;
        case 'u': {
            if (i + 4 >= inlen) { i = inlen; break; }
            char hex[5] = { in[i+1], in[i+2], in[i+3], in[i+4], 0 };
            unsigned cp = (unsigned)strtoul(hex, NULL, 16);
            i += 4;
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) {
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: out[o++] = in[i];
        }
    }
    out[o] = 0;
}

/* extract "text":"...." value from agent response */
static int agent_parse_text(const char *resp, char *out, size_t outsz) {
    const char *k = strstr(resp, "\"text\":");
    if (!k) return -1;
    const char *p = k + 7;
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    const char *start = p;
    while (*p) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') break;
        p++;
    }
    json_unescape(start, (size_t)(p - start), out, outsz);
    return 0;
}

/* The active display is not always DISPLAY1: with the vendor boot display
 * gone, Windows puts the virtio-gpu desktop on a different index.  Probe
 * once and remember which one has a real mode. */
static char agent_display[32] = "";

static int agent_get_guest_mode(int *w, int *h) {
    char resp[512];
    if (!agent_display[0]) {
        for (int i = 1; i <= 4; i++) {
            char dev[32], esc[64], req[160];
            snprintf(dev, sizeof dev, "\\\\.\\DISPLAY%d", i);
            json_escape(dev, esc, sizeof esc);
            snprintf(req, sizeof req, "{\"cmd\":\"getmode\",\"dev\":\"%s\"}", esc);
            if (agent_call(req, resp, sizeof resp) < 0) continue;
            const char *p = strstr(resp, "\"w\":");
            if (!p) continue;
            if (atoi(p + 4) > 0) {
                snprintf(agent_display, sizeof agent_display, "%s", dev);
                fprintf(stderr, "agent: active display is %s\n", agent_display);
                *w = atoi(p + 4);
                const char *q = strstr(resp, "\"h\":");
                if (q) *h = atoi(q + 4);
                return 0;
            }
        }
        return -1;
    }
    {
        char esc[64], req[160];
        json_escape(agent_display, esc, sizeof esc);
        snprintf(req, sizeof req, "{\"cmd\":\"getmode\",\"dev\":\"%s\"}", esc);
        if (agent_call(req, resp, sizeof resp) < 0) return -1;
    }
    const char *p = strstr(resp, "\"w\":");
    if (!p) return -1;
    *w = atoi(p + 4);
    p = strstr(resp, "\"h\":");
    if (!p) return -1;
    *h = atoi(p + 4);
    return (*w > 0 && *h > 0) ? 0 : -1;
}

static int agent_setres(int w, int h) {
    char req[192], resp[512], esc[64];
    if (!agent_display[0]) { int gw, gh; agent_get_guest_mode(&gw, &gh); }
    json_escape(agent_display[0] ? agent_display : "\\\\.\\DISPLAY1", esc, sizeof esc);
    snprintf(req, sizeof req, "{\"cmd\":\"setres\",\"w\":%d,\"h\":%d,\"dev\":\"%s\"}", w, h, esc);
    if (agent_call(req, resp, sizeof resp) < 0) return -1;
    return strstr(resp, "\"ok\":true") ? 0 : -1;
}

static int agent_clipget(char *out, size_t outsz) {
    char resp[70000];
    if (agent_call("{\"cmd\":\"clipget\"}", resp, sizeof resp) < 0) return -1;
    if (!strstr(resp, "\"ok\":true")) return -1;
    return agent_parse_text(resp, out, outsz);
}

static int agent_clipset(const char *text) {
    size_t n = strlen(text);
    size_t cap = n * 6 + 128;
    char *req = malloc(cap);
    if (!req) return -1;
    char *esc = malloc(n * 6 + 8);
    if (!esc) { free(req); return -1; }
    json_escape(text, esc, n * 6 + 4);
    snprintf(req, cap, "{\"cmd\":\"clipset\",\"text\":\"%s\"}", esc);
    free(esc);
    char resp[512];
    int rc = agent_call(req, resp, sizeof resp);
    free(req);
    if (rc < 0) return -1;
    return strstr(resp, "\"ok\":true") ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* X11 clipboard                                                       */
/* ------------------------------------------------------------------ */

static char clip_last_host[65536];/* last text we read from X */
static char clip_last_guest[65536];/* last text we saw in the guest */

/* xclip-backed clipboard bridge: simpler and far more robust than
 * hand-rolling ICCCM selection ownership inside this process. */

static pid_t clip_serve_pid = -1;

static void x11_set_clipboard(const char *text) {
    if (clip_serve_pid > 0) { kill(clip_serve_pid, SIGTERM); clip_serve_pid = -1; }
    int pfd[2];
    if (pipe(pfd) < 0) return;
    pid_t p = fork();
    if (p == 0) {
        dup2(pfd[0], 0);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
        close(pfd[0]); close(pfd[1]);
        /* never leak the QMP/audio sockets into the helper: the display socket is
         * single-client, so a surviving child would block the next viewer */
        for (int fd = 3; fd < 256; fd++) if (fd != 0) close(fd);
        execlp("xclip", "xclip", "-selection", "clipboard", "-i", "-quiet", (char *)NULL);
        _exit(127);
    }
    close(pfd[0]);
    if (p > 0) {
        size_t len = strlen(text), off = 0;
        while (off < len) {
            ssize_t n = write(pfd[1], text + off, len - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(pfd[1]);
        clip_serve_pid = p;
    } else {
        close(pfd[1]);
    }
}

static int x11_read_clipboard(char *out, size_t outsz) {
    out[0] = 0;
    FILE *f = popen("timeout 1 xclip -o -selection clipboard 2>/dev/null", "r");
    if (!f) return -1;
    size_t got = fread(out, 1, outsz - 1, f);
    out[got] = 0;
    int rc = pclose(f);
    if (rc != 0 && got == 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* QMP                                                                 */
/* ------------------------------------------------------------------ */

static void qmp_send_json(const char *json) {
    if (qmp_fd < 0) return;
    send_all(qmp_fd, json, strlen(json));
    send_all(qmp_fd, "\n", 1);
}

static void qmp_drain(void) {
    if (qmp_fd < 0) return;
    char buf[16384];
    while (1) {
        ssize_t n = recv(qmp_fd, buf, sizeof buf - 1, MSG_DONTWAIT);
        if (n <= 0) { if (n == 0) qmp_dead = 1; break; }
        buf[n] = 0;
        qmp_handle_event(buf);
        if (strstr(buf, "\"error\"")) {
            FILE *f = fopen("/tmp/winvm-viewer-qmp.log", "a");
            if (f) { fputs(buf, f); fclose(f); }
        }
    }
}

static void qmp_init(void) {
    qmp_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (qmp_fd < 0) return;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", qmp_path);
    if (connect(qmp_fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(qmp_fd); qmp_fd = -1; return;
    }
    qmp_drain();
    qmp_send_json("{\"execute\":\"qmp_capabilities\",\"id\":1}");
    usleep(100000);
    qmp_drain();
}

static void qmp_key(const char *qcode, int down) {
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
             "{\"type\":\"key\",\"data\":{\"down\":%s,\"key\":{\"type\":\"qcode\",\"data\":\"%s\"}}}]},\"id\":%d}",
             down ? "true" : "false", qcode, qmp_id++);
    qmp_send_json(buf);
}

static void qmp_mouse_abs(int x, int y) {
    if (win_w <= 1 || win_h <= 1) return;
    int ax = (int)((int64_t)x * 32767 / (win_w > 1 ? win_w - 1 : 1));
    int ay = (int)((int64_t)y * 32767 / (win_h > 1 ? win_h - 1 : 1));
    if (ax < 0) ax = 0;
    if (ax > 32767) ax = 32767;
    if (ay < 0) ay = 0;
    if (ay > 32767) ay = 32767;
    char buf[320];
    snprintf(buf, sizeof buf,
             "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
             "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":%d}},"
             "{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":%d}}]},\"id\":%d}",
             ax, ay, qmp_id++);
    qmp_send_json(buf);
}

static void qmp_button(const char *button, int down) {
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
             "{\"type\":\"btn\",\"data\":{\"button\":\"%s\",\"down\":%s}}]},\"id\":%d}",
             button, down ? "true" : "false", qmp_id++);
    qmp_send_json(buf);
}

static const struct { const char *xname; const char *qcode; } keymap[] = {
    {"space","spc"},{"Return","ret"},{"KP_Enter","kp_enter"},{"Tab","tab"},
    {"Escape","esc"},{"BackSpace","backspace"},{"Delete","del"},{"Insert","ins"},
    {"Home","home"},{"End","end"},{"Prior","pgup"},{"Next","pgdn"},
    {"Left","left"},{"Right","right"},{"Up","up"},{"Down","down"},
    {"Shift_L","shift"},{"Shift_R","shift"},{"Control_L","ctrl"},{"Control_R","ctrl"},
    {"Alt_L","alt"},{"Alt_R","alt"},{"Super_L","meta_l"},{"Super_R","meta_r"},
    {"Caps_Lock","caps_lock"},{"Num_Lock","num_lock"},{"Scroll_Lock","scroll_lock"},
    {"Print","print"},{"Pause","pause"},
    {"minus","minus"},{"equal","equal"},{"bracketleft","bracket_left"},
    {"bracketright","bracket_right"},{"backslash","backslash"},
    {"semicolon","semicolon"},{"apostrophe","apostrophe"},{"grave","grave_accent"},
    {"comma","comma"},{"period","dot"},{"slash","slash"},
    {"F1","f1"},{"F2","f2"},{"F3","f3"},{"F4","f4"},{"F5","f5"},{"F6","f6"},
    {"F7","f7"},{"F8","f8"},{"F9","f9"},{"F10","f10"},{"F11","f11"},{"F12","f12"},
    {0,0}
};

static const char *keysym_to_qcode(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) { static char b[2]; b[0] = (char)ks; b[1] = 0; return b; }
    if (ks >= XK_A && ks <= XK_Z) { static char b[2]; b[0] = (char)(ks - XK_A + 'a'); b[1] = 0; return b; }
    if (ks >= XK_0 && ks <= XK_9) { static char b[2]; b[0] = (char)ks; b[1] = 0; return b; }
    const char *name = XKeysymToString(ks);
    if (!name) return NULL;
    for (int i = 0; keymap[i].xname; i++)
        if (!strcmp(keymap[i].xname, name)) return keymap[i].qcode;
    return NULL;
}



static Atom wm_delete_atom = 0;

/* Losing the window (WM kill, session end) must not take the process down with
 * an Xlib abort: log it and let the main loop exit cleanly. */
static int x_error_handler(Display *d, XErrorEvent *e) {
    char msg[256];
    XGetErrorText(d, e->error_code, msg, sizeof msg);
    fprintf(stderr, "x11: %s (request %d)\n", msg, e->request_code);
    /* A ShmSeg the server has dropped must never turn every later frame into
     * a no-op: that is a black window, which looks exactly like a hang.
     * Latch a flag and let present_damage() fall back to XPutImage. */
    if (strstr(msg, "ShmSeg")) shm_broken = 1;
    return 0;
}

static void xinit(void) {
    XSetErrorHandler(x_error_handler);
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "cannot open display\n"); exit(1); }
    int scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)win_w, (unsigned)win_h, 0,
                              BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XStoreName(dpy, win, "Windows 11 (M1000)");
    /* let the desktop link this window to winvm.desktop (rabbit icon) */
    {
        XClassHint ch;
        ch.res_name = (char *)"winvm";
        ch.res_class = (char *)"Winvm";
        XSetClassHint(dpy, win, &ch);
    }
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                 StructureNotifyMask | FocusChangeMask | PropertyChangeMask);
    wm_delete_atom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_atom, 1);
    XMapWindow(dpy, win);

    img = XCreateImage(dpy, DefaultVisual(dpy, scr), DefaultDepth(dpy, scr),
                       ZPixmap, 0, NULL, (unsigned)win_w, (unsigned)win_h, 32, 0);
    img->data = calloc(1, (size_t)img->bytes_per_line * (size_t)win_h);
    gc = XCreateGC(dpy, win, 0, NULL);
    hide_local_cursor();   /* Windows draws its own pointer; ours would lag behind */
    splash_load(splash_path);
    shm_try_enable();
}

/* ---- MIT-SHM presentation ------------------------------------------ */
/* Replace the client-side calloc()d XImage with one backed by a SysV shared
 * segment, so XShmPutImage hand the compositor a pointer instead of copying
 * the window through the X11 socket.  Falls back silently to XPutImage when
 * the server has no MIT-SHM (remote X, some nested servers).               */
static void shm_try_enable(void) {
    if (shm_ok) return;
    /* a segment the server has already dropped must not be recreated: any new
     * one could hit the same path and we would black out the window again */
    if (shm_broken) return;
    int major = 0, minor = 0, ev = 0, er = 0;
    if (!XShmQueryExtension(dpy)) return;
    XShmQueryVersion(dpy, &major, &minor, &ev);
    (void)er;
    XImage *shm_img = XShmCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                                      DefaultDepth(dpy, DefaultScreen(dpy)), ZPixmap,
                                      NULL, &shm_info, (unsigned)win_w, (unsigned)win_h);
    if (!shm_img) return;
    shm_info.shmid = shmget(IPC_PRIVATE,
                            (size_t)shm_img->bytes_per_line * (size_t)win_h,
                            IPC_CREAT | 0600);
    if (shm_info.shmid < 0) { shm_img->data = NULL; XDestroyImage(shm_img); return; }
    shm_info.shmaddr = shmat(shm_info.shmid, NULL, 0);
    if (shm_info.shmaddr == (void *)-1) {
        shmctl(shm_info.shmid, IPC_RMID, NULL);
        shm_img->data = NULL; XDestroyImage(shm_img);
        return;
    }
    shm_info.readOnly = False;
    shm_img->data = shm_info.shmaddr;
    if (!XShmAttach(dpy, &shm_info)) {
        shmdt(shm_info.shmaddr);
        shmctl(shm_info.shmid, IPC_RMID, NULL);
        shm_img->data = NULL; XDestroyImage(shm_img);
        return;
    }
    XSync(dpy, False);
    /* Keep the id until shm_detach(): marking IPC_RMID here lets the segment
     * be torn down the moment the server drops its reference, and a stray
     * XShmPutImage after that turns every frame into BadShmSeg. */
    memset(shm_img->data, 0, (size_t)shm_img->bytes_per_line * (size_t)win_h);

    XDestroyImage(img);
    img = shm_img;
    shm_ok = 1;
    fprintf(stderr, "present: MIT-SHM enabled (%dx%d)\n", win_w, win_h);
}

static void shm_detach(void) {
    if (!shm_ok) return;
    XShmDetach(dpy, &shm_info);
    XSync(dpy, False);
    /* XDestroyImage would free() the shm address, which is not ours to free */
    img->data = NULL;
    XDestroyImage(img);
    shmdt(shm_info.shmaddr);
    shmctl(shm_info.shmid, IPC_RMID, NULL);
    shm_ok = 0;
}

/* mark the whole window as damaged (splash, dialog, resize) */
static void damage_all(void) {
    dmg_x0 = 0; dmg_y0 = 0;
    dmg_x1 = win_w; dmg_y1 = win_h;
}

static void damage_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (dmg_x1 <= dmg_x0) {          /* empty: start from this rect */
        dmg_x0 = x; dmg_y0 = y; dmg_x1 = x + w; dmg_y1 = y + h;
        return;
    }
    if (x < dmg_x0) dmg_x0 = x;
    if (y < dmg_y0) dmg_y0 = y;
    if (x + w > dmg_x1) dmg_x1 = x + w;
    if (y + h > dmg_y1) dmg_y1 = y + h;
}

static int damage_empty(void) { return dmg_x1 <= dmg_x0 || dmg_y1 <= dmg_y0; }

/* push the damaged rectangle to the server */
static void present_damage(void) {
    if (damage_empty()) return;
    int x = dmg_x0, y = dmg_y0;
    int w = dmg_x1 - dmg_x0, h = dmg_y1 - dmg_y0;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > img->width) w = img->width - x;
    if (y + h > img->height) h = img->height - y;
    if (w > 0 && h > 0) {
        if (shm_ok && !shm_broken) {
            XShmPutImage(dpy, win, gc, img, x, y, x, y, (unsigned)w, (unsigned)h, False);
        } else {
            XPutImage(dpy, win, gc, img, x, y, x, y, (unsigned)w, (unsigned)h);
        }
        XFlush(dpy);
    }
    dmg_x1 = dmg_x0;                 /* reset to empty */
    dmg_y1 = dmg_y0;
}


/* The guest composites its own pointer into the scanout, so the local X11
 * cursor would show up as a second, lagging arrow on top of it. */
static void hide_local_cursor(void) {
    static char empty = 0;
    Pixmap pm = XCreateBitmapFromData(dpy, win, &empty, 1, 1);
    if (!pm) return;
    XColor black;
    memset(&black, 0, sizeof black);
    Cursor cur = XCreatePixmapCursor(dpy, pm, pm, &black, &black, 0, 0);
    if (cur) XDefineCursor(dpy, win, cur);
    XFreePixmap(dpy, pm);
}

/* A modal dialog is answered with the *host* pointer, so the hidden cursor has
 * to come back for as long as the dialog is up. */
static void show_local_cursor(void) {
    XUndefineCursor(dpy, win);
    XFlush(dpy);
}


/* ---- input grab -----------------------------------------------------
 * Like VMware/Parallels: clicking into the window grabs pointer + keyboard so
 * the host stops stealing input (Alt+Tab, Super, ...) and the host cursor can
 * no longer wander off the guest surface.  Ctrl+Alt gives it back. */
static int grabbed = 0;

static void grab_input(void) {
    if (grabbed) return;
    int pr = XGrabPointer(dpy, win, True,
                          PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                          GrabModeAsync, GrabModeAsync, win, None, CurrentTime);
    int kr = XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    if (pr == GrabSuccess) grabbed = 1;
    fprintf(stderr, "input: grab pointer=%d keyboard=%d grabbed=%d\n", pr, kr, grabbed);
}

static void ungrab_input(void) {
    if (!grabbed) return;
    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    grabbed = 0;
    fprintf(stderr, "input: released (Ctrl+Alt)\n");
}

static void rebuild_image(void) {
    if (shm_ok) shm_detach();
    else XDestroyImage(img);
    img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), DefaultDepth(dpy, DefaultScreen(dpy)),
                       ZPixmap, 0, NULL, (unsigned)win_w, (unsigned)win_h, 32, 0);
    img->data = calloc(1, (size_t)img->bytes_per_line * (size_t)win_h);
    shm_try_enable();
    damage_all();
}
/* set once the guest has actually painted something; before that we keep the
 * splash up, because the SBD framebuffer is valid-but-all-black while the
 * firmware/loader runs and we do not want to replace our splash with black */
static int guest_painted = 0;
/* the firmware sends Disable when it hands the console over to the OS; until
 * then every frame we get is the firmware logo, which we keep covered */
static int seen_disable = 0;

/* true when the frame we just blitted is all black: the firmware handover and
 * the GPU mode switches produce black scanouts we do not want to show */
static int img_is_blank(void) {
    int w = img->width, h = img->height;
    if (w <= 0 || h <= 0) return 1;
    for (int y = 0; y < h; y += 16) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)img->data
                                                 + (size_t)y * (size_t)img->bytes_per_line);
        for (int x = 0; x < w; x += 16) {
            if (row[x] & 0x00ffffffu) return 0;
        }
    }
    return 1;
}

static void render(void) {
    if (!dirty || !have_frame) return;
    if (!splash_start_ms) splash_start_ms = now_ms();
    /* A halted/suspended/blanked console keeps delivering the frame it froze
     * on.  Painting that hides the state we are trying to communicate, so the
     * splash wins until the guest is running again. */
    if (dlg == DLG_NONE &&
        (vstate == V_SHUTTING || vstate == V_OFF ||
         vstate == V_SUSPENDED || vstate == V_DISPLAY_OFF)) {
        render_splash();
        dirty = 0;
        return;
    }
    /* While booting, keep our splash over the firmware logo (TianoCore) and the
     * black phases that follow it.  Once Windows is up we render its frames. */
    if (vstate == V_BOOTING) {
        if (dlg == DLG_NONE &&
            (now_ms() - splash_start_ms < SPLASH_MIN_MS || img_is_blank() ||
             (!seen_disable && now_ms() - splash_start_ms < SPLASH_GIVEUP_MS))) {
            splash_phase = (int)((now_ms() - splash_start_ms) / 350) % 3;
            render_splash();
            dirty = 0;
            return;
        }
        if (dlg == DLG_NONE) {
            vstate = V_RUNNING;     /* hand over to Windows */
            /* the splash filled the whole image; repaint all of it once so no
             * splash pixels survive under the first partial guest frame */
            damage_all();
            fprintf(stderr, "splash: handing over to the guest\n");
        }
    }
    if (dlg != DLG_NONE) {
        if (dlg_dirty) { dialog_draw(); dlg_dirty = 0; }
        dirty = 0;
        return;
    }
    present_damage();
    dirty = 0;
}

/* ------------------------------------------------------------------ */
/* boot splash                                                        */
/* ------------------------------------------------------------------ */

static void splash_img_load(const char *path, spl_img *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    uint8_t hdr[12];
    if (read(fd, hdr, 12) != 12 || memcmp(hdr, "SPL1", 4) != 0) { close(fd); return; }
    int w = (int)rd32(hdr + 4), h = (int)rd32(hdr + 8);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { close(fd); return; }
    size_t sz = (size_t)w * (size_t)h * 4, got = 0;
    uint8_t *buf = malloc(sz);
    if (!buf) { close(fd); return; }
    while (got < sz) {
        ssize_t n = read(fd, buf + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != sz) { free(buf); return; }
    free(out->px);
    out->px = (uint32_t *)buf;
    out->w = w;
    out->h = h;
    fprintf(stderr, "splash: loaded %s %dx%d\n", path, w, h);
}

static void splash_load(const char *path) {
    splash_img_load(path, &spl_logo);
    static const char *names[SP_COUNT] = { "text-boot", "text-shutdown", "text-suspend", "text-off" };
    for (int i = 0; i < SP_COUNT; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s.rgba", splash_dir, names[i]);
        splash_img_load(p, &spl_text[i]);
    }
    char p[512];
    snprintf(p, sizeof p, "%s/dlg-ask.rgba", splash_dir);   splash_img_load(p, &dlg_ask);
    snprintf(p, sizeof p, "%s/dlg-force.rgba", splash_dir); splash_img_load(p, &dlg_force);
    snprintf(p, sizeof p, "%s/dlg-yes.rgba", splash_dir);   splash_img_load(p, &dlg_btn_yes);
    snprintf(p, sizeof p, "%s/dlg-no.rgba", splash_dir);    splash_img_load(p, &dlg_btn_no);
    snprintf(p, sizeof p, "%s/dlg-forcebtn.rgba", splash_dir); splash_img_load(p, &dlg_btn_force);
}

/* bilinear blit, composited over the (black) background */
static void blit_img(int dw, int dh, int ox, int oy, const spl_img *src) {
    if (!src->px || dw <= 0 || dh <= 0) return;
    double sx = (double)src->w / (double)dw;
    double sy = (double)src->h / (double)dh;
    for (int y = 0; y < dh; y++) {
        int ty = oy + y;
        if (ty < 0 || ty >= win_h) continue;
        double fy0 = (y + 0.5) * sy - 0.5;
        if (fy0 < 0) fy0 = 0;
        if (fy0 > src->h - 1) fy0 = src->h - 1;
        int y0 = (int)fy0, y1 = (y0 + 1 < src->h) ? y0 + 1 : y0;
        double fy = fy0 - y0;
        const uint32_t *r0 = src->px + (size_t)y0 * (size_t)src->w;
        const uint32_t *r1 = src->px + (size_t)y1 * (size_t)src->w;
        uint8_t *drow = (uint8_t *)img->data + (size_t)ty * (size_t)img->bytes_per_line;
        for (int x = 0; x < dw; x++) {
            int tx = ox + x;
            if (tx < 0 || tx >= win_w) continue;
            double fx0 = (x + 0.5) * sx - 0.5;
            if (fx0 < 0) fx0 = 0;
            if (fx0 > src->w - 1) fx0 = src->w - 1;
            int x0 = (int)fx0, x1 = (x0 + 1 < src->w) ? x0 + 1 : x0;
            double fx = fx0 - x0;
            uint32_t p00 = r0[x0], p01 = r0[x1], p10 = r1[x0], p11 = r1[x1];
            double a = (1 - fy) * ((1 - fx) * ((p00 >> 24) & 0xff) + fx * ((p01 >> 24) & 0xff))
                     + fy * ((1 - fx) * ((p10 >> 24) & 0xff) + fx * ((p11 >> 24) & 0xff));
            if (a <= 0.5) continue;
            uint8_t *p = drow + (size_t)tx * 4;
            for (int c = 0; c < 3; c++) {
                int sh = 16 - c * 8;
                double v00 = (p00 >> sh) & 0xff, v01 = (p01 >> sh) & 0xff;
                double v10 = (p10 >> sh) & 0xff, v11 = (p11 >> sh) & 0xff;
                double v = (1 - fy) * ((1 - fx) * v00 + fx * v01) + fy * ((1 - fx) * v10 + fx * v11);
                p[2 - c] = (uint8_t)(v * (a / 255.0) + 0.5);
            }
            p[3] = 0;
        }
    }
}

static void draw_dot(int cx, int cy, int r, int on) {
    for (int y = -r; y <= r; y++) {
        int ty = cy + y;
        if (ty < 0 || ty >= win_h) continue;
        uint8_t *drow = (uint8_t *)img->data + (size_t)ty * (size_t)img->bytes_per_line;
        for (int x = -r; x <= r; x++) {
            int tx = cx + x;
            if (tx < 0 || tx >= win_w) continue;
            if (x * x + y * y > r * r) continue;
            uint8_t *p = drow + (size_t)tx * 4;
            uint8_t v = on ? 225 : 80;
            p[0] = v; p[1] = v; p[2] = v; p[3] = 0;
        }
    }
}

static void render_splash(void) {
    if (!splash_start_ms) splash_start_ms = now_ms();
    memset(img->data, 0, (size_t)img->bytes_per_line * (size_t)win_h);

    /* logo: about a third of the window height, a little above centre */
    int logo_h = (int)(win_h * 0.34);
    int logo_w = logo_h;
    if (spl_logo.w > 0 && spl_logo.h > 0)
        logo_w = (int)((double)logo_h * spl_logo.w / spl_logo.h);
    if (logo_w > (int)(win_w * 0.55)) {
        logo_w = (int)(win_w * 0.55);
        logo_h = (spl_logo.w > 0) ? (int)((double)logo_w * spl_logo.h / spl_logo.w) : logo_w;
    }
    int logo_x = (win_w - logo_w) / 2;
    int logo_y = (int)(win_h * 0.40) - logo_h / 2;
    blit_img(logo_w, logo_h, logo_x, logo_y, &spl_logo);

    /* status line underneath, one text per power state */
    int y = logo_y + logo_h + win_h / 18;
    const spl_img *txt = &spl_text[SP_BOOT];
    if (vstate == V_SHUTTING)      txt = &spl_text[SP_SHUTDOWN];
    else if (vstate == V_SUSPENDED) txt = &spl_text[SP_SUSPEND];
    else if (vstate == V_OFF)       txt = &spl_text[SP_OFF];
    /* guest still running, console gone: same "click to continue" hint as a
     * suspend, because a click is what brings the desktop back */
    else if (vstate == V_DISPLAY_OFF || vstate == V_RUNNING) txt = &spl_text[SP_SUSPEND];
    if (txt->px) {
        blit_img(txt->w, txt->h, (win_w - txt->w) / 2, y, txt);
        y += txt->h;
    }

    /* three dots, swept one at a time (not while halted / suspended / blanked) */
    int r = win_h / 110;
    if (r < 4) r = 4;
    int step = r * 4;
    int total = step * 3;
    int cx = (win_w - total) / 2 + step / 2;
    int cy = y + win_h / 20 + r;
    if (vstate == V_BOOTING || vstate == V_SHUTTING)
        for (int i = 0; i < 3; i++)
            draw_dot(cx + i * step, cy, r, i == splash_phase);

    if (dlg != DLG_NONE) {
        if (dlg_dirty) { dialog_draw(); dlg_dirty = 0; }
        dirty = 0;
        return;
    }
    damage_all();          /* splash repaints the whole window */
    present_damage();
    dirty = 0;
}

/* ------------------------------------------------------------------ */
/* modal dialog: "close the window = power the guest off?"             */
/*                                                                    */
/* Blocking by design: while a dialog is up the viewer ignores guest   */
/* input and keeps the grab released, so the user can click the        */
/* buttons with the host pointer.  The buttons are the only clickable  */
/* targets until the question is answered.                             */
/* ------------------------------------------------------------------ */

/* button rectangles, recomputed every draw */
static int btn_rect[2][4];   /* x, y, w, h */
static int btn_count = 0;
/* snapshot of the guest frame taken when a dialog opens, so the dim is applied
 * to a stable copy once instead of to an image we rewrite every frame */
static uint8_t *dlg_bg = NULL;
static size_t dlg_bg_sz = 0;

static void dialog_snapshot(void) {
    if (!img) return;
    size_t need = (size_t)img->bytes_per_line * (size_t)win_h;
    if (dlg_bg_sz != need) {
        free(dlg_bg);
        dlg_bg = malloc(need);
        dlg_bg_sz = dlg_bg ? need : 0;
    }
    if (dlg_bg) memcpy(dlg_bg, img->data, need);
}

static void fill_rect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= win_h) continue;
        uint8_t *row = (uint8_t *)img->data + (size_t)y * (size_t)img->bytes_per_line;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= win_w) continue;
            uint8_t *p = row + (size_t)x * 4;
            p[0] = b; p[1] = g; p[2] = r; p[3] = 0;
        }
    }
}

static void dialog_draw(void) {
    fprintf(stderr, "dialog: draw dlg=%d win=%dx%d\n", dlg, win_w, win_h);
    /* dim the guest frame so the dialog stands out.  Start from the snapshot
     * taken when the dialog opened: applying /3 to the live image on every
     * frame drove the background to black within a few redraws. */
    int npx = img->bytes_per_line * win_h;
    if (dlg_bg) {
        memcpy(img->data, dlg_bg, (size_t)npx);
    }
    for (int i = 0; i < npx; i += 4) {
        uint8_t *p = (uint8_t *)img->data + i;
        p[0] = (uint8_t)(p[0] / 3);
        p[1] = (uint8_t)(p[1] / 3);
        p[2] = (uint8_t)(p[2] / 3);
    }

    int bw = (int)(win_w * 0.62), bh = (int)(win_h * 0.30);
    if (bw > 900) bw = 900;
    if (bh > 300) bh = 300;
    int bx = (win_w - bw) / 2, by = (win_h - bh) / 2;
    fill_rect(bx, by, bw, bh, 0x1e, 0x1e, 0x1e);
    fill_rect(bx, by, bw, 2, 0x50, 0x50, 0x50);
    fill_rect(bx, by + bh - 2, bw, 2, 0x50, 0x50, 0x50);

    const spl_img *q = (dlg == DLG_ASK_FORCE) ? &dlg_force : &dlg_ask;
    if (q->px) blit_img(q->w, q->h, bx + (bw - q->w) / 2, by + bh / 4 - q->h / 2, q);

    /* buttons: yes/force on the left, cancel on the right */
    const spl_img *l1 = (dlg == DLG_ASK_FORCE) ? &dlg_btn_force : &dlg_btn_yes;
    const spl_img *l2 = &dlg_btn_no;
    int bwid = (int)(bw * 0.28), bhei = (int)(bh * 0.22);
    if (bhei < 34) bhei = 34;
    int gap = (int)(bw * 0.06);
    int total = bwid * 2 + gap;
    int x0 = bx + (bw - total) / 2;
    int y0 = by + bh - bhei - bh / 6;
    btn_count = 2;
    btn_rect[0][0] = x0; btn_rect[0][1] = y0; btn_rect[0][2] = bwid; btn_rect[0][3] = bhei;
    btn_rect[1][0] = x0 + bwid + gap; btn_rect[1][1] = y0; btn_rect[1][2] = bwid; btn_rect[1][3] = bhei;
    for (int i = 0; i < 2; i++) {
        int hovered = (dlg_hover == i);
        fill_rect(btn_rect[i][0], btn_rect[i][1], bwid, bhei,
                  hovered ? 0x3a : 0x2a, hovered ? 0x3a : 0x2a, hovered ? 0x3a : 0x2a);
        const spl_img *lb = i == 0 ? l1 : l2;
        if (lb->px)
            blit_img(lb->w, lb->h, btn_rect[i][0] + (bwid - lb->w) / 2,
                     btn_rect[i][1] + (bhei - lb->h) / 2, lb);
    }

    damage_all();          /* the dialog redraws the whole window */
    present_damage();
}

static int hit_button(int x, int y) {
    for (int i = 0; i < btn_count; i++)
        if (x >= btn_rect[i][0] && x < btn_rect[i][0] + btn_rect[i][2] &&
            y >= btn_rect[i][1] && y < btn_rect[i][1] + btn_rect[i][3])
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* power management: shutdown escalation + guest state tracking        */
/* ------------------------------------------------------------------ */

static void agent_shutdown(void);   /* fwd */
static void qmp_powerdown(void);    /* fwd */

static void start_shutdown_flow(void) {
    if (dlg != DLG_ASK_SHUTDOWN) return;
    dlg = DLG_NONE;
    btn_count = 0;
    vstate = V_SHUTTING;
    splash_start_ms = now_ms();
    shut_stage = 0;
    shut_deadline = now_ms() + 20000;   /* give the agent 20 s */
    fprintf(stderr, "shutdown: asking the agent\n");
    agent_shutdown();
    dirty = 1;
}

static void force_power_off(void) {
    fprintf(stderr, "shutdown: forcing power off\n");
    qmp_send_json("{\"execute\":\"quit\",\"id\":9001}");
    /* give QMP a moment to be seen, then make sure */
    for (int i = 0; i < 10; i++) { qmp_drain(); usleep(100000); }
}

static void dialog_click(int x, int y, int button) {
    int i = hit_button(x, y);
    if (button != 1 || i < 0) return;
    if (dlg == DLG_ASK_SHUTDOWN) {
        if (i == 0) start_shutdown_flow();          /* yes: shut down */
        else {
            /* cancel: back to the guest, and give it the pointer back */
            dlg = DLG_NONE; btn_count = 0; dlg_dirty = 1; dirty = 1;
            damage_all();
            hide_local_cursor();
        }
    } else if (dlg == DLG_ASK_FORCE) {
        if (i == 0) { dlg = DLG_NONE; force_power_off(); }
        else dlg = DLG_NONE;
        btn_count = 0;
        dirty = 1;
        damage_all();
    }
}

static void open_close_dialog(void) {
    fprintf(stderr, "close: opening ask-shutdown dialog\n");
    ungrab_input();                 /* the dialog needs the host pointer */
    show_local_cursor();            /* ...and it has to be visible to click */
    dlg = DLG_ASK_SHUTDOWN;
    dlg_hover = -1;
    dlg_dirty = 1;
    dialog_snapshot();
    dirty = 1;
    if (have_frame) render();
    else render_splash();
}

static void agent_shutdown(void) {
    char resp[256];
    if (agent_call("{\"cmd\":\"shutdown\"}", resp, sizeof resp) == 0)
        fprintf(stderr, "shutdown: agent accepted (%s)\n", resp);
    else
        fprintf(stderr, "shutdown: agent did not answer\n");
}

static void qmp_powerdown(void) {
    qmp_send_json("{\"execute\":\"system_powerdown\",\"id\":9000}");
    qmp_drain();
}

/* called from the main loop; drives the shutdown escalation */
static void power_tick(void) {
    if (vstate != V_SHUTTING) return;
    long now = now_ms();
    if (now < shut_deadline) return;
    if (shut_stage == 0) {
        fprintf(stderr, "shutdown: agent silent, sending ACPI power button\n");
        qmp_powerdown();
        shut_stage = 1;
        shut_deadline = now + 25000;
    } else if (shut_stage == 1) {
        fprintf(stderr, "shutdown: still running, asking to force off\n");
        shut_stage = 2;
        dlg = DLG_ASK_FORCE;
        dlg_dirty = 1;
        dirty = 1;
    }
}

/* Windows stops the guest agent early in a shutdown it started itself, so a
 * port that stops accepting connections is the earliest signal we get.  QEMU
 * only tells us once the guest has already halted, which leaves the frozen
 * desktop on screen for the whole shutdown. */
static void power_liveness_tick(void) {
    static long next_ms = 0;
    static int misses = 0;
    if (vstate != V_RUNNING || dlg != DLG_NONE) { misses = 0; return; }
    if (now_ms() < next_ms) return;
    next_ms = now_ms() + 2000;
    if (agent_port_open()) { misses = 0; return; }
    if (++misses < 3) return;
    misses = 0;
    fprintf(stderr, "power: guest agent gone, showing shutdown state\n");
    vstate = V_SHUTTING;
    ungrab_input();
    splash_start_ms = now_ms();
    dirty = 1;
    render_splash();
}

/* QMP events tell us what the guest is doing (shutdown, suspend, ...) */
static void qmp_handle_event(const char *buf) {
    /* query-status replies carry the authoritative runstate.  It is the only
     * way to tell "the guest powered off" from "the console went away while
     * the guest keeps running", which is what picks the splash wording. */
    {
        const char *sp = strstr(buf, "\"status\":");
        if (!sp) sp = strstr(buf, "\"status\": ");
        if (sp) {
            sp = strchr(sp, ':');
            if (sp) {
                sp++;
                while (*sp == ' ') sp++;
                if (*sp == '"') {
                    sp++;
                    size_t n = 0;
                    while (sp[n] && sp[n] != '"' && n + 1 < sizeof qmp_runstate)
                        n++;
                    memcpy(qmp_runstate, sp, n);
                    qmp_runstate[n] = 0;
                    if (!strcmp(qmp_runstate, "shutdown") && vstate != V_OFF) {
                        fprintf(stderr, "qmp: runstate=shutdown\n");
                        vstate = V_OFF;
                        ungrab_input();
                        dirty = 1;
                        render_splash();
                    }
                }
            }
        }
    }
    if (strstr(buf, "\"event\": \"SHUTDOWN\"") || strstr(buf, "\"event\":\"SHUTDOWN\"")) {
        fprintf(stderr, "qmp: guest shut down\n");
        vstate = V_OFF;
        ungrab_input();
        dirty = 1;
        render_splash();
        /* QEMU exits on guest shutdown; give it a moment then leave */
        for (int i = 0; i < 15; i++) { usleep(200000); }
        exit(0);
    }
    if (strstr(buf, "\"event\": \"STOP\"") || strstr(buf, "\"event\":\"STOP\"")) {
        /* the guest suspended itself (S3): show "suspended, click to resume" */
        fprintf(stderr, "qmp: guest suspended\n");
        vstate = V_SUSPENDED;
        ungrab_input();
        dirty = 1;
        render_splash();
    }
    if (strstr(buf, "\"event\": \"RESUME\"") || strstr(buf, "\"event\":\"RESUME\"")) {
        fprintf(stderr, "qmp: guest resumed\n");
        vstate = V_RUNNING;
        dirty = 1;
    }
    if (strstr(buf, "\"event\": \"POWERDOWN\"") || strstr(buf, "\"event\":\"POWERDOWN\"")) {
        fprintf(stderr, "qmp: guest asked for powerdown\n");
        vstate = V_SHUTTING;
        ungrab_input();
        shut_stage = 1;
        shut_deadline = now_ms() + 30000;
        dirty = 1;
    }
}

/* Poll QMP runstate now and then.  The guest can drop its console or stop
 * without any event that we handle, so ask rather than guess. */
static void qmp_status_tick(void) {
    if (qmp_fd < 0) return;
    long now = now_ms();
    if (now < qmp_status_next) return;
    qmp_status_next = now + 3000;
    qmp_send_json("{\"execute\":\"query-status\",\"id\":9100}");
}


/*                                                                    */
/* Handshake: QEMU sends 12 bytes "0DUA"|ver|memsize plus two fds     */
/* (eventfd, ring memfd) in one recvmsg.  Two things are mandatory:   */
/*   1. echo those 12 bytes back, and                                 */
/*   2. keep writing to the eventfd ("I consumed a period").          */
/* Without both, QEMU never fills the ring and the client sees only   */
/* the descriptor header.  PCM then lands at [ring_off, ring_off+      */
/* ring_size) and is pumped to pacat on the host.                     */
/* ------------------------------------------------------------------ */

/* We hand that stream to pacat, so guest audio comes out of the host */
/* speakers.                                                          */
/* ------------------------------------------------------------------ */

static int au_listen = -1, au_conn = -1, au_pacat = -1;
static int au_port = 8791;
static int au_hdr_got = 0, au_ready = 0;
static uint8_t au_hdr[16];
static uint32_t au_rate = 44100, au_ch = 2;

static void audio_tcp_close_conn(void) {
    if (au_pacat >= 0) { close(au_pacat); au_pacat = -1; }
    if (au_conn >= 0) { close(au_conn); au_conn = -1; }
    au_hdr_got = 0; au_ready = 0;
}

static int audio_tcp_listen(int port) {
    au_port = port;
    au_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (au_listen < 0) return -1;
    int one = 1;
    setsockopt(au_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(au_listen, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        listen(au_listen, 1) < 0) {
        close(au_listen); au_listen = -1; return -1;
    }
    fcntl(au_listen, F_SETFL, fcntl(au_listen, F_GETFL) | O_NONBLOCK);
    fprintf(stderr, "audio: listening on 0.0.0.0:%d for the guest capturer\n", port);
    return 0;
}

static void audio_spawn_pacat(void) {
    char rate_s[16], ch_s[16];
    snprintf(rate_s, sizeof rate_s, "%u", au_rate);
    snprintf(ch_s, sizeof ch_s, "%u", au_ch);
    int pfd[2];
    if (pipe(pfd) < 0) return;
    pid_t p = fork();
    if (p == 0) {
        dup2(pfd[0], 0);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
        close(pfd[0]); close(pfd[1]);
        for (int fd = 3; fd < 256; fd++) close(fd);
        if (!getenv("PULSE_SERVER")) {
            const char *xdg = getenv("XDG_RUNTIME_DIR");
            char ps[256];
            if (xdg && *xdg) { snprintf(ps, sizeof ps, "unix:%s/pulse/native", xdg); setenv("PULSE_SERVER", ps, 1); }
        }
        execlp("pacat", "pacat", "--format=s16le", "--rate", rate_s,
               "--channels", ch_s, "--raw", "--latency-msec=120", (char *)NULL);
        _exit(127);
    }
    close(pfd[0]);
    if (p < 0) { close(pfd[1]); return; }
    fcntl(pfd[1], F_SETFL, fcntl(pfd[1], F_GETFL) | O_NONBLOCK);
    au_pacat = pfd[1];
    fprintf(stderr, "audio: guest stream %u Hz %u ch -> pacat\n", au_rate, au_ch);
}

static void audio_tcp_poll(void) {
    if (au_listen < 0) return;
    if (au_conn < 0) {
        au_conn = accept(au_listen, NULL, NULL);
        if (au_conn >= 0) {
            fcntl(au_conn, F_SETFL, fcntl(au_conn, F_GETFL) | O_NONBLOCK);
            au_hdr_got = 0; au_ready = 0;
        }
    }
    if (au_conn >= 0 && !au_ready) {
        ssize_t n = recv(au_conn, au_hdr + au_hdr_got, (size_t)(16 - au_hdr_got), 0);
        if (n > 0) {
            au_hdr_got += (int)n;
            if (au_hdr_got == 16) {
                if (memcmp(au_hdr, "PCM1", 4) != 0) { audio_tcp_close_conn(); return; }
                au_rate = rd32(au_hdr + 4);
                au_ch   = rd32(au_hdr + 8);
                if (au_rate < 8000 || au_rate > 192000) au_rate = 44100;
                if (au_ch < 1 || au_ch > 8) au_ch = 2;
                audio_spawn_pacat();
                au_ready = 1;
            }
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            audio_tcp_close_conn();
            return;
        }
    }
    if (au_ready && au_pacat >= 0) {
        uint8_t buf[65536];
        for (int i = 0; i < 4; i++) {
            ssize_t n = recv(au_conn, buf, sizeof buf, 0);
            if (n > 0) {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(au_pacat, buf + off, (size_t)(n - off));
                    if (w > 0) off += w;
                    else break;
                }
            } else if (n == 0) { audio_tcp_close_conn(); return; }
            else break;
        }
    }
}



/* ------------------------------------------------------------------ */
/* QEMU dbus display (upstream backend, native client)                */
/*                                                                    */
/* QEMU exports the console on a private bus; we hand it one end of a */
/* socketpair via RegisterListener(h) and it opens a peer D-Bus       */
/* connection where it calls Scanout/Update on our Listener object.   */
/* Frames arrive inline as x8r8g8b8, so we blit straight into the     */
/* XImage.  Scanout carries the new size, which is what makes the     */
/* resolution follow work (VNC pins the framebuffer at connect time). */
/* ------------------------------------------------------------------ */

static GDBusConnection *qdbus_bus = NULL;
static GDBusConnection *qdbus_peer = NULL;

static const char *qemu_listener_xml =
  "<node>"
  " <interface name='org.qemu.Display1.Listener'>"
  "  <method name='Scanout'>"
  "   <arg type='u' direction='in'/><arg type='u' direction='in'/>"
  "   <arg type='u' direction='in'/><arg type='u' direction='in'/>"
  "   <arg type='ay' direction='in'/>"
  "  </method>"
  "  <method name='Update'>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='u' direction='in'/><arg type='u' direction='in'/>"
  "   <arg type='ay' direction='in'/>"
  "  </method>"
  "  <method name='Disable'/>"
  "  <method name='MouseSet'>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='i' direction='in'/>"
  "  </method>"
  "  <method name='CursorDefine'>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='ay' direction='in'/>"
  "  </method>"
  " </interface>"
  "</node>";

/* frame accounting: how much pixel data actually reaches us per second */
static long fq_frames = 0, fq_bytes = 0, fq_last = 0;

/* ---- SBD (sanway-boot-display) zero-copy backend --------------------
 * QEMU allocates the scanout in host memory and hands the client the very
 * buffer the guest writes into.  Framing: 4 bytes "SBD1" plus four fds over
 * SCM_RIGHTS -- fd0 spare framebuffer, fd1 64 KiB state block, fd2 eventfd
 * that ticks per presented frame, fd3 the dmabuf we blit from.  Nothing is
 * copied or serialised on the way; that is the whole point of this path.
 *
 * NOTE: the device lives in the vendor QEMU and the guest needs the vendor's
 * signed display driver.  Neither is distributed with this project; supply
 * both yourself if you want this backend. */
static int sbd_sock = -1;
static int sbd_fd[4] = { -1, -1, -1, -1 };
static const uint8_t *sbd_state = NULL;
static const uint8_t *sbd_fb = NULL;
static size_t sbd_fb_len = 0;
static int sbd_enabled = 0;
static int sbd_w = 0, sbd_h = 0, sbd_stride = 0;
static const char *sbd_path = NULL;

#define SBD_STATE_LEN  65536
#define SBD_OFF_VALID  0x1100
#define SBD_OFF_W      0x1108
#define SBD_OFF_H      0x110c
#define SBD_OFF_STRIDE 0x1110

static int sbd_open(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "sbd: connect %s: %s\n", path, strerror(errno));
        close(s);
        return -1;
    }
    char buf[16] = {0};
    char cbuf[CMSG_SPACE(sizeof(int) * 8)];
    struct iovec iov = { buf, sizeof buf };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    if (recvmsg(s, &mh, 0) < 0) {
        fprintf(stderr, "sbd: recvmsg: %s\n", strerror(errno));
        close(s); return -1;
    }
    if (memcmp(buf, "SBD1", 4)) {
        fprintf(stderr, "sbd: bad magic\n");
        close(s); return -1;
    }
    int nf = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c && nf < 4; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < cnt && nf < 4; i++) sbd_fd[nf++] = ((int *)CMSG_DATA(c))[i];
        }
    if (nf < 4) {
        fprintf(stderr, "sbd: got %d fds, need 4\n", nf);
        close(s); return -1;
    }
    sbd_state = mmap(NULL, SBD_STATE_LEN, PROT_READ, MAP_SHARED, sbd_fd[1], 0);
    if (sbd_state == MAP_FAILED) { sbd_state = NULL; fprintf(stderr, "sbd: state mmap failed\n"); close(s); return -1; }
    struct stat st;
    if (fstat(sbd_fd[3], &st) == 0 && st.st_size > 0) sbd_fb_len = (size_t)st.st_size;
    if (sbd_fb_len == 0) sbd_fb_len = 64u << 20;
    sbd_fb = mmap(NULL, sbd_fb_len, PROT_READ, MAP_SHARED, sbd_fd[3], 0);
    if (sbd_fb == MAP_FAILED) { sbd_fb = NULL; fprintf(stderr, "sbd: fb mmap failed\n"); close(s); return -1; }

    /* advertise the window size we can show */
    uint8_t hint[12] = { 0x02, 0x00, 'B', 'S', 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t hw = (uint32_t)win_w, hh = (uint32_t)win_h;
    memcpy(hint + 4, &hw, 4);
    memcpy(hint + 8, &hh, 4);
    (void)!write(s, hint, sizeof hint);

    (void)fcntl(sbd_fd[2], F_SETFL, fcntl(sbd_fd[2], F_GETFL, 0) | O_NONBLOCK);
    sbd_sock = s;
    fprintf(stderr, "sbd: zero-copy backend on %s (%zu MiB dmabuf)\n", path, sbd_fb_len >> 20);
    return 0;
}

/* blit the shared frame into our XImage; the buffer is already the guest's */
static void sbd_frame(void) {
    if (sbd_fd[2] >= 0) { uint64_t t = 0; (void)!read(sbd_fd[2], &t, sizeof t); }
    if (!sbd_state || !sbd_fb) return;
    if (!rd32(sbd_state + SBD_OFF_VALID)) return;
    int w = (int)rd32(sbd_state + SBD_OFF_W);
    int h = (int)rd32(sbd_state + SBD_OFF_H);
    int stride = (int)rd32(sbd_state + SBD_OFF_STRIDE);
    if (w <= 0 || h <= 0 || stride < w * 4) return;
    if ((size_t)stride * (size_t)h > sbd_fb_len) return;
    if (w != sbd_w || h != sbd_h || stride != sbd_stride) {
        sbd_w = w; sbd_h = h; sbd_stride = stride;
        fprintf(stderr, "sbd: guest mode %dx%d stride %d\n", w, h, stride);
        if (w != win_w || h != win_h) {
            win_w = w; win_h = h;
            XResizeWindow(dpy, win, (unsigned)win_w, (unsigned)win_h);
            XFlush(dpy);
            rebuild_image();
        }
    }
    int cw = w < img->width ? w : img->width;
    int ch = h < img->height ? h : img->height;
    if (cw <= 0 || ch <= 0) return;
    const uint8_t *src = sbd_fb;
    uint8_t *dst = (uint8_t *)img->data;
    for (int r = 0; r < ch; r++)
        memcpy(dst + (size_t)r * (size_t)img->bytes_per_line,
               src + (size_t)r * (size_t)stride, (size_t)cw * 4);
    fq_frames++;
    fq_bytes += (long)cw * (long)ch * 4;
    have_frame = 1;
    damage_add(0, 0, cw, ch);
    dirty = 1;
}

static void qdbus_blit(const guint8 *src, int stride, int x, int y, int w, int h) {
    fq_frames++;
    fq_bytes += (long)w * (long)h * 4;
    if (!img || !src || stride <= 0) return;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > img->width) w = img->width - x;
    if (y + h > img->height) h = img->height - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        memcpy((uint8_t *)img->data + (size_t)(y + r) * (size_t)img->bytes_per_line + (size_t)x * 4,
               src + (size_t)r * (size_t)stride, (size_t)w * 4);
    }
    have_frame = 1;
    damage_add(x, y, w, h);
    dirty = 1;
}

static void qdbus_method(GDBusConnection *c, const gchar *sender, const gchar *path,
                         const gchar *iface, const gchar *method, GVariant *params,
                         GDBusMethodInvocation *inv, gpointer user_data) {
    (void)c; (void)sender; (void)path; (void)iface; (void)user_data;
    if (!strcmp(method, "Scanout")) {
        guint32 w = 0, h = 0, stride = 0, fmt = 0;
        GVariant *data = NULL;
        g_variant_get(params, "(uuuu@ay)", &w, &h, &stride, &fmt, &data);
        if (fmt != 0x20020888u) {
            static int warned = 0;
            if (!warned) { fprintf(stderr, "dbus: unexpected pixman format 0x%x\n", fmt); warned = 1; }
        }
        if ((int)w != win_w || (int)h != win_h) {
            win_w = (int)w; win_h = (int)h;
            XResizeWindow(dpy, win, (unsigned)win_w, (unsigned)win_h);
            XFlush(dpy);
            rebuild_image();
            fprintf(stderr, "dbus: scanout %ux%u\n", w, h);
        }
        qdbus_blit((const guint8 *)g_variant_get_data(data), (int)stride, 0, 0, (int)w, (int)h);
        g_variant_unref(data);
    } else if (!strcmp(method, "Update")) {
        gint32 x = 0, y = 0, w = 0, h = 0;
        guint32 stride = 0, fmt = 0;
        GVariant *data = NULL;
        g_variant_get(params, "(iiiiuu@ay)", &x, &y, &w, &h, &stride, &fmt, &data);
        qdbus_blit((const guint8 *)g_variant_get_data(data), (int)stride, x, y, w, h);
        g_variant_unref(data);
    } else if (!strcmp(method, "Disable")) {
        seen_disable = 1;
        have_frame = 0;
        /* the console went away while the guest itself is still up: blank
         * screen, driver reset, or the first half of a shutdown.  Say so
         * instead of falling back to the "starting Windows" text. */
        if (vstate == V_RUNNING) {
            vstate = V_DISPLAY_OFF;
            ungrab_input();         /* nothing left to point at */
            fprintf(stderr, "dbus: console disabled, guest still running\n");
        }
        dirty = 1;
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable qdbus_vtable = { .method_call = qdbus_method };

static int qdbus_setup(const char *addr) {
    GError *err = NULL;
    GUnixFDList *fdl = NULL;
    GVariant *r = NULL;
    int sv[2];

    qdbus_bus = g_dbus_connection_new_for_address_sync(addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &err);
    if (!qdbus_bus) { fprintf(stderr, "dbus: bus %s\n", err->message); return -1; }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return -1; }
    fdl = g_unix_fd_list_new();
    if (g_unix_fd_list_append(fdl, sv[0], &err) < 0) {
        fprintf(stderr, "dbus: fd list %s\n", err->message); return -1;
    }
    r = g_dbus_connection_call_with_unix_fd_list_sync(qdbus_bus, "org.qemu",
            "/org/qemu/Display1/Console_0", "org.qemu.Display1.Console",
            "RegisterListener", g_variant_new("(h)", 0), G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE, 5000, fdl, NULL, NULL, &err);
    if (!r) { fprintf(stderr, "dbus: RegisterListener %s\n", err->message); return -1; }
    g_variant_unref(r);

    GSocket *sock = g_socket_new_from_fd(sv[1], &err);
    if (!sock) { fprintf(stderr, "dbus: socket %s\n", err->message); return -1; }
    GSocketConnection *sconn = g_socket_connection_factory_create_connection(sock);
    qdbus_peer = g_dbus_connection_new_sync(G_IO_STREAM(sconn), NULL,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, &err);
    if (!qdbus_peer) { fprintf(stderr, "dbus: peer %s\n", err->message); return -1; }

    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(qemu_listener_xml, &err);
    if (!info) { fprintf(stderr, "dbus: node info %s\n", err->message); return -1; }
    if (!g_dbus_connection_register_object(qdbus_peer, "/org/qemu/Display1/Listener",
            info->interfaces[0], &qdbus_vtable, NULL, NULL, &err)) {
        fprintf(stderr, "dbus: register object %s\n", err->message); return -1;
    }
    fprintf(stderr, "dbus: listener registered on %s\n", addr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* VNC display (upstream QEMU backend)                                */
/*                                                                    */
/* With the vendor display device gone, QEMU hands the virtio-gpu */
/* scanout to a normal display backend.  We speak minimal RFB 3.8     */
/* here so our own viewer is the only thing on the host that renders  */
/* the guest.  Only Raw + CopyRect + DesktopSize are needed for a     */
/* loopback connection; input still goes through QMP and clipboard /  */
/* resolution through the guest agent.                                */
/* ------------------------------------------------------------------ */

static int vnc_w = 0, vnc_h = 0;
static const char *vnc_addr = NULL;
static int vnc_desync = 0;
static int vnc_need_full = 0;

static int vnc_read(void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(vnc_fd, (char *)buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int vnc_discard(size_t n) {
    char tmp[8192];
    while (n > 0) {
        size_t chunk = n < sizeof tmp ? n : sizeof tmp;
        if (vnc_read(tmp, chunk) < 0) return -1;
        n -= chunk;
    }
    return 0;
}

static int vnc_write(const void *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(vnc_fd, (const char *)buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

static void vnc_request_update(int incremental) {
    uint8_t m[10];
    m[0] = 3;                                   /* FramebufferUpdateRequest */
    m[1] = (uint8_t)(incremental ? 1 : 0);
    m[2] = 0; m[3] = 0;                         /* x */
    m[4] = 0; m[5] = 0;                         /* y */
    m[6] = (uint8_t)(vnc_w >> 8); m[7] = (uint8_t)(vnc_w & 0xff);
    m[8] = (uint8_t)(vnc_h >> 8); m[9] = (uint8_t)(vnc_h & 0xff);
    vnc_write(m, sizeof m);
}

static int vnc_connect_to(const char *addr) {
    char host[128];
    int port = 5900;
    const char *colon = strrchr(addr, ':');
    if (colon) {
        size_t hl = (size_t)(colon - addr);
        if (hl >= sizeof host) hl = sizeof host - 1;
        memcpy(host, addr, hl);
        host[hl] = 0;
        port = atoi(colon + 1);
    } else {
        snprintf(host, sizeof host, "%s", addr);
    }
    vnc_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (vnc_fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) { close(vnc_fd); vnc_fd = -1; return -1; }
        memcpy(&sa.sin_addr, he->h_addr, sizeof sa.sin_addr);
    }
    if (connect(vnc_fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(vnc_fd); vnc_fd = -1; return -1;
    }

    char ver[12];
    if (vnc_read(ver, 12) < 0) return -1;
    if (memcmp(ver, "RFB ", 4) != 0) return -1;
    if (vnc_write("RFB 003.008\n", 12) < 0) return -1;

    uint8_t nsec = 0;
    if (vnc_read(&nsec, 1) < 0) return -1;
    if (nsec == 0) { uint32_t why; vnc_read(&why, 4); return -1; }
    uint8_t types[255];
    if (vnc_read(types, nsec) < 0) return -1;
    int have_none = 0;
    for (int i = 0; i < nsec; i++) if (types[i] == 1) have_none = 1;
    if (!have_none) { fprintf(stderr, "vnc: server wants auth (only None supported)\n"); return -1; }
    uint8_t choice = 1;
    if (vnc_write(&choice, 1) < 0) return -1;
    uint32_t res = 1;
    if (vnc_read(&res, 4) < 0) return -1;
    if (res != 0) { fprintf(stderr, "vnc: security result %u\n", res); return -1; }
    uint8_t shared = 1;
    if (vnc_write(&shared, 1) < 0) return -1;

    uint8_t si[24];
    if (vnc_read(si, 24) < 0) return -1;
    vnc_w = (si[0] << 8) | si[1];
    vnc_h = (si[2] << 8) | si[3];
    uint32_t nlen = ((uint32_t)si[20] << 24) | ((uint32_t)si[21] << 16) |
                    ((uint32_t)si[22] << 8) | si[23];
    if (nlen) vnc_discard(nlen);

    /* ask for BGRX so we can blit straight into the XImage */
    uint8_t pf[20] = { 0 };
    pf[0] = 32; pf[1] = 24; pf[2] = 0; pf[3] = 1;
    pf[4] = 0;   pf[5] = 255; pf[6] = 0;  pf[7] = 255; pf[8] = 0; pf[9] = 255;
    pf[10] = 16; pf[11] = 8; pf[12] = 0;
    /* SetPixelFormat is type 0 + 3 pad + 16 byte pixfmt */
    uint8_t m[20] = { 0 };
    memcpy(m + 4, pf, 16);
    if (vnc_write(m, 20) < 0) return -1;

    /* SetEncodings: Raw(0), CopyRect(1), DesktopSize(-223) */
    int32_t encs[4] = { 0, 1, -308, -223 };
    uint8_t se[4 + 16];
    se[0] = 2; se[1] = 0; se[2] = 0; se[3] = 4;
    memcpy(se + 4, encs, 16);
    if (vnc_write(se, sizeof se) < 0) return -1;

    fprintf(stderr, "vnc: connected %s, framebuffer %dx%d\n", addr, vnc_w, vnc_h);
    return 0;
}

static void vnc_poll(void) {
    uint8_t type;
    if (vnc_read(&type, 1) < 0) { vnc_desync = 1; return; }
    fprintf(stderr, "vnc: msg type %d\n", type);
    if (type == 0) {
        uint8_t pad;
        uint16_t nrects;
        if (vnc_read(&pad, 1) < 0 || vnc_read(&nrects, 2) < 0) { vnc_desync = 1; return; }
        nrects = (uint16_t)((nrects >> 8) | (nrects << 8));
        for (int i = 0; i < nrects; i++) {
            uint8_t rh[12];
            if (vnc_read(rh, 12) < 0) { vnc_desync = 1; return; }
            int x = (rh[0] << 8) | rh[1];
            int y = (rh[2] << 8) | rh[3];
            int w = (rh[4] << 8) | rh[5];
            int h = (rh[6] << 8) | rh[7];
            int32_t enc = (int32_t)(((uint32_t)rh[8] << 24) | ((uint32_t)rh[9] << 16) |
                                    ((uint32_t)rh[10] << 8) | rh[11]);
            if (enc == 0) {
                for (int row = 0; row < h; row++) {
                    int ty = y + row;
                    if (ty < 0 || ty >= img->height || x < 0 || x + w > img->width) {
                        if (vnc_discard((size_t)w * 4) < 0) { vnc_desync = 1; return; }
                        continue;
                    }
                    uint8_t *dst = (uint8_t *)img->data + (size_t)ty * (size_t)img->bytes_per_line + (size_t)x * 4;
                    if (vnc_read(dst, (size_t)w * 4) < 0) { vnc_desync = 1; return; }
                }
                have_frame = 1;
            } else if (enc == 1) {                        /* CopyRect */
                vnc_discard(4);
            } else if (enc == -308) {                     /* ExtendedDesktopSize */
                uint8_t ns[4];
                if (vnc_read(ns, 4) < 0) { vnc_desync = 1; return; }
                vnc_discard((size_t)ns[0] * 16);
                vnc_w = w; vnc_h = h;
                if (w > 0 && h > 0 && (w != img->width || h != img->height)) {
                    win_w = w; win_h = h;
                    XResizeWindow(dpy, win, (unsigned)win_w, (unsigned)win_h);
                    XFlush(dpy);
                    rebuild_image();
                    have_frame = 0;
                    vnc_need_full = 1;
                    fprintf(stderr, "vnc: ExtendedDesktopSize -> %dx%d\n", w, h);
                }
            } else if (enc == -223) {                     /* DesktopSize */
                vnc_w = w; vnc_h = h;
                if (w > 0 && h > 0 && (w != win_w || h != win_h)) {
                    win_w = w; win_h = h;
                    XResizeWindow(dpy, win, (unsigned)win_w, (unsigned)win_h);
                    XFlush(dpy);
                    rebuild_image();
                    have_frame = 0;
                    vnc_need_full = 1;
                    fprintf(stderr, "vnc: desktop resized to %dx%d\n", w, h);
                }
            } else {
                fprintf(stderr, "vnc: unsupported encoding %d, dropping link\n", enc);
                vnc_desync = 1;
                return;
            }
        }
        dirty = 1;
        vnc_request_update(vnc_need_full ? 0 : 1);
        vnc_need_full = 0;
    } else if (type == 1) {                                /* SetColourMapEntries */
        uint8_t hdr[5];
        if (vnc_read(hdr, 5) < 0) { vnc_desync = 1; return; }
        int n = (hdr[3] << 8) | hdr[4];
        vnc_discard((size_t)n * 6);
    } else if (type == 2) {                                /* Bell */
        /* nothing */
    } else if (type == 3) {                                /* ServerCutText */
        uint8_t hdr[7];
        if (vnc_read(hdr, 7) < 0) { vnc_desync = 1; return; }
        uint32_t len = ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[4] << 16) |
                       ((uint32_t)hdr[5] << 8) | hdr[6];
        vnc_discard(len);
    } else {
        vnc_desync = 1;
    }
}

static void vnc_close(void) {
    if (vnc_fd >= 0) { close(vnc_fd); vnc_fd = -1; }
}

/* QEMU only reports the framebuffer geometry at handshake time for our
 * minimal client, so a guest resolution change means reconnecting. */

/* ------------------------------------------------------------------ */
/* resolution follow                                                   */
/* ------------------------------------------------------------------ */

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static long pending_resize_at = 0;   /* when to push our size to the guest */
static long last_push_ms = 0;        /* last time we asked the guest to change */
static int pushed_w = 0, pushed_h = 0;

/* window resized by the user -> ask the guest to switch mode */
static void schedule_guest_resize(void) {
    pending_resize_at = now_ms() + 600;   /* debounce */
}

static void flush_pending_resize(void) {
    if (!pending_resize_at || now_ms() < pending_resize_at) return;
    pending_resize_at = 0;
    if (pushed_w == win_w && pushed_h == win_h && last_push_ms) return;
    if (agent_setres(win_w, win_h) == 0) {
        last_push_ms = now_ms();
        pushed_w = win_w;
        pushed_h = win_h;
        fprintf(stderr, "res: asked guest for %dx%d\n", win_w, win_h);
    } else {
        fprintf(stderr, "res: agent unreachable, cannot set %dx%d\n", win_w, win_h);
    }
}

/* poll the guest directly as well: the SBD fb mode occasionally lags behind
 * an actual ChangeDisplaySettings until the driver repaints */
static long guest_poll_next = 0;
static void poll_guest_mode(void) {
    if (dbus_display_enabled) return;   /* Scanout 才是权威尺寸 */
    if (vnc_enabled) return;   /* VNC 帧缓冲尺寸固定，guest 模式变化不跟随 */
    if (pending_resize_at) return;            /* user just resized: let it win */
    if (now_ms() < guest_poll_next) return;
    guest_poll_next = now_ms() + 2000;
    if (now_ms() - last_push_ms < 3000) return;
    int aw = 0, ah = 0;
    if (agent_get_guest_mode(&aw, &ah) != 0) return;
    if (aw < 320 || ah < 200) return;
    if (aw == win_w && ah == win_h) return;
    if (aw == pushed_w && ah == pushed_h) return;
    win_w = aw; win_h = ah;
    XResizeWindow(dpy, win, (unsigned)win_w, (unsigned)win_h);
    XFlush(dpy);
    rebuild_image();
    dirty = 1;
    fprintf(stderr, "res: followed guest (agent) to %dx%d\n", win_w, win_h);
}

/* guest changed mode on its own -> follow by resizing the window */

/* ------------------------------------------------------------------ */
/* clipboard sync                                                      */
/* ------------------------------------------------------------------ */

static long clip_next_poll = 0;

static void clipboard_poll(void) {
    if (now_ms() < clip_next_poll) return;
    clip_next_poll = now_ms() + 700;

    /* guest -> host */
    char g[65536];
    if (agent_clipget(g, sizeof g) == 0) {
        if (strcmp(g, clip_last_guest) != 0) {
            snprintf(clip_last_guest, sizeof clip_last_guest, "%s", g);
            if (strcmp(g, clip_last_host) != 0) {
                x11_set_clipboard(g);
                snprintf(clip_last_host, sizeof clip_last_host, "%s", g);
                fprintf(stderr, "clip: guest -> host (%zu bytes)\n", strlen(g));
            }
        }
    }

    /* host -> guest */
    char h[65536];
    if (x11_read_clipboard(h, sizeof h) == 0 && h[0]) {
        if (strcmp(h, clip_last_host) != 0) {
            snprintf(clip_last_host, sizeof clip_last_host, "%s", h);
            if (strcmp(h, clip_last_guest) != 0 && agent_clipset(h) == 0) {
                snprintf(clip_last_guest, sizeof clip_last_guest, "%s", h);
                fprintf(stderr, "clip: host -> guest (%zu bytes)\n", strlen(h));
            }
        }
    }
}

int main(int argc, char **argv) {
    qmp_path = "/tmp/qemudisplay.sock";
    const char *title = "Windows 11";
    const char *agent_addr = "127.0.0.1:8790";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--qmp") && i + 1 < argc) qmp_path = argv[++i];
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) win_w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) win_h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window-title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--splash") && i + 1 < argc) splash_path = argv[++i];
        else if (!strcmp(argv[i], "--splash-dir") && i + 1 < argc) splash_dir = argv[++i];
        else if (!strcmp(argv[i], "--agent") && i + 1 < argc) agent_addr = argv[++i];
        else if (!strcmp(argv[i], "--audio-tcp-port") && i + 1 < argc) audio_tcp_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio-tcp")) audio_tcp_wanted = 0;
        else if (!strcmp(argv[i], "--vnc") && i + 1 < argc) { vnc_addr = argv[++i]; vnc_enabled = 1; }
        else if (!strcmp(argv[i], "--dbus-display") && i + 1 < argc) { dbus_display_addr = argv[++i]; dbus_display_enabled = 1; }
        else if (!strcmp(argv[i], "--dod-sock") && i + 1 < argc) { sbd_path = argv[++i]; sbd_enabled = 1; }
    }
    {
        const char *colon = strrchr(agent_addr, ':');
        if (colon) {
            size_t hl = (size_t)(colon - agent_addr);
            if (hl >= sizeof agent_host) hl = sizeof agent_host - 1;
            memcpy(agent_host, agent_addr, hl);
            agent_host[hl] = 0;
            agent_port = atoi(colon + 1);
        }
    }
    if (dbus_display_enabled) {
        /* nothing to do before xinit: size arrives with the first Scanout */
    } else if (sbd_enabled) {
        /* the dmabuf carries the guest mode, adopted on the first frame */
    } else if (vnc_enabled) {
        /* connect first so the window is created at the guest framebuffer size */
        if (vnc_connect_to(vnc_addr) < 0) { fprintf(stderr, "vnc connect failed\n"); return 1; }
        win_w = vnc_w; win_h = vnc_h;
    } else {
        fprintf(stderr, "need --dbus-display <addr>, --dod-sock <path> or --vnc <host:port>\n");
        return 1;
    }
    xinit();
    if (dbus_display_enabled && qdbus_setup(dbus_display_addr) < 0) {
        fprintf(stderr, "dbus display setup failed\n");
        return 1;
    }
    if (sbd_enabled && sbd_open(sbd_path) < 0) {
        fprintf(stderr, "sbd setup failed\n");
        return 1;
    }
    if (vnc_enabled) vnc_request_update(0);
    XStoreName(dpy, win, title);
    render_splash();   /* paint immediately: the agent handshake below can block
                        * for seconds while Windows is still booting */
    qmp_init();
    if (audio_tcp_wanted) audio_tcp_listen(audio_tcp_port);

    /* initial handshake with the guest agent: adopt the guest mode as window size */
    if (!vnc_enabled) {
        int aw, ah;
        if (agent_get_guest_mode(&aw, &ah) == 0 && (aw != win_w || ah != win_h)) {
            win_w = aw; win_h = ah;
            XResizeWindow(dpy, win, (unsigned)win_w, (unsigned)win_h);
            XFlush(dpy);
            rebuild_image();
            fprintf(stderr, "res: initial guest mode %dx%d\n", win_w, win_h);
        }
    }
    /* A viewer that attaches to an already-running VM never receives a
     * Scanout: QEMU only emits one when the guest mode changes.  Bounce the
     * resolution by 8px once so the console sends us the current frame. */
    if (!vnc_enabled && !have_frame) {
        int aw, ah;
        if (agent_get_guest_mode(&aw, &ah) == 0 && aw == win_w && ah == win_h) {
            fprintf(stderr, "dbus: no frame yet, nudging guest mode\n");
            if (agent_setres(win_w + 8, win_h) == 0) {
                usleep(300000);
                agent_setres(win_w, win_h);
            }
        }
    }

    x11_set_clipboard("");
    clip_next_poll = now_ms() + 1200;

    int xfd = ConnectionNumber(dpy);
    for (;;) {
        struct pollfd p[4] = {
            { .fd = vnc_enabled ? vnc_fd : qmp_fd, .events = POLLIN },
            { .fd = xfd, .events = POLLIN },
            { .fd = qmp_fd, .events = POLLIN },
            { .fd = sbd_enabled ? sbd_fd[2] : -1, .events = POLLIN },
        };
        int n = poll(p, 4, 10);
        if (n < 0) break;
        while (g_main_context_iteration(NULL, FALSE)) { }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (vnc_enabled) {
                vnc_poll();
                if (vnc_desync) { fprintf(stderr, "vnc: link dropped\n"); vnc_close(); return 1; }
            }
        }
        if (p[3].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (sbd_enabled) {
                if (p[3].revents & (POLLHUP | POLLERR))
                    fprintf(stderr, "sbd: device went away\n");
                sbd_frame();
            }
        }
        if (p[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            /* POLLHUP without POLLIN is how QEMU's exit looks: the socket is
             * gone and recv() returns 0, which qmp_drain turns into qmp_dead.
             * Watching POLLIN alone left the viewer spinning on a dead guest
             * with the grab still held. */
            qmp_drain();
        }

        while (XPending(dpy)) {
            XEvent e; XNextEvent(dpy, &e);
            switch (e.type) {
            case Expose:
                if (e.xexpose.count == 0) { damage_all(); dirty = 1; }
                break;
            case DestroyNotify:
                if (e.xdestroywindow.window == win) {
                    fprintf(stderr, "x11: window destroyed, exiting\n");
                    return 0;
                }
                break;
            case ClientMessage:
                fprintf(stderr, "x11: client message %ld (delete=%lu)\n",
                        e.xclient.data.l[0], (unsigned long)wm_delete_atom);
                if ((Atom)e.xclient.data.l[0] == wm_delete_atom) {
                    if (vstate == V_OFF || qmp_dead) {               /* already off */
                        fprintf(stderr, "close: guest already off, exiting\n");
                        return 0;
                    }
                    if (dlg == DLG_NONE) open_close_dialog();
                }
                break;
            case ConfigureNotify:
                if (e.xconfigure.width != win_w || e.xconfigure.height != win_h) {
                    win_w = e.xconfigure.width; win_h = e.xconfigure.height;
                    if (win_w < 64) win_w = 64;
                    if (win_h < 64) win_h = 64;
                    if (dbus_display_enabled) {
                        /* 窗口尺寸由 Scanout 决定；用户拉伸时请 guest 跟随 */
                        schedule_guest_resize();
                    } else if (vnc_enabled) {
                        schedule_guest_resize();   /* 由 guest 改模式，桌面尺寸以 VNC 为准 */
                    } else {
                        rebuild_image();
                        dirty = 1;
                        schedule_guest_resize();
                    }
                }
                break;
            case MotionNotify: {
                XMotionEvent *m = &e.xmotion;
                if (dlg != DLG_NONE) {
                    int h = hit_button(m->x, m->y);
                    if (h != dlg_hover) { dlg_hover = h; dlg_dirty = 1; dirty = 1; }
                    break;
                }
                if (vstate == V_SUSPENDED || vstate == V_DISPLAY_OFF) break;
                qmp_mouse_abs(m->x, m->y);
                break;
            }
            case ButtonPress:
            case ButtonRelease: {
                XButtonEvent *b = &e.xbutton;
                int down = (e.type == ButtonPress);
                if (dlg != DLG_NONE) {
                    if (down) {
                        fprintf(stderr, "input: dialog click at %d,%d btn=%d hit=%d\n",
                                b->x, b->y, b->button, hit_button(b->x, b->y));
                        dialog_click(b->x, b->y, b->button);
                    }
                    break;
                }
                if (down && (vstate == V_SUSPENDED || vstate == V_DISPLAY_OFF)) {
                    /* a click is what brings the desktop back: ask for a wakeup
                     * and deliver the press too, since a blanked console only
                     * notices real input */
                    qmp_send_json("{\"execute\":\"system_wakeup\",\"id\":9002}");
                    qmp_drain();
                    qmp_mouse_abs(b->x, b->y);
                    qmp_button("left", 1);
                    qmp_button("left", 0);
                    break;
                }
                if (down && b->button == 1 && !grabbed) grab_input();
                if (b->button == 4 || b->button == 5) {
                    if (down) { qmp_button(b->button == 4 ? "wheel-up" : "wheel-down", 1);
                                qmp_button(b->button == 4 ? "wheel-up" : "wheel-down", 0); }
                } else if (b->button == 1 || b->button == 2 || b->button == 3) {
                    const char *name = b->button == 1 ? "left" : b->button == 2 ? "middle" : "right";
                    qmp_button(name, down);
                }
                break;
            }
            case KeyPress:
            case KeyRelease: {
                XKeyEvent *k = &e.xkey;
                KeySym ks = XLookupKeysym(k, 0);
                if (dlg != DLG_NONE) {
                    if (e.type == KeyPress && ks == XK_Escape) {
                        dlg = DLG_NONE; dlg_dirty = 1; dirty = 1; damage_all(); hide_local_cursor();
                    }
                    break;
                }
                if (grabbed && (k->state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask)) {
                    ungrab_input();
                    break;
                }
                const char *q = keysym_to_qcode(ks);
                if (q) qmp_key(q, e.type == KeyPress);
                break;
            }
            }
        }

        if (!guest_painted && now_ms() >= splash_next_ms) {
            splash_next_ms = now_ms() + 350;
            splash_phase = (splash_phase + 1) % 3;
            dirty = 1;
        }
        if (dirty) {
            if (dbus_display_enabled) {
                if (have_frame) { guest_painted = 1; render(); }
                else render_splash();
            } else if (sbd_enabled) {
                if (have_frame) { guest_painted = 1; render(); }
                else render_splash();
            } else if (vnc_enabled) {
                if (have_frame) {
                    guest_painted = 1;
                    if (vstate == V_BOOTING) vstate = V_RUNNING;
                    render();
                }
                else render_splash();
            }
        }

        flush_pending_resize();
        poll_guest_mode();
        clipboard_poll();
        audio_tcp_poll();
        power_tick();
        power_liveness_tick();
        qmp_status_tick();
        if (now_ms() - fq_last >= 5000) {
            static long last_frames = 0, last_bytes = 0;
            long el = now_ms() - (fq_last ? fq_last : now_ms());
            fprintf(stderr, "perf: %.1f fps, %.1f MB/s (%ld frames, %ld bytes)\n",
                    el ? (double)(fq_frames - last_frames) * 1000.0 / (double)el : 0.0,
                    el ? (double)(fq_bytes - last_bytes) / 1048576.0 * 1000.0 / (double)el : 0.0,
                    fq_frames, fq_bytes);
            last_frames = fq_frames; last_bytes = fq_bytes; fq_last = now_ms();
        }

        /* a suspend/shutdown/blanked console leaves the last guest frame in
         * place; repaint the splash instead so the state is obvious */
        if (vstate == V_SUSPENDED || vstate == V_SHUTTING || vstate == V_OFF ||
            (vstate == V_DISPLAY_OFF && dlg == DLG_NONE)) {
            if (now_ms() >= splash_next_ms) {
                splash_next_ms = now_ms() + 350;
                splash_phase = (splash_phase + 1) % 3;
                render_splash();
            }
        }
        if (qmp_dead) {
            /* QEMU is gone: the guest powered off (or was killed).  Release the
             * grab and show the closed state briefly before the window goes,
             * rather than leaving a grabbed, frozen frame behind. */
            fprintf(stderr, "qmp: connection closed, guest is gone\n");
            ungrab_input();
            if (vstate != V_OFF) {
                vstate = V_OFF;
                dirty = 1;
                render_splash();
            }
            return 0;
        }
    }
    return 0;
}
