/* bwm, a compositing window manager for /dev/fb0. It takes the mouse from
 * /dev/input/mice and the keyboard from /dev/input/event0 (from the
 * terminal if there is no event0), gives the pointer a
 * cursor, and keeps the windows. A click raises and focuses a window,
 * dragging a title bar moves it, the box at the right of a title bar
 * closes the window (the boxes left of it maximize and minimize), dragging an edge or corner of a resizable window resizes it,
 * and what is typed goes to the focused window. Ctrl-T
 * (or the Term button of the task bar at the bottom, which also has a button for each window) starts a terminal (bterm), Alt-Tab brings up the window at the bottom (with Shift, it sends the top one to the bottom) and Ctrl-Q
 * ends bwm (the console comes back when /dev/fb0 closes). The Menu button at the
 * right end of the task bar opens a launcher: Terminal, Files, System, Hello window, Quit bwm.
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
#include <time.h>
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
#define BAR_H 24                         /* the task bar along the bottom */
#define TERM_X 4                         /* its Term button */
#define TERM_W 48
#define BTN_X0 60                        /* and the window buttons after it */
#define BTN_W 120
#define BTN_GAP 4
#define CLOCK_W 48                       /* the clock left of it */
#define MENU_W 52                        /* the Menu button at the right end of the task bar */
#define MENU_ITEM_W 140                  /* the launcher menu it opens above itself */
#define MENU_ITEM_H 24
#define EDGE 4                          /* the strip along each side of a frame that resizes it */
#define GRIP 14                          /* the resize grip at the corner of a frame */
#define MIN_CW 64
#define MIN_CH 32
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
    int resizable;                       /* has a grip */
    int again;                           /* a size was wanted while a request was pending: ask again when it is answered */
    int hidden;                          /* minimized: only its task bar button shows */
    int maxed;                           /* maximized: sx, sy, sw, sh are the frame it goes back to */
    int sx, sy, sw, sh;
    int pend;                            /* a BWM_RESIZE has been sent and not answered */
    int want_cw, want_ch;                /* the size of the contents the pointer is asking for */
    int keep_r_on, keep_r, keep_b_on, keep_b;   /* a left or top edge is being dragged: the right or bottom edge stays at keep_r / keep_b */
    int cols, rows, cx, cy;
    char text[MAX_ROWS][MAX_COLS];
};
static struct win pool[MAXWIN];
static struct win *z[MAXWIN];            /* z[nz - 1] is on top */
static int nz;
static int next_id = 1;
static void touch_bar(void);
static struct win *focus(void);
static int menu_open;                    /* the launcher menu is showing */
static int menu_mx, menu_my;             /* the pointer, for the entry that is lit */
static struct win *sizing;               /* the window whose grip is held */
static int sox, soy;                     /* from the pointer to the edge or corner of the frame it holds, when it was taken */
static int size_edges;                   /* which: 1 left, 2 right, 4 top, 8 bottom */

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
        w->resizable = cli < 0;
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
        touch_bar();
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
    if (sizing == w)
        sizing = NULL;
    w->used = 0;
    touch_bar();
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

static void draw_grip(const struct win *w)
{
    uint32_t c = rgb(120, 120, 140);
    int x1 = w->x + w->w - BORDER - 1, y1 = w->y + w->h - BORDER - 1;
    for (int k = 4; k <= 12; k += 4)
        for (int j = 0; j < k; j++) {
            int px = x1 - j, py = y1 - (k - 1 - j);
            if (px >= 0 && py >= 0 && px < (int)W && py < (int)H)
                scene[py * W + px] = c;
        }
}

/* The left edge of title bar button i of a window: 0 is the close box, then
 * maximize (when it has a grip), then minimize. */
static int title_btn(const struct win *w, int i)
{
    return w->x + w->w - BORDER - CLOSE_W - i * (CLOSE_W + 2);
}

static void draw_win(const struct win *w, int focused)
{
    uint32_t bar = focused ? rgb(40, 90, 200) : rgb(110, 110, 120);
    int bx = w->x + BORDER, by = w->y + TITLE_H;
    fill(scene, w->x, w->y, w->x + w->w, w->y + w->h, rgb(20, 20, 30));
    fill(scene, w->x + BORDER, w->y + BORDER, w->x + w->w - BORDER, w->y + TITLE_H, bar);
    text(scene, w->x + 8, w->y + BORDER + 1, w->title, rgb(255, 255, 255));
    fill(scene, w->x + w->w - BORDER - CLOSE_W, w->y + BORDER + 2, w->x + w->w - BORDER - 2, w->y + TITLE_H - 2, rgb(200, 60, 60));
    if (w->resizable) {                  /* maximize: a box, minimize: a bar */
        int x0 = title_btn(w, 1);
        fill(scene, x0, w->y + BORDER + 2, x0 + CLOSE_W - 2, w->y + TITLE_H - 2, rgb(70, 160, 90));
        fill(scene, x0 + 3, w->y + BORDER + 5, x0 + CLOSE_W - 5, w->y + BORDER + 6, rgb(255, 255, 255));
        fill(scene, x0 + 3, w->y + BORDER + 5, x0 + 4, w->y + TITLE_H - 5, rgb(255, 255, 255));
        fill(scene, x0 + CLOSE_W - 6, w->y + BORDER + 5, x0 + CLOSE_W - 5, w->y + TITLE_H - 5, rgb(255, 255, 255));
        fill(scene, x0 + 3, w->y + TITLE_H - 6, x0 + CLOSE_W - 5, w->y + TITLE_H - 5, rgb(255, 255, 255));
    }
    {
        int x0 = title_btn(w, w->resizable ? 2 : 1);
        fill(scene, x0, w->y + BORDER + 2, x0 + CLOSE_W - 2, w->y + TITLE_H - 2, rgb(200, 160, 40));
        fill(scene, x0 + 3, w->y + TITLE_H - 7, x0 + CLOSE_W - 5, w->y + TITLE_H - 5, rgb(255, 255, 255));
    }
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
        if (w->resizable)
            draw_grip(w);
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
    if (w->resizable)
        draw_grip(w);
}

/* Reads exactly n bytes of an answer from the (non-blocking) mouse, waiting
 * a little for each; -1 if they do not come. */
static int mice_read_n(int fd, unsigned char *p, size_t n)
{
    size_t got = 0;
    while (got < n) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 300) <= 0)
            return -1;
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Asks the mouse for the wheel (the IntelliMouse sequence, then its id) and
 * returns the packet length: 4 if it answered with id 3, else 3. The
 * kernel keeps only the answer to the last byte written, so the sequence and
 * the id request are written apart. The answers are read here, so no packet
 * is mistaken for one. */
static int mice_wheel(int fd)
{
    static const unsigned char seq[] = { 0xF3, 200, 0xF3, 100, 0xF3, 80 };
    unsigned char got[2];
    if (write(fd, seq, sizeof seq) != (ssize_t)sizeof seq || mice_read_n(fd, got, 1) < 0 || got[0] != 0xFA)
        return 3;
    if (write(fd, "\xF2", 1) != 1 || mice_read_n(fd, got, 2) < 0)
        return 3;
    return got[0] == 0xFA && got[1] == 3 ? 4 : 3;
}

/* Minutes east of UTC for the clock: the TZ variable, or the first line of /etc/TZ, in the POSIX form NAME[+-]H[:MM]
 * ("JST-9" is nine hours east; "EST5" five west). No zone database: a name or no variable means UTC. */
