/* bterm, a terminal window for bwm (user/bwmproto.h): a grid of 8 x 16
 * cells (80 x 24 to begin with; dragging the grip of the window changes it)
 * drawn with the console's font, with BusyBox's ash on a pseudo-terminal
 * behind it. What bwm sends as keys goes to the shell;
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
#include "bfiles.h"

#define MAXC 160                          /* the most columns and rows the grid can have */
#define MAXR 60
#define CW 8
#define CH 16
#define MINC 10
#define MINR 4

static int cols = 80, rows = 24;          /* the size of the grid now */
#define WIDTH (cols * CW)
#define HEIGHT (rows * CH)

static const uint32_t palette[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

struct cell { unsigned char ch, fg, bg; };
static struct cell grid[MAXR][MAXC];
static uint32_t *pix;            /* the window's pixels: bwm's shared memory when it gave some */
static int shared;
static unsigned char dirty[MAXR];
static int cx, cy, cur_fg = 7, cur_bg = 0, bold, reverse;
static int state, params[8], nparam;      /* 0 ground, 1 after ESC, 2 in CSI */
static int priv;                          /* the CSI had a '?' prefix */
static int top, bot = 23;           /* the scroll region, inclusive */
static int saved_cx, saved_cy;
static struct cell main_grid[MAXR][MAXC];
static int main_cx, main_cy, alt;
#define HIST 200                          /* lines that scrolled off the top, kept for the wheel */
static struct cell hist[HIST][MAXC];
static int hist_n, hist_head;             /* how many are kept; where the next one goes */
static int back;                          /* how many lines the view is scrolled back (0 is the live screen) */
static int sel_on, selecting;             /* a selection is lit; the button is still down */
static int sel_ar, sel_ac, sel_br, sel_bc;   /* where it began and where the pointer is, as cells */
#define CLIP_FILE "/tmp/bwm.clip"         /* what is selected, for any terminal to paste */

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
    if (n > 0 && top == 0 && !alt) {
        for (int i = 0; i < k; i++) {
            memcpy(hist[hist_head], grid[i], sizeof hist[0]);
            hist_head = (hist_head + 1) % HIST;
            if (hist_n < HIST)
                hist_n++;
            if (back > 0 && back < hist_n)
                back++;                       /* a scrolled-back view stays on the same text */
        }
    }
    if (n > 0)
        memmove(grid[top], grid[top + k], sizeof grid[0] * (h - k));
    else
        memmove(grid[top + k], grid[top], sizeof grid[0] * (h - k));
    for (int i = 0; i < k; i++)
        clear_cells(n > 0 ? bot - i : top + i, 0, cols);
    for (int r = top; r <= bot; r++)
        dirty[r] = 1;
}

static void linefeed(void)
{
    if (cy == bot)
        scroll_region(1);
    else if (cy < rows - 1)
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
    back = 0;
    if (on) {
        memcpy(main_grid, grid, sizeof grid);
        main_cx = cx;
        main_cy = cy;
        for (int r = 0; r < rows; r++)
            clear_cells(r, 0, cols);
    } else {
        memcpy(grid, main_grid, sizeof grid);
        cx = main_cx;
        cy = main_cy;
    }
    top = 0;
    bot = rows - 1;
    memset(dirty, 1, sizeof dirty);
}

/* Whether cell (r, c) is inside the selection (reading order). */
static int selected(int r, int c)
{
    if (!sel_on)
        return 0;
    int ar = sel_ar, ac = sel_ac, br = sel_br, bc = sel_bc;
    if (br < ar || (br == ar && bc < ac)) {
        int t = ar; ar = br; br = t;
        t = ac; ac = bc; bc = t;
    }
    if (r < ar || r > br)
        return 0;
    if (r == ar && c < ac)
        return 0;
    if (r == br && c > bc)
        return 0;
    return 1;
}

/* The cells drawn on row r: the live screen, or lines of the history when
 * the view is scrolled back. */
