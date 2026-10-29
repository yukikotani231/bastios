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
#include <time.h>
#include <termios.h>

#include "bwmlib.h"
#include "font8x8.h"
#include "font16.h"
#include "bfiles.h"
#include "bsys.h"
#include "bview.h"

#define MAXC 160                          /* the most columns and rows the grid can have */
#define MAXR 60
static int fz = 1;                        /* the text size: each pixel of the 8 x 16 font is fz x fz */
#define MAXZ 3
#define ZOOM_FILE "/etc/bterm.zoom"       /* the size the next window starts with: a digit, 1 to MAXZ */
#define CW (8 * fz)
#define CH (16 * fz)
#define MINC 10
#define MINR 4

static int cols = 80, rows = 24;          /* the size of the grid now */
#define WIDTH (cols * CW)
#define HEIGHT (rows * CH)

static const uint32_t palette[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

/* The colour of index i of the xterm 256-colour palette: the 16 above, a 6 x 6 x 6 cube, then 24 greys. */
static uint32_t pal256(int i)
{
    if (i < 16)
        return palette[i];
    if (i >= 232) {
        unsigned g = 8 + 10 * (i - 232);
        return g << 16 | g << 8 | g;
    }
    static const unsigned char lv[6] = { 0, 95, 135, 175, 215, 255 };
    i -= 16;
    return (uint32_t)lv[i / 36] << 16 | (uint32_t)lv[i / 6 % 6] << 8 | lv[i % 6];
}

/* The nearest cube index to a 24-bit colour. */
static int nearest256(int r, int g, int b)
{
    int q[3] = { r, g, b };
    for (int k = 0; k < 3; k++)
        q[k] = q[k] < 48 ? 0 : q[k] < 115 ? 1 : (q[k] - 35) / 40;
    return 16 + 36 * q[0] + 6 * q[1] + q[2];
}

struct cell { unsigned short ch; unsigned char fg, bg, attr;      /* ch: a code point (0 in the right half of a wide character); attr: 1 underline, 2 italic, 4 strike-through, 8 right half of a wide character */ };
static struct cell grid[MAXR][MAXC];
static uint32_t *pix;            /* the window's pixels: bwm's shared memory when it gave some */
static int shared;
static unsigned char dirty[MAXR];
static int cx, cy, cur_fg = 7, cur_bg = 0, bold, reverse, cur_attr;
static int state, params[8], nparam;      /* 0 ground, 1 after ESC, 2 in CSI */
static int priv;                          /* the CSI had a '?' prefix */
static int bracketed;                       /* bracketed paste: xterm mode 2004 */
static int mouse_mode, mouse_sgr;           /* xterm mouse reporting: 1000 (1), 1002 (2), 1003 (3); 1006 (the SGR encoding) */
static unsigned mouse_btn;                /* the buttons held at the last report */
static int mouse_r = -1, mouse_c = -1;
static int top, bot = 23;           /* the scroll region, inclusive */
static int saved_cx, saved_cy;
static struct cell main_grid[MAXR][MAXC];
static int main_cx, main_cy, alt;
#define HIST 2000                         /* lines that scrolled off the top, kept for the wheel */
static struct cell hist[HIST][MAXC];
static int hist_n, hist_head;             /* how many are kept; where the next one goes */
static int back;                          /* how many lines the view is scrolled back (0 is the live screen) */
static int clicks, click_r, click_c;       /* clicks in a row on one cell (a double click selects a word, a triple one the line) */
static long click_ms;
static int sel_on, selecting;             /* a selection is lit; the button is still down */
static int sel_ar, sel_ac, sel_br, sel_bc;   /* where it began and where the pointer is, as cells */
#define CLIP_FILE "/tmp/bwm.clip"         /* what is selected, for any terminal to paste */

static void put_cell(int r, int c, unsigned ch)
{
    if (grid[r][c].attr & 8 && c > 0) {          /* writing over half of a wide character blanks the other half */
        grid[r][c - 1].ch = ' ';
        grid[r][c - 1].attr = 0;
    }
    if (c + 1 < cols && grid[r][c + 1].attr & 8) {
        grid[r][c + 1].ch = ' ';
        grid[r][c + 1].attr = 0;
    }
    grid[r][c].ch = ch;
    grid[r][c].fg = cur_fg + (bold && cur_fg < 8 ? 8 : 0);
    grid[r][c].bg = cur_bg;
    grid[r][c].attr = cur_attr;
    dirty[r] = 1;
}

static void clear_cells(int r, int c0, int c1)
{
    for (int c = c0; c < c1; c++) {
        grid[r][c].ch = ' ';
        grid[r][c].fg = 7;
        grid[r][c].bg = cur_bg;
        grid[r][c].attr = 0;
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

/* What each cell was last drawn as, so that a row that is redrawn paints (and sends) only the cells that differ. */
struct shown { uint32_t f, b; unsigned cp; unsigned char half, attr, fz; };
static struct shown shown[MAXR][MAXC];
static unsigned char shown_ok[MAXR][MAXC];                  /* 0 after the pixels were lost (a new size) */

/* Returns 1 when the cell was drawn, 0 when its pixels were already right. */
static int draw_cell(int r, int c, int cursor)
{
    const struct cell *e = &view_row(r)[c];
    int fg = e->fg, bg = e->bg;
    uint32_t f = pal256(fg), b = pal256(bg);
    if (!back && (cursor || selected(r, c))) {
        uint32_t t = f;
        f = b;
        b = t;
        if (f == b)
            f = 0xFFFFFF;
    }
    unsigned cp = e->ch;
    int half = 0, wide = 0;
    if ((e->attr & 8) && c > 0) {                       /* the right half of a wide character */
        cp = view_row(r)[c - 1].ch;
        half = 1;
    }
    struct shown now = { f, b, cp, (unsigned char)half, (unsigned char)(e->attr & 7), (unsigned char)fz };
    if (shown_ok[r][c] && memcmp(&shown[r][c], &now, sizeof now) == 0)
        return 0;
    shown[r][c] = now;
    shown_ok[r][c] = 1;
    const unsigned char *g = cp >= 128 ? glyph16(cp, &wide) : 0;
    int ch = cp < 128 ? cp : ' ';
    for (int y = 0; y < CH; y++) {
        int fy = y / fz;                                 /* the row of the 8 x 16 font this pixel row shows */
        unsigned bits = g ? (wide ? g[fy * 2 + half] : g[fy]) : font8x8[ch * 8 + fy / 2];
        int lean = !(e->attr & 2) ? 0 : fy < 5 ? 1 : fy >= 11 ? -1 : 0;      /* italic: the top leans right, the bottom left */
        for (int x = 0; x < CW; x++) {
            int sx = x / fz - lean;
            int on = sx >= 0 && sx < 8 && (bits >> sx & 1);
            if (((e->attr & 1) && fy >= 14) || ((e->attr & 4) && (fy == 7 || fy == 8)))
                on = 1;
            pix[(r * CH + y) * WIDTH + c * CW + x] = on ? f : b;
        }
    }
    return 1;
}

/* After cells of a row were erased, inserted or deleted, a wide character may be left with one half: blank it. */
static void fix_wide(int r)
{
    for (int c = 0; c < cols; c++) {
        struct cell *e = &grid[r][c];
        int right = e->attr & 8;
        int left = !right && e->ch >= 0x1100 && cp_width(e->ch) == 2;
        if ((right && (c == 0 || grid[r][c - 1].attr & 8 || cp_width(grid[r][c - 1].ch) != 2)) ||
            (left && (c + 1 >= cols || !(grid[r][c + 1].attr & 8)))) {
            e->ch = ' ';
            e->attr = 0;
            dirty[r] = 1;
        }
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
                else if (params[i] == 1000 || params[i] == 1002 || params[i] == 1003)
                    mouse_mode = final == 'h' ? (params[i] == 1000 ? 1 : params[i] == 1002 ? 2 : 3) : 0;
                else if (params[i] == 1006)
                    mouse_sgr = final == 'h';
                else if (params[i] == 2004)
                    bracketed = final == 'h';
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
            if (p == 0) { cur_fg = 7; cur_bg = 0; bold = 0; reverse = 0; cur_attr = 0; }
            else if (p == 3) cur_attr |= 2;
            else if (p == 4) cur_attr |= 1;
            else if (p == 9) cur_attr |= 4;
            else if (p == 23) cur_attr &= ~2;
            else if (p == 24) cur_attr &= ~1;
            else if (p == 29) cur_attr &= ~4;
            else if (p == 1) bold = 1;
            else if (p == 22) bold = 0;
            else if (p == 7) { int t = cur_fg; cur_fg = cur_bg; cur_bg = t; reverse = !reverse; }
            else if (p >= 30 && p <= 37) cur_fg = p - 30;
            else if (p == 39) cur_fg = 7;
            else if (p >= 40 && p <= 47) cur_bg = p - 40;
            else if (p == 49) cur_bg = 0;
            else if (p >= 90 && p <= 97) cur_fg = p - 90 + 8;
            else if (p >= 100 && p <= 107) cur_bg = p - 100 + 8;
            else if (p == 38 || p == 48) {                /* 38;5;N or 38;2;R;G;B (48 for the background) */
                int *to = p == 38 ? &cur_fg : &cur_bg;
                if (i + 2 < nparam && params[i + 1] == 5) {
                    *to = params[i + 2] & 255;
                    i += 2;
                } else if (i + 4 < nparam && params[i + 1] == 2) {
                    *to = nearest256(params[i + 2] & 255, params[i + 3] & 255, params[i + 4] & 255);
                    i += 4;
                }
            }
        }
        break;
    default:
        break;                                    /* a sequence this terminal does not know */
    }
    if (final == 'X' || final == '@' || final == 'P' || final == 'K' || final == 'J')
        fix_wide(cy);
}

static char osc[64];                       /* the text of an ESC ] ... string being read (state 4; 5 after an ESC in it) */
static int osc_n;
static char new_title[24];                 /* a title the program set (OSC 0 or 2), not yet sent to bwm */
static int title_set;

static void osc_end(void)
{
    osc[osc_n] = 0;
    if ((osc[0] == '0' || osc[0] == '2') && osc[1] == ';') {
        snprintf(new_title, sizeof new_title, "%.23s", osc + 2);
        title_set = 1;
    }
}

static unsigned utf_cp;                    /* the code point being read from a UTF-8 sequence */
static int utf_left;                       /* continuation bytes still to come */

/* The font has the 128 ASCII glyphs only, so a character outside it is drawn as a look-alike (box drawing as
 * - | +, a bullet as *) or as '?'; a character that takes no room (a combining mark, a zero-width one) as 0. */
static int utf_glyph(unsigned cp)
{
    if (cp >= 0x300 && cp <= 0x36F)
        return 0;
    if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF)
        return 0;
    if (cp >= 0x2500 && cp <= 0x257F) {
        if (cp == 0x2500 || cp == 0x2501 || (cp >= 0x2504 && cp <= 0x2509) || cp == 0x254C || cp == 0x254D || cp == 0x2550)
            return '-';
        if (cp == 0x2502 || cp == 0x2503 || (cp >= 0x250A && cp <= 0x250B) || cp == 0x254E || cp == 0x254F || cp == 0x2551)
            return '|';
        return '+';
    }
    if (cp >= 0x2580 && cp <= 0x259F)
        return '#';
    switch (cp) {
    case 0xA0: return ' ';
    case 0xB7: case 0x2024: case 0x2026: return '.';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: case 0x25CF: case 0x25CB: return '*';
    case 0x2190: case 0x25C0: return '<';
    case 0x2192: case 0x25B6: return '>';
    case 0x2191: case 0x25B2: return '^';
    case 0x2193: case 0x25BC: return 'v';
    default: return '?';
    }
}

static void put_char(unsigned b)
{
    if (cx >= cols) {
        cx = 0;
        linefeed();
    }
    put_cell(cy, cx, b);
    cx++;
    if (cx >= cols)
        cx = cols;                            /* wraps when the next character comes */
}

/* A character that takes two cells: the left one holds the code point, the right one is marked as its other half. */
static void put_wide(unsigned cp)
{
    if (cx >= cols - 1) {                       /* it does not fit on this row */
        if (cx == cols - 1)
            put_cell(cy, cx, ' ');
        cx = 0;
        linefeed();
    }
    put_cell(cy, cx, cp);
    put_cell(cy, cx + 1, 0);
    grid[cy][cx + 1].attr |= 8;
    cx += 2;
    if (cx >= cols)
        cx = cols;
}

static void feed(unsigned char b)
{
    if (utf_left > 0) {                                 /* inside a UTF-8 sequence */
        if ((b & 0xC0) == 0x80) {
            utf_cp = utf_cp << 6 | (b & 0x3F);
            if (--utf_left == 0) {
                int w = cp_width(utf_cp), wide = 0;
                if (w == 0) {
                    /* a combining or zero-width character takes no room */
                } else if (utf_cp < 0x10000 && glyph16(utf_cp, &wide) && wide == (w == 2)) {
                    if (wide)
                        put_wide(utf_cp);
                    else
                        put_char(utf_cp);
                } else {
                    int g = utf_glyph(utf_cp);
                    if (g)
                        put_char(g);
                }
            }
            return;
        }
        utf_left = 0;                                   /* cut short: this byte starts something else */
        put_char('?');
    }
    if (b >= 0xC2 && b < 0xF8 && state == 0) {
        utf_left = b < 0xE0 ? 1 : b < 0xF0 ? 2 : 3;
        utf_cp = b & (b < 0xE0 ? 0x1F : b < 0xF0 ? 0x0F : 0x07);
        return;
    }
    if (state == 4 || state == 5) {                     /* ends at BEL or ESC \ */
        if (b == 7 || (state == 5 && b == '\\')) {
            osc_end();
            state = 0;
        } else if (b == 27) {
            state = 5;
        } else {
            state = 4;
            if (osc_n < (int)sizeof osc - 1)
                osc[osc_n++] = b;
        }
        return;
    }
    if (state == 1) {
        state = 0;
        if (b == ']') {
            state = 4;
            osc_n = 0;
        } else if (b == '[') {
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
        put_char(b);
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
    memset(shown_ok, 0, sizeof shown_ok);                  /* the pixels start again */
}

static void save_zoom(void)
{
    FILE *f = fopen(ZOOM_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", fz);
        fclose(f);
    }
}

static void load_zoom(void)
{
    FILE *f = fopen(ZOOM_FILE, "r");
    int z = 0;
    if (f) {
        if (fscanf(f, "%d", &z) != 1)
            z = 0;
        fclose(f);
    }
    if (z >= 1 && z <= MAXZ)
        fz = z;
}

/* The grid gets nc x nr cells (the window is told, and the program on the pty); returns -1 when the server is gone. */
static int set_grid(int fd, int id, int m, int nc, int nr)
{
    int maxc = MAXC / fz, maxr = MAXR / fz;           /* the pixel buffer holds as many pixels at any size */
    nc = nc < MINC ? MINC : nc > maxc ? maxc : nc;
    nr = nr < MINR ? MINR : nr > maxr ? maxr : nr;
    if (shared) {
        if (bwm_resized_shm(fd, id, nc * CW, nr * CH, &pix, (size_t)nc * CW * nr * CH * 4) != 0)
            return -1;
    } else if (bwm_resized(fd, id, nc * CW, nr * CH) != 0) {
        return -1;
    }
    resize_grid(nc, nr);
    struct winsize nws = { rows, cols, WIDTH, HEIGHT };
    ioctl(m, TIOCSWINSZ, &nws);
    return 0;
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
        int c0 = cols, c1 = -1;                           /* the cells that changed */
        for (int c = 0; c < cols; c++)
            if (draw_cell(r, c, r == cy && c == ccx)) {
                if (c < c0) c0 = c;
                c1 = c;
            }
        dirty[r] = 0;
        if (c1 < 0)
            continue;
        int x = c0 * CW, w = (c1 + 1) * CW - x;
        if ((shared ? bwm_damage(fd, id, x, r * CH, w, CH) : bwm_draw(fd, id, pix, WIDTH, x, r * CH, w, CH)) != 0)
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
    static char text[MAXR * (MAXC * 3 + 1) + 1];
    size_t n = 0;
    for (int r = ar; r <= br && r < rows; r++) {
        int c0 = r == ar ? ac : 0, c1 = r == br ? bc : cols - 1;
        if (c1 >= cols) c1 = cols - 1;
        size_t start = n;
        for (int c = c0; c <= c1; c++) {
            unsigned ch = grid[r][c].ch;
            if (grid[r][c].attr & 8)
                continue;                               /* the right half of a wide character: the left half wrote it */
            if (ch >= 0x800) {
                text[n++] = 0xE0 | ch >> 12;
                text[n++] = 0x80 | (ch >> 6 & 0x3F);
                text[n++] = 0x80 | (ch & 0x3F);
            } else if (ch >= 0x80) {
                text[n++] = 0xC0 | ch >> 6;
                text[n++] = 0x80 | (ch & 0x3F);
            } else {
                text[n++] = ch >= 32 && ch < 127 ? ch : ' ';
            }
        }
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

/* A click on cell (r, c) that follows another within 400 ms on the same cell
 * is a double click: it selects the run of non-blank characters under it. A
 * third makes the whole row the selection. */
static void click_select(int r, int c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long now = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    if (clicks > 0 && r == click_r && c == click_c && now - click_ms < 400)
        clicks++;
    else
        clicks = 1;
    click_r = r; click_c = c; click_ms = now;
    if (clicks < 2 || r >= rows)
        return;
    if (clicks >= 3) {
        sel_ar = sel_br = r;
        sel_ac = 0;
        sel_bc = cols - 1;
        clicks = 0;
    } else {
        if (grid[r][c].ch <= ' ' && !(grid[r][c].attr & 8))
            return;
        int a = c, b = c;
        while (a > 0 && (grid[r][a - 1].ch > ' ' || (grid[r][a - 1].attr & 8)))
            a--;
        while (b < cols - 1 && (grid[r][b + 1].ch > ' ' || (grid[r][b + 1].attr & 8)))
            b++;
        sel_ar = sel_br = r;
        sel_ac = a;
        sel_bc = b;
    }
    sel_on = 1;
    copy_selection();
}

/* Types the clipboard into the shell, between ESC [ 200 ~ and ESC [ 201 ~ when the program asked for that. */
static void paste(int m)
{
    int f = open(CLIP_FILE, O_RDONLY);
    if (f < 0)
        return;
    char b[512];
    ssize_t n;
    if (bracketed)
        (void)!write(m, "\033[200~", 6);
    while ((n = read(f, b, sizeof b)) > 0)
        (void)!write(m, b, n);
    if (bracketed)
        (void)!write(m, "\033[201~", 6);
    close(f);
}

/* Tells the program on the pty about the pointer, in xterm's encoding (CODE: 0 left, 1 middle, 2 right,
 * 64 and 65 the wheel, plus 32 for a move, 3 for a release without the SGR encoding). */
static void mouse_emit(int m, int code, int press, int r, int c)
{
    char b[32];
    int n;
    if (mouse_sgr)
        n = snprintf(b, sizeof b, "\033[<%d;%d;%d%c", code, c + 1, r + 1, press ? 'M' : 'm');
    else if (c + 33 > 255 || r + 33 > 255)
        return;
    else
        n = snprintf(b, sizeof b, "\033[M%c%c%c", 32 + (press ? code : 3), 33 + c, 33 + r);
    (void)!write(m, b, n);
}

static void mouse_report(int m, unsigned buttons, int r, int c)
{
    static const unsigned bit[3] = { 1, 4, 2 };        /* left, middle, right */
    int wheel = (signed char)((buttons >> 8) & 0xFF);
    for (int i = 0; i < (wheel < 0 ? -wheel : wheel) && i < 3; i++)
        mouse_emit(m, wheel < 0 ? 64 : 65, 1, r, c);
    int moved = r != mouse_r || c != mouse_c, held = -1;
    for (int i = 0; i < 3; i++) {
        if ((buttons & bit[i]) != (mouse_btn & bit[i]))
            mouse_emit(m, i, (buttons & bit[i]) != 0, r, c);
        else if (buttons & bit[i])
            held = i;
    }
    if (moved && held >= 0 && mouse_mode >= 2 && buttons == mouse_btn)
        mouse_emit(m, 32 + held, 1, r, c);
    else if (moved && !(buttons & 7) && mouse_mode == 3)
        mouse_emit(m, 35, 1, r, c);
    mouse_btn = buttons & 7;
    mouse_r = r;
    mouse_c = c;
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    if (strcmp(base ? base + 1 : argv[0], "bfiles") == 0)
        return bfiles_main(argc, argv);
    if (strcmp(base ? base + 1 : argv[0], "bsys") == 0)
        return bsys_main();
    if (strcmp(base ? base + 1 : argv[0], "bview") == 0)
        return bview_main(argc, argv);
    unsigned sw, sh;
    load_zoom();
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
        if (argc > 2 && strcmp(argv[1], "-e") == 0)           /* bterm -e COMMAND: run it in ash instead of a prompt */
            execl("/bin/ash", "ash", "-c", argv[2], (char *)NULL);
        execl("/bin/ash", "ash", (char *)NULL);
        _exit(127);
    }
    for (int r = 0; r < rows; r++)
        clear_cells(r, 0, cols);
    memset(dirty, 1, sizeof dirty);
    int ocx = 0, ocy = 0, mid = 0;
    repaint(fd, id, &ocx, &ocy);
    printf("bterm: window %d\n", id);
    if (fz != 1)
        printf("bterm: text size %d (%d x %d)\n", fz, cols, rows);
    fflush(stdout);
    for (;;) {
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { m, POLLIN, 0 } };
        if (poll(p, 2, -1) < 0)
            continue;
        if (p[1].revents & (POLLIN | POLLHUP)) {
            unsigned char b[4096];
            ssize_t n = read(m, b, sizeof b);
            if (n <= 0) {
                puts("bterm: shell exited");
                break;
            }
            int again = 0;
            for (;;) {                            /* more output already waiting: take it before drawing, up to 32 KiB */
                for (ssize_t i = 0; i < n; i++)
                    feed(b[i]);
                again += (int)n;
                struct pollfd more = { m, POLLIN, 0 };
                if (again >= 32768 || poll(&more, 1, 0) <= 0 || !(more.revents & POLLIN))
                    break;
                n = read(m, b, sizeof b);
                if (n <= 0)
                    break;
            }
            if (sel_on && !selecting) {
                sel_on = 0;                       /* new output: the lit text may be gone */
                memset(dirty, 1, sizeof dirty);
            }
            if (title_set) {
                struct bwm_settitle st = { (uint32_t)id, "" };
                snprintf(st.title, sizeof st.title, "%s", new_title);
                title_set = 0;
                if (bwm_send(fd, BWM_SETTITLE, &st, sizeof st) != 0)
                    break;
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
                if (k->ch >= BWM_KEY_SCROLL_UP && k->ch <= BWM_KEY_SCROLL_END) {
                    if (!alt) {
                        int nb = k->ch == BWM_KEY_SCROLL_TOP ? hist_n : k->ch == BWM_KEY_SCROLL_END ? 0
                               : back + (k->ch == BWM_KEY_SCROLL_UP ? rows - 1 : -(rows - 1));
                        nb = nb < 0 ? 0 : nb > hist_n ? hist_n : nb;
                        if (nb != back) {
                            back = nb;
                            memset(dirty, 1, sizeof dirty);
                            printf("bterm: back %d\n", back);
                            fflush(stdout);
                            if (repaint(fd, id, &ocx, &ocy) != 0)
                                break;
                        }
                    }
                    continue;
                }
                if (back) {
                    back = 0;                    /* typing brings the live screen back */
                    memset(dirty, 1, sizeof dirty);
                    if (repaint(fd, id, &ocx, &ocy) != 0)
                        break;
                }
                if (k->ch >= BWM_KEY_ZOOM_IN && k->ch <= BWM_KEY_ZOOM_RESET) {
                    int nz = k->ch == BWM_KEY_ZOOM_IN ? fz + 1 : k->ch == BWM_KEY_ZOOM_OUT ? fz - 1 : 1;
                    if (nz >= 1 && nz <= MAXZ && nz != fz) {
                        int pw = cols * CW, ph = rows * CH;       /* the window keeps its size: the grid changes */
                        fz = nz;
                        save_zoom();
                        if (set_grid(fd, id, m, pw / CW, ph / CH) != 0)
                            break;
                        memset(dirty, 1, sizeof dirty);
                        printf("bterm: text size %d (%d x %d)\n", fz, cols, rows);
                        fflush(stdout);
                        if (repaint(fd, id, &ocx, &ocy) != 0)
                            break;
                    }
                    continue;
                }
                if (k->ch == BWM_KEY_PASTE)
                    paste(m);
                else if (write(m, &c, 1) != 1)
                    break;
            } else if (t == BWM_MOUSE) {
                struct bwm_mouse *e = (struct bwm_mouse *)buf;
                if (mouse_mode) {                /* the program asked for the pointer: no selection, no scrolling back */
                    int mr = (int)e->y < 0 ? 0 : e->y / CH, mc = (int)e->x < 0 ? 0 : e->x / CW;
                    mouse_report(m, e->buttons, mr >= rows ? rows - 1 : mr, mc >= cols ? cols - 1 : mc);
                    continue;
                }
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
                int er = (int)e->y < 0 ? 0 : e->y / CH, ec = (int)e->x < 0 ? 0 : e->x / CW;
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
                    if (sel_ar != sel_br || sel_ac != sel_bc) {
                        copy_selection();
                        clicks = 0;
                    } else {
                        sel_on = 0;
                        click_select(er, ec);
                    }
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
                if (set_grid(fd, id, m, r->w / CW, r->h / CH) != 0)
                    break;
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
