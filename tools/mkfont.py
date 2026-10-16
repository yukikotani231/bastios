#!/usr/bin/env python3
"""Builds src/font16.bin, the glyphs the framebuffer console draws beyond ASCII.

    tools/mkfont.py SHINONOME_BDF_DIR FONT8X8_DIR src/font16.bin

Sources (all public domain):
  Shinonome 16 dot fonts, https://openlab.ring.gr.jp/efont/shinonome/
    (xfonts-shinonome 0.9.11): shnmk16.bdf, JIS X 0208 in 16x16 cells, and
    shnm8x16r.bdf, whose JIS X 0201 half-width katakana are 8x16.
  font8x8, https://github.com/dhepper/font8x8: font8x8_ext_latin.h, _greek.h,
    _box.h and _block.h, 8x8 glyphs that are doubled vertically here.

A glyph's rows are bytes with bit 0 the leftmost pixel. A narrow glyph (one
cell) is 16 rows of one byte; a wide glyph (two cells, East Asian wide and
fullwidth characters) is 16 rows of two bytes, the left cell's row first.

File layout, all numbers little endian:
    u32 nwide, u32 nnarrow
    nwide   * u16   code points of the wide glyphs, ascending
    nnarrow * u16   code points of the narrow glyphs, ascending
    nwide   * 32    wide glyphs, in the same order
    nnarrow * 16    narrow glyphs

Which code points go where follows cp_width in src/utf8.bastio: a wide glyph
is kept only for a code point that function says takes two cells, a narrow one
only for a code point that takes one, so the glyph always fits its cells.
"""
import re, struct, sys

def cp_width(cp):
    """The same ranges as cp_width in src/utf8.bastio."""
    if cp < 0x300: return 1
    if cp < 0x370: return 0
    if cp < 0x1100: return 1
    if cp < 0x1160: return 2
    if 0x200B <= cp < 0x2010: return 0
    if 0xFE00 <= cp < 0xFE10: return 0
    for lo, hi in ((0x2E80, 0x303F), (0x3041, 0x3400), (0x3400, 0x4DC0), (0x4E00, 0xA4D0),
                   (0xAC00, 0xD7A4), (0xF900, 0xFB00), (0xFE30, 0xFE70), (0xFF00, 0xFF61),
                   (0xFFE0, 0xFFE7)):
        if lo <= cp < hi: return 2
    return 1

def bdf(path):
    """{encoding: [row as int, ...]} of a BDF file."""
    out, enc, rows, inbits = {}, None, [], False
    for line in open(path, encoding="latin-1"):
        line = line.strip()
        if line.startswith("ENCODING"):
            enc = int(line.split()[1])
        elif line == "BITMAP":
            rows, inbits = [], True
        elif line == "ENDCHAR":
            inbits = False
            out[enc] = rows
        elif inbits:
            out[enc] = None
            rows.append(int(line, 16))
    return out

def flip(byte):
    """BDF puts the leftmost pixel in bit 7; the kernel's glyphs in bit 0."""
    return int(format(byte, "08b")[::-1], 2)

def h8x8(path):
    """{code point: 8 rows} from a font8x8 header."""
    out = {}
    for m in re.finditer(r"\{\s*((?:0x[0-9A-Fa-f]{2},?\s*){8})\},\s*//\s*U\+([0-9A-Fa-f]{4})", open(path).read()):
        out[int(m.group(2), 16)] = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]
    return out

def main(shn, f8, dest):
    wide, narrow = {}, {}
    k16 = bdf(f"{shn}/shnmk16.bdf")
    for jis, rows in k16.items():
        if len(rows) != 16 or not 0x2121 <= jis <= 0x7E7E:
            continue
        try:
            cp = ord(bytes([(jis >> 8) | 0x80, (jis & 0xFF) | 0x80]).decode("euc_jp"))
        except UnicodeDecodeError:
            continue
        if cp_width(cp) == 2 and cp < 0x10000:
            g = b""
            for r in rows:
                g += bytes([flip(r >> 8), flip(r & 0xFF)])
            wide[cp] = g
    k8 = bdf(f"{shn}/shnm8x16r.bdf")
    for code, rows in k8.items():
        if 0xA1 <= code <= 0xDF and len(rows) == 16:
            narrow[0xFF61 + code - 0xA1] = bytes(flip(r) for r in rows)
    for name in ("ext_latin", "greek", "box", "block"):
        for cp, rows in h8x8(f"{f8}/font8x8_{name}.h").items():
            if cp >= 0xA0 and cp_width(cp) == 1 and cp not in narrow:
                narrow[cp] = bytes(b for r in rows for b in (r, r))
    wcps, ncps = sorted(wide), sorted(narrow)
    blob = struct.pack("<II", len(wcps), len(ncps))
    blob += b"".join(struct.pack("<H", c) for c in wcps)
    blob += b"".join(struct.pack("<H", c) for c in ncps)
    blob += b"".join(wide[c] for c in wcps)
    blob += b"".join(narrow[c] for c in ncps)
    open(dest, "wb").write(blob)
    print(f"{dest}: {len(wcps)} wide and {len(ncps)} narrow glyphs, {len(blob)} bytes")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
