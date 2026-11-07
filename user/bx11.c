/* bx11: shows an X11 screen in a bwm window and sends the window's keys and
 * pointer events back to the X server, so ordinary X clients (xterm and
 * whatever else Alpine ships) run on the desktop.
 *
 *     bx11 [-f FILE] [-s SOCKET] [-t TITLE]
 *
 * The X server is Xvfb, started with `-fbdir DIR`: it keeps the screen in
 * the memory-mapped file DIR/Xvfb_screen0 (an XWD file: a header, then the
 * pixels). FILE names it (default /tmp/.bx11/Xvfb_screen0, for Xvfb run in
 * a chroot below /tmp/.bx11 is the caller's choice). SOCKET is the X
 * server's UNIX socket (default /tmp/.X11-unix/X0). Input goes in through
 * the XTEST extension, so the X server needs no input devices.
 *
 * The clipboard is shared both ways: the text in /tmp/bwm.clip (what a bterm
 * window selected, or the editor cut) becomes the X CLIPBOARD and PRIMARY
 * selections, and a selection an X client takes (a copy in the X program)
 * goes into /tmp/bwm.clip. It needs the XFIXES extension to hear of that.
 *
 * Keyboard focus follows the windows: the X server has no window manager, so
 * bx11 gives the focus to each top-level window that is mapped (a dialog
 * takes the keys when it opens) and back to the one below when it goes, and
 * a click on a window focuses it. Menus (override-redirect windows) are left
 * alone.
 *
 * A bwm window cannot be resized for it: the X screen has one size. The
 * pixels are copied 20 times a second, only the rows that changed. */
#include "bwmlib.h"

#include <poll.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

static int xfd = -1;
static unsigned xseq;
static uint32_t xroot, xrid_base;
static int xtest, min_kc, max_kc, kpk;
static uint32_t *keymap;            /* kpk keysyms per keycode, from min_kc */

static int xwrite(const void *p, size_t n)
{
    return bwm_write_all(xfd, p, n);
}

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Reads the reply to request number SEQ (events and errors in front of it
 * are dropped). Returns the reply, 32 bytes and what follows, or NULL. */
static void handle_event(const unsigned char *e);

static unsigned char *xreply(unsigned seq)
{
    for (;;) {
        unsigned char h[32];
        if (bwm_read_all(xfd, h, 32) != 0)
            return NULL;
        if (h[0] == 1) {
            size_t extra = (size_t)rd32(h + 4) * 4;
            unsigned char *r = malloc(32 + extra);
            if (!r)
                return NULL;
            memcpy(r, h, 32);
            if (extra && bwm_read_all(xfd, r + 32, extra) != 0) {
                free(r);
                return NULL;
            }
            if (rd16(h + 2) == (seq & 0xffff))
                return r;
            free(r);
        } else if (h[0] == 0 && rd16(h + 2) == (seq & 0xffff)) {
            return NULL;                 /* an error for the request we wait on */
        } else if (h[0] >= 2) {
            handle_event(h);
        }
    }
}

