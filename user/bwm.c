/* bwm, the window server's first form: a compositing window manager for
 * /dev/fb0 that draws its own windows, takes the mouse from
 * /dev/input/mice and the keyboard from the terminal, and gives the
 * pointer a cursor. A click raises and focuses a window, dragging a
 * title bar moves it, and what is typed goes into the focused window's
 * text. Ctrl-Q ends it (the console comes back when /dev/fb0 closes).
 * There are no client programs yet: the windows are the server's own.
 *
 * Every frame is composed in memory (`scene`) and only the rectangles
 * that changed are written to the screen, line by line, through
 * lseek and write. test/iso_test.sh drives it with QEMU's monitor
 * (mouse_move, mouse_button, sendkey) and reads the screen back from a
 * screendump. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

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
#define MAX_COLS 80
#define MAX_ROWS 40
#define NWIN 3

static int fb = -1;
static unsigned W, H, PITCH;
static unsigned roff, goff, boff;
static uint32_t *scene, *frame;          /* W * H pixels, without and with the cursor */

struct win {
    int x, y, w, h;
    char title[24];
    uint32_t body;
    int cols, rows, cx, cy;
    char text[MAX_ROWS][MAX_COLS];
};
static struct win wins[NWIN];            /* wins[NWIN - 1] is on top */
static int nwin = NWIN;
static int focus = NWIN - 1;             /* an index into wins */