static int tz_minutes(void)
{
    char buf[64];
    const char *tz = getenv("TZ");
    if (!tz || !*tz) {
        FILE *f = fopen("/etc/TZ", "r");
        if (!f)
            return 0;
        char *got = fgets(buf, sizeof buf, f);
        fclose(f);
        if (!got)
            return 0;
        buf[strcspn(buf, "\r\n")] = 0;
        tz = buf;
    }
    if (*tz == '<') {
        while (*tz && *tz != '>')
            tz++;
        if (*tz)
            tz++;
    } else {
        while ((*tz >= 'A' && *tz <= 'Z') || (*tz >= 'a' && *tz <= 'z'))
            tz++;
    }
    int sign = 1;
    if (*tz == '-' || *tz == '+')
        sign = *tz++ == '-' ? -1 : 1;
    int h = 0, m = 0;
    while (*tz >= '0' && *tz <= '9' && h < 100)
        h = h * 10 + *tz++ - '0';
    if (*tz == ':') {
        tz++;
        while (*tz >= '0' && *tz <= '9' && m < 100)
            m = m * 10 + *tz++ - '0';
    }
    int east = -sign * (h * 60 + m);
    return east > 14 * 60 || east < -12 * 60 ? 0 : east;
}

/* The time of day (UTC, as the system keeps it, moved by tz_minutes) for the clock in the task bar. */
static void clock_hm(int *h, int *m)
{
    static int off = 1 << 20;
    if (off == 1 << 20)
        off = tz_minutes();
    long min = ((long)(time(NULL) / 60) + off) % 1440;
    if (min < 0)
        min += 1440;
    *h = (int)(min / 60);
    *m = (int)(min % 60);
}

static char ime_buf[6];                   /* the romaji letters typed that do not make a kana yet, shown in the task bar */
static int ime_n;
static int ime_on;                        /* Alt-Space: 0 off, 1 romaji typed becomes hiragana, 2 katakana */
#define IME_W 76

/* The task bar: a button that starts a terminal, then one for each window
 * in the order they were opened, the top one lit. */
static void draw_bar(void)
{
    int y0 = (int)H - BAR_H;
    fill(scene, 0, y0, W, H, rgb(25, 25, 35));
    fill(scene, TERM_X, y0 + 3, TERM_X + TERM_W, H - 3, rgb(40, 140, 70));
    text(scene, TERM_X + 8, y0 + 4, "Term", rgb(255, 255, 255));
    int n = 0;
    for (int i = 0; i < MAXWIN; i++) {
        if (!pool[i].used)
            continue;
        int x = BTN_X0 + n * (BTN_W + BTN_GAP);
        if (x + BTN_W > (int)W - MENU_W - 12 - CLOCK_W - (ime_on ? IME_W : 0))
            break;
        fill(scene, x, y0 + 3, x + BTN_W, H - 3, &pool[i] == focus() ? rgb(40, 90, 200) : pool[i].hidden ? rgb(45, 45, 60) : rgb(70, 70, 85));
        char t[16];
        snprintf(t, sizeof t, "%.14s", pool[i].title);
        text(scene, x + 6, y0 + 4, t, rgb(255, 255, 255));
        n++;
    }
    if (ime_on) {
        int ix = (int)W - MENU_W - 12 - CLOCK_W - IME_W;
        fill(scene, ix, y0 + 3, ix + IME_W - 4, H - 3, rgb(150, 90, 30));
        char im[16];
        snprintf(im, sizeof im, "%s %.3s", ime_on == 2 ? "Kata" : "Kana", ime_buf);
        text(scene, ix + 4, y0 + 4, im, rgb(255, 255, 255));
    }
    int hh, mm;
    clock_hm(&hh, &mm);
    char ck[8];
    snprintf(ck, sizeof ck, "%02d:%02d", hh, mm);
    text(scene, (int)W - MENU_W - 12 - CLOCK_W + 4, y0 + 4, ck, rgb(255, 255, 255));
    fill(scene, (int)W - MENU_W - 4, y0 + 3, (int)W - 4, H - 3, menu_open ? rgb(40, 90, 200) : rgb(70, 70, 85));
    text(scene, (int)W - MENU_W + 2, y0 + 4, "Menu", rgb(255, 255, 255));
}

/* The launcher menu: what it starts, and where its entries are. */
#define MENU_USER 8                       /* entries read from /etc/bwm.menu */
#define MENU_MAX (MENU_USER + 5)
static char menu_label[MENU_MAX][24];
static char menu_cmd[MENU_USER][160];     /* the shell command of entry 3 + i, run in a terminal window */
static int menu_n;
static int menu_x0, menu_y0;             /* where the menu is: above the Menu button, or at a right click */
#define MENU_X0 menu_x0
#define MENU_Y0 menu_y0

/* Opens the menu with its corner at (x, y), kept inside the screen above the task bar. */
static void menu_show(int x, int y)
{
    int h = menu_n * MENU_ITEM_H;
    menu_x0 = x + MENU_ITEM_W > (int)W ? (int)W - MENU_ITEM_W : x;
    menu_y0 = y + h > (int)H - BAR_H ? (int)H - BAR_H - h : y;
    menu_open = 1;
}

/* Builds the menu: Terminal, Files and System, then a line "Label=command" of /etc/bwm.menu
 * for each program to start in a terminal window, then Hello window and Quit bwm. */
static void menu_load(void)
{
    const char *fixed[] = { "Terminal", "Files", "System" };
    menu_n = 0;
    for (int i = 0; i < 3; i++)
        snprintf(menu_label[menu_n++], sizeof menu_label[0], "%s", fixed[i]);
    FILE *f = fopen("/etc/bwm.menu", "r");
    char line[200];
    int nu = 0;
    while (f && nu < MENU_USER && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (!eq || eq == line || !eq[1] || line[0] == '#')
            continue;
        *eq = 0;
        snprintf(menu_label[menu_n++], sizeof menu_label[0], "%.23s", line);
        snprintf(menu_cmd[nu++], sizeof menu_cmd[0], "%s", eq + 1);
    }
    if (f)
        fclose(f);
    snprintf(menu_label[menu_n++], sizeof menu_label[0], "Hello window");
    snprintf(menu_label[menu_n++], sizeof menu_label[0], "Quit bwm");
}

/* The entry of the menu at (x, y), or -1. */
static int menu_at(int x, int y)
{
    if (x < MENU_X0 || x >= MENU_X0 + MENU_ITEM_W || y < MENU_Y0 || y >= MENU_Y0 + menu_n * MENU_ITEM_H)
        return -1;
    return (y - MENU_Y0) / MENU_ITEM_H;
}

static void draw_menu(void)
{
    if (!menu_open)
        return;
    int lit = menu_at(menu_mx, menu_my);
    fill(scene, MENU_X0 - 1, MENU_Y0 - 1, MENU_X0 + MENU_ITEM_W + 1, MENU_Y0 + menu_n * MENU_ITEM_H + 1, rgb(200, 200, 210));
    for (int i = 0; i < menu_n; i++) {
        int y = MENU_Y0 + i * MENU_ITEM_H;
        fill(scene, MENU_X0, y, MENU_X0 + MENU_ITEM_W, y + MENU_ITEM_H, i == lit ? rgb(40, 90, 200) : rgb(45, 45, 60));
        text(scene, MENU_X0 + 10, y + 4, menu_label[i], rgb(255, 255, 255));
    }
}

/* The window whose button is at x in the task bar, or NULL. */
static struct win *bar_window(int x)
{
    int n = 0;
    for (int i = 0; i < MAXWIN; i++) {
        if (!pool[i].used)
            continue;
        int x0 = BTN_X0 + n * (BTN_W + BTN_GAP);
        if (x >= x0 && x < x0 + BTN_W)
            return &pool[i];
        n++;
    }
    return NULL;
}

