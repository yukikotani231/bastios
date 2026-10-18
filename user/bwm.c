/* bwm, a compositing window manager for /dev/fb0. It takes the mouse from
 * /dev/input/mice and the keyboard from /dev/input/event0 (from the
 * terminal if there is no event0), gives the pointer a
 * cursor, and keeps the windows. A click raises and focuses a window,
 * dragging a title bar moves it, the box at the right of a title bar
 * closes the window, and what is typed goes to the focused window. Ctrl-T
 * starts a terminal (bterm) and Ctrl-Q
 * ends bwm (the console comes back when /dev/fb0 closes).
 *
 * Programs open windows over a UNIX socket (user/bwmproto.h): bwm keeps
 * each window's pixels, takes rectangles to draw, and sends back keys and
 * pointer events. Three windows of its own, with text only, are there at
 * the start unless it is run as "bwm -n".
 *
 * Every frame is composed in memory (`scene`) and only the rectangles
 * that changed are written to the screen, line by line, through lseek and
 * write. test/iso_test.sh drives it with QEMU's monitor (mouse_move,
 * mouse_button, sendkey) and reads the screen back from a screendump. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include "bwmproto.h"
#include "font8x8.h"

struct bitfield { uint32_t offset, length, msb_right; };
struct var_info {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct bitfield red, green, blue, transp;
    uint32_t rest[20];
};
struct fix_info {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

#define CELL_W 8
#define CELL_H 16
#define TITLE_H 20
#define BORDER 2
#define CLOSE_W 16
#define MAX_COLS 80
#define MAX_ROWS 40
#define MAXWIN 16
#define MAXCLI 8
#define RXCAP (BWM_MAX_PAYLOAD + 64)

static int fb = -1;
static unsigned W, H, PITCH;
static unsigned roff, goff, boff;
static uint32_t *scene, *frame;          /* W * H pixels, without and with the cursor */

struct win {
    int used;
    int id;
    int cli;                             /* the client that owns it, or -1 */
    int x, y, w, h;                      /* the frame, with its title bar */
    char title[24];
    uint32_t body;
    uint32_t *shm;                       /* the client's memory file, mapped, when it gave one */
    size_t shm_len;
    uint32_t *pix;                       /* a client's contents: (w - 2 * BORDER) * (h - TITLE_H - BORDER) */
    int cols, rows, cx, cy;
    char text[MAX_ROWS][MAX_COLS];
};
static struct win pool[MAXWIN];
static struct win *z[MAXWIN];            /* z[nz - 1] is on top */
static int nz;
static int next_id = 1;

struct cli {
    int fd;                              /* -1 when free */
    size_t have;
    unsigned char *rx;
    int pend_fd;                         /* a descriptor that came with the bytes read, or -1 */
};
static struct cli clis[MAXCLI];
static char path[64];

static uint32_t rgb(unsigned r, unsigned g, unsigned b)
{
    return (r << roff) | (g << goff) | (b << boff);
}

static uint32_t from_wire(uint32_t p)
{
    return rgb(p >> 16 & 255, p >> 8 & 255, p & 255);
}

static void fill(uint32_t *buf, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)W) x1 = W;
    if (y1 > (int)H) y1 = H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            buf[y * W + x] = c;
}

static void glyph(uint32_t *buf, int x, int y, int ch, uint32_t fg)
{
    if (ch < 0 || ch >= 128)
        ch = 32;
    for (int r = 0; r < CELL_H; r++) {
        unsigned bits = font8x8[ch * 8 + r / 2];
        for (int p = 0; p < CELL_W; p++, bits >>= 1) {
            int px = x + p, py = y + r;
            if ((bits & 1) && px >= 0 && py >= 0 && px < (int)W && py < (int)H)
                buf[py * W + px] = fg;
        }
    }
}

static void text(uint32_t *buf, int x, int y, const char *s, uint32_t fg)
{
    for (; *s; s++, x += CELL_W)
        glyph(buf, x, y, *s, fg);
}

static int cw_of(const struct win *w) { return w->w - 2 * BORDER; }
static int ch_of(const struct win *w) { return w->h - TITLE_H - BORDER; }

