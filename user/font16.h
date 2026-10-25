#ifndef FONT16_H
#define FONT16_H
/* The glyphs beyond ASCII, for bterm and the programs that run inside it. */
/* The glyphs beyond ASCII come from src/font16.bin, the kernel console's font (layout in tools/mkfont.py). */
__asm__(".section .rodata\n.balign 4\nfont16_bin:\n.incbin \"src/font16.bin\"\n.previous\n");
extern const unsigned char font16_bin[];

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* The glyph of code point CP: 16 rows of one byte, or of two (the left cell's first) when *WIDE is set; 0 if the font has none. */
static const unsigned char *glyph16(unsigned cp, int *wide)
{
    static unsigned nw, nn;
    static const unsigned char *wtab, *ntab, *wg, *ng;
    if (!wtab) {
        nw = font16_bin[0] | font16_bin[1] << 8 | font16_bin[2] << 16 | (unsigned)font16_bin[3] << 24;
        nn = font16_bin[4] | font16_bin[5] << 8 | font16_bin[6] << 16 | (unsigned)font16_bin[7] << 24;
        wtab = font16_bin + 8;
        ntab = wtab + 2 * nw;
        wg = ntab + 2 * nn;
        ng = wg + 32 * nw;
    }
    for (int pass = 0; pass < 2; pass++) {
        const unsigned char *tab = pass ? ntab : wtab;
        unsigned lo = 0, hi = pass ? nn : nw;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2, v = rd16(tab + 2 * mid);
            if (v == cp) {
                *wide = !pass;
                return pass ? ng + 16 * mid : wg + 32 * mid;
            }
            if (v < cp)
                lo = mid + 1;
            else
                hi = mid;
        }
    }
    return 0;
}

/* How many cells a code point takes: 0 for a combining or zero-width one, 2 for a wide one (the ranges of cp_width in
 * src/utf8.bastio). */
static int cp_width(unsigned cp)
{
    static const unsigned wide[][2] = { { 0x2E80, 0x303F }, { 0x3041, 0x3400 }, { 0x3400, 0x4DC0 }, { 0x4E00, 0xA4D0 },
                                        { 0xAC00, 0xD7A4 }, { 0xF900, 0xFB00 }, { 0xFE30, 0xFE70 }, { 0xFF00, 0xFF61 },
                                        { 0xFFE0, 0xFFE7 } };
    if (cp < 0x300) return 1;
    if (cp < 0x370) return 0;
    if (cp < 0x1100) return 1;
    if (cp < 0x1160) return 2;
    if (cp >= 0x200B && cp < 0x2010) return 0;
    if (cp >= 0xFE00 && cp < 0xFE10) return 0;
    for (unsigned i = 0; i < sizeof wide / sizeof wide[0]; i++)
        if (cp >= wide[i][0] && cp < wide[i][1])
            return 2;
    return 1;
}

#endif