static uint32_t rgb(unsigned r, unsigned g, unsigned b)
{
    return (r << roff) | (g << goff) | (b << boff);
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

static void glyph(uint32_t *buf, int x, int y, int ch, uint32_t fg, int has_bg, uint32_t bg)
{
    if (ch < 0 || ch >= 128)
        ch = 32;
    for (int r = 0; r < CELL_H; r++) {
        unsigned bits = font8x8[ch * 8 + r / 2];
        for (int p = 0; p < CELL_W; p++, bits >>= 1) {
            int px = x + p, py = y + r;
            if (px < 0 || py < 0 || px >= (int)W || py >= (int)H)
                continue;
            if (bits & 1)
                buf[py * W + px] = fg;
            else if (has_bg)
                buf[py * W + px] = bg;
        }
    }
}

static void text(uint32_t *buf, int x, int y, const char *s, uint32_t fg)
{
    for (; *s; s++, x += CELL_W)
        glyph(buf, x, y, *s, fg, 0, 0);
}

static void win_init(struct win *w, int x, int y, int cw, int ch, const char *title, uint32_t body)
{
    memset(w, 0, sizeof *w);
    w->x = x; w->y = y; w->w = cw; w->h = ch;
    snprintf(w->title, sizeof w->title, "%s", title);
    w->body = body;
    w->cols = (cw - 2 * BORDER - 8) / CELL_W;
    w->rows = (ch - TITLE_H - 2 * BORDER - 8) / CELL_H;
    if (w->cols > MAX_COLS) w->cols = MAX_COLS;
    if (w->rows > MAX_ROWS) w->rows = MAX_ROWS;
    for (int r = 0; r < MAX_ROWS; r++)
        memset(w->text[r], ' ', MAX_COLS);
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
    uint32_t frame_c = rgb(20, 20, 30);
    uint32_t bar = focused ? rgb(40, 90, 200) : rgb(110, 110, 120);
    fill(scene, w->x, w->y, w->x + w->w, w->y + w->h, frame_c);
    fill(scene, w->x + BORDER, w->y + BORDER, w->x + w->w - BORDER, w->y + TITLE_H, bar);
    text(scene, w->x + 8, w->y + BORDER + 1, w->title, rgb(255, 255, 255));
    fill(scene, w->x + BORDER, w->y + TITLE_H, w->x + w->w - BORDER, w->y + w->h - BORDER, w->body);
    for (int r = 0; r < w->rows; r++)
        for (int c = 0; c < w->cols; c++) {
            int ch = w->text[r][c];
            if (ch != ' ')
                glyph(scene, w->x + BORDER + 4 + c * CELL_W, w->y + TITLE_H + 4 + r * CELL_H, ch, rgb(10, 10, 10), 0, 0);
        }
    if (focused)
        fill(scene, w->x + BORDER + 4 + w->cx * CELL_W, w->y + TITLE_H + 4 + w->cy * CELL_H + CELL_H - 3,
             w->x + BORDER + 4 + (w->cx + 1) * CELL_W, w->y + TITLE_H + 4 + w->cy * CELL_H + CELL_H - 1, rgb(10, 10, 10));
}

static void draw_scene(void)
{
    for (unsigned y = 0; y < H; y++) {
        uint32_t c = rgb(10 + y * 40 / H, 60 + y * 50 / H, 80 + y * 60 / H);
        for (unsigned x = 0; x < W; x++)
            scene[y * W + x] = c;
    }
    for (int i = 0; i < nwin; i++)
        draw_win(&wins[i], i == focus);
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

static int hit(int x, int y)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (x >= wins[i].x && x < wins[i].x + wins[i].w && y >= wins[i].y && y < wins[i].y + wins[i].h)
            return i;
    return -1;
}

static void raise_win(int i)
{
    struct win t = wins[i];
    memmove(&wins[i], &wins[i + 1], (nwin - 1 - i) * sizeof t);
    wins[nwin - 1] = t;
    focus = nwin - 1;
}

int main(void)
{
    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0 && errno == ENODEV) {
        puts("bwm: no screen (ENODEV)");
        return 2;
    }
    int mice = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
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

    win_init(&wins[0], 80, 60, 360, 220, "Notes", rgb(250, 245, 200));
    win_init(&wins[1], 300, 200, 400, 260, "About", rgb(220, 235, 250));
    win_init(&wins[2], 120, 120, 400, 300, "Terminal", rgb(235, 235, 235));
    win_say(&wins[0], "A window manager\nfor BastiOS.");
    win_say(&wins[1], "Drag a title bar.\nClick to focus.\nType into the focus.");
    win_say(&wins[2], "bwm: type here.\nCtrl-Q quits.\n");

    int mx = W / 2, my = H / 2;
    draw_scene();
    flush(0, 0, W, H, mx, my);
    printf("bwm: ready %ux%u\n", W, H);
    fflush(stdout);

    int drag = -1, gx = 0, gy = 0, buttons = 0, running = 1;
    while (running) {
        struct pollfd p[2] = { { 0, POLLIN, 0 }, { mice, POLLIN, 0 } };
        if (poll(p, mice >= 0 ? 2 : 1, -1) < 0 && errno != EINTR)
            break;
        int ox = mx, oy = my, dirty = 0;
        int sx0 = W, sy0 = H, sx1 = 0, sy1 = 0;       /* the scene rectangle that changed */
#define TOUCH(w_) do { \
            if ((w_)->x < sx0) sx0 = (w_)->x; if ((w_)->y < sy0) sy0 = (w_)->y; \
            if ((w_)->x + (w_)->w > sx1) sx1 = (w_)->x + (w_)->w; if ((w_)->y + (w_)->h > sy1) sy1 = (w_)->y + (w_)->h; \
            dirty = 1; } while (0)
        if (p[0].revents & POLLIN) {
            char b[32];
            ssize_t n = read(0, b, sizeof b);
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n; i++) {
                if (b[i] == 17) {                      /* Ctrl-Q */
                    running = 0;
                } else {
                    TOUCH(&wins[focus]);
                    win_put(&wins[focus], b[i]);
                }
            }
        }
        if (mice >= 0 && (p[1].revents & POLLIN)) {
            unsigned char m[3];
            while (read(mice, m, 3) == 3) {
                int dx = (signed char)m[1], dy = (signed char)m[2];
                mx += dx; my -= dy;
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if (mx >= (int)W) mx = W - 1;
                if (my >= (int)H) my = H - 1;
                int down = m[0] & 1;
                if (down && !(buttons & 1)) {
                    int i = hit(mx, my);
                    if (i >= 0) {
                        int grab = my < wins[i].y + TITLE_H;
                        gx = mx - wins[i].x;
                        gy = my - wins[i].y;
                        if (i != nwin - 1 || focus != nwin - 1) {
                            raise_win(i);
                            sx0 = 0; sy0 = 0; sx1 = W; sy1 = H; dirty = 1;
                        }
                        drag = grab ? nwin - 1 : -1;
                    }
                } else if (!down) {
                    drag = -1;
                }
                if (drag >= 0 && down) {
                    struct win *w = &wins[drag];
                    TOUCH(w);
                    w->x = mx - gx; w->y = my - gy;
                    TOUCH(w);
                }
                buttons = m[0];
            }
        }
        if (dirty) {
            draw_scene();
            if (sx0 < sx1 && flush(sx0, sy0, sx1, sy1, mx, my) != 0)
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
    if (have_tty)
        tcsetattr(0, TCSANOW, &old);
    close(fb);
    puts("bwm: bye");
    return 0;
}