static const struct cell *view_row(int r)
{
    int v = r - back;
    if (v >= 0)
        return grid[v];
    return hist[(hist_head + HIST + v) % HIST];
}

static void draw_cell(int r, int c, int cursor)
{
    const struct cell *e = &view_row(r)[c];
    int fg = e->fg, bg = e->bg;
    uint32_t f = palette[fg & 15], b = palette[bg & 15];
    if (!back && (cursor || selected(r, c))) {
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
    case 'G': cx = n - 1 < 0 ? 0 : n - 1 >= cols ? cols - 1 : n - 1; break;
    case 'd': cy = n - 1 < 0 ? 0 : n - 1 >= rows ? rows - 1 : n - 1; break;
    case 'X':
        clear_cells(cy, cx, cx + n > cols ? cols : cx + n);
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
        int t = param(0, 1) - 1, b = param(1, rows) - 1;
        if (t < 0) t = 0;
        if (b >= rows) b = rows - 1;
        if (t < b) {
            top = t;
            bot = b;
            cx = 0;
            cy = 0;
        }
        break;
    }
    case 'A': cy = cy - n < 0 ? 0 : cy - n; break;
    case 'B': cy = cy + n >= rows ? rows - 1 : cy + n; break;
    case 'C': cx = cx + n >= cols ? cols - 1 : cx + n; break;
    case 'D': cx = cx - n < 0 ? 0 : cx - n; break;
    case 'H': case 'f':
        cy = param(0, 1) - 1;
        cx = param(1, 1) - 1;
        if (cy < 0) cy = 0;
        if (cy >= rows) cy = rows - 1;
        if (cx < 0) cx = 0;
        if (cx >= cols) cx = cols - 1;
        break;
    case 'J':
        if (nparam == 0 || params[0] == 0) {
            clear_cells(cy, cx, cols);
            for (int r = cy + 1; r < rows; r++)
                clear_cells(r, 0, cols);
        } else if (params[0] == 2 || params[0] == 3) {
            for (int r = 0; r < rows; r++)
                clear_cells(r, 0, cols);
        } else if (params[0] == 1) {
            for (int r = 0; r < cy; r++)
                clear_cells(r, 0, cols);
            clear_cells(cy, 0, cx + 1);
        }
        break;
    case 'K':
        if (nparam == 0 || params[0] == 0)
            clear_cells(cy, cx, cols);
        else if (params[0] == 1)
            clear_cells(cy, 0, cx + 1);
        else
            clear_cells(cy, 0, cols);
        break;
    case '@':
        for (int c = cols - 1; c >= cx + n; c--)
            grid[cy][c] = grid[cy][c - n];
        clear_cells(cy, cx, cx + n > cols ? cols : cx + n);
        break;
    case 'P':
        if (n > cols - cx)
            n = cols - cx;
        for (int c = cx; c + n < cols; c++)
            grid[cy][c] = grid[cy][c + n];
        clear_cells(cy, cols - n, cols);
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
        if (cx >= cols)
            cx = cols - 1;
    } else if (b >= 32 && b < 127) {
        if (cx >= cols) {
            cx = 0;
            linefeed();
        }
        put_cell(cy, cx, b);
        cx++;
        if (cx >= cols)
            cx = cols;                            /* wraps when the next character comes */
    }
}

/* The grid gets nc columns and nr rows. What was in it stays where it is;
 * the new cells are blank. */
static void resize_grid(int nc, int nr)
{
    for (int r = 0; r < MAXR; r++)
        for (int c = 0; c < MAXC; c++)
            if (r >= rows || c >= cols) {
                grid[r][c] = (struct cell){ ' ', 7, 0 };
                main_grid[r][c] = (struct cell){ ' ', 7, 0 };
            }
    cols = nc;
    rows = nr;
    back = 0;
    if (cx > cols) cx = cols;
    if (cy >= rows) cy = rows - 1;
    if (main_cx > cols) main_cx = cols;
    if (main_cy >= rows) main_cy = rows - 1;
    if (saved_cx > cols) saved_cx = cols;
    if (saved_cy >= rows) saved_cy = rows - 1;
    top = 0;
    bot = rows - 1;
    memset(dirty, 1, sizeof dirty);
}