static int xconnect(const char *path)
{
    xfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (xfd < 0 || connect(xfd, (struct sockaddr *)&a, sizeof a) != 0)
        return -1;
    unsigned char req[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (xwrite(req, sizeof req) != 0)
        return -1;
    unsigned char h[8];
    if (bwm_read_all(xfd, h, 8) != 0 || h[0] != 1)
        return -1;
    size_t n = (size_t)rd16(h + 6) * 4;
    unsigned char *b = malloc(n);
    if (!b || bwm_read_all(xfd, b, n) != 0)
        return -1;
    unsigned vlen = rd16(b + 16), formats = b[21];
    min_kc = b[26];
    max_kc = b[27];
    size_t scr = 32 + ((vlen + 3) & ~3u) + (size_t)formats * 8;
    if (scr + 4 > n)
        return -1;
    xroot = rd32(b + scr);
    xrid_base = rd32(b + 4);
    free(b);
    return 0;
}

static int xquery_extension(const char *name)
{
    unsigned n = (unsigned)strlen(name), words = 2 + ((n + 3) >> 2);
    unsigned char *q = calloc(1, words * 4);
    q[0] = 98;
    q[2] = words & 0xff;
    q[3] = words >> 8;
    q[4] = n & 0xff;
    memcpy(q + 8, name, n);
    xseq++;
    if (xwrite(q, words * 4) != 0)
        return -1;
    free(q);
    unsigned char *r = xreply(xseq);
    if (!r)
        return -1;
    int op = r[8] ? r[9] : -1;
    free(r);
    return op;
}

static int xload_keymap(void)
{
    int count = max_kc - min_kc + 1;
    unsigned char q[8] = { 101, 0, 2, 0, (unsigned char)min_kc, (unsigned char)count, 0, 0 };
    xseq++;
    if (xwrite(q, sizeof q) != 0)
        return -1;
    unsigned char *r = xreply(xseq);
    if (!r)
        return -1;
    kpk = r[1];
    keymap = calloc((size_t)count * (kpk ? kpk : 1), 4);
    for (int i = 0; i < count * kpk; i++)
        keymap[i] = rd32(r + 32 + i * 4);
    free(r);
    return kpk > 0 ? 0 : -1;
}


/* ---- the clipboard ---------------------------------------------------- */

#define CLIP_FILE "/tmp/bwm.clip"
#define CLIP_MAX 60000
#define A_PRIMARY 1
#define A_ATOM 4
#define A_STRING 31
#define NSEL 2                           /* CLIPBOARD and PRIMARY */

static uint32_t cwin, a_clipboard, a_utf8, a_targets, a_prop;
static uint32_t sel_atom[NSEL];
static int owned[NSEL];                  /* bx11 owns the selection */
static int xfixes_ev = -1;               /* the first event number of XFIXES */
static int conv_want[NSEL];              /* a new owner appeared: fetch its text */
static int conv_sel = -1, conv_try;      /* the conversion in flight, and 0 UTF8_STRING or 1 STRING */
static int conv_sent;                    /* its ConvertSelection went out */
static int got_prop;                     /* its SelectionNotify named a property */
static long clip_mtime_s, clip_mtime_ns, clip_size = -1;

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8 & 0xff;
    p[2] = v >> 16 & 0xff;
    p[3] = v >> 24;
}

static uint32_t intern(const char *name)
{
    unsigned n = (unsigned)strlen(name), words = 2 + ((n + 3) >> 2);
    unsigned char *q = calloc(1, words * 4);
    q[0] = 16;
    q[2] = words & 0xff;
    q[3] = words >> 8;
    q[4] = n & 0xff;
    memcpy(q + 8, name, n);
    xseq++;
    int bad = xwrite(q, words * 4);
    free(q);
    unsigned char *r = bad ? NULL : xreply(xseq);
    if (!r)
        return 0;
    uint32_t a = rd32(r + 8);
    free(r);
    return a;
}

static int sel_index(uint32_t atom)
{
    for (int i = 0; i < NSEL; i++)
        if (sel_atom[i] == atom)
            return i;
    return -1;
}

static int clip_read(unsigned char *buf, int cap)
{
    int f = open(CLIP_FILE, O_RDONLY);
    if (f < 0)
        return 0;
    int n = (int)read(f, buf, (size_t)cap);
    close(f);
    return n < 0 ? 0 : n;
}

static void clip_stat(void)
{
    struct stat st;
    if (stat(CLIP_FILE, &st) != 0) {
        clip_size = -1;
        return;
    }
    clip_mtime_s = (long)st.st_mtim.tv_sec;
    clip_mtime_ns = (long)st.st_mtim.tv_nsec;
    clip_size = (long)st.st_size;
}

static void change_property(uint32_t win, uint32_t prop, uint32_t type, int format, const void *data, unsigned n)
{
    unsigned bytes = n * (unsigned)(format / 8), words = 6 + ((bytes + 3) >> 2);
    unsigned char *q = calloc(1, words * 4);
    if (!q)
        return;
    q[0] = 18;
    q[2] = words & 0xff;
    q[3] = words >> 8;
    put32(q + 4, win);
    put32(q + 8, prop);
    put32(q + 12, type);
    q[16] = (unsigned char)format;
    put32(q + 20, n);
    memcpy(q + 24, data, bytes);
    xseq++;
    xwrite(q, words * 4);
    free(q);
}

static void send_notify(uint32_t requestor, uint32_t selection, uint32_t target, uint32_t property, uint32_t time)
{
    unsigned char q[44];
    memset(q, 0, sizeof q);
    q[0] = 25;
    q[2] = 11;
    put32(q + 4, requestor);
    q[12] = 31;                          /* SelectionNotify */
    put32(q + 16, time);
    put32(q + 20, requestor);
    put32(q + 24, selection);
    put32(q + 28, target);
    put32(q + 32, property);
    xseq++;
    xwrite(q, sizeof q);
}