static struct win *win_new(int x, int y, int cw, int ch, const char *title, int cli)
{
    for (int i = 0; i < MAXWIN; i++) {
        struct win *w = &pool[i];
        if (w->used)
            continue;
        memset(w, 0, sizeof *w);
        w->used = 1;
        w->id = next_id++;
        w->cli = cli;
        w->x = x; w->y = y;
        w->w = cw + 2 * BORDER;
        w->h = ch + TITLE_H + BORDER;
        snprintf(w->title, sizeof w->title, "%s", title);
        w->body = rgb(235, 235, 235);
        w->cols = (cw - 8) / CELL_W;
        w->rows = (ch - 8) / CELL_H;
        if (w->cols > MAX_COLS) w->cols = MAX_COLS;
        if (w->rows > MAX_ROWS) w->rows = MAX_ROWS;
        for (int r = 0; r < MAX_ROWS; r++)
            memset(w->text[r], ' ', MAX_COLS);
        z[nz++] = w;
        return w;
    }
    return NULL;
}

static void win_free(struct win *w)
{
    int i = 0;
    while (i < nz && z[i] != w)
        i++;
    if (i < nz) {
        memmove(&z[i], &z[i + 1], (nz - 1 - i) * sizeof z[0]);
        nz--;
    }
    free(w->pix);
    if (w->shm)
        munmap(w->shm, w->shm_len);
    w->shm = NULL;
    w->used = 0;
}

static void win_put(struct win *w, int c)
{
    if (c == '\r' || c == '\n') {
        w->cx = 0;
        w->cy++;
    } else if (c == 127 || c == 8) {
        if (w->cx > 0) {
            w->cx--;
            w->text[w->cy][w->cx] = ' ';
        }
    } else if (c >= 32 && c < 127) {
        w->text[w->cy][w->cx] = c;
        if (++w->cx >= w->cols) {
            w->cx = 0;
            w->cy++;
        }
    }
    if (w->cy >= w->rows) {
        for (int r = 1; r < w->rows; r++)
            memcpy(w->text[r - 1], w->text[r], MAX_COLS);
        memset(w->text[w->rows - 1], ' ', MAX_COLS);
        w->cy = w->rows - 1;
    }
}

static void win_say(struct win *w, const char *s)
{
    for (; *s; s++)
        win_put(w, *s);
}

static void draw_win(const struct win *w, int focused)
{
    uint32_t bar = focused ? rgb(40, 90, 200) : rgb(110, 110, 120);
    int bx = w->x + BORDER, by = w->y + TITLE_H;
    fill(scene, w->x, w->y, w->x + w->w, w->y + w->h, rgb(20, 20, 30));
    fill(scene, w->x + BORDER, w->y + BORDER, w->x + w->w - BORDER, w->y + TITLE_H, bar);
    text(scene, w->x + 8, w->y + BORDER + 1, w->title, rgb(255, 255, 255));
    fill(scene, w->x + w->w - BORDER - CLOSE_W, w->y + BORDER + 2, w->x + w->w - BORDER - 2, w->y + TITLE_H - 2, rgb(200, 60, 60));
    if (w->pix) {
        int cw = cw_of(w), ch = ch_of(w);
        for (int y = 0; y < ch; y++) {
            int py = by + y;
            if (py < 0 || py >= (int)H)
                continue;
            for (int x = 0; x < cw; x++) {
                int px = bx + x;
                if (px >= 0 && px < (int)W)
                    scene[py * W + px] = w->pix[y * cw + x];
            }
        }
        return;
    }
    fill(scene, bx, by, w->x + w->w - BORDER, w->y + w->h - BORDER, w->body);
    for (int r = 0; r < w->rows; r++)
        for (int c = 0; c < w->cols; c++)
            if (w->text[r][c] != ' ')
                glyph(scene, bx + 4 + c * CELL_W, by + 4 + r * CELL_H, w->text[r][c], rgb(10, 10, 10));
    if (focused)
        fill(scene, bx + 4 + w->cx * CELL_W, by + 4 + w->cy * CELL_H + CELL_H - 3,
             bx + 4 + (w->cx + 1) * CELL_W, by + 4 + w->cy * CELL_H + CELL_H - 1, rgb(10, 10, 10));
}

static void draw_scene(void)
{
    for (unsigned y = 0; y < H; y++) {
        uint32_t c = rgb(10 + y * 40 / H, 60 + y * 50 / H, 80 + y * 60 / H);
        for (unsigned x = 0; x < W; x++)
            scene[y * W + x] = c;
    }
    for (int i = 0; i < nz; i++)
        draw_win(z[i], i == nz - 1);
}