/* Desktop icons: one for each launcher entry but Hello window and Quit bwm, in a column at the left. */
#define ICON_X 12
#define ICON_Y0 12
#define ICON_W 72
#define ICON_H 64
#define ICON_MAX 8
static uint32_t icon_colour(int i)
{
    static const uint32_t c[] = { 0x001E1E1E, 0x00E6B428, 0x003CA05A };
    return i < 3 ? c[i] : 0x009658C8;
}

/* The icon at (x, y), or -1; no more icons than fit above the task bar. */
static int icon_at(int x, int y)
{
    int n = menu_n - 2 < ICON_MAX ? menu_n - 2 : ICON_MAX;
    if (x < ICON_X || x >= ICON_X + ICON_W || y < ICON_Y0)
        return -1;
    int i = (y - ICON_Y0) / ICON_H;
    return i < n && ICON_Y0 + (i + 1) * ICON_H <= (int)H - BAR_H ? i : -1;
}

static void draw_icons(void)
{
    int n = menu_n - 2 < ICON_MAX ? menu_n - 2 : ICON_MAX;
    for (int i = 0; i < n; i++) {
        int x = ICON_X, y = ICON_Y0 + i * ICON_H;
        if (y + ICON_H > (int)H - BAR_H)
            break;
        fill(scene, x + 16, y + 4, x + 56, y + 36, icon_colour(i));
        char lab[9];
        snprintf(lab, sizeof lab, "%.8s", menu_label[i]);
        text(scene, x + (ICON_W - (int)strlen(lab) * CELL_W) / 2, y + 42, lab, rgb(255, 255, 255));
    }
}

static void draw_scene(void)
{
    for (unsigned y = 0; y < H; y++) {
        uint32_t c = rgb(10 + y * 40 / H, 60 + y * 50 / H, 80 + y * 60 / H);
        for (unsigned x = 0; x < W; x++)
            scene[y * W + x] = c;
    }
    draw_icons();
    for (int i = 0; i < nz; i++)
        if (!z[i]->hidden)
            draw_win(z[i], z[i] == focus());
    draw_bar();
    draw_menu();
}

/* The pointer: '#' black outline, '.' white fill. */
static const char *arrow[] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ", "#....#      ",
    "#.....#     ", "#......#    ", "#.......#   ", "#........#  ", "#.....#####", "#..#..#     ",
    "#.# #..#    ", "##  #..#    ", "#    #..#   ", "     #..#   ", "      ##    ",
};
#define ARROW_W 12
#define ARROW_H 17

/* Which edges of a resizable window the point is on: 1 left, 2 right, 4 top, 8 bottom. */
static int edges_at(const struct win *w, int mx, int my)
{
    int edges = 0;
    if (w->resizable) {
        if (mx < w->x + EDGE) edges |= 1;
        if (mx >= w->x + w->w - EDGE) edges |= 2;
        if (my < w->y + EDGE) edges |= 4;
        if (my >= w->y + w->h - EDGE) edges |= 8;
        if (mx >= w->x + w->w - GRIP && my >= w->y + w->h - GRIP) edges |= 2 | 8;
    }
    return edges;
}

static struct win *hit(int x, int y);

/* The resize cursors are drawn, not kept as pictures: a white shape
 * (a double-headed arrow along the direction the edge moves in) with a black
 * outline, around the pointer. */
#define CUR_R 9                          /* they reach this far from the pointer */
#define CUR_BOX 20                       /* the arrow reaches 17 below and 12 right of it */

static int resize_white(int shape, int dx, int dy)
{
    if (shape == 1 || shape == 2) {                /* 1: left and right, 2: up and down */
        int a = shape == 1 ? dx : dy, b = shape == 1 ? dy : dx;
        int aa = a < 0 ? -a : a, ab = b < 0 ? -b : b;
        return aa <= 7 && (ab == 0 || (aa >= 4 && ab <= 7 - aa));
    }
    if (shape == 4)                                /* anti-diagonal: the same as the other one, mirrored */
        dy = -dy;
    if (dx > 7 || dx < -7 || dy > 7 || dy < -7)
        return 0;
    if (dx - dy >= -1 && dx - dy <= 1)
        return 1;
    return (dx > 0 && dy > 0 && dx + dy >= 10) || (dx < 0 && dy < 0 && dx + dy <= -10);
}

/* 0 for the arrow, else 1 to 4 for a resize cursor over the point. */
static int cursor_shape(int mx, int my)
{
    int e = 0;
    if (sizing)
        e = size_edges;
    else if (!menu_open && my < (int)H - BAR_H) {
        struct win *w = hit(mx, my);
        if (w)
            e = edges_at(w, mx, my);
    }
    if (e == 1 || e == 2) return 1;
    if (e == 4 || e == 8) return 2;
    if (e == (1 | 4) || e == (2 | 8)) return 3;
    if (e == (1 | 8) || e == (2 | 4)) return 4;
    return 0;
}