static void own_selections(void)
{
    for (int i = 0; i < NSEL; i++) {
        unsigned char q[16];
        memset(q, 0, sizeof q);
        q[0] = 22;
        q[2] = 4;
        put32(q + 4, cwin);
        put32(q + 8, sel_atom[i]);
        xseq++;
        xwrite(q, sizeof q);
        owned[i] = 1;
    }
}

static void selection_request(const unsigned char *e)
{
    uint32_t time = rd32(e + 4), req = rd32(e + 12), sel = rd32(e + 16), target = rd32(e + 20), prop = rd32(e + 24);
    if (!prop)
        prop = target;
    if (sel_index(sel) < 0 || !owned[sel_index(sel)]) {
        send_notify(req, sel, target, 0, time);
    } else if (target == a_targets) {
        uint32_t t[3] = { a_targets, a_utf8, A_STRING };
        unsigned char b[12];
        for (int i = 0; i < 3; i++)
            put32(b + i * 4, t[i]);
        change_property(req, prop, A_ATOM, 32, b, 3);
        send_notify(req, sel, target, prop, time);
    } else if (target == a_utf8 || target == A_STRING) {
        static unsigned char buf[CLIP_MAX];
        int n = clip_read(buf, sizeof buf);
        change_property(req, prop, target, 8, buf, (unsigned)n);
        send_notify(req, sel, target, prop, time);
    } else {
        send_notify(req, sel, target, 0, time);
    }
}

/* ---- keyboard focus ---- */

#define MAXTOP 32
static uint32_t tops[MAXTOP];            /* the mapped top-level windows, the focused one last */
static int ntop;

static void set_focus(uint32_t w)        /* w 1 is PointerRoot */
{
    unsigned char q[12];
    memset(q, 0, sizeof q);
    q[0] = 42;                           /* SetInputFocus, revert to PointerRoot */
    q[1] = 1;
    q[2] = 3;
    put32(q + 4, w);
    xseq++;
    xwrite(q, sizeof q);
}

static void top_remove(uint32_t w)
{
    for (int i = 0; i < ntop; i++)
        if (tops[i] == w) {
            memmove(&tops[i], &tops[i + 1], (size_t)(ntop - i - 1) * sizeof tops[0]);
            ntop--;
            if (i == ntop)               /* it had the focus: the one below takes it */
                set_focus(ntop ? tops[ntop - 1] : 1);
            return;
        }
}

static void top_focus(uint32_t w)        /* w is mapped: it comes to the top and takes the focus */
{
    if (ntop && tops[ntop - 1] == w)
        return;
    for (int i = 0; i < ntop; i++)
        if (tops[i] == w) {
            memmove(&tops[i], &tops[i + 1], (size_t)(ntop - i - 1) * sizeof tops[0]);
            ntop--;
            break;
        }
    if (ntop == MAXTOP)
        memmove(tops, tops + 1, (size_t)(--ntop) * sizeof tops[0]);
    tops[ntop++] = w;
    set_focus(w);
}

/* SubstructureNotify on the root: the top-level windows that map and unmap. */
static void focus_init(void)
{
    unsigned char q[16];
    memset(q, 0, sizeof q);
    q[0] = 2;                            /* ChangeWindowAttributes: event-mask */
    q[2] = 4;
    put32(q + 4, xroot);
    put32(q + 8, 0x800);
    put32(q + 12, 1u << 19);
    xseq++;
    xwrite(q, sizeof q);
    /* Windows that were mapped before bx11 started are not announced: ask for them (QueryTree lists the children bottom to
     * top) and, in that order, take the viewable ones that are not menus; the topmost ends up focused. */
    memset(q, 0, sizeof q);
    q[0] = 15;                           /* QueryTree */
    q[2] = 2;
    put32(q + 4, xroot);
    xseq++;
    if (xwrite(q, 8) != 0)
        return;
    unsigned char *r = xreply(xseq);
    if (!r)
        return;
    unsigned n = rd16(r + 16);
    for (unsigned i = 0; i < n && i < 256; i++) {
        uint32_t w = rd32(r + 32 + 4 * i);
        unsigned char g[8];
        memset(g, 0, sizeof g);
        g[0] = 3;                        /* GetWindowAttributes */
        g[2] = 2;
        put32(g + 4, w);
        xseq++;
        if (xwrite(g, sizeof g) != 0)
            break;
        unsigned char *a = xreply(xseq);
        if (!a)
            continue;
        int viewable = a[26] == 2, override = a[27];
        free(a);
        if (viewable && !override)
            top_focus(w);
    }
    free(r);
}

