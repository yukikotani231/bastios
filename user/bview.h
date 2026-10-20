#ifndef BVIEW_H
#define BVIEW_H
/* bview, an image viewer for bwm: "bview FILE" shows a PPM (P6, 8 bits) or an
 * uncompressed 24 or 32 bit BMP, scaled by a whole factor to fit 512 x 360
 * (small images are enlarged, big ones sampled), in a window of its own. The
 * pixels live in bfiles' buffer. Any key or the close box ends it. Says
 * "bview: window ID WxH" when the window is open and "bview: cannot show
 * FILE" otherwise. It is part of the bterm program, which runs it when
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
    for (long y = 0; y < wh; y++)
        for (long x = 0; x < ww; x++) {
            uint32_t c = 0x00101018;
            long ox = x - (ww - w) / 2, oy = y - (wh - h) / 2;
            if (ox >= 0 && ox < w && oy >= 0 && oy < h)
                c = k ? bv_get(&m, ox * dn, oy * dn) : bv_get(&m, ox / up, oy / up);
            bf_pix[y * ww + x] = c;
        }
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
    for (;;) {
        int t = bwm_event(fd, buf);
        if (t < 0 || t == BWM_CLOSE || t == BWM_KEY)
            break;
    }
    return 0;
}
#endif
