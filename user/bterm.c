/* bterm, a terminal window for bwm (user/bwmproto.h): an 80 x 24 grid of
 * 8 x 16 cells drawn with the console's font, with BusyBox's ash on a
 * pseudo-terminal behind it. What bwm sends as keys goes to the shell;
 * what the shell writes is read as a VT100 subset: CR, LF, backspace, tab,
 * the cursor moves (CSI A B C D G H d f, ESC 7 8), erase (J K X), insert and
 * delete of characters (@ P) and of lines (L M), scrolling (S T, ESC M D E)
 * inside a scroll region (r), the alternate screen (CSI ? 1049 h and l) and
 * colours (m): enough for vi and other full-screen programs. Says "bterm: window ID" when the
 * window is open and "bterm: shell exited" when the shell ends. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

#include "bwmlib.h"
#include "font8x8.h"

#define COLS 80
#define ROWS 24
#define CW 8
#define CH 16
#define WIDTH (COLS * CW)
#define HEIGHT (ROWS * CH)

static const uint32_t palette[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

struct cell { unsigned char ch, fg, bg; };
static struct cell grid[ROWS][COLS];
static uint32_t pix[WIDTH * HEIGHT];
static unsigned char dirty[ROWS];
static int cx, cy, cur_fg = 7, cur_bg = 0, bold, reverse;
static int state, params[8], nparam;      /* 0 ground, 1 after ESC, 2 in CSI */
static int priv;                          /* the CSI had a '?' prefix */
static int top, bot = ROWS - 1;           /* the scroll region, inclusive */
static int saved_cx, saved_cy;
static struct cell main_grid[ROWS][COLS];
static int main_cx, main_cy, alt;

static void put_cell(int r, int c, unsigned char ch)
{
    grid[r][c].ch = ch;
    grid[r][c].fg = cur_fg + (bold && cur_fg < 8 ? 8 : 0);
    grid[r][c].bg = cur_bg;
    dirty[r] = 1;
}

static void clear_cells(int r, int c0, int c1)
{
    for (int c = c0; c < c1; c++) {
        grid[r][c].ch = ' ';
        grid[r][c].fg = 7;
        grid[r][c].bg = cur_bg;
    }
    dirty[r] = 1;
}

/* Scrolls the lines top..bot up by n (down when n < 0). */
static void scroll_region(int n)
{
    int h = bot - top + 1;
    int k = n < 0 ? -n : n;
    if (k > h)
        k = h;
    if (n > 0)
        memmove(grid[top], grid[top + k], sizeof grid[0] * (h - k));
    else
        memmove(grid[top + k], grid[top], sizeof grid[0] * (h - k));
    for (int i = 0; i < k; i++)
        clear_cells(n > 0 ? bot - i : top + i, 0, COLS);
    for (int r = top; r <= bot; r++)
        dirty[r] = 1;
}

static void linefeed(void)
{
    if (cy == bot)
        scroll_region(1);
    else if (cy < ROWS - 1)
        cy++;
}

static void reverse_index(void)
{
    if (cy == top)
        scroll_region(-1);
    else if (cy > 0)
        cy--;
}

static void set_alt(int on)
{
    if (on == alt)
        return;
    alt = on;
    if (on) {
        memcpy(main_grid, grid, sizeof grid);
        main_cx = cx;
        main_cy = cy;
        for (int r = 0; r < ROWS; r++)
            clear_cells(r, 0, COLS);
    } else {
        memcpy(grid, main_grid, sizeof grid);
        cx = main_cx;
        cy = main_cy;
    }
    top = 0;
    bot = ROWS - 1;
    memset(dirty, 1, sizeof dirty);
}

static void draw_cell(int r, int c, int cursor)
{
    struct cell *e = &grid[r][c];
    int fg = e->fg, bg = e->bg;
    uint32_t f = palette[fg & 15], b = palette[bg & 15];
    if (cursor) {
        uint32_t t = f;
        f = b;
        b = t;
        if (f == b)
            f = 0xFFFFFF;
    }
    int ch = e->ch < 128 ? e->ch : ' ';
    for (int y = 0; y < CH; y++) {
        unsigned bits = font8x8[ch * 8 + y / 2];
        for (int x = 0; x < CW; x++, bits >>= 1)
            pix[(r * CH + y) * WIDTH + c * CW + x] = bits & 1 ? f : b;
    }
}

static int param(int i, int dflt)
{
    return i < nparam && params[i] > 0 ? params[i] : dflt;
}