/* The top-level window under the pointer, or 0. */
static uint32_t window_under_pointer(void)
{
    unsigned char q[8];
    memset(q, 0, sizeof q);
    q[0] = 38;                           /* QueryPointer */
    q[2] = 2;
    put32(q + 4, xroot);
    xseq++;
    if (xwrite(q, sizeof q) != 0)
        return 0;
    unsigned char *r = xreply(xseq);
    if (!r)
        return 0;
    uint32_t child = rd32(r + 12);
    free(r);
    return child;
}

static void handle_event(const unsigned char *e)
{
    int code = e[0] & 0x7f;
    if (code == 19) {                                /* MapNotify */
        if (!e[12] && rd32(e + 8) != cwin)           /* not override-redirect: a window, a dialog */
            top_focus(rd32(e + 8));
    } else if (code == 18 || code == 17) {           /* UnmapNotify, DestroyNotify */
        top_remove(rd32(e + 8));
    } else if (code == 30) {
        selection_request(e);
    } else if (code == 29) {                         /* SelectionClear: somebody else took it */
        int i = sel_index(rd32(e + 12));
        if (i >= 0)
            owned[i] = 0;
    } else if (code == 31) {                         /* SelectionNotify: the answer to our conversion */
        if (conv_sel < 0 || sel_index(rd32(e + 12)) != conv_sel)
            return;
        if (rd32(e + 20) != 0) {
            got_prop = 1;
        } else if (conv_try == 0) {
            conv_try = 1;                            /* ask again for plain STRING */
            conv_sent = 0;
        } else {
            conv_sel = -1;
        }
    } else if (xfixes_ev >= 0 && code == xfixes_ev) {      /* XFixesSelectionNotify */
        uint32_t owner = rd32(e + 8);
        int i = sel_index(rd32(e + 12));
        if (i >= 0 && owner != cwin && owner != 0) {
            owned[i] = 0;
            conv_want[i] = 1;
        }
    }
}

/* Reads every event that waits. */
static int xdrain(void)
{
    for (;;) {
        struct pollfd p = { xfd, POLLIN, 0 };
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN))
            return 0;
        unsigned char h[32];
        if (bwm_read_all(xfd, h, 32) != 0)
            return -1;
        if (h[0] == 1 || (h[0] & 0x7f) == 35) {      /* a stray reply or a generic event: skip its extra words */
            size_t extra = (size_t)rd32(h + 4) * 4;
            unsigned char skip[256];
            while (extra) {
                size_t k = extra < sizeof skip ? extra : sizeof skip;
                if (bwm_read_all(xfd, skip, k) != 0)
                    return -1;
                extra -= k;
            }
        } else if (h[0] >= 2) {
            handle_event(h);
        }
    }
}


