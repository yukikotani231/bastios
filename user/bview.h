#ifndef BVIEW_H
#define BVIEW_H
/* bview, an image viewer for bwm: "bview FILE" shows a PPM (P6, 8 bits) or an
 * uncompressed 24 or 32 bit BMP, scaled by a whole factor to fit 512 x 360
 * (small images are enlarged, big ones sampled), in a window of its own. The
 * pixels live in bfiles' buffer. Any other key or the close box ends it. Says
 * "bview: window ID WxH" when the window is open and "bview: cannot show
 * FILE" otherwise. + and - (or =) zoom, h j k l or the arrow keys move
 * about a zoomed image, 0 goes back to the first view, n and p show the next
 * and the previous image of the same directory (by name) in the same window
 * and say "bview: show PATH". It is part of the bterm program, which runs it when
 * started as "bview" (bfiles' i runs it on the file shown). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BV_MAXW 512
#define BV_MAXH 360

static unsigned bv_le(const unsigned char *p, int n)
{
    unsigned v = 0;
    while (n--)
        v = v << 8 | p[n];
    return v;
}

/* Reads the next decimal number of a PPM header, skipping blanks and comments. */
static long bv_num(const unsigned char **p, const unsigned char *e)
{
    while (*p < e && (**p == '#' || **p == ' ' || **p == '\n' || **p == '\r' || **p == '\t')) {
        if (**p == '#')
            while (*p < e && **p != '\n') (*p)++;
        else
            (*p)++;
    }
    long v = 0;
    while (*p < e && **p >= '0' && **p <= '9')
        v = v * 10 + *(*p)++ - '0';
    return v;
}

/* Decodes the file in d[0..n) into 0x00RRGGBB pixels: *pw x *ph, rows top first.
 * Returns the pixel at (x, y) through get; 0 when the format is not understood. */
struct bv_img { const unsigned char *d, *px; long w, h, bpp, stride; int bmp, flip; };

static int bv_open(struct bv_img *m, const unsigned char *d, size_t n)
{
    memset(m, 0, sizeof *m);
    m->d = d;
    if (n > 26 && d[0] == 'B' && d[1] == 'M') {
        m->bmp = 1;
        m->w = (int)bv_le(d + 18, 4);
        m->h = (int)bv_le(d + 22, 4);
        m->bpp = bv_le(d + 28, 2);
        if (m->h > 0) m->flip = 1; else m->h = -m->h;       /* rows run bottom up when the height is positive */
        m->stride = (m->w * m->bpp / 8 + 3) & ~3L;
        m->px = d + bv_le(d + 10, 4);
        return (m->bpp == 24 || m->bpp == 32) && bv_le(d + 30, 4) == 0 && m->w > 0 && m->h > 0
            && (size_t)(m->px - d) + (size_t)m->stride * m->h <= n;
    }
    if (n > 10 && d[0] == 'P' && d[1] == '6') {
        const unsigned char *p = d + 2, *e = d + n;
        m->w = bv_num(&p, e);
        m->h = bv_num(&p, e);
        long max = bv_num(&p, e);
        m->px = p + 1;                                       /* one blank ends the header */
        m->bpp = 24;
        m->stride = m->w * 3;
        return max == 255 && m->w > 0 && m->h > 0 && m->w < 65536 && m->h < 65536
            && (size_t)(m->px - d) + (size_t)m->stride * m->h <= n;
    }
    return 0;
}

