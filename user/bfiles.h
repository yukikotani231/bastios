#ifndef BFILES_H
#define BFILES_H
/* bfiles, a file browser and text viewer for bwm (user/bwmproto.h): a grid
 * of 8 x 16 cells, 60 x 24, drawn with the console's font. The first row
 * shows the directory; below it come ".." and the entries, directories
 * first. A click on a directory goes into it, a click on a file shows its
 * first 64 KiB as text (non-printing bytes as dots) and a click anywhere in
 * the viewer, or Backspace, goes back. j and k (or Space and b) scroll by
 * a line (a page), also in the viewer. In the viewer, e edits a file smaller
 * than 64 KiB in place: typing inserts, Enter, Backspace, Delete, the arrow,
 * Home, End and Page keys and a click move and change the text, Ctrl-S saves
 * ("bfiles: saved PATH") and Ctrl-X goes back to the viewer, which shows the
 * file as it is on disk. "bfiles /path/to/file" opens the file in the viewer.
 * In the list n makes an empty file (untitled, untitled2, ...) and opens it
 * in the editor. The mouse wheel scrolls three lines a step (and says "bfiles: top N"). Says "bfiles: window ID" when the
 * window is open and "bfiles: dir PATH" or "bfiles: view PATH" as it
 * moves.
 * It is part of the bterm program, which runs it when started as "bfiles"
 * (/bin/bterm with that argv[0], so it adds no image to the kernel). */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define BF_COLS 60
static int bf_rows = 24;
#define BF_ROWS bf_rows
#define BF_MAXROWS 24
#define BF_CW 8
#define BF_CH 16
#define BF_WIDTH (BF_COLS * BF_CW)
#define BF_HEIGHT (BF_ROWS * BF_CH)
#define BF_MAXENT 1024
#define BF_VIEWMAX 65536
#define BF_MAXLINES 8192

struct bf_ent { char name[NAME_MAX + 1]; int dir; off_t size; };

static uint32_t bf_pix[BF_WIDTH * BF_MAXROWS * BF_CH];
static struct bf_ent bf_ents[BF_MAXENT];
static int bf_nent, bf_top;                     /* the entries (".." is the first) and the first one shown */
static char bf_cwd[PATH_MAX] = "/";
static int bf_viewing;
static int bf_confirm;                    /* D was pressed once in the viewer: a second D deletes the file */
static int bf_stale;                      /* a file was made: the list must be read again */
static char bf_vpath[PATH_MAX];
static char bf_vtext[BF_VIEWMAX];
static const char *bf_vline[BF_MAXLINES];       /* each line of the file, cut at COLS characters */
static int bf_nvline, bf_vtop;
static size_t bf_vlen;                          /* bytes in bf_vtext */
static int bf_editing, bf_modified;
static size_t bf_cur;                           /* the cursor, as an offset in bf_vtext */
static int bf_etop;                             /* the first display row shown while editing */
static int bf_esc, bf_escnum;                   /* where we are in an ESC [ ... key sequence */
static size_t bf_erow[BF_MAXLINES + 1];         /* where each display row starts while editing */
static int bf_nerow;

static int bf_cmp(const void *a, const void *b)
{
    const struct bf_ent *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    return strcmp(x->name, y->name);
}

static void bf_glyph(int r, int c, unsigned char ch, uint32_t f, uint32_t b)
{
    if (ch < 32 || ch >= 127)
        ch = ' ';
    for (int y = 0; y < BF_CH; y++) {
        unsigned bits = font8x8[ch * 8 + y / 2];
        for (int x = 0; x < BF_CW; x++, bits >>= 1)
            bf_pix[(r * BF_CH + y) * BF_WIDTH + c * BF_CW + x] = bits & 1 ? f : b;
    }
}

static void bf_put_row(int r, const char *s, size_t n, uint32_t f, uint32_t b)
{
    for (int c = 0; c < BF_COLS; c++)
        bf_glyph(r, c, (size_t)c < n ? (unsigned char)s[c] : ' ', f, b);
}