/* A window of our own to own selections with, the atoms, and XFIXES, which tells us when a client takes a selection. */
static int clip_init(void)
{
    cwin = xrid_base | 1;
    unsigned char q[40];
    memset(q, 0, sizeof q);
    q[0] = 1;                                        /* CreateWindow: an InputOnly child of the root */
    q[2] = 8;
    put32(q + 4, cwin);
    put32(q + 8, xroot);
    q[16] = 1;                                       /* 1 x 1 */
    q[18] = 1;
    q[22] = 2;                                       /* class InputOnly (depth 0, visual CopyFromParent) */
    xseq++;
    if (xwrite(q, 32) != 0)
        return -1;
    a_clipboard = intern("CLIPBOARD");
    a_utf8 = intern("UTF8_STRING");
    a_targets = intern("TARGETS");
    a_prop = intern("BX11_SELECTION");
    sel_atom[0] = a_clipboard;
    sel_atom[1] = A_PRIMARY;
    if (!a_clipboard || !a_utf8 || !a_targets || !a_prop)
        return -1;
    const char *name = "XFIXES";
    unsigned char qe[16];
    memset(qe, 0, sizeof qe);
    qe[0] = 98;
    qe[2] = 4;
    qe[4] = (unsigned char)strlen(name);
    memcpy(qe + 8, name, strlen(name));
    xseq++;
    if (xwrite(qe, sizeof qe) != 0)
        return -1;
    unsigned char *r = xreply(xseq);
    if (!r || !r[8]) {
        free(r);
        return -1;
    }
    int op = r[9];
    xfixes_ev = r[10];
    free(r);
    unsigned char qv[12] = { (unsigned char)op, 0, 3, 0, 5, 0, 0, 0, 0, 0, 0, 0 };    /* QueryVersion 5.0 */
    xseq++;
    if (xwrite(qv, sizeof qv) != 0)
        return -1;
    r = xreply(xseq);
    if (!r)
        return -1;
    free(r);
    for (int i = 0; i < NSEL; i++) {                 /* SelectSelectionInput: owner changes */
        unsigned char qs[16];
        memset(qs, 0, sizeof qs);
        qs[0] = (unsigned char)op;
        qs[1] = 2;
        qs[2] = 4;
        put32(qs + 4, cwin);
        put32(qs + 8, sel_atom[i]);
        put32(qs + 12, 1);
        xseq++;
        if (xwrite(qs, sizeof qs) != 0)
            return -1;
    }
    clip_stat();                                     /* what is in the file now is not news */
    return 0;
}

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The clipboard's part of one turn of the main loop. */
static void clip_step(void)
{
    static long stat_at, conv_at;
    long now = mono_ms();
    if (now - stat_at >= 500 || now < stat_at) {     /* the file changed under us: the text is ours to offer */
        stat_at = now;
        long s0 = clip_mtime_s, n0 = clip_mtime_ns, z0 = clip_size;
        clip_stat();
        if (clip_size > 0 && (s0 != clip_mtime_s || n0 != clip_mtime_ns || z0 != clip_size))
            own_selections();
    }
    if (conv_sel >= 0 && got_prop) {                 /* fetch the property the owner filled */
        unsigned char q[24];
        memset(q, 0, sizeof q);
        q[0] = 20;
        q[1] = 1;                                    /* delete it */
        q[2] = 6;
        put32(q + 4, cwin);
        put32(q + 8, a_prop);
        put32(q + 20, CLIP_MAX / 4);
        xseq++;
        xwrite(q, sizeof q);
        unsigned char *r = xreply(xseq);
        if (r) {
            unsigned len = rd32(r + 16);
            if (r[1] == 8 && len > 0 && len <= CLIP_MAX) {
                int f = open(CLIP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (f >= 0) {
                    if (write(f, r + 32, len) < 0)
                        len = 0;
                    close(f);
                    clip_stat();                     /* our own write: do not offer it back */
                    printf("bx11: clipboard from X, %u bytes\n", len);
                    fflush(stdout);
                }
            }
            free(r);
        }
        got_prop = 0;
        conv_sel = -1;
    }
    if (conv_sel >= 0 && now - conv_at > 3000)       /* the owner never answered */
        conv_sel = -1;
    if (conv_sel < 0)
        for (int i = 0; i < NSEL; i++)
            if (conv_want[i]) {
                conv_want[i] = 0;
                conv_sel = i;
                conv_try = 0;
                conv_sent = 0;
                got_prop = 0;
                break;
            }
    if (conv_sel >= 0 && !conv_sent) {               /* ask the owner for UTF8_STRING, then STRING */
        unsigned char q[24];
        memset(q, 0, sizeof q);
        q[0] = 24;
        q[2] = 6;
        put32(q + 4, cwin);
        put32(q + 8, sel_atom[conv_sel]);
        put32(q + 12, conv_try ? A_STRING : a_utf8);
        put32(q + 16, a_prop);
        xseq++;
        xwrite(q, sizeof q);
        conv_sent = 1;
        conv_at = now;
    }
}

/* XTEST FakeInput: TYPE 2 key press, 3 key release, 4 button press, 5
 * button release, 6 motion (absolute). */
static void fake(int type, int detail, int x, int y)
{
    unsigned char q[36];
    memset(q, 0, sizeof q);
    q[0] = (unsigned char)xtest;
    q[1] = 2;
    q[2] = 9;
    q[4] = (unsigned char)type;
    q[5] = (unsigned char)detail;
    q[12] = xroot & 0xff;
    q[13] = xroot >> 8 & 0xff;
    q[14] = xroot >> 16 & 0xff;
    q[15] = xroot >> 24;
    q[24] = x & 0xff;
    q[25] = x >> 8 & 0xff;
    q[26] = y & 0xff;
    q[27] = y >> 8 & 0xff;
    xseq++;
    xwrite(q, sizeof q);
}

/* The keycode whose first (unshifted) or second keysym is SYM; *SHIFT says which. */
static int keycode_of(uint32_t sym, int *shift)
{
    for (int k = 0; k <= max_kc - min_kc; k++)
        for (int c = 0; c < 2 && c < kpk; c++)
            if (keymap[k * kpk + c] == sym) {
                *shift = c;
                return k + min_kc;
            }
    return 0;
}

static void tap(uint32_t sym, uint32_t modsym)
{
    int shift = 0, mod = 0, msh = 0;
    int kc = keycode_of(sym, &shift);
    if (!kc)
        return;
    if (modsym)
        mod = keycode_of(modsym, &msh);
    int sh = shift ? keycode_of(0xffe1, &msh) : 0;
    if (mod)
        fake(2, mod, 0, 0);
    if (sh)
        fake(2, sh, 0, 0);
    fake(2, kc, 0, 0);
    fake(3, kc, 0, 0);
    if (sh)
        fake(3, sh, 0, 0);
    if (mod)
        fake(3, mod, 0, 0);
}

/* A byte from bwm. VT escape sequences for the cursor keys arrive as
 * bytes; a lone ESC waits (esc_pending) until the next byte or the next
 * tick says whether it starts one. */
static int esc_state;               /* 0 none, 1 after ESC, 2 after ESC [ or ESC O */
static int esc_num;

static void key_byte(unsigned c)
{
    if (esc_state == 1) {
        if (c == '[' || c == 'O') {
            esc_state = 2;
            esc_num = 0;
            return;
        }
        esc_state = 0;
        tap(0xff1b, 0);
    } else if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            esc_num = esc_num * 10 + (int)(c - '0');
            return;
        }
        if (c == ';')
            return;
        esc_state = 0;
        uint32_t sym = 0;
        switch (c) {
        case 'A': sym = 0xff52; break;
        case 'B': sym = 0xff54; break;
        case 'C': sym = 0xff53; break;
        case 'D': sym = 0xff51; break;
        case 'H': sym = 0xff50; break;
        case 'F': sym = 0xff57; break;
        case 'Z': tap(0xff09, 0xffe1); return;
        case '~':
            sym = esc_num == 1 ? 0xff50 : esc_num == 2 ? 0xff63 : esc_num == 3 ? 0xffff : esc_num == 4 ? 0xff57
                : esc_num == 5 ? 0xff55 : esc_num == 6 ? 0xff56 : 0;
            break;
        }
        if (sym)
            tap(sym, 0);
        return;
    }
    if (c == 27) {
        esc_state = 1;
        return;
    }
    if (c == 8 || c == 127)
        tap(0xff08, 0);
    else if (c == 9)
        tap(0xff09, 0);
    else if (c == 10 || c == 13)
        tap(0xff0d, 0);
    else if (c >= 1 && c <= 26)
        tap('a' + c - 1, 0xffe3);
    else if (c >= 32 && c < 127)
        tap(c, 0);
}

