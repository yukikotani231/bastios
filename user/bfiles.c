/* bfiles, a file browser and text viewer for bwm (user/bwmproto.h): a grid
 * of 8 x 16 cells, 60 x 24, drawn with the console's font. The first row
 * shows the directory; below it come ".." and the entries, directories
 * first. A click on a directory goes into it, a click on a file shows its
 * first 64 KiB as text (non-printing bytes as dots) and a click anywhere in
 * the viewer, or Backspace, goes back. j and k (or Space and b) scroll by
 * a line (a page), also in the viewer. Says "bfiles: window ID" when the
 * window is open and "bfiles: dir PATH" or "bfiles: view PATH" as it
 * moves. */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bwmlib.h"
#include "font8x8.h"

#define COLS 60
#define ROWS 24
#define CW 8
#define CH 16
#define WIDTH (COLS * CW)
#define HEIGHT (ROWS * CH)
#define MAXENT 1024
#define VIEWMAX 65536
#define MAXLINES 8192

struct ent { char name[NAME_MAX + 1]; int dir; off_t size; };

static uint32_t pix[WIDTH * HEIGHT];
static struct ent ents[MAXENT];
static int nent, top;                     /* the entries (".." is the first) and the first one shown */
static char cwd[PATH_MAX] = "/";
static int viewing;
static char vpath[PATH_MAX];
static char vtext[VIEWMAX];
static const char *vline[MAXLINES];       /* each line of the file, cut at COLS characters */
static int nvline, vtop;

static int cmp(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    return strcmp(x->name, y->name);
}

static void glyph(int r, int c, unsigned char ch, uint32_t f, uint32_t b)
{
    if (ch < 32 || ch >= 127)
        ch = ' ';
    for (int y = 0; y < CH; y++) {
        unsigned bits = font8x8[ch * 8 + y / 2];
        for (int x = 0; x < CW; x++, bits >>= 1)
            pix[(r * CH + y) * WIDTH + c * CW + x] = bits & 1 ? f : b;
    }
}

static void put_row(int r, const char *s, size_t n, uint32_t f, uint32_t b)
{
    for (int c = 0; c < COLS; c++)
        glyph(r, c, (size_t)c < n ? (unsigned char)s[c] : ' ', f, b);
}

static void load_dir(void)
{
    nent = 0;
    top = 0;
    snprintf(ents[nent].name, sizeof ents[nent].name, "..");
    ents[nent].dir = 1;
    ents[nent++].size = 0;
    DIR *d = opendir(cwd);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nent < MAXENT) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char p[PATH_MAX + NAME_MAX + 2];
        struct stat st;
        snprintf(p, sizeof p, "%s%s%s", cwd, strcmp(cwd, "/") == 0 ? "" : "/", e->d_name);
        snprintf(ents[nent].name, sizeof ents[nent].name, "%s", e->d_name);
        ents[nent].dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        ents[nent].size = stat(p, &st) == 0 ? st.st_size : 0;
        nent++;
    }
    closedir(d);
    qsort(&ents[1], nent - 1, sizeof ents[0], cmp);
    printf("bfiles: dir %s\n", cwd);
    fflush(stdout);
}

static void path_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s", cwd, strcmp(cwd, "/") == 0 ? "" : "/", name);
}

static void go_up(void)
{
    char *s = strrchr(cwd, '/');
    if (s == cwd)
        s[1] = 0;
    else if (s)
        *s = 0;
}

static void load_file(const char *path)
{
    int f = open(path, O_RDONLY);
    size_t n = 0;
    if (f >= 0) {
        ssize_t r;
        while (n < VIEWMAX - 1 && (r = read(f, vtext + n, VIEWMAX - 1 - n)) > 0)
            n += r;
        close(f);
    }
    vtext[n] = 0;
    nvline = 0;
    vtop = 0;
    size_t i = 0;
    while (i <= n && nvline < MAXLINES) {
        vline[nvline++] = vtext + i;
        size_t k = 0;
        while (i < n && vtext[i] != '\n') {
            if (++k > COLS) {                    /* a long line goes on in the next row */
                vline[nvline++] = vtext + i;
                k = 1;
                if (nvline >= MAXLINES)
                    break;
            }
            i++;
        }
        if (i >= n)
            break;
        i++;
    }
    snprintf(vpath, sizeof vpath, "%s", path);
    viewing = 1;
    printf("bfiles: view %s\n", path);
    fflush(stdout);
}

