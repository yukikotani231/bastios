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
 * A bwm window cannot be resized for it: the X screen has one size. The
 * pixels are copied 20 times a second, only the rows that changed. */
#include "bwmlib.h"

#include <poll.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

static int xfd = -1;
static unsigned xseq;
static uint32_t xroot;
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
        if (p[1].revents & POLLIN) {              /* nothing is asked for; drop what comes */
            unsigned char junk[512];
            if (read(xfd, junk, sizeof junk) <= 0) {
                puts("bx11: the X server ended");
                return 0;
            }
        }
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