static void bf_load_dir(void)
{
    bf_nent = 0;
    bf_top = 0;
    snprintf(bf_ents[bf_nent].name, sizeof bf_ents[bf_nent].name, "..");
    bf_ents[bf_nent].dir = 1;
    bf_ents[bf_nent++].size = 0;
    DIR *d = opendir(bf_cwd);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && bf_nent < BF_MAXENT) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char p[PATH_MAX + NAME_MAX + 2];
        struct stat st;
        snprintf(p, sizeof p, "%s%s%s", bf_cwd, strcmp(bf_cwd, "/") == 0 ? "" : "/", e->d_name);
        snprintf(bf_ents[bf_nent].name, sizeof bf_ents[bf_nent].name, "%s", e->d_name);
        bf_ents[bf_nent].dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        bf_ents[bf_nent].size = stat(p, &st) == 0 ? st.st_size : 0;
        bf_nent++;
    }
    closedir(d);
    qsort(&bf_ents[1], bf_nent - 1, sizeof bf_ents[0], bf_cmp);
    printf("bfiles: dir %s\n", bf_cwd);
    fflush(stdout);
}

static void bf_path_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s", bf_cwd, strcmp(bf_cwd, "/") == 0 ? "" : "/", name);
}

static void bf_go_up(void)
{
    char *s = strrchr(bf_cwd, '/');
    if (s == bf_cwd)
        s[1] = 0;
    else if (s)
        *s = 0;
}

static void bf_load_file(const char *path)
{
    int f = open(path, O_RDONLY);
    size_t n = 0;
    if (f >= 0) {
        ssize_t r;
        while (n < BF_VIEWMAX - 1 && (r = read(f, bf_vtext + n, BF_VIEWMAX - 1 - n)) > 0)
            n += r;
        close(f);
    }
    bf_vtext[n] = 0;
    bf_vlen = n;
    bf_nvline = 0;
    bf_vtop = 0;
    size_t i = 0;
    while (i <= n && bf_nvline < BF_MAXLINES) {
        bf_vline[bf_nvline++] = bf_vtext + i;
        size_t k = 0;
        while (i < n && bf_vtext[i] != '\n') {
            if (++k > BF_COLS) {                    /* a long line goes on in the next row */
                bf_vline[bf_nvline++] = bf_vtext + i;
                k = 1;
                if (bf_nvline >= BF_MAXLINES)
                    break;
            }
            i++;
        }
        if (i >= n)
            break;
        i++;
    }
    snprintf(bf_vpath, sizeof bf_vpath, "%s", path);
    bf_viewing = 1;
    printf("bfiles: view %s\n", path);
    fflush(stdout);
}

static void be_paint(void);

static void bf_paint(void)
{
    if (bf_editing) {
        be_paint();
        return;
    }
    uint32_t bg = 0x00101018, fg = 0x00D8D8D8, hd = 0x00305090;
    if (bf_viewing) {
        char h[BF_COLS + 1];
        if (bf_confirm)
            snprintf(h, sizeof h, "Delete %s? D again to confirm", bf_vpath);
        else
            snprintf(h, sizeof h, "%s", bf_vpath);
        bf_put_row(0, h, strlen(h), 0x00FFFFFF, bf_confirm ? 0x00A03030 : hd);
        for (int r = 1; r < BF_ROWS; r++) {
            int i = bf_vtop + r - 1;
            char t[BF_COLS + 1];
            size_t n = 0;
            if (i < bf_nvline) {
                const char *s = bf_vline[i];
                while (n < BF_COLS && *s && *s != '\n') {
                    t[n++] = (unsigned char)*s >= 32 && (unsigned char)*s < 127 ? *s : (*s == '\t' ? ' ' : '.');
                    s++;
                }
            }
            bf_put_row(r, t, n, fg, bg);
        }
        return;
    }
    char h[BF_COLS + 1];
    snprintf(h, sizeof h, "%s", bf_cwd);
    bf_put_row(0, h, strlen(h), 0x00FFFFFF, hd);
    for (int r = 1; r < BF_ROWS; r++) {
        int i = bf_top + r - 1;
        if (i >= bf_nent) {
            bf_put_row(r, "", 0, fg, bg);
            continue;
        }
        char t[BF_COLS + 32];
        if (bf_ents[i].dir)
            snprintf(t, sizeof t, "%s/", bf_ents[i].name);
        else
            snprintf(t, sizeof t, "%-44.44s %10lld", bf_ents[i].name, (long long)bf_ents[i].size);
        bf_put_row(r, t, strlen(t), bf_ents[i].dir ? 0x0080C0FF : fg, bg);
    }
}