/* The pointer: '#' black outline, '.' white fill. */
static const char *arrow[] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ", "#....#      ",
    "#.....#     ", "#......#    ", "#.......#   ", "#........#  ", "#.....#####", "#..#..#     ",
    "#.# #..#    ", "##  #..#    ", "#    #..#   ", "     #..#   ", "      ##    ",
};
#define ARROW_W 12
#define ARROW_H 17

static void draw_cursor(int mx, int my)
{
    for (int y = 0; y < ARROW_H; y++)
        for (int x = 0; x < ARROW_W; x++) {
            char c = arrow[y][x];
            int px = mx + x, py = my + y;
            if (c == ' ' || px < 0 || py < 0 || px >= (int)W || py >= (int)H)
                continue;
            frame[py * W + px] = c == '#' ? rgb(0, 0, 0) : rgb(255, 255, 255);
        }
}

/* Copies the rectangle from scene to frame, puts the cursor on it, and
 * writes it to the screen. */
static int flush(int x0, int y0, int x1, int y1, int mx, int my)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)W) x1 = W;
    if (y1 > (int)H) y1 = H;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    for (int y = y0; y < y1; y++)
        memcpy(&frame[y * W + x0], &scene[y * W + x0], (x1 - x0) * 4);
    draw_cursor(mx, my);
    for (int y = y0; y < y1; y++) {
        off_t at = (off_t)y * PITCH + (off_t)x0 * 4;
        if (lseek(fb, at, SEEK_SET) != at || write(fb, &frame[y * W + x0], (x1 - x0) * 4) != (ssize_t)((x1 - x0) * 4))
            return -1;
    }
    return 0;
}

/* The part of the screen that has changed. */
static int dx0, dy0, dx1, dy1;
static void touch(int x0, int y0, int x1, int y1)
{
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}
static void touch_win(const struct win *w) { touch(w->x, w->y, w->x + w->w, w->y + w->h); }
static void touch_all(void) { touch(0, 0, W, H); }

static struct win *hit(int x, int y)
{
    for (int i = nz - 1; i >= 0; i--)
        if (x >= z[i]->x && x < z[i]->x + z[i]->w && y >= z[i]->y && y < z[i]->y + z[i]->h)
            return z[i];
    return NULL;
}

static void raise_win(struct win *w)
{
    int i = 0;
    while (i < nz && z[i] != w)
        i++;
    if (i >= nz || i == nz - 1)
        return;
    memmove(&z[i], &z[i + 1], (nz - 1 - i) * sizeof z[0]);
    z[nz - 1] = w;
}

/* ---- clients ---- */

static void to_client(int cli, uint32_t type, const void *p, uint32_t len)
{
    if (cli < 0 || clis[cli].fd < 0)
        return;
    unsigned char b[sizeof(struct bwm_hdr) + 32];
    struct bwm_hdr h = { type, len };
    memcpy(b, &h, sizeof h);
    memcpy(b + sizeof h, p, len);
    (void)!write(clis[cli].fd, b, sizeof h + len);        /* events are small: a full socket drops them */
}

static void drop_client(int i)
{
    for (int j = 0; j < MAXWIN; j++)
        if (pool[j].used && pool[j].cli == i) {
            touch_win(&pool[j]);
            win_free(&pool[j]);
        }
    close(clis[i].fd);
    if (clis[i].pend_fd >= 0)
        close(clis[i].pend_fd);
    clis[i].pend_fd = -1;
    clis[i].fd = -1;
    clis[i].have = 0;
}

static struct win *by_id(int cli, uint32_t id)
{
    for (int j = 0; j < MAXWIN; j++)
        if (pool[j].used && pool[j].cli == cli && (uint32_t)pool[j].id == id)
            return &pool[j];
    return NULL;
}