static void draw_cursor(int mx, int my)
{
    int shape = cursor_shape(mx, my);
    if (shape) {
        for (int dy = -CUR_R; dy <= CUR_R; dy++)
            for (int dx = -CUR_R; dx <= CUR_R; dx++) {
                int px = mx + dx, py = my + dy;
                if (px < 0 || py < 0 || px >= (int)W || py >= (int)H)
                    continue;
                int white = resize_white(shape, dx, dy), black = 0;
                for (int j = -1; j <= 1 && !white && !black; j++)
                    for (int i = -1; i <= 1; i++)
                        if (resize_white(shape, dx + i, dy + j)) {
                            black = 1;
                            break;
                        }
                if (white || black)
                    frame[py * W + px] = white ? rgb(255, 255, 255) : rgb(0, 0, 0);
            }
        return;
    }
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
static void touch_bar(void) { touch(0, (int)H - BAR_H, W, H); }

static struct win *hit(int x, int y)
{
    for (int i = nz - 1; i >= 0; i--)
        if (!z[i]->hidden && x >= z[i]->x && x < z[i]->x + z[i]->w && y >= z[i]->y && y < z[i]->y + z[i]->h)
            return z[i];
    return NULL;
}

/* The window that gets the keys: the top one that is not minimized. */
static struct win *focus(void)
{
    for (int i = nz - 1; i >= 0; i--)
        if (!z[i]->hidden)
            return z[i];
    return NULL;
}

static void raise_win(struct win *w)
{
    w->hidden = 0;
    int i = 0;
    while (i < nz && z[i] != w)
        i++;
    if (i >= nz || i == nz - 1)
        return;
    memmove(&z[i], &z[i + 1], (nz - 1 - i) * sizeof z[0]);
    z[nz - 1] = w;
}

/* ---- clients ---- */

static void to_client(int cli, uint32_t type, const void *p, uint32_t len);

static void clamp_size(int *cw, int *ch)
{
    if (*cw < MIN_CW) *cw = MIN_CW;
    if (*ch < MIN_CH) *ch = MIN_CH;
    if (*cw > (int)W - 2 * BORDER) *cw = W - 2 * BORDER;
    if (*ch > (int)H - TITLE_H - BORDER) *ch = H - TITLE_H - BORDER;
}

/* Puts the frame where the edge that is being dragged left it: a left or top
 * edge moves, so the opposite one stays. */
static void keep_edges(struct win *w)
{
    if (w->keep_r_on) {
        w->x = w->keep_r - w->w;
        if (w->x < 0) w->x = 0;
    }
    if (w->keep_b_on) {
        w->y = w->keep_b - w->h;
        if (w->y < 0) w->y = 0;
    }
}

/* Gives a window the size its grip is asking for: at once if bwm keeps its
 * contents, else by asking the program, one request at a time. */
static void request_size(struct win *w)
{
    int cw = w->want_cw, ch = w->want_ch;
    clamp_size(&cw, &ch);
    if (cw == cw_of(w) && ch == ch_of(w))
        return;
    if (w->cli < 0) {
        touch_win(w);
        w->w = cw + 2 * BORDER;
        w->h = ch + TITLE_H + BORDER;
        w->cols = (cw - 8) / CELL_W;
        w->rows = (ch - 8) / CELL_H;
        if (w->cols > MAX_COLS) w->cols = MAX_COLS;
        if (w->rows > MAX_ROWS) w->rows = MAX_ROWS;
        if (w->cols < 1) w->cols = 1;
        if (w->rows < 1) w->rows = 1;
        if (w->cx >= w->cols) w->cx = w->cols - 1;
        if (w->cy >= w->rows) w->cy = w->rows - 1;
        keep_edges(w);
        touch_win(w);
    } else if (!w->pend) {
        struct bwm_resize r = { (uint32_t)w->id, (uint32_t)cw, (uint32_t)ch };
        to_client(w->cli, BWM_RESIZE, &r, sizeof r);
        w->pend = 1;
    }
}

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

/* The window that has the mouse grabbed (a press in its contents with button 1, still held), as a client and an id. */
static int grab_cli = -1;
static uint32_t grab_id;

/* Whether button 1 was already down (OLD) and the window that grabbed the mouse is still there. */
static int grab_active(int old)
{
    return grab_cli >= 0 && (old & 1) && by_id(grab_cli, grab_id) != NULL;
}

/* The window a drag holds while the pointer rests above or below its contents, else NULL: the
 * window then gets the motion again every SCROLL_MS, so the editor goes on scrolling. */
#define SCROLL_MS 60
static long long scroll_due;
static struct win *grab_outside(int old, int my)
{
    if (!grab_active(old))
        return NULL;
    struct win *t = by_id(grab_cli, grab_id);
    if (my >= t->y + TITLE_H && my < t->y + t->h - BORDER)
        return NULL;
    return t;
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
        if (c.w < 16 || c.h < 16 || c.w > W - 2 * BORDER || c.h > H - BAR_H - TITLE_H - BORDER) {
            if (mf >= 0)
                close(mf);
            drop_client(i);
            return;
        }
        int px = 40 + 30 * (next_id % 8), py = 40 + 30 * (next_id % 8);
        if (px > (int)W - (c.w + 2 * BORDER)) px = (int)W - (c.w + 2 * BORDER);
        if (py > (int)H - BAR_H - (c.h + TITLE_H + BORDER)) py = (int)H - BAR_H - (c.h + TITLE_H + BORDER);
        struct win *w = win_new(px < 0 ? 0 : px, py < 0 ? 0 : py, c.w, c.h, c.title, i);
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
    } else if (type == BWM_RESIZABLE && len == sizeof(struct bwm_resizable)) {
        struct bwm_resizable r;
        memcpy(&r, p, sizeof r);
        struct win *w = by_id(i, r.id);
        if (w) {
            w->resizable = 1;
            touch_win(w);
        }
    } else if (type == BWM_RESIZED && len == sizeof(struct bwm_resized)) {
        struct bwm_resized r;
        memcpy(&r, p, sizeof r);
        int mf = clis[i].pend_fd;
        clis[i].pend_fd = -1;
        struct win *w = by_id(i, r.id);
        int ok = w && w->pend && r.w >= MIN_CW && r.h >= MIN_CH && r.w <= W - 2 * BORDER && r.h <= H - TITLE_H - BORDER
                 && (w->shm ? mf >= 0 : mf < 0);
        void *m = MAP_FAILED;
        size_t need = (size_t)r.w * r.h * 4;
        if (ok && w->shm) {
            struct stat st;
            if (fstat(mf, &st) == 0 && st.st_size >= 0 && (size_t)st.st_size >= need)
                m = mmap(NULL, need, PROT_READ, MAP_SHARED, mf, 0);
            ok = m != MAP_FAILED;
        }
        uint32_t *np = ok ? calloc((size_t)r.w * r.h, 4) : NULL;
        if (mf >= 0)
            close(mf);
        if (!np) {
            if (m != MAP_FAILED)
                munmap(m, need);
            drop_client(i);                                /* an answer that cannot be right */
            return;
        }
        for (size_t k = 0; k < (size_t)r.w * r.h; k++)
            np[k] = w->body;
        touch_win(w);
        free(w->pix);
        w->pix = np;
        if (w->shm) {
            munmap(w->shm, w->shm_len);
            w->shm = m;
            w->shm_len = need;
        }
        w->w = r.w + 2 * BORDER;
        w->h = r.h + TITLE_H + BORDER;
        w->pend = 0;
        keep_edges(w);
        touch_win(w);
        if (sizing == w || w->again) {
            w->again = 0;
            request_size(w);                               /* the pointer may have moved on, or a maximize came in */
        } else {
            w->keep_r_on = w->keep_b_on = 0;
        }
    } else if (type == BWM_SETTITLE && len == sizeof(struct bwm_settitle)) {
        struct bwm_settitle t;
        memcpy(&t, p, sizeof t);
        struct win *w = by_id(i, t.id);
        if (w) {
            t.title[sizeof t.title - 1] = 0;
            snprintf(w->title, sizeof w->title, "%s", t.title);
            printf("bwm: title %u %s\n", t.id, w->title);
            fflush(stdout);
            touch_win(w);
            touch_bar();
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
static int k_shift, k_ctrl, k_alt, k_caps;

/* Auto-repeat is done here, since the key events carry only presses and
 * releases: the last key pressed repeats after 400 ms, then every 40 ms,
 * until it is released. */
#define REPEAT_FIRST_MS 400
#define REPEAT_NEXT_MS  40
#define DOUBLE_CLICK_MS 400
static struct win *last_click_win;        /* the window whose title bar was clicked last, and when */
static long long last_click_ms;
static int last_click_maxed;              /* whether it was maximized before that click */
static int clock_shown = -1;              /* the minute of the day the task bar shows */
static unsigned rep_code;
static long long rep_due;                 /* when the next repeat is due, in ms; valid while rep_code != 0 */

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* With key events the terminal also gets the bytes of every key, so Ctrl-Q
 * reaches it as a ^Q a moment after the event. bwm waits for that byte
 * (or 2 s) before it ends, so the ^Q is not left for the shell. */
#define QUIT_WAIT_MS 2000
static long long quit_due;                /* when to end without the ^Q; 0 until Ctrl-Q came as an event */
static int tty_q;                         /* the ^Q came first */

/* The bytes a key press makes (at most 6) in out; returns how many. */
static int key_bytes(unsigned code, char *out)
{
    static const struct { unsigned code; char final; } nav[] = {
        { 103, 'A' }, { 108, 'B' }, { 106, 'C' }, { 105, 'D' }, { 102, 'H' }, { 107, 'F' } };
    for (unsigned i = 0; i < sizeof nav / sizeof nav[0]; i++)
        if (nav[i].code == code) {
            int mod = 1 + k_shift + 2 * k_alt + 4 * k_ctrl;      /* xterm's modifier number; sent with Ctrl or Shift, as Alt alone keeps the plain key */
            if (k_ctrl || k_shift) {
                memcpy(out, "\033[1;", 4);
                out[4] = '0' + mod;
                out[5] = nav[i].final;
                return 6;
            }
            out[0] = 27; out[1] = '['; out[2] = nav[i].final;
            return 3;
        }
    static const struct { unsigned code; const char *seq; } fkey[] = {
        { 59, "\033OP" }, { 60, "\033OQ" }, { 61, "\033OR" }, { 62, "\033OS" },
        { 63, "\033[15~" }, { 64, "\033[17~" }, { 65, "\033[18~" }, { 66, "\033[19~" },
        { 67, "\033[20~" }, { 68, "\033[21~" }, { 87, "\033[23~" }, { 88, "\033[24~" } };
    for (unsigned i = 0; i < sizeof fkey / sizeof fkey[0]; i++)
        if (fkey[i].code == code) {
            size_t n = strlen(fkey[i].seq);
            memcpy(out, fkey[i].seq, n);
            return (int)n;
        }
    if (code == 15 && k_shift) { memcpy(out, "\033[Z", 3); return 3; }   /* Shift-Tab */
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

/* Minimize: the window goes to the bottom of the stack and out of sight,
 * its task bar button brings it back. */
static void minimize(struct win *w)
{
    int i = 0;
    while (i < nz && z[i] != w)
        i++;
    if (i >= nz)
        return;
    touch_win(w);
    w->hidden = 1;
    memmove(&z[1], &z[0], i * sizeof z[0]);
    z[0] = w;
    if (sizing == w)
        sizing = NULL;
    touch_bar();
}

/* Maximize fills the screen above the task bar; doing it again puts the
 * window back where it was. */
static void toggle_max(struct win *w)
{
    touch_win(w);
    w->keep_r_on = w->keep_b_on = 0;
    if (!w->maxed) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->maxed = 1;
        w->x = 0; w->y = 0;
        w->want_cw = (int)W - 2 * BORDER;
        w->want_ch = (int)H - BAR_H - TITLE_H - BORDER;
    } else {
        w->maxed = 0;
        w->x = w->sx; w->y = w->sy;
        w->want_cw = w->sw - 2 * BORDER;
        w->want_ch = w->sh - TITLE_H - BORDER;
    }
    if (w->cli >= 0 && w->pend)
        w->again = 1;
    request_size(w);
    touch_win(w);
}

/* The close box, or Alt-F4: a client is asked to close the window (it ends
 * with the answer); bwm's own window goes at once. */
static void close_win(struct win *w)
{
    if (w->cli >= 0) {
        struct bwm_close cl = { (uint32_t)w->id };
        to_client(w->cli, BWM_CLOSE, &cl, sizeof cl);
    } else {
        touch_win(w);
        win_free(w);
    }
}

/* Fills the left (side -1) or right (side 1) half of the screen above the
 * task bar, or all of it (side 0); the maximize button, or toggle_max, puts the window back. */
static void snap_win(struct win *w, int side)
{
    touch_win(w);
    w->keep_r_on = w->keep_b_on = 0;
    if (!w->maxed) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->maxed = 1;
    }
    int half = side == 0 ? (int)W : side > 0 ? (int)W - (int)W / 2 : (int)W / 2;
    w->x = side > 0 ? (int)W / 2 : 0;
    w->y = 0;
    w->want_cw = half - 2 * BORDER;
    w->want_ch = (int)H - BAR_H - TITLE_H - BORDER;
    if (w->cli >= 0 && w->pend)
        w->again = 1;
    request_size(w);
    touch_win(w);
}

/* Starts /bin/IMAGE with argv[0] NAME (bfiles is bterm run under that name). */
static void launch_program(const char *name, const char *image)
{
    if (fork() == 0) {
        char path[64];
        char *argv[] = { (char *)name, NULL };
        snprintf(path, sizeof path, "/bin/%s", image);
        signal(SIGCHLD, SIG_DFL);
        execv(path, argv);
        _exit(127);
    }
}

/* Runs the shell command CMD in a terminal window, which waits for Enter when the command ends. */
static void launch_command(const char *cmd)
{
    if (fork() == 0) {
        char sh[256];
        char *argv[] = { "bterm", "-e", sh, NULL };
        snprintf(sh, sizeof sh, "%s; echo; echo '[done, press Enter]'; read x", cmd);
        signal(SIGCHLD, SIG_DFL);
        execv("/bin/bterm", argv);
        _exit(127);
    }
}

/* Starts menu entry IT; returns 1 for Quit bwm. */
static int launch_entry(int it)
{
    if (it == 0)
        launch_terminal();
    else if (it == 1)
        launch_program("bfiles", "bterm");
    else if (it == 2)
        launch_program("bsys", "bterm");
    else if (it >= 3 && it < menu_n - 2)
        launch_command(menu_cmd[it - 3]);
    else if (it == menu_n - 2)
        launch_program("bhello", "bhello");
    else if (it == menu_n - 1)
        return 1;
    return 0;
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

/* Romaji to hiragana or katakana (no conversion to kanji). Typed letters collect in
 * ime_buf until they spell a kana; no entry is the start of another. */
static const struct { const char *r, *k; } ime_tab[] = {
    { ",", "、" },
    { "-", "ー" },
    { ".", "。" },
    { "a", "あ" },
    { "ba", "ば" },
    { "be", "べ" },
    { "bi", "び" },
    { "bo", "ぼ" },
    { "bu", "ぶ" },
    { "bya", "びゃ" },
    { "byo", "びょ" },
    { "byu", "びゅ" },
    { "cha", "ちゃ" },
    { "che", "ちぇ" },
    { "chi", "ち" },
    { "cho", "ちょ" },
    { "chu", "ちゅ" },
    { "da", "だ" },
    { "de", "で" },
    { "di", "ぢ" },
    { "do", "ど" },
    { "du", "づ" },
    { "e", "え" },
    { "fa", "ふぁ" },
    { "fe", "ふぇ" },
    { "fi", "ふぃ" },
    { "fo", "ふぉ" },
    { "fu", "ふ" },
    { "ga", "が" },
    { "ge", "げ" },
    { "gi", "ぎ" },
    { "go", "ご" },
    { "gu", "ぐ" },
    { "gya", "ぎゃ" },
    { "gyo", "ぎょ" },
    { "gyu", "ぎゅ" },
    { "ha", "は" },
    { "he", "へ" },
    { "hi", "ひ" },
    { "ho", "ほ" },
    { "hu", "ふ" },
    { "hya", "ひゃ" },
    { "hyo", "ひょ" },
    { "hyu", "ひゅ" },
    { "i", "い" },
    { "ja", "じゃ" },
    { "je", "じぇ" },
    { "ji", "じ" },
    { "jo", "じょ" },
    { "ju", "じゅ" },
    { "ka", "か" },
    { "ke", "け" },
    { "ki", "き" },
    { "ko", "こ" },
    { "ku", "く" },
    { "kya", "きゃ" },
    { "kyo", "きょ" },
    { "kyu", "きゅ" },
    { "la", "ぁ" },
    { "le", "ぇ" },
    { "li", "ぃ" },
    { "lo", "ぉ" },
    { "ltu", "っ" },
    { "lu", "ぅ" },
    { "lya", "ゃ" },
    { "lyo", "ょ" },
    { "lyu", "ゅ" },
    { "ma", "ま" },
    { "me", "め" },
    { "mi", "み" },
    { "mo", "も" },
    { "mu", "む" },
    { "mya", "みゃ" },
    { "myo", "みょ" },
    { "myu", "みゅ" },
    { "n'", "ん" },
    { "na", "な" },
    { "ne", "ね" },
    { "ni", "に" },
    { "nn", "ん" },
    { "no", "の" },
    { "nu", "ぬ" },
    { "nya", "にゃ" },
    { "nyo", "にょ" },
    { "nyu", "にゅ" },
    { "o", "お" },
    { "pa", "ぱ" },
    { "pe", "ぺ" },
    { "pi", "ぴ" },
    { "po", "ぽ" },
    { "pu", "ぷ" },
    { "pya", "ぴゃ" },
    { "pyo", "ぴょ" },
    { "pyu", "ぴゅ" },
    { "ra", "ら" },
    { "re", "れ" },
    { "ri", "り" },
    { "ro", "ろ" },
    { "ru", "る" },
    { "rya", "りゃ" },
    { "ryo", "りょ" },
    { "ryu", "りゅ" },
    { "sa", "さ" },
    { "se", "せ" },
    { "sha", "しゃ" },
    { "she", "しぇ" },
    { "shi", "し" },
    { "sho", "しょ" },
    { "shu", "しゅ" },
    { "si", "し" },
    { "so", "そ" },
    { "su", "す" },
    { "sya", "しゃ" },
    { "syo", "しょ" },
    { "syu", "しゅ" },
    { "ta", "た" },
    { "te", "て" },
    { "ti", "ち" },
    { "to", "と" },
    { "tsu", "つ" },
    { "tu", "つ" },
    { "tya", "ちゃ" },
    { "tyo", "ちょ" },
    { "tyu", "ちゅ" },
    { "u", "う" },
    { "wa", "わ" },
    { "wo", "を" },
    { "xa", "ぁ" },
    { "xe", "ぇ" },
    { "xi", "ぃ" },
    { "xo", "ぉ" },
    { "xtsu", "っ" },
    { "xtu", "っ" },
    { "xu", "ぅ" },
    { "xwa", "ゎ" },
    { "xya", "ゃ" },
    { "xyo", "ょ" },
    { "xyu", "ゅ" },
    { "ya", "や" },
    { "yo", "よ" },
    { "yu", "ゆ" },
    { "za", "ざ" },
    { "ze", "ぜ" },
    { "zi", "じ" },
    { "zo", "ぞ" },
    { "zu", "ず" },
    { "zya", "じゃ" },
    { "zyo", "じょ" },
    { "zyu", "じゅ" },
};

static int ime_put(struct win *top, const char *s)
{
    int q = 0;
    for (; *s; s++) {
        unsigned char b2 = (unsigned char)s[0] == 0xe3 && s[1] && s[2] ? (unsigned char)s[1] : 0;
        unsigned cp = b2 ? 0x3000 | (unsigned)(b2 & 3) << 6 | ((unsigned char)s[2] & 0x3f) : 0;
        if (ime_on == 2 && cp >= 0x3041 && cp <= 0x3096) {     /* katakana is hiragana + 0x60 */
            cp += 0x60;
            q |= type_byte(top, (char)0xe3);
            q |= type_byte(top, (char)(0x80 | (cp >> 6 & 0x3f)));
            q |= type_byte(top, (char)(0x80 | (cp & 0x3f)));
            s += 2;
        } else {
            q |= type_byte(top, *s);
        }
    }
    return q;
}

/* 1 when ime_buf is a whole entry (emitted and cleared) or the start of one. */
static int ime_step(struct win *top, int *quit)
{
    for (size_t i = 0; i < sizeof ime_tab / sizeof ime_tab[0]; i++)
        if (strlen(ime_tab[i].r) == (size_t)ime_n && !memcmp(ime_tab[i].r, ime_buf, ime_n)) {
            *quit |= ime_put(top, ime_tab[i].k);
            ime_n = 0;
            return 1;
        }
    for (size_t i = 0; i < sizeof ime_tab / sizeof ime_tab[0]; i++)
        if (strlen(ime_tab[i].r) > (size_t)ime_n && !memcmp(ime_tab[i].r, ime_buf, ime_n))
            return 1;
    return 0;
}

static int ime_type1(struct win *top, char c);

/* Like type_byte, but through the romaji table while the mode is on; returns 1 for Ctrl-Q. */
static int ime_type(struct win *top, char c)
{
    int before = ime_n;
    int quit = ime_type1(top, c);
    if (ime_on && (before || ime_n)) {                 /* the pending letters in the task bar changed */
        ime_buf[ime_n] = 0;
        touch_bar();
    }
    return quit;
}

static int ime_type1(struct win *top, char c)
{
    int quit = 0;
    if (!ime_on)
        return type_byte(top, c);
    if ((c == 127 || c == 8) && ime_n > 0) {          /* Backspace takes back a pending letter */
        ime_n--;
        return 0;
    }
    int ours = (c >= 'a' && c <= 'z') || c == '-' || c == ',' || c == '.' || c == '\'';
    if (!ours) {
        if (ime_n == 1 && ime_buf[0] == 'n')
            quit |= ime_put(top, "\xe3\x82\x93");   /* ん */
        ime_n = 0;
        return quit | type_byte(top, c);
    }
    ime_buf[ime_n++] = c;
    while (ime_n > 0 && !ime_step(top, &quit)) {
        if (ime_n >= 2 && ime_buf[0] == ime_buf[1] && ime_buf[0] != 'n' && !strchr("aiueo", ime_buf[0]))
            quit |= ime_put(top, "\xe3\x81\xa3");   /* っ */
        else if (ime_buf[0] == 'n')
            quit |= ime_put(top, "\xe3\x82\x93");   /* ん */
        else
            quit |= type_byte(top, ime_buf[0]);
        memmove(ime_buf, ime_buf + 1, --ime_n);
    }
    return quit;
}

int main(int argc, char **argv)
{
    int builtin = 1;
    int a = 1;
    if (a < argc && strcmp(argv[a], "-n") == 0) {
        builtin = 0;
        a++;
    }
    unsigned mode_w = 0, mode_h = 0;
    if (a + 1 < argc && strcmp(argv[a], "-m") == 0) {                    /* -m WxH: ask the display for that mode */
        char *end;
        mode_w = strtoul(argv[a + 1], &end, 10);
        if (*end == 'x')
            mode_h = strtoul(end + 1, NULL, 10);
        a += 2;
    }
    unsigned want_w = 0, want_h = 0;
    if (a + 1 < argc && strcmp(argv[a], "-s") == 0) {
        char *end;
        want_w = strtoul(argv[a + 1], &end, 10);
        if (*end == 'x')
            want_h = strtoul(end + 1, NULL, 10);
        a += 2;
    }
    char **child = a + 1 < argc && strcmp(argv[a], "-c") == 0 ? &argv[a + 1] : NULL;
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0 && errno == ENODEV) {
        puts("bwm: no screen (ENODEV)");
        return 2;
    }
    int mice = open("/dev/input/mice", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (mice < 0)
        mice = open("/dev/input/mice", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int pktlen = mice >= 0 ? mice_wheel(mice) : 3;           /* 4 with a wheel */
    int keys = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct var_info v;
    struct fix_info f;
    if (fb < 0 || ioctl(fb, 0x4600, &v) != 0 || ioctl(fb, 0x4602, &f) != 0 || v.bits_per_pixel != 32) {
        printf("bwm: no usable /dev/fb0 (errno %d)\n", errno);
        return 1;
    }
    if (mode_w && (mode_w != v.xres || mode_h != v.yres)) {
        v.xres = v.xres_virtual = mode_w;
        v.yres = v.yres_virtual = mode_h;
        if (ioctl(fb, 0x4601, &v) != 0 || ioctl(fb, 0x4600, &v) != 0 || ioctl(fb, 0x4602, &f) != 0)
            printf("bwm: cannot set the mode %ux%u (errno %d)\n", mode_w, mode_h, errno);
    }
    W = v.xres; H = v.yres; PITCH = f.line_length;
    if (want_w >= 320 && want_w < W && want_h >= 240 && want_h < H) {    /* pretend the screen is smaller */
        W = want_w;
        H = want_h;
    }
    static char dims[48];
    snprintf(dims, sizeof dims, "BWM_SCREEN=%ux%u", W, H);
    putenv(dims);                            /* clients size their first window to fit */
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
    menu_load();
    draw_scene();
    flush(0, 0, W, H, mx, my);
    printf("bwm: ready %ux%u\n", W, H);
    fflush(stdout);
    if (child && fork() == 0) {                               /* -c PROGRAM [ARGS]: start a client */
        const char *img = strcmp(child[0], "bfiles") == 0 || strcmp(child[0], "bsys") == 0 || strcmp(child[0], "bview") == 0 ? "bterm" : child[0];     /* bfiles is bterm under that name */
        execv(img[0] == '/' ? img : (snprintf(path, sizeof path, "/bin/%s", img), path), child);
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
        int wait = -1;
        if (rep_code) {
            long long d = rep_due - now_ms();
            wait = d < 0 ? 0 : (int)d;
        }
        if (quit_due) {
            long long d = quit_due - now_ms();
            if (wait < 0 || d < wait)
                wait = d < 0 ? 0 : (int)d;
        }
        if (grab_outside(buttons, my)) {
            long long d = scroll_due - now_ms();
            if (wait < 0 || d < wait)
                wait = d < 0 ? 0 : (int)d;
        }
        int until_min = (int)(60 - time(NULL) % 60) * 1000;      /* the clock changes at the next minute */
        if (wait < 0 || until_min < wait)
            wait = until_min;
        if (poll(p, np, wait) < 0 && errno != EINTR)
            break;
        {
            struct win *g = grab_outside(buttons, my);
            if (g && g == focus() && now_ms() >= scroll_due) {
                struct bwm_mouse ev = { (uint32_t)g->id, (uint32_t)(mx - g->x - BORDER), (uint32_t)(my - g->y - TITLE_H), (uint32_t)buttons & 7u };
                to_client(g->cli, BWM_MOUSE, &ev, sizeof ev);
                scroll_due = now_ms() + SCROLL_MS;
            }
        }
        int ox = mx, oy = my;
        dx0 = W; dy0 = H; dx1 = 0; dy1 = 0;
        int clock_h, clock_m;
        clock_hm(&clock_h, &clock_m);
        if (clock_h * 60 + clock_m != clock_shown) {
            clock_shown = clock_h * 60 + clock_m;
            touch_bar();
        }
        struct win *top = focus();
        if (p[0].revents & POLLIN) {
            char b[32];
            ssize_t n = read(0, b, sizeof b);
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n; i++) {
                if (keys >= 0) {                 /* with key events, the terminal's bytes are the same keys again */
                    if (b[i] == 17) {
                        if (quit_due)
                            running = 0;
                        else
                            tty_q = 1;
                    }
                } else if (type_byte(top, b[i])) {
                    running = 0;
                }
            }
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
                    } else if (code == 56 || code == 100) {
                        k_alt = down;
                    } else if (code == 15 && down && k_alt) {      /* Alt-Tab: the window at the bottom comes up */
                        if (nz > 1) {
                            if (k_shift) {                              /* the other way: the top one goes to the bottom */
                                struct win *t = z[nz - 1];
                                memmove(&z[1], &z[0], (nz - 1) * sizeof z[0]);
                                z[0] = t;
                            } else {
                                raise_win(z[0]);
                            }
                            touch_all();
                        }
                    } else if ((code == 105 || code == 106 || code == 103) && down && k_alt) {
                        struct win *t = focus();                    /* Alt-Left, Alt-Right: half the screen; Alt-Up: all of it */
                        if (t && t->resizable) {
                            raise_win(t);
                            snap_win(t, code == 103 ? 0 : code == 106 ? 1 : -1);
                            touch_all();
                        }
                    } else if (code == 67 && down && k_alt) {      /* Alt-F9: minimize the focused window */
                        struct win *t = focus();
                        if (t)
                            minimize(t);
                        touch_all();
                    } else if (code == 108 && down && k_alt) {     /* Alt-Down: a snapped or maximized window goes back, any other is minimized */
                        struct win *t = focus();
                        if (t && t->maxed)
                            toggle_max(t);
                        else if (t)
                            minimize(t);
                        touch_all();
                    } else if (down && k_shift && (code == 110 || (code == 47 && k_ctrl))) {
                        if (top && top->cli >= 0) {                /* Shift-Insert, Ctrl-Shift-V: paste (key 0x100, for bterm) */
                            struct bwm_key pk = { (uint32_t)top->id, BWM_KEY_PASTE };
                            to_client(top->cli, BWM_KEY, &pk, sizeof pk);
                        }
                    } else if (down && k_shift && (code == 104 || code == 109)) {
                        if (top && top->cli >= 0) {                /* Shift-Page Up and Down: scroll back (keys 0x101 and 0x102, for bterm) */
                            struct bwm_key sk = { (uint32_t)top->id, code == 104 ? BWM_KEY_SCROLL_UP : BWM_KEY_SCROLL_DOWN };
                            to_client(top->cli, BWM_KEY, &sk, sizeof sk);
                        }
                    } else if (code == 68 && down && k_alt) {      /* Alt-F10: maximize the focused window, or put it back */
                        struct win *t = focus();
                        if (t && t->resizable) {
                            raise_win(t);
                            toggle_max(t);
                            touch_all();
                        }
                    } else if (code == 62 && down && k_alt) {      /* Alt-F4: close the focused window */
                        struct win *t = focus();
                        if (t)
                            close_win(t);
                    } else if (code == 57 && down && k_alt) {      /* Alt-Space: off, romaji to hiragana, romaji to katakana */
                        ime_on = (ime_on + 1) % 3;                 /* off, hiragana, katakana */
                        ime_n = 0;
                        touch_bar();
                    } else if (code == 58 && down) {
                        k_caps = !k_caps;
                    } else if (down) {
                        char out[8];
                        int m = key_bytes(code, out);
                        for (int j = 0; j < m; j++)
                            if (ime_type(top, out[j]))
                                quit_due = now_ms() + QUIT_WAIT_MS;
                        if (m > 0 && ev[i].value == 1 && out[0] != 17 && out[0] != 20) {
                            rep_code = code;
                            rep_due = now_ms() + REPEAT_FIRST_MS;
                        }
                    }
                    if (!down && code == rep_code)
                        rep_code = 0;
                }
            }
        }
        if (quit_due && (tty_q || now_ms() >= quit_due))
            running = 0;
        if (rep_code && now_ms() >= rep_due) {
            char out[8];
            int m = key_bytes(rep_code, out);
            for (int j = 0; j < m; j++)
                if (ime_type(top, out[j]))
                    quit_due = now_ms() + QUIT_WAIT_MS;
            rep_due += REPEAT_NEXT_MS;
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
            unsigned char m[4] = { 0, 0, 0, 0 };
            while (read(mice, m, pktlen) == pktlen) {
                int dx = (signed char)m[1], dy = (signed char)m[2];
                int wheel = pktlen == 4 ? (signed char)m[3] : 0;
                mx += dx; my -= dy;
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if (mx >= (int)W) mx = W - 1;
                if (my >= (int)H) my = H - 1;
                int down = m[0] & 1;
                int in_bar = my >= (int)H - BAR_H;
                struct win *under = in_bar ? NULL : hit(mx, my);
                if (menu_open && (dx || dy)) {
                    touch(MENU_X0 - 1, MENU_Y0 - 1, MENU_X0 + MENU_ITEM_W + 1, MENU_Y0 + menu_n * MENU_ITEM_H + 1);
                    menu_mx = mx; menu_my = my;
                }
                if (down && !(buttons & 1) && menu_open) {
                    int it = menu_at(mx, my);            /* a click chooses an entry or, anywhere else, closes the menu */
                    menu_open = 0;
                    touch_all();
                    if (launch_entry(it))
                        running = 0;
                    buttons = m[0];
                    continue;
                }
                if ((m[0] & 2) && !(buttons & 2) && !in_bar && !under && !menu_open) {
                    menu_show(mx, my);                   /* a right click on the desktop opens the menu there */
                    menu_mx = mx; menu_my = my;
                    touch_all();
                } else if (down && !(buttons & 1) && in_bar && mx >= (int)W - MENU_W - 4) {
                    menu_show((int)W - 4 - MENU_ITEM_W, (int)H - BAR_H - menu_n * MENU_ITEM_H);
                    menu_mx = mx; menu_my = my;
                    touch_all();
                } else if (down && !(buttons & 1) && in_bar) {
                    if (mx >= TERM_X && mx < TERM_X + TERM_W) {
                        launch_terminal();
                    } else {
                        struct win *bw = bar_window(mx);
                        if (bw && (bw != focus() || bw->hidden)) {
                            raise_win(bw);
                            touch_all();
                        }
                    }
                } else if (down && !(buttons & 1) && under) {
                    gx = mx - under->x;
                    gy = my - under->y;
                    int on_close = my < under->y + TITLE_H && mx >= under->x + under->w - BORDER - CLOSE_W && mx < under->x + under->w - BORDER - 2;
                    int in_title = my >= under->y + BORDER && my < under->y + TITLE_H;
                    int on_max = under->resizable && in_title && mx >= title_btn(under, 1) && mx < title_btn(under, 1) + CLOSE_W - 2;
                    int on_min = in_title && mx >= title_btn(under, under->resizable ? 2 : 1) && mx < title_btn(under, under->resizable ? 2 : 1) + CLOSE_W - 2;
                    int edges = edges_at(under, mx, my);
                    if (on_close) {
                        close_win(under);
                        under = NULL;
                    } else if (on_max || on_min) {
                        if (on_max) {
                            raise_win(under);
                            toggle_max(under);
                        } else {
                            minimize(under);
                        }
                        touch_all();
                        under = NULL;
                    } else {
                        if (under != focus()) {
                            raise_win(under);
                            touch_all();
                        }
                        int on_title = my < under->y + TITLE_H && !edges;
                        long long now = now_ms();
                        int dbl = under->resizable && on_title && under == last_click_win && now - last_click_ms < DOUBLE_CLICK_MS;
                        if (dbl) {                               /* a double click on the title bar: maximize or restore */
                            under->maxed = last_click_maxed;
                            toggle_max(under);
                            touch_all();
                            last_click_win = NULL;
                        } else {
                            last_click_win = on_title ? under : NULL;
                            last_click_ms = now;
                            last_click_maxed = under->maxed;
                        }
                        drag = !dbl && my < under->y + TITLE_H ? under : NULL;
                        if (drag)
                            under->maxed = 0;                    /* moved by hand: no longer "maximized" */
                        if (edges) {
                            sizing = under;
                            under->maxed = 0;
                            size_edges = edges;
                            sox = edges & 1 ? mx - under->x : under->x + under->w - mx;
                            soy = edges & 4 ? my - under->y : under->y + under->h - my;
                            under->want_cw = cw_of(under);
                            under->want_ch = ch_of(under);
                            under->keep_r_on = edges & 1;
                            under->keep_r = under->x + under->w;
                            under->keep_b_on = (edges & 4) != 0;
                            under->keep_b = under->y + under->h;
                            drag = NULL;
                        }
                    }
                } else if (down && !(buttons & 1) && !in_bar && icon_at(mx, my) >= 0) {
                    if (launch_entry(icon_at(mx, my)))
                        running = 0;
                } else if (!down) {
                    drag = NULL;
                    if (sizing && !sizing->pend)
                        sizing->keep_r_on = sizing->keep_b_on = 0;     /* else the answer lets go of them */
                    sizing = NULL;
                }
                if (drag && down) {
                    touch_win(drag);
                    drag->x = mx - gx; drag->y = my - gy;
                    /* The title bar stays reachable: some of it on screen, all of it above the task bar. */
                    if (drag->x > (int)W - 60) drag->x = (int)W - 60;
                    if (drag->x < 60 - drag->w) drag->x = 60 - drag->w;
                    if (drag->y > (int)H - BAR_H - TITLE_H) drag->y = (int)H - BAR_H - TITLE_H;
                    if (drag->y < 0) drag->y = 0;
                    touch_win(drag);
                } else if (sizing && down) {
                    if (size_edges & 2)
                        sizing->want_cw = mx + sox - sizing->x - 2 * BORDER;
                    else if (size_edges & 1)
                        sizing->want_cw = sizing->keep_r - (mx - sox) - 2 * BORDER;
                    if (size_edges & 8)
                        sizing->want_ch = my + soy - sizing->y - TITLE_H - BORDER;
                    else if (size_edges & 4)
                        sizing->want_ch = sizing->keep_b - (my - soy) - TITLE_H - BORDER;
                    request_size(sizing);
                } else if (grab_active(buttons) || (under && under->cli >= 0 && my >= under->y + TITLE_H)) {
                    /* While button 1 is held after a press in a window's contents, that window gets the motion
                     * wherever the pointer is, as signed coordinates (the editor scrolls when a drag leaves it). */
                    struct win *tgt = grab_active(buttons) ? by_id(grab_cli, grab_id) : under;
                    if (tgt && (dx || dy || wheel || (m[0] & 7) != (buttons & 7))) {
                        /* the wheel (negative is up) goes in bits 8 to 15 of the buttons, as a signed byte */
                        struct bwm_mouse ev = { (uint32_t)tgt->id, (uint32_t)(mx - tgt->x - BORDER), (uint32_t)(my - tgt->y - TITLE_H), (m[0] & 7u) | ((uint32_t)(wheel & 0xFF) << 8) };
                        if (tgt == focus() || wheel)
                            to_client(tgt->cli, BWM_MOUSE, &ev, sizeof ev);
                    }
                }
                if (!(m[0] & 1))
                    grab_cli = -1;
                else if (!(buttons & 1) && under && under->cli >= 0 && my >= under->y + TITLE_H && !drag && !sizing)
                    grab_cli = under->cli, grab_id = (uint32_t)under->id;
                buttons = m[0];
            }
        }
        if (dx0 < dx1) {
            draw_scene();
            if (flush(dx0, dy0, dx1, dy1, mx, my) != 0)
                break;
            if (flush(ox - CUR_R, oy - CUR_R, ox + CUR_BOX, oy + CUR_BOX, mx, my) != 0)
                break;
        } else if (mx != ox || my != oy) {
            if (flush(ox - CUR_R, oy - CUR_R, ox + CUR_BOX, oy + CUR_BOX, mx, my) != 0)
                break;
        }
        if (mx != ox || my != oy)
            flush(mx - CUR_R, my - CUR_R, mx + CUR_BOX, my + CUR_BOX, mx, my);
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