static uint32_t last_buttons;

static void mouse(const struct bwm_mouse *m)
{
    int x = (int)m->x, y = (int)m->y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    fake(6, 0, x, y);
    static const int xb[3] = { 1, 3, 2 };            /* bwm: left, right, middle */
    uint32_t now = m->buttons & 7, was = last_buttons & 7;
    if (now & ~was) {                                /* a press focuses the window it is on */
        uint32_t under = window_under_pointer();
        for (int i = 0; under && i < ntop; i++)
            if (tops[i] == under) {
                top_focus(under);
                break;
            }
    }
    for (int i = 0; i < 3; i++) {
        if ((now ^ was) & (1u << i))
            fake((now >> i & 1) ? 4 : 5, xb[i], x, y);
    }
    last_buttons = now;
    int wheel = (signed char)(m->buttons >> 8 & 0xff);
    for (int i = 0; i < (wheel < 0 ? -wheel : wheel); i++) {
        fake(4, wheel < 0 ? 4 : 5, x, y);
        fake(5, wheel < 0 ? 4 : 5, x, y);
    }
}

int main(int argc, char **argv)
{
    const char *file = "/tmp/.bx11/Xvfb_screen0", *sock = "/tmp/.X11-unix/X0", *title = "X11";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "-f") == 0) file = argv[i + 1];
        else if (strcmp(argv[i], "-s") == 0) sock = argv[i + 1];
        else if (strcmp(argv[i], "-t") == 0) title = argv[i + 1];
    }
    if (xconnect(sock) != 0) {
        puts("bx11: cannot connect to the X server");
        return 1;
    }
    xtest = xquery_extension("XTEST");
    if (xtest < 0 || xload_keymap() != 0) {
        puts("bx11: the X server has no XTEST extension or no keyboard map");
        return 1;
    }
    focus_init();
    if (clip_init() != 0)
        puts("bx11: no clipboard sharing (the X server has no XFIXES extension)");
    /* Opened for writing, so that the mapping shares the pages the X server
     * writes (BastiOS gives a read-only descriptor a copy that stays stale). */
    int ffd = open(file, O_RDWR);
    if (ffd < 0)
        ffd = open(file, O_RDONLY);
    unsigned char hdr[256];
    if (ffd < 0 || pread(ffd, hdr, sizeof hdr, 0) < 100) {
        puts("bx11: cannot read the screen file");
        return 1;
    }
    uint32_t hsize = be32(hdr), w = be32(hdr + 16), h = be32(hdr + 20), bpp = be32(hdr + 44), bpl = be32(hdr + 48),
             ncol = be32(hdr + 76);
    if (bpp != 32 || bpl < w * 4 || w > 4096 || h > 4096) {
        puts("bx11: the screen must be 24 bit deep (Xvfb -screen 0 WxHx24)");
        return 1;
    }
    size_t off = hsize + (size_t)ncol * 12, len = off + (size_t)bpl * h;
    unsigned char *scr = mmap(NULL, len, PROT_READ, MAP_SHARED, ffd, 0);
    if (scr == MAP_FAILED) {
        puts("bx11: cannot map the screen file");
        return 1;
    }
    scr += off;
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bx11: no window server");
        return 2;
    }
    uint32_t *pix = NULL;
    int id = bwm_create_shm(fd, w, h, title, &pix);
    if (id < 0) {
        puts("bx11: cannot open a window");
        return 1;
    }
    printf("bx11: window %d, screen %ux%u\n", id, w, h);
    fflush(stdout);
    static unsigned char buf[BWM_MAX_PAYLOAD];
    int first = 1;
    for (;;) {
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { xfd, POLLIN, 0 } };
        poll(p, 2, 50);
        if (p[1].revents & (POLLHUP | POLLERR)) {
            puts("bx11: the X server ended");
            return 0;
        }
        if (p[1].revents & POLLIN) {
            if (xdrain() != 0) {
                puts("bx11: the X server ended");
                return 0;
            }
        }
        if (xfixes_ev >= 0)
            clip_step();
        while (p[0].revents & POLLIN) {
            int t = bwm_event(fd, buf);
            if (t < 0)
                return 0;
            if (t == BWM_KEY) {
                key_byte(((struct bwm_key *)buf)->ch & 0xff);
            } else if (t == BWM_MOUSE) {
                mouse((struct bwm_mouse *)buf);
            } else if (t == BWM_CLOSE) {
                bwm_send(fd, BWM_DESTROY, &(struct bwm_destroy){ (uint32_t)id }, sizeof(struct bwm_destroy));
                return 0;
            }
            struct pollfd q = { fd, POLLIN, 0 };
            if (poll(&q, 1, 0) <= 0 || !(q.revents & POLLIN))
                break;
        }
        if (esc_state == 1 && !(p[0].revents & POLLIN)) {        /* a lone ESC */
            esc_state = 0;
            tap(0xff1b, 0);
        }
        /* copy the rows that changed */
        int y0 = -1, y1 = 0;
        for (uint32_t y = 0; y < h; y++) {
            if (!first && memcmp(pix + (size_t)y * w, scr + (size_t)y * bpl, (size_t)w * 4) == 0)
                continue;
            memcpy(pix + (size_t)y * w, scr + (size_t)y * bpl, (size_t)w * 4);
            if (y0 < 0)
                y0 = (int)y;
            y1 = (int)y + 1;
        }
        first = 0;
        if (y0 >= 0 && bwm_damage(fd, id, 0, (unsigned)y0, w, (unsigned)(y1 - y0)) != 0)
            return 0;
    }
}