static void csi(int final)
{
    int n = param(0, 1);
    if (priv) {
        if (final == 'h' || final == 'l')
            for (int i = 0; i < nparam; i++)
                if (params[i] == 1049 || params[i] == 1047 || params[i] == 47)
                    set_alt(final == 'h');
        return;
    }
    switch (final) {
    case 'G': cx = n - 1 < 0 ? 0 : n - 1 >= COLS ? COLS - 1 : n - 1; break;
    case 'd': cy = n - 1 < 0 ? 0 : n - 1 >= ROWS ? ROWS - 1 : n - 1; break;
    case 'X':
        clear_cells(cy, cx, cx + n > COLS ? COLS : cx + n);
        break;
    case 'S': scroll_region(n); break;
    case 'T': scroll_region(-n); break;
    case 'L':
    case 'M':
        if (cy >= top && cy <= bot) {
            int t = top;
            top = cy;
            scroll_region(final == 'M' ? n : -n);
            top = t;
        }
        break;
    case 'r': {
        int t = param(0, 1) - 1, b = param(1, ROWS) - 1;
        if (t < 0) t = 0;
        if (b >= ROWS) b = ROWS - 1;
        if (t < b) {
            top = t;
            bot = b;
            cx = 0;
            cy = 0;
        }
        break;
    }
    case 'A': cy = cy - n < 0 ? 0 : cy - n; break;
    case 'B': cy = cy + n >= ROWS ? ROWS - 1 : cy + n; break;
    case 'C': cx = cx + n >= COLS ? COLS - 1 : cx + n; break;
    case 'D': cx = cx - n < 0 ? 0 : cx - n; break;
    case 'H': case 'f':
        cy = param(0, 1) - 1;
        cx = param(1, 1) - 1;
        if (cy < 0) cy = 0;
        if (cy >= ROWS) cy = ROWS - 1;
        if (cx < 0) cx = 0;
        if (cx >= COLS) cx = COLS - 1;
        break;
    case 'J':
        if (nparam == 0 || params[0] == 0) {
            clear_cells(cy, cx, COLS);
            for (int r = cy + 1; r < ROWS; r++)
                clear_cells(r, 0, COLS);
        } else if (params[0] == 2 || params[0] == 3) {
            for (int r = 0; r < ROWS; r++)
                clear_cells(r, 0, COLS);
        } else if (params[0] == 1) {
            for (int r = 0; r < cy; r++)
                clear_cells(r, 0, COLS);
            clear_cells(cy, 0, cx + 1);
        }
        break;
    case 'K':
        if (nparam == 0 || params[0] == 0)
            clear_cells(cy, cx, COLS);
        else if (params[0] == 1)
            clear_cells(cy, 0, cx + 1);
        else
            clear_cells(cy, 0, COLS);
        break;
    case '@':
        for (int c = COLS - 1; c >= cx + n; c--)
            grid[cy][c] = grid[cy][c - n];
        clear_cells(cy, cx, cx + n > COLS ? COLS : cx + n);
        break;
    case 'P':
        if (n > COLS - cx)
            n = COLS - cx;
        for (int c = cx; c + n < COLS; c++)
            grid[cy][c] = grid[cy][c + n];
        clear_cells(cy, COLS - n, COLS);
        break;
    case 'm':
        if (nparam == 0)
            params[nparam++] = 0;
        for (int i = 0; i < nparam; i++) {
            int p = params[i];
            if (p == 0) { cur_fg = 7; cur_bg = 0; bold = 0; reverse = 0; }
            else if (p == 1) bold = 1;
            else if (p == 22) bold = 0;
            else if (p == 7) { int t = cur_fg; cur_fg = cur_bg; cur_bg = t; reverse = !reverse; }
            else if (p >= 30 && p <= 37) cur_fg = p - 30;
            else if (p == 39) cur_fg = 7;
            else if (p >= 40 && p <= 47) cur_bg = p - 40;
            else if (p == 49) cur_bg = 0;
            else if (p >= 90 && p <= 97) cur_fg = p - 90 + 8;
        }
        break;
    default:
        break;                                    /* a sequence this terminal does not know */
    }
}