/* One whole message from client i, `n` bytes at c->rx. */
static void message(int i, uint32_t type, const unsigned char *p, uint32_t len)
{
    if ((type == BWM_CREATE || type == BWM_CREATE_SHM) && len == sizeof(struct bwm_create)) {
        struct bwm_create c;
        memcpy(&c, p, sizeof c);
        c.title[sizeof c.title - 1] = 0;
        int mf = type == BWM_CREATE_SHM ? clis[i].pend_fd : -1;
        clis[i].pend_fd = -1;
        if (type == BWM_CREATE_SHM && mf < 0) {
            drop_client(i);                                /* a shared window needs its file */
            return;
        }
        if (c.w < 16 || c.h < 16 || c.w > W - 2 * BORDER || c.h > H - TITLE_H - BORDER) {
            if (mf >= 0)
                close(mf);
            drop_client(i);
            return;
        }
        struct win *w = win_new(40 + 30 * (next_id % 8), 40 + 30 * (next_id % 8), c.w, c.h, c.title, i);
        if (!w) {
            if (mf >= 0)
                close(mf);
            drop_client(i);
            return;
        }
        w->pix = calloc((size_t)c.w * c.h, 4);
        if (!w->pix) {
            if (mf >= 0)
                close(mf);
            win_free(w);
            drop_client(i);
            return;
        }
        if (mf >= 0) {
            struct stat st;
            size_t need = (size_t)c.w * c.h * 4;
            void *m = MAP_FAILED;
            if (fstat(mf, &st) == 0 && st.st_size >= 0 && (size_t)st.st_size >= need)
                m = mmap(NULL, need, PROT_READ, MAP_SHARED, mf, 0);
            close(mf);
            if (m == MAP_FAILED) {
                win_free(w);
                drop_client(i);
                return;
            }
            w->shm = m;
            w->shm_len = need;
        }
        for (size_t k = 0; k < (size_t)c.w * c.h; k++)
            w->pix[k] = w->body;
        struct bwm_created r = { (uint32_t)w->id, c.w, c.h };
        to_client(i, BWM_CREATED, &r, sizeof r);
        raise_win(w);
        touch_win(w);
    } else if (type == BWM_DRAW && len >= sizeof(struct bwm_draw)) {
        struct bwm_draw d;
        memcpy(&d, p, sizeof d);
        struct win *w = by_id(i, d.id);
        int cw = w ? cw_of(w) : 0, ch = w ? ch_of(w) : 0;
        if (!w || d.x >= (uint32_t)cw || d.y >= (uint32_t)ch || d.w > (uint32_t)cw - d.x || d.h > (uint32_t)ch - d.y)
            return;
        if (w->shm) {
            if (len != sizeof d)
                return;
            for (uint32_t r = 0; r < d.h; r++)
                for (uint32_t c = 0; c < d.w; c++) {
                    size_t k = (size_t)(d.y + r) * cw + d.x + c;
                    w->pix[k] = from_wire(w->shm[k]);
                }
        } else {
            if (len != sizeof d + (size_t)d.w * d.h * 4)
                return;
            const unsigned char *s = p + sizeof d;
            for (uint32_t r = 0; r < d.h; r++)
                for (uint32_t c = 0; c < d.w; c++) {
                    uint32_t px;
                    memcpy(&px, s + ((size_t)r * d.w + c) * 4, 4);
                    w->pix[(size_t)(d.y + r) * cw + d.x + c] = from_wire(px);
                }
        }
        touch(w->x + BORDER + d.x, w->y + TITLE_H + d.y, w->x + BORDER + d.x + d.w, w->y + TITLE_H + d.y + d.h);
    } else if (type == BWM_DESTROY && len == sizeof(struct bwm_destroy)) {
        struct bwm_destroy d;
        memcpy(&d, p, sizeof d);
        struct win *w = by_id(i, d.id);
        if (w) {
            touch_win(w);
            win_free(w);
        }
    } else if (type == BWM_SETTITLE && len == sizeof(struct bwm_settitle)) {
        struct bwm_settitle t;
        memcpy(&t, p, sizeof t);
        struct win *w = by_id(i, t.id);
        if (w) {
            t.title[sizeof t.title - 1] = 0;
            snprintf(w->title, sizeof w->title, "%s", t.title);
            touch_win(w);
        }
    } else {
        drop_client(i);                                    /* not a message we know */
    }
}