static uint32_t bv_get(const struct bv_img *m, long x, long y)
{
    const unsigned char *p = m->px + (m->flip ? m->h - 1 - y : y) * m->stride + x * (m->bpp / 8);
    return m->bmp ? (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0] : (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

/* Draws the image into bf_pix, WW x WH, at the scale BN / BD doubled (E > 0) or halved (E < 0) E times. The window shows
 * the middle of the image when it is smaller, else the part at (*PANX, *PANY), moved by DX, DY; a change of scale keeps
 * the middle of the window where it was. */
static long bv_cur_n = 1, bv_cur_d = 1, bv_cur_w, bv_cur_h;

static void bv_draw(const struct bv_img *m, long bn, long bd, int e, long ww, long wh, long *panx, long *pany, long dx, long dy)
{
    long n = e > 0 ? bn << e : bn, d = e < 0 ? bd << -e : bd;
    long w = m->w * n / d, h = m->h * n / d;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (bv_cur_w) {                                        /* the image coordinate at the middle of the window, at the old scale */
        long ox = bv_cur_w < ww ? (ww - bv_cur_w) / 2 : -*panx, oy = bv_cur_h < wh ? (wh - bv_cur_h) / 2 : -*pany;
        *panx = (ww / 2 - ox) * bv_cur_d * n / (bv_cur_n * d) - ww / 2;
        *pany = (wh / 2 - oy) * bv_cur_d * n / (bv_cur_n * d) - wh / 2;
    }
    *panx += dx;
    *pany += dy;
    if (*panx > w - ww) *panx = w - ww;
    if (*pany > h - wh) *pany = h - wh;
    if (*panx < 0) *panx = 0;
    if (*pany < 0) *pany = 0;
    long ox = w < ww ? (ww - w) / 2 : -*panx, oy = h < wh ? (wh - h) / 2 : -*pany;
    for (long y = 0; y < wh; y++)
        for (long x = 0; x < ww; x++) {
            uint32_t c = 0x00101018;
            long sx = x - ox, sy = y - oy;
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                long ix = sx * d / n, iy = sy * d / n;
                c = bv_get(m, ix < m->w ? ix : m->w - 1, iy < m->h ? iy : m->h - 1);
            }
            bf_pix[y * ww + x] = c;
        }
    bv_cur_n = n;
    bv_cur_d = d;
    bv_cur_w = w;
    bv_cur_h = h;
}

/* Reads a whole file (up to 8 MiB) into a new buffer; NULL when it cannot. */
static unsigned char *bv_read(const char *path, size_t *np)
{
    unsigned char *d = NULL;
    size_t n = 0, cap = 0;
    int f = open(path, O_RDONLY);
    for (ssize_t r; f >= 0 && n < (8u << 20); ) {
        if (n == cap) {
            unsigned char *g = realloc(d, cap = cap ? cap * 2 : 65536);
            if (!g)
                break;
            d = g;
        }
        if ((r = read(f, d + n, cap - n)) <= 0)
            break;
        n += r;
    }
    if (f >= 0)
        close(f);
    *np = n;
    return d;
}

/* The whole factor that fits the image into mw x mh: enlarged by bn, or sampled by bd. */
static void bv_fit(const struct bv_img *m, long mw, long mh, long *bn, long *bd)
{
    long up = mw / m->w < mh / m->h ? mw / m->w : mh / m->h;
    long dn = (m->w + mw - 1) / mw > (m->h + mh - 1) / mh ? (m->w + mw - 1) / mw : (m->h + mh - 1) / mh;
    int k = m->w > mw || m->h > mh;
    *bn = k ? 1 : up;
    *bd = k ? dn : 1;
}

static int bv_isimg(const char *name)
{
    size_t l = strlen(name);
    return l > 4 && (strcasecmp(name + l - 4, ".ppm") == 0 || strcasecmp(name + l - 4, ".bmp") == 0);
}

/* Puts into out the image file next to path that comes after it by name (step 1) or before it (step -1), wrapping round;
 * 0 when there is no other one. */
static int bv_neighbour(const char *path, int step, char *out, size_t cap)
{
    char dir[512];
    const char *base = strrchr(path, '/');
    size_t dl = base ? (size_t)(base - path) : 0;
    base = base ? base + 1 : path;
    if (dl >= sizeof dir)
        return 0;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    DIR *dp = opendir(dl ? dir : (path[0] == '/' ? "/" : "."));
    if (!dp)
        return 0;
    char best[256] = "", first[256] = "", last[256] = "";       /* best: the nearest name in the wanted direction */
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!bv_isimg(e->d_name) || strlen(e->d_name) >= sizeof best)
            continue;
        if (!first[0] || strcmp(e->d_name, first) < 0) strcpy(first, e->d_name);
        if (!last[0] || strcmp(e->d_name, last) > 0) strcpy(last, e->d_name);
        int c = strcmp(e->d_name, base);
        if (c != 0 && (step > 0 ? c > 0 : c < 0) && (!best[0] || (step > 0 ? strcmp(e->d_name, best) < 0 : strcmp(e->d_name, best) > 0)))
            strcpy(best, e->d_name);
    }
    closedir(dp);
    const char *pick = best[0] ? best : step > 0 ? first : last;
    if (!pick[0] || strcmp(pick, base) == 0)
        return 0;
    return snprintf(out, cap, "%s%s%s", dl ? dir : "", dl ? "/" : (path[0] == '/' ? "/" : ""), pick) < (int)cap;
}