static void feed(unsigned char b)
{
    if (state == 1) {
        state = 0;
        if (b == '[') {
            state = 2;
            nparam = 0;
            priv = 0;
            memset(params, 0, sizeof params);
        } else if (b == 'M') {
            reverse_index();
        } else if (b == 'D') {
            linefeed();
        } else if (b == 'E') {
            cx = 0;
            linefeed();
        } else if (b == '7') {
            saved_cx = cx;
            saved_cy = cy;
        } else if (b == '8') {
            cx = saved_cx;
            cy = saved_cy;
        }
        return;
    }
    if (state == 2) {
        if (b >= '0' && b <= '9') {
            if (nparam == 0)
                nparam = 1;
            params[nparam - 1] = params[nparam - 1] * 10 + (b - '0');
        } else if (b == ';') {
            if (nparam == 0)
                nparam = 1;
            if (nparam < 8)
                nparam++;
        } else if (b == '?') {
            priv = 1;
        } else if (b == '>' || b == '=') {
            priv = 1;                             /* ignored at its final byte */
        } else if (b >= 0x40 && b <= 0x7E) {
            csi(b);
            state = 0;
        }
        return;
    }
    if (b == 27) {
        state = 1;
    } else if (b == '\r') {
        cx = 0;
    } else if (b == '\n') {
        linefeed();
    } else if (b == 8) {
        if (cx > 0)
            cx--;
    } else if (b == '\t') {
        cx = (cx + 8) & ~7;
        if (cx >= COLS)
            cx = COLS - 1;
    } else if (b >= 32 && b < 127) {
        if (cx >= COLS) {
            cx = 0;
            linefeed();
        }
        put_cell(cy, cx, b);
        cx++;
        if (cx >= COLS)
            cx = COLS;                            /* wraps when the next character comes */
    }
}

/* Redraws the rows that changed (and the cursor's) and sends them. */
static int repaint(int fd, int id, int *old_cx, int *old_cy)
{
    dirty[*old_cy] = 1;
    int ccx = cx >= COLS ? COLS - 1 : cx;
    dirty[cy] = 1;
    for (int r = 0; r < ROWS; r++) {
        if (!dirty[r])
            continue;
        for (int c = 0; c < COLS; c++)
            draw_cell(r, c, r == cy && c == ccx);
        dirty[r] = 0;
        if (bwm_draw(fd, id, pix, WIDTH, 0, r * CH, WIDTH, CH) != 0)    /* a row: 640 pixels, 2560 bytes */
            return -1;
    }
    *old_cx = ccx;
    *old_cy = cy;
    return 0;
}

int main(void)
{
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bterm: no window server");
        return 2;
    }
    int id = bwm_create(fd, WIDTH, HEIGHT, "Terminal");
    if (id < 0) {
        puts("bterm: cannot open a window");
        return 1;
    }
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) {
        puts("bterm: no pseudo-terminal");
        return 1;
    }
    char *slave = ptsname(m);
    struct winsize ws = { ROWS, COLS, WIDTH, HEIGHT };
    ioctl(m, TIOCSWINSZ, &ws);
    pid_t child = fork();
    if (child == 0) {
        setsid();
        int s = open(slave, O_RDWR);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        close(m);
        close(fd);
        setenv("TERM", "vt100", 1);
        setenv("HOME", "/", 1);
        execl("/bin/ash", "ash", (char *)NULL);
        _exit(127);
    }
    for (int r = 0; r < ROWS; r++)
        clear_cells(r, 0, COLS);
    memset(dirty, 1, sizeof dirty);
    int ocx = 0, ocy = 0;
    repaint(fd, id, &ocx, &ocy);
    printf("bterm: window %d\n", id);
    fflush(stdout);
    for (;;) {
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { m, POLLIN, 0 } };
        if (poll(p, 2, -1) < 0)
            continue;
        if (p[1].revents & (POLLIN | POLLHUP)) {
            unsigned char b[512];
            ssize_t n = read(m, b, sizeof b);
            if (n <= 0) {
                puts("bterm: shell exited");
                break;
            }
            for (ssize_t i = 0; i < n; i++)
                feed(b[i]);
            if (repaint(fd, id, &ocx, &ocy) != 0)
                break;
        }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            unsigned char buf[BWM_MAX_PAYLOAD];
            int t = bwm_event(fd, buf);
            if (t == BWM_KEY) {
                struct bwm_key *k = (struct bwm_key *)buf;
                unsigned char c = k->ch;
                if (write(m, &c, 1) != 1)
                    break;
            } else if (t == BWM_CLOSE || t < 0) {
                break;
            }
        }
    }
    kill(child, SIGHUP);
    waitpid(child, NULL, 0);
    return 0;
}