static void client_input(int i)
{
    struct cli *c = &clis[i];
    struct iovec iov = { c->rx + c->have, RXCAP - c->have };
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } u;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = u.b;
    mh.msg_controllen = sizeof u.b;
    ssize_t n = recvmsg(c->fd, &mh, MSG_CMSG_CLOEXEC);
    for (struct cmsghdr *cm = n >= 0 ? CMSG_FIRSTHDR(&mh) : NULL; cm; cm = CMSG_NXTHDR(&mh, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS && cm->cmsg_len == CMSG_LEN(sizeof(int))) {
            if (c->pend_fd >= 0)
                close(c->pend_fd);
            memcpy(&c->pend_fd, CMSG_DATA(cm), sizeof(int));
        }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        drop_client(i);
        return;
    }
    if (n < 0)
        return;
    c->have += n;
    for (;;) {
        struct bwm_hdr h;
        if (c->have < sizeof h)
            return;
        memcpy(&h, c->rx, sizeof h);
        if (h.len > BWM_MAX_PAYLOAD) {
            drop_client(i);
            return;
        }
        if (c->have < sizeof h + h.len)
            return;
        message(i, h.type, c->rx + sizeof h, h.len);
        if (c->fd < 0)
            return;
        memmove(c->rx, c->rx + sizeof h + h.len, c->have - sizeof h - h.len);
        c->have -= sizeof h + h.len;
    }
}

static int listen_socket(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, BWM_SOCK);
    unlink(BWM_SOCK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) {
        close(s);
        return -1;
    }
    return s;
}

/* The keyboard as key events (/dev/input/event0): a press or release of a
 * key with its Linux key code (1 to 88 are the PS/2 set 1 codes). bwm keeps
 * the modifiers and makes the bytes the terminal would have given, so the
 * clients see the same keys either way. */
struct kev { uint64_t sec, usec; uint16_t type, code; int32_t value; };
static const char kmap[]  = "\0\033" "1234567890-=\177\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char kmaps[] = "\0\033" "!@#$%^&*()_+\177\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
static int k_shift, k_ctrl, k_caps;

/* The bytes a key press makes (at most 4) in out; returns how many. */
static int key_bytes(unsigned code, char *out)
{
    static const struct { unsigned code; char final; } nav[] = {
        { 103, 'A' }, { 108, 'B' }, { 106, 'C' }, { 105, 'D' }, { 102, 'H' }, { 107, 'F' } };
    for (unsigned i = 0; i < sizeof nav / sizeof nav[0]; i++)
        if (nav[i].code == code) {
            out[0] = 27; out[1] = '['; out[2] = nav[i].final;
            return 3;
        }
    if (code == 111) { memcpy(out, "\033[3~", 4); return 4; }        /* Delete */
    if (code == 104) { memcpy(out, "\033[5~", 4); return 4; }        /* Page Up */
    if (code == 109) { memcpy(out, "\033[6~", 4); return 4; }        /* Page Down */
    if (code == 110) { memcpy(out, "\033[2~", 4); return 4; }        /* Insert */
    if (code >= sizeof kmap - 1)
        return 0;
    char c = k_shift ? kmaps[code] : kmap[code];
    if (c >= 'a' && c <= 'z' && k_caps)
        c -= 32;
    else if (c >= 'A' && c <= 'Z' && k_caps)
        c += 32;
    if (k_ctrl && c >= 'a' && c <= 'z')
        c -= 96;
    if (c == 0)
        return 0;
    out[0] = c;
    return 1;
}

/* Ctrl-T: a new terminal window (bterm, with ash behind it), whatever the
 * focused window is. */
static void launch_terminal(void)
{
    if (fork() == 0) {
        char *argv[] = { "bterm", NULL };
        signal(SIGCHLD, SIG_DFL);                             /* an ignored SIGCHLD would stay ignored in ash */
        execv("/bin/bterm", argv);
        _exit(127);
    }
}

