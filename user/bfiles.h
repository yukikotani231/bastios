#ifndef BFILES_H
#define BFILES_H
/* bfiles, a file browser and text viewer for bwm (user/bwmproto.h): a grid
 * of 8 x 16 cells, 60 x 24, drawn with the console's font. The first row
 * shows the directory; below it come ".." and the entries, directories
 * first. A click on a directory goes into it, a click on a file shows its
 * first 64 KiB as text (non-printing bytes as dots) and a click anywhere in
 * the viewer, or Backspace, goes back. j and k (or Space and b) scroll by
 * a line (a page), also in the viewer. Says "bfiles: window ID" when the
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
#define BF_ROWS 24
#define BF_CW 8
#define BF_CH 16
#define BF_WIDTH (BF_COLS * BF_CW)
#define BF_HEIGHT (BF_ROWS * BF_CH)
#define BF_MAXENT 1024
#define BF_VIEWMAX 65536
#define BF_MAXLINES 8192

struct bf_ent { char name[NAME_MAX + 1]; int dir; off_t size; };

static uint32_t bf_pix[BF_WIDTH * BF_HEIGHT];
static struct bf_ent bf_ents[BF_MAXENT];
static int bf_nent, bf_top;                     /* the entries (".." is the first) and the first one shown */
static char bf_cwd[PATH_MAX] = "/";
static int bf_viewing;
static char bf_vpath[PATH_MAX];
static char bf_vtext[BF_VIEWMAX];
static const char *bf_vline[BF_MAXLINES];       /* each line of the file, cut at COLS characters */
static int bf_nvline, bf_vtop;

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

static void bf_paint(void)
{
    uint32_t bg = 0x00101018, fg = 0x00D8D8D8, hd = 0x00305090;
    if (bf_viewing) {
        char h[BF_COLS + 1];
        snprintf(h, sizeof h, "%s", bf_vpath);
        bf_put_row(0, h, strlen(h), 0x00FFFFFF, hd);
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
    if (argc > 1 && argv[1][0] == '/')
        snprintf(bf_cwd, sizeof bf_cwd, "%s", argv[1]);
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
            if (c == 'j')
                bf_scroll(1);
            else if (c == 'k')
                bf_scroll(-1);
            else if (c == ' ')
                bf_scroll(BF_ROWS - 2);
            else if (c == 'b')
                bf_scroll(-(BF_ROWS - 2));
            else if (c == 127 || c == 8) {
                if (bf_viewing)
                    bf_viewing = 0;
                else {
                    bf_go_up();
                    bf_load_dir();
                }
            } else
                continue;
        } else if (t == BWM_MOUSE) {
            struct bwm_mouse *m = (struct bwm_mouse *)buf;
            int pressed = (m->buttons & 1) && !(prev & 1);
            prev = m->buttons;
            if (!pressed)
                continue;
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