/* The editor. Display rows are cut at COLS characters, like the viewer's. */
static void be_layout(void)
{
    size_t col = 0;
    bf_nerow = 0;
    bf_erow[bf_nerow++] = 0;
    for (size_t i = 0; i < bf_vlen && bf_nerow < BF_MAXLINES; i++) {
        if (bf_vtext[i] == '\n') {
            bf_erow[bf_nerow++] = i + 1;
            col = 0;
        } else {
            if (col == BF_COLS) {
                bf_erow[bf_nerow++] = i;
                col = 0;
            }
            col++;
        }
    }
}

static int be_row_of(size_t off)                /* the display row an offset is on */
{
    int r = 0;
    while (r + 1 < bf_nerow && bf_erow[r + 1] <= off)
        r++;
    return r;
}

static size_t be_row_len(int r)                 /* characters on a row, without its newline */
{
    size_t end = r + 1 < bf_nerow ? bf_erow[r + 1] : bf_vlen;
    size_t n = end - bf_erow[r];
    if (n > 0 && bf_vtext[end - 1] == '\n')
        n--;
    return n;
}

static void be_move_rows(int by)
{
    int r = be_row_of(bf_cur);
    size_t col = bf_cur - bf_erow[r];
    r += by;
    if (r < 0) r = 0;
    if (r >= bf_nerow) r = bf_nerow - 1;
    size_t n = be_row_len(r);
    bf_cur = bf_erow[r] + (col < n ? col : n);
}

static void be_insert(unsigned char c)
{
    if (bf_vlen + 1 >= BF_VIEWMAX)
        return;
    memmove(bf_vtext + bf_cur + 1, bf_vtext + bf_cur, bf_vlen - bf_cur);
    bf_vtext[bf_cur++] = c;
    bf_vlen++;
    bf_vtext[bf_vlen] = 0;
    bf_modified = 1;
}

static void be_delete(size_t at)
{
    if (at >= bf_vlen)
        return;
    memmove(bf_vtext + at, bf_vtext + at + 1, bf_vlen - at - 1);
    bf_vlen--;
    bf_vtext[bf_vlen] = 0;
    bf_modified = 1;
}

static void be_save(void)
{
    int f = open(bf_vpath, O_WRONLY | O_TRUNC);
    size_t n = 0;
    if (f >= 0) {
        ssize_t r;
        while (n < bf_vlen && (r = write(f, bf_vtext + n, bf_vlen - n)) > 0)
            n += r;
        close(f);
    }
    if (n == bf_vlen) {
        bf_modified = 0;
        printf("bfiles: saved %s\n", bf_vpath);
    } else {
        printf("bfiles: cannot save %s\n", bf_vpath);
    }
    fflush(stdout);
}

static void be_start(void)
{
    struct stat st;
    if (stat(bf_vpath, &st) != 0 || st.st_size >= BF_VIEWMAX - 1) {
        printf("bfiles: cannot edit %s\n", bf_vpath);
        fflush(stdout);
        return;
    }
    bf_editing = 1;
    bf_modified = 0;
    bf_cur = 0;
    bf_etop = 0;
    bf_esc = 0;
    printf("bfiles: edit %s\n", bf_vpath);
    fflush(stdout);
}

/* n in the directory list: makes an empty file ("untitled", "untitled2",
 * ...) here and opens it in the editor. */
static void bf_new_file(void)
{
    char path[PATH_MAX];
    for (int i = 1; i < 100; i++) {
        char name[32];
        if (i == 1)
            snprintf(name, sizeof name, "untitled");
        else
            snprintf(name, sizeof name, "untitled%d", i);
        bf_path_of(name, path, sizeof path);
        int f = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (f < 0)
            continue;
        close(f);
        printf("bfiles: new %s\n", path);
        fflush(stdout);
        bf_load_file(path);
        be_start();
        bf_stale = 1;
        return;
    }
}