static int bview_main(int argc, char **argv)
{
    char path[512];
    snprintf(path, sizeof path, "%s", argc > 1 ? argv[1] : "");
    size_t n = 0;
    unsigned char *d = bv_read(path, &n);
    struct bv_img m;
    if (!d || !bv_open(&m, d, n)) {
        printf("bview: cannot show %s\n", path);
        return 1;
    }
    long bn, bd;
    bv_fit(&m, BV_MAXW, BV_MAXH, &bn, &bd);
    long w = m.w * bn / bd, h = m.h * bn / bd;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    long ww = w < 128 ? 128 : w, wh = h < 64 ? 64 : h;       /* a window never smaller than this */
    int ze = 0;                                              /* + and - double and halve it, up to 4 times each way */
    long panx = 0, pany = 0;                                 /* where the window's corner is in the scaled image, when that is bigger */
    bv_draw(&m, bn, bd, ze, ww, wh, &panx, &pany, 0, 0);
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bview: no window server");
        return 2;
    }
    int id = bwm_create(fd, ww, wh, "Image");
    if (id < 0 || bwm_draw(fd, id, bf_pix, ww, 0, 0, ww, wh) != 0) {
        puts("bview: cannot open a window");
        return 1;
    }
    printf("bview: window %d %ldx%ld\n", id, m.w, m.h);
    fflush(stdout);
    static unsigned char buf[BWM_MAX_PAYLOAD];
    int esc = 0;                                             /* 1 after ESC, 2 after ESC [ (an arrow key is three bytes) */
    for (;;) {
        int t = bwm_event(fd, buf);
        if (t < 0 || t == BWM_CLOSE)
            break;
        unsigned c;
        if (t == BWM_KEY) {
            c = ((struct bwm_key *)buf)->ch;
        } else if (t == BWM_MOUSE) {                         /* the wheel zooms: up in, down out */
            int wheel = (signed char)((((struct bwm_mouse *)buf)->buttons >> 8) & 0xFF);
            if (!wheel)
                continue;
            c = wheel < 0 ? '+' : '-';
            esc = 0;
        } else {
            continue;
        }
        long dx = 0, dy = 0;
        int dz = 0, reset = 0;
        if (esc == 0 && (c == 'n' || c == 'p')) {            /* the next or the previous image of the directory, in this window */
            char np[512];
            unsigned char *nd;
            size_t nn;
            struct bv_img nm;
            snprintf(np, sizeof np, "%s", path);
            for (int tries = 0; tries < 32 && bv_neighbour(np, c == 'n' ? 1 : -1, np, sizeof np); tries++) {
                if (strcmp(np, path) == 0)
                    break;
                if ((nd = bv_read(np, &nn)) && bv_open(&nm, nd, nn)) {
                    free(d);
                    d = nd;
                    m = nm;
                    snprintf(path, sizeof path, "%s", np);
                    bv_fit(&m, ww, wh, &bn, &bd);
                    ze = 0;
                    panx = pany = 0;
                    bv_draw(&m, bn, bd, ze, ww, wh, &panx, &pany, 0, 0);
                    bwm_draw(fd, id, bf_pix, ww, 0, 0, ww, wh);
                    printf("bview: show %s\n", path);
                    fflush(stdout);
                    break;
                }
                free(nd);
            }
            continue;
        }
        if (esc == 0 && c == 27) { esc = 1; continue; }
        if (esc == 1 && c == '[') { esc = 2; continue; }
        if (esc == 2 && (c == '5' || c == '6')) { esc = c; continue; }      /* Page Up and Page Down are ESC [ 5 ~ and ESC [ 6 ~ */
        if (esc == '5' || esc == '6') {
            c = c == '~' ? (esc == '5' ? '+' : '-') : 0;                     /* Page Up zooms in, Page Down out */
            esc = 0;
        } else if (esc == 2) {
            esc = 0;
            c = c == 'A' ? 'k' : c == 'B' ? 'j' : c == 'C' ? 'l' : c == 'D' ? 'h' : c == 'H' ? '0' : c == 'F' ? '9' : 0;   /* Home resets the zoom, End zooms in all the way */
        } else if (esc == 1) {
            break;
        }
        if (c == 'h') dx = -32; else if (c == 'l') dx = 32;
        else if (c == 'k') dy = -32; else if (c == 'j') dy = 32;
        else if (c == '+' || c == '=') dz = 1;
        else if (c == '-') dz = -1;
        else if (c == '0') reset = 1;
        else if (c == '9') dz = 4 - ze;
        else if (c == 0) continue;
        else break;
        long nz = reset ? 0 : ze + dz;
        if (nz < -4 || nz > 4)
            continue;
        bv_draw(&m, bn, bd, nz, ww, wh, &panx, &pany, dx, dy);
        ze = (int)nz;
        bwm_draw(fd, id, bf_pix, ww, 0, 0, ww, wh);
    }
    return 0;
}
#endif
