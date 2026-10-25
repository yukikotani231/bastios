#ifndef BVIEW_H
#define BVIEW_H
/* bview, an image viewer for bwm: "bview FILE" shows a PPM (P6, 8 bits) or an
 * uncompressed 24 or 32 bit BMP, scaled by a whole factor to fit 512 x 360
 * (small images are enlarged, big ones sampled), in a window of its own. The
 * pixels live in bfiles' buffer. Any other key or the close box ends it. Says
 * "bview: window ID WxH" when the window is open and "bview: cannot show
 * FILE" otherwise. + and - (or =) zoom, h j k l or the arrow keys move
 * about a zoomed image, 0 goes back to the first view. It is part of the bterm program, which runs it when
 * started as "bview" (bfiles' i runs it on the file shown). */
#include <stdio.h>
#include <stdlib.h>

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

static int bview_main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "";
    unsigned char *d = NULL;
    size_t n = 0, cap = 0;
    int f = open(path, O_RDONLY);
    for (ssize_t r; f >= 0 && n < (8u << 20); ) {
        if (n == cap && !(d = realloc(d, cap = cap ? cap * 2 : 65536)))
            break;
        if ((r = read(f, d + n, cap - n)) <= 0)
            break;
        n += r;
    }
    struct bv_img m;
    if (f >= 0)
        close(f);
    if (!d || !bv_open(&m, d, n)) {
        printf("bview: cannot show %s\n", path);
        return 1;
    }
    long k = m.w <= BV_MAXW && m.h <= BV_MAXH ? 0 : 1;      /* k: the factor to enlarge or to sample by */
    long up = BV_MAXW / m.w < BV_MAXH / m.h ? BV_MAXW / m.w : BV_MAXH / m.h;
    long dn = (m.w + BV_MAXW - 1) / BV_MAXW > (m.h + BV_MAXH - 1) / BV_MAXH ? (m.w + BV_MAXW - 1) / BV_MAXW : (m.h + BV_MAXH - 1) / BV_MAXH;
    long w = k ? m.w / dn : m.w * up, h = k ? m.h / dn : m.h * up;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    long ww = w < 128 ? 128 : w, wh = h < 64 ? 64 : h;       /* a window never smaller than this */
    long bn = k ? 1 : up, bd = k ? dn : 1;                   /* the scale at the start: bn / bd window pixels per image pixel */
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
        if (esc == 0 && c == 27) { esc = 1; continue; }
        if (esc == 1 && c == '[') { esc = 2; continue; }
        if (esc == 2) {
            esc = 0;
            c = c == 'A' ? 'k' : c == 'B' ? 'j' : c == 'C' ? 'l' : c == 'D' ? 'h' : c == 'H' ? '0' : 0;   /* Home resets the zoom */
        } else if (esc == 1) {
            break;
        }
        if (c == 'h') dx = -32; else if (c == 'l') dx = 32;
        else if (c == 'k') dy = -32; else if (c == 'j') dy = 32;
        else if (c == '+' || c == '=') dz = 1;
        else if (c == '-') dz = -1;
        else if (c == '0') reset = 1;
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