static void paint(void)
{
    uint32_t bg = 0x00101018, fg = 0x00D8D8D8, hd = 0x00305090;
    if (viewing) {
        char h[COLS + 1];
        snprintf(h, sizeof h, "%s", vpath);
        put_row(0, h, strlen(h), 0x00FFFFFF, hd);
        for (int r = 1; r < ROWS; r++) {
            int i = vtop + r - 1;
            char t[COLS + 1];
            size_t n = 0;
            if (i < nvline) {
                const char *s = vline[i];
                while (n < COLS && *s && *s != '\n') {
                    t[n++] = (unsigned char)*s >= 32 && (unsigned char)*s < 127 ? *s : (*s == '\t' ? ' ' : '.');
                    s++;
                }
            }
            put_row(r, t, n, fg, bg);
        }
        return;
    }
    char h[COLS + 1];
    snprintf(h, sizeof h, "%s", cwd);
    put_row(0, h, strlen(h), 0x00FFFFFF, hd);
    for (int r = 1; r < ROWS; r++) {
        int i = top + r - 1;
        if (i >= nent) {
            put_row(r, "", 0, fg, bg);
            continue;
        }
        char t[COLS + 32];
        if (ents[i].dir)
            snprintf(t, sizeof t, "%s/", ents[i].name);
        else
            snprintf(t, sizeof t, "%-44.44s %10lld", ents[i].name, (long long)ents[i].size);
        put_row(r, t, strlen(t), ents[i].dir ? 0x0080C0FF : fg, bg);
    }
}

static void scroll(int by)
{
    if (viewing) {
        vtop += by;
        if (vtop > nvline - 1) vtop = nvline - 1;
        if (vtop < 0) vtop = 0;
    } else {
        top += by;
        if (top > nent - 1) top = nent - 1;
        if (top < 0) top = 0;
    }
}

static void click(int row)
{
    if (viewing) {
        viewing = 0;
        return;
    }
    if (row < 1)
        return;
    int i = top + row - 1;
    if (i >= nent)
        return;
    if (i == 0) {
        go_up();
        load_dir();
    } else if (ents[i].dir) {
        char p[PATH_MAX];
        path_of(ents[i].name, p, sizeof p);
        snprintf(cwd, sizeof cwd, "%s", p);
        load_dir();
    } else {
        char p[PATH_MAX];
        path_of(ents[i].name, p, sizeof p);
        load_file(p);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == '/')
        snprintf(cwd, sizeof cwd, "%s", argv[1]);
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bfiles: no window server");
        return 2;
    }
    int id = bwm_create(fd, WIDTH, HEIGHT, "Files");
    if (id < 0) {
        puts("bfiles: cannot open a window");
        return 1;
    }
    load_dir();
    paint();
    if (bwm_draw(fd, id, pix, WIDTH, 0, 0, WIDTH, HEIGHT) != 0)
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
                scroll(1);
            else if (c == 'k')
                scroll(-1);
            else if (c == ' ')
                scroll(ROWS - 2);
            else if (c == 'b')
                scroll(-(ROWS - 2));
            else if (c == 127 || c == 8) {
                if (viewing)
                    viewing = 0;
                else {
                    go_up();
                    load_dir();
                }
            } else
                continue;
        } else if (t == BWM_MOUSE) {
            struct bwm_mouse *m = (struct bwm_mouse *)buf;
            int pressed = (m->buttons & 1) && !(prev & 1);
            prev = m->buttons;
            if (!pressed)
                continue;
            click(m->y / CH);
        } else {
            continue;
        }
        paint();
        if (bwm_draw(fd, id, pix, WIDTH, 0, 0, WIDTH, HEIGHT) != 0)
            break;
    }
    return 0;
}