/* Redraws the rows that changed (and the cursor's) and sends them. */
static int repaint(int fd, int id, int *old_cx, int *old_cy)
{
    dirty[*old_cy] = 1;
    int ccx = cx >= cols ? cols - 1 : cx;
    dirty[cy] = 1;
    for (int r = 0; r < rows; r++) {
        if (!dirty[r])
            continue;
        for (int c = 0; c < cols; c++)
            draw_cell(r, c, r == cy && c == ccx);
        dirty[r] = 0;
        if ((shared ? bwm_damage(fd, id, 0, r * CH, WIDTH, CH) : bwm_draw(fd, id, pix, WIDTH, 0, r * CH, WIDTH, CH)) != 0)
            return -1;
    }
    *old_cx = ccx;
    *old_cy = cy;
    return 0;
}

/* Writes the selected text to CLIP_FILE, a line to a row with the trailing
 * blanks cut off. */
static void copy_selection(void)
{
    int ar = sel_ar, ac = sel_ac, br = sel_br, bc = sel_bc;
    if (br < ar || (br == ar && bc < ac)) {
        int t = ar; ar = br; br = t;
        t = ac; ac = bc; bc = t;
    }
    static char text[MAXR * (MAXC + 1) + 1];
    size_t n = 0;
    for (int r = ar; r <= br && r < rows; r++) {
        int c0 = r == ar ? ac : 0, c1 = r == br ? bc : cols - 1;
        if (c1 >= cols) c1 = cols - 1;
        size_t start = n;
        for (int c = c0; c <= c1; c++)
            text[n++] = grid[r][c].ch >= 32 && grid[r][c].ch < 127 ? grid[r][c].ch : ' ';
        while (n > start && text[n - 1] == ' ')
            n--;
        if (r < br)
            text[n++] = '\n';
    }
    int f = open(CLIP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (f >= 0) {
        (void)!write(f, text, n);
        close(f);
    }
}

/* Types the clipboard into the shell. */
static void paste(int m)
{
    int f = open(CLIP_FILE, O_RDONLY);
    if (f < 0)
        return;
    char b[512];
    ssize_t n;
    while ((n = read(f, b, sizeof b)) > 0)
        (void)!write(m, b, n);
    close(f);
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    if (strcmp(base ? base + 1 : argv[0], "bfiles") == 0)
        return bfiles_main(argc, argv);
    unsigned sw, sh;
    bwm_screen(&sw, &sh);                         /* a small screen gets a smaller grid than 80 x 24 */
    if (sw >= 16 + MINC * CW && sh >= 80 + MINR * CH) {
        if (cols > (int)(sw - 16) / CW) cols = (sw - 16) / CW;
        if (rows > (int)(sh - 80) / CH) rows = (sh - 80) / CH;
    }
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bterm: no window server");
        return 2;
    }
    int id = bwm_create_shm(fd, WIDTH, HEIGHT, "Terminal", &pix);
    if (id >= 0) {
        shared = 1;
    } else {
        pix = NULL;
        close(fd);                                /* a failed try may have cost the connection */
        fd = bwm_connect();
        pix = malloc((size_t)MAXC * CW * MAXR * CH * 4);
        id = fd < 0 || !pix ? -1 : bwm_create(fd, WIDTH, HEIGHT, "Terminal");
    }
    if (id < 0) {
        puts("bterm: cannot open a window");
        return 1;
    }
    bwm_resizable(fd, id);
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) {
        puts("bterm: no pseudo-terminal");
        return 1;
    }
    char *slave = ptsname(m);
    struct winsize ws = { rows, cols, WIDTH, HEIGHT };
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
    for (int r = 0; r < rows; r++)
        clear_cells(r, 0, cols);
    memset(dirty, 1, sizeof dirty);
    int ocx = 0, ocy = 0, mid = 0;
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
            if (sel_on && !selecting) {
                sel_on = 0;                       /* new output: the lit text may be gone */
                memset(dirty, 1, sizeof dirty);
            }
            if (repaint(fd, id, &ocx, &ocy) != 0)
                break;
        }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            unsigned char buf[BWM_MAX_PAYLOAD];
            int t = bwm_event(fd, buf);
            if (t == BWM_KEY) {
                struct bwm_key *k = (struct bwm_key *)buf;
                unsigned char c = k->ch;
                if (back) {
                    back = 0;                    /* typing brings the live screen back */
                    memset(dirty, 1, sizeof dirty);
                    if (repaint(fd, id, &ocx, &ocy) != 0)
                        break;
                }
                if (write(m, &c, 1) != 1)
                    break;
            } else if (t == BWM_MOUSE) {
                struct bwm_mouse *e = (struct bwm_mouse *)buf;
                int wheel = (signed char)((e->buttons >> 8) & 0xFF);       /* negative is up */
                if (wheel && !alt) {
                    int nb = back - wheel * 3;
                    nb = nb < 0 ? 0 : nb > hist_n ? hist_n : nb;
                    if (nb != back) {
                        back = nb;
                        memset(dirty, 1, sizeof dirty);
                        printf("bterm: back %d\n", back);
                        fflush(stdout);
                        if (repaint(fd, id, &ocx, &ocy) != 0)
                            break;
                    }
                    continue;
                }
                if (back && (e->buttons & 1)) {
                    back = 0;                    /* a click works on the live screen */
                    memset(dirty, 1, sizeof dirty);
                }
                int er = e->y / CH, ec = e->x / CW;
                if (er >= rows) er = rows - 1;
                if (ec >= cols) ec = cols - 1;
                if ((e->buttons & 1) && !selecting) {
                    selecting = 1;               /* a press begins a selection */
                    sel_on = 1;
                    sel_ar = sel_br = er;
                    sel_ac = sel_bc = ec;
                    memset(dirty, 1, sizeof dirty);
                } else if ((e->buttons & 1) && selecting && (er != sel_br || ec != sel_bc)) {
                    sel_br = er;
                    sel_bc = ec;
                    memset(dirty, 1, sizeof dirty);
                } else if (!(e->buttons & 1) && selecting) {
                    selecting = 0;               /* the release ends it: a drag is copied, a click only clears */
                    if (sel_ar != sel_br || sel_ac != sel_bc)
                        copy_selection();
                    else
                        sel_on = 0;
                    memset(dirty, 1, sizeof dirty);
                }
                if ((e->buttons & 4) && !mid) {
                    paste(m);                    /* the middle button */
                }
                mid = e->buttons & 4;
                if (repaint(fd, id, &ocx, &ocy) != 0)
                    break;
            } else if (t == BWM_RESIZE) {
                struct bwm_resize *r = (struct bwm_resize *)buf;
                int nc = r->w / CW, nr = r->h / CH;
                nc = nc < MINC ? MINC : nc > MAXC ? MAXC : nc;
                nr = nr < MINR ? MINR : nr > MAXR ? MAXR : nr;
                if (shared) {
                    if (bwm_resized_shm(fd, id, nc * CW, nr * CH, &pix, (size_t)WIDTH * HEIGHT * 4) != 0)
                        break;
                } else if (bwm_resized(fd, id, nc * CW, nr * CH) != 0) {
                    break;
                }
                resize_grid(nc, nr);
                struct winsize nws = { rows, cols, WIDTH, HEIGHT };
                ioctl(m, TIOCSWINSZ, &nws);
                if (repaint(fd, id, &ocx, &ocy) != 0)
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