static void be_key(int c)
{
    int page = BF_ROWS - 2;
    if (bf_esc == 1) {
        bf_esc = c == '[' ? 2 : 0;
        if (bf_esc)
            return;
    } else if (bf_esc == 2 || bf_esc == 3) {
        if (c >= '0' && c <= '9') {
            bf_escnum = bf_esc == 2 ? c - '0' : bf_escnum * 10 + c - '0';
            bf_esc = 3;
            return;
        }
        int num = bf_esc == 3 ? bf_escnum : 0;
        bf_esc = 0;
        if (c == 'A') be_move_rows(-1);
        else if (c == 'B') be_move_rows(1);
        else if (c == 'C') { if (bf_cur < bf_vlen) bf_cur++; }
        else if (c == 'D') { if (bf_cur > 0) bf_cur--; }
        else if (c == 'H' || (c == '~' && (num == 1 || num == 7))) bf_cur = bf_erow[be_row_of(bf_cur)];
        else if (c == 'F' || (c == '~' && (num == 4 || num == 8))) {
            int r = be_row_of(bf_cur);
            bf_cur = bf_erow[r] + be_row_len(r);
        } else if (c == '~' && num == 3) be_delete(bf_cur);
        else if (c == '~' && num == 5) be_move_rows(-page);
        else if (c == '~' && num == 6) be_move_rows(page);
        return;
    }
    if (c == 27) {
        bf_esc = 1;
    } else if (c == 19) {
        be_save();
    } else if (c == 24) {
        bf_editing = 0;
        bf_load_file(bf_vpath);
    } else if (c == 127 || c == 8) {
        if (bf_cur > 0)
            be_delete(--bf_cur);
    } else if (c == '\n' || c == '\r') {
        be_insert('\n');
    } else if (c == '\t') {
        for (int i = 0; i < 4; i++)
            be_insert(' ');
    } else if (c >= 32 && c < 127) {
        be_insert(c);
    }
}

static void be_click(int row, int x)
{
    int r = bf_etop + row - 1;
    if (row < 1)
        return;
    if (r >= bf_nerow) r = bf_nerow - 1;
    size_t col = x / BF_CW, n = be_row_len(r);
    bf_cur = bf_erow[r] + (col < n ? col : n);
}

static void be_paint(void)
{
    uint32_t bg = 0x00101018, fg = 0x00D8D8D8, hd = 0x00305090;
    be_layout();
    if (bf_cur > bf_vlen)
        bf_cur = bf_vlen;
    int cr = be_row_of(bf_cur);
    if (cr < bf_etop) bf_etop = cr;
    if (cr > bf_etop + BF_ROWS - 2) bf_etop = cr - (BF_ROWS - 2);
    char h[BF_COLS + 1];
    snprintf(h, sizeof h, "%.30s%s  ^S save ^X done", bf_vpath, bf_modified ? " [modified]" : "");
    bf_put_row(0, h, strlen(h), 0x00FFFFFF, hd);
    for (int r = 1; r < BF_ROWS; r++) {
        int i = bf_etop + r - 1;
        char t[BF_COLS + 1];
        size_t n = 0;
        if (i < bf_nerow) {
            n = be_row_len(i);
            for (size_t k = 0; k < n; k++) {
                unsigned char ch = bf_vtext[bf_erow[i] + k];
                t[k] = ch >= 32 && ch < 127 ? ch : '.';
            }
        }
        bf_put_row(r, t, n, fg, bg);
    }
    int row = cr - bf_etop + 1, col = (int)(bf_cur - bf_erow[cr]);
    if (col >= BF_COLS) col = BF_COLS - 1;
    unsigned char under = ' ';
    if (bf_cur < bf_vlen && bf_vtext[bf_cur] != '\n')
        under = (unsigned char)bf_vtext[bf_cur];
    bf_glyph(row, col, under, bg, fg);                /* the cursor: the cell with its colours swapped */
}

static void bf_scroll(int by)
{
    if (bf_viewing) {
        bf_vtop += by;
        if (bf_vtop > bf_nvline - 1) bf_vtop = bf_nvline - 1;
        if (bf_vtop < 0) bf_vtop = 0;
    } else {
        bf_top += by;
        if (bf_top > bf_nent - 1) bf_top = bf_nent - 1;
        if (bf_top < 0) bf_top = 0;
    }
}