/* Gives one typed byte to the focused window; returns 1 for Ctrl-Q. */
static int type_byte(struct win *top, char c)
{
    if (c == 17)
        return 1;
    if (c == 20) {
        launch_terminal();
        return 0;
    }
    if (top && top->cli >= 0) {
        struct bwm_key k = { (uint32_t)top->id, (unsigned char)c };
        to_client(top->cli, BWM_KEY, &k, sizeof k);
    } else if (top) {
        touch_win(top);
        win_put(top, c);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int builtin = 1;
    int a = 1;
    if (a < argc && strcmp(argv[a], "-n") == 0) {
        builtin = 0;
        a++;
    }
    char **child = a + 1 < argc && strcmp(argv[a], "-c") == 0 ? &argv[a + 1] : NULL;
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0 && errno == ENODEV) {
        puts("bwm: no screen (ENODEV)");
        return 2;
    }
    int mice = open("/dev/input/mice", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int keys = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct var_info v;
    struct fix_info f;
    if (fb < 0 || ioctl(fb, 0x4600, &v) != 0 || ioctl(fb, 0x4602, &f) != 0 || v.bits_per_pixel != 32) {
        printf("bwm: no usable /dev/fb0 (errno %d)\n", errno);
        return 1;
    }
    W = v.xres; H = v.yres; PITCH = f.line_length;
    roff = v.red.offset; goff = v.green.offset; boff = v.blue.offset;
    scene = malloc((size_t)W * H * 4);
    frame = malloc((size_t)W * H * 4);
    if (!scene || !frame) {
        puts("bwm: out of memory");
        return 1;
    }
    for (int i = 0; i < MAXCLI; i++) {
        clis[i].fd = -1;
        clis[i].pend_fd = -1;
        clis[i].rx = malloc(RXCAP);
    }
    int ls = listen_socket();
    struct termios old, raw;
    int have_tty = tcgetattr(0, &old) == 0;
    if (have_tty) {
        raw = old;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_iflag &= ~ICRNL;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }

    if (builtin) {
        struct win *a = win_new(80, 60, 356, 200, "Notes", -1);
        struct win *b = win_new(300, 200, 396, 240, "About", -1);
        struct win *c = win_new(120, 120, 396, 280, "Terminal", -1);
        a->body = rgb(250, 245, 200);
        b->body = rgb(220, 235, 250);
        win_say(a, "A window manager\nfor BastiOS.");
        win_say(b, "Drag a title bar.\nClick to focus.\nType into the focus.");
        win_say(c, "bwm: type here.\nCtrl-Q quits.\n");
    }

    signal(SIGCHLD, SIG_IGN);                                 /* launched terminals need no wait */
    int mx = W / 2, my = H / 2;
    draw_scene();
    flush(0, 0, W, H, mx, my);
    printf("bwm: ready %ux%u\n", W, H);
    fflush(stdout);
    if (child && fork() == 0) {                               /* -c PROGRAM [ARGS]: start a client */
        execv(child[0][0] == '/' ? child[0] : (snprintf(path, sizeof path, "/bin/%s", child[0]), path), child);
        _exit(127);
    }

    struct win *drag = NULL;
    int gx = 0, gy = 0, buttons = 0, running = 1;
    while (running) {
        struct pollfd p[4 + MAXCLI];
        int np = 0, imice = -1, ikeys = -1, ils = -1, icli[MAXCLI];
        p[np++] = (struct pollfd){ 0, POLLIN, 0 };
        if (mice >= 0) { imice = np; p[np++] = (struct pollfd){ mice, POLLIN, 0 }; }
        if (keys >= 0) { ikeys = np; p[np++] = (struct pollfd){ keys, POLLIN, 0 }; }
        if (ls >= 0) { ils = np; p[np++] = (struct pollfd){ ls, POLLIN, 0 }; }
        for (int i = 0; i < MAXCLI; i++) {
            icli[i] = -1;
            if (clis[i].fd >= 0) { icli[i] = np; p[np++] = (struct pollfd){ clis[i].fd, POLLIN, 0 }; }
        }
        if (poll(p, np, -1) < 0 && errno != EINTR)
            break;
        int ox = mx, oy = my;
        dx0 = W; dy0 = H; dx1 = 0; dy1 = 0;
        struct win *top = nz ? z[nz - 1] : NULL;
        if (p[0].revents & POLLIN) {
            char b[32];
            ssize_t n = read(0, b, sizeof b);
            if (n <= 0)
                break;
            for (ssize_t i = 0; keys < 0 && i < n; i++)      /* with key events, the terminal's bytes are the same keys again */
                if (type_byte(top, b[i]))
                    running = 0;
        }
        if (ikeys >= 0 && (p[ikeys].revents & POLLIN)) {
            struct kev ev[16];
            ssize_t n;
            while ((n = read(keys, ev, sizeof ev)) > 0) {
                for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
                    if (ev[i].type != 1)                     /* EV_KEY */
                        continue;
                    unsigned code = ev[i].code;
                    int down = ev[i].value != 0;
                    if (code == 42 || code == 54) {
                        k_shift = down;
                    } else if (code == 29 || code == 97) {
                        k_ctrl = down;
                    } else if (code == 58 && down) {
                        k_caps = !k_caps;
                    } else if (down) {
                        char out[4];
                        int m = key_bytes(code, out);
                        for (int j = 0; j < m; j++)
                            if (type_byte(top, out[j]))
                                running = 0;
                    }
                }
            }
        }
        if (ils >= 0 && (p[ils].revents & POLLIN)) {
            int c;
            while ((c = accept4(ls, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
                int slot = 0;
                while (slot < MAXCLI && clis[slot].fd >= 0)
                    slot++;
                if (slot == MAXCLI) {
                    close(c);
                } else {
                    clis[slot].fd = c;
                    clis[slot].have = 0;
                }
            }
        }
        for (int i = 0; i < MAXCLI; i++)
            if (icli[i] >= 0 && clis[i].fd >= 0 && (p[icli[i]].revents & (POLLIN | POLLHUP | POLLERR)))
                client_input(i);
        if (imice >= 0 && (p[imice].revents & POLLIN)) {
            unsigned char m[3];
            while (read(mice, m, 3) == 3) {
                int dx = (signed char)m[1], dy = (signed char)m[2];
                mx += dx; my -= dy;
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if (mx >= (int)W) mx = W - 1;
                if (my >= (int)H) my = H - 1;
                int down = m[0] & 1;
                struct win *under = hit(mx, my);
                if (down && !(buttons & 1) && under) {
                    gx = mx - under->x;
                    gy = my - under->y;
                    int on_close = my < under->y + TITLE_H && mx >= under->x + under->w - BORDER - CLOSE_W && mx < under->x + under->w - BORDER - 2;
                    if (on_close) {
                        if (under->cli >= 0) {
                            struct bwm_close cl = { (uint32_t)under->id };
                            to_client(under->cli, BWM_CLOSE, &cl, sizeof cl);
                        } else {
                            touch_win(under);
                            win_free(under);
                        }
                        under = NULL;
                    } else {
                        if (under != z[nz - 1]) {
                            raise_win(under);
                            touch_all();
                        }
                        drag = my < under->y + TITLE_H ? under : NULL;
                    }
                } else if (!down) {
                    drag = NULL;
                }
                if (drag && down) {
                    touch_win(drag);
                    drag->x = mx - gx; drag->y = my - gy;
                    touch_win(drag);
                } else if (under && under->cli >= 0 && my >= under->y + TITLE_H && (dx || dy || (m[0] & 7) != (buttons & 7))) {
                    struct bwm_mouse ev = { (uint32_t)under->id, (uint32_t)(mx - under->x - BORDER), (uint32_t)(my - under->y - TITLE_H), m[0] & 7u };
                    if (under == z[nz - 1])
                        to_client(under->cli, BWM_MOUSE, &ev, sizeof ev);
                }
                buttons = m[0];
            }
        }
        if (dx0 < dx1) {
            draw_scene();
            if (flush(dx0, dy0, dx1, dy1, mx, my) != 0)
                break;
            if (flush(ox, oy, ox + ARROW_W, oy + ARROW_H, mx, my) != 0)
                break;
        } else if (mx != ox || my != oy) {
            if (flush(ox, oy, ox + ARROW_W, oy + ARROW_H, mx, my) != 0)
                break;
        }
        if (mx != ox || my != oy)
            flush(mx, my, mx + ARROW_W, my + ARROW_H, mx, my);
    }
    if (keys >= 0) {                  /* the terminal sees every key too: what it got of the last ones must not reach the shell */
        struct pollfd q = { 0, POLLIN, 0 };
        char junk[64];
        while (poll(&q, 1, 150) > 0 && read(0, junk, sizeof junk) > 0)
            ;
    }
    if (have_tty)
        tcsetattr(0, TCSANOW, &old);
    for (int i = 0; i < MAXCLI; i++)
        if (clis[i].fd >= 0)
            close(clis[i].fd);
    if (ls >= 0) {
        close(ls);
        unlink(BWM_SOCK);
    }
    close(fb);
    puts("bwm: bye");
    return 0;
}