static void bf_click(int row)
{
    if (bf_viewing) {
        bf_viewing = 0;
        if (bf_stale) {
            bf_stale = 0;
            bf_load_dir();
        }
        return;
    }
    if (row < 1)
        return;
    int i = bf_top + row - 1;
    if (i >= bf_nent)
        return;
    if (i == 0) {
        bf_go_up();
        bf_load_dir();
    } else if (bf_ents[i].dir) {
        char p[PATH_MAX];
        bf_path_of(bf_ents[i].name, p, sizeof p);
        snprintf(bf_cwd, sizeof bf_cwd, "%s", p);
        bf_load_dir();
    } else {
        char p[PATH_MAX];
        bf_path_of(bf_ents[i].name, p, sizeof p);
        bf_load_file(p);
    }
}

static int bfiles_main(int argc, char **argv)
{
    char first[PATH_MAX] = "";                         /* a file given: show it (e edits it) */
    if (argc > 1 && argv[1][0] == '/') {
        struct stat st;
        snprintf(bf_cwd, sizeof bf_cwd, "%s", argv[1]);
        if (stat(argv[1], &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(first, sizeof first, "%s", argv[1]);
            bf_go_up();
        }
    }
    unsigned sw, sh;
    bwm_screen(&sw, &sh);                              /* fewer rows on a short screen */
    if (sh >= 80 + 4 * BF_CH && bf_rows > (int)(sh - 80) / BF_CH)
        bf_rows = (sh - 80) / BF_CH;
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bfiles: no window server");
        return 2;
    }
    int id = bwm_create(fd, BF_WIDTH, BF_HEIGHT, "Files");
    if (id < 0) {
        puts("bfiles: cannot open a window");
        return 1;
    }
    bf_load_dir();
    if (first[0])
        bf_load_file(first);
    bf_paint();
    if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
        return 1;
    printf("bfiles: window %d\n", id);
    fflush(stdout);
    static unsigned char buf[BWM_MAX_PAYLOAD];
    unsigned prev = 0;
    for (;;) {
        int t = bwm_event(fd, buf);
        if (t < 0 || t == BWM_CLOSE)
            break;
        if (t == BWM_KEY) {
            int c = ((struct bwm_key *)buf)->ch;
            int was_confirm = bf_confirm;
            bf_confirm = 0;
            if (c == 'D' && bf_viewing && !bf_editing) {
                if (was_confirm) {
                    if (unlink(bf_vpath) == 0) {
                        printf("bfiles: deleted %s\n", bf_vpath);
                        bf_viewing = 0;
                        bf_load_dir();
                    } else {
                        printf("bfiles: cannot delete %s\n", bf_vpath);
                    }
                    fflush(stdout);
                } else {
                    bf_confirm = 1;
                }
            } else if (bf_editing)
                be_key(c);
            else if (c == 'e' && bf_viewing)
                be_start();
            else if (c == 'n' && !bf_viewing)
                bf_new_file();
            else if (c == 'j')
                bf_scroll(1);
            else if (c == 'k')
                bf_scroll(-1);
            else if (c == ' ')
                bf_scroll(BF_ROWS - 2);
            else if (c == 'b')
                bf_scroll(-(BF_ROWS - 2));
            else if (c == 127 || c == 8) {
                if (bf_viewing) {
                    bf_viewing = 0;
                    if (bf_stale) {
                        bf_stale = 0;
                        bf_load_dir();
                    }
                } else {
                    bf_go_up();
                    bf_load_dir();
                }
            } else
                continue;
        } else if (t == BWM_MOUSE) {
            struct bwm_mouse *m = (struct bwm_mouse *)buf;
            int wheel = (signed char)((m->buttons >> 8) & 0xFF);       /* negative is up */
            if (wheel) {
                if (bf_editing) {
                    be_move_rows(wheel * 3);
                } else {
                    bf_scroll(wheel * 3);
                    printf("bfiles: top %d\n", bf_viewing ? bf_vtop : bf_top);
                    fflush(stdout);
                }
                bf_paint();
                if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
                    break;
                continue;
            }
            int pressed = (m->buttons & 1) && !(prev & 1);
            prev = m->buttons & 0xFF;
            if (!pressed)
                continue;
            if (bf_editing)
                be_click(m->y / BF_CH, m->x);
            else
                bf_click(m->y / BF_CH);
        } else {
            continue;
        }
        bf_paint();
        if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
            break;
    }
    return 0;
}

#endif
