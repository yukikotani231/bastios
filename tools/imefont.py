#!/usr/bin/env python3
"""Writes the glyphs bwm's kanji conversion draws, as a C table: those of the
dictionary (user/bwmdict.h) and the hiragana and katakana, taken from
src/font16.bin (layout in tools/mkfont.py).

usage: tools/imefont.py src/font16.bin user/bwmdict.h > build/user/imefont.h
"""
import struct
import sys


def main():
    font = open(sys.argv[1], "rb").read()
    dic = open(sys.argv[2], encoding="utf-8").read()
    nw, nn = struct.unpack_from("<II", font, 0)
    wtab = struct.unpack_from("<%dH" % nw, font, 8)
    ntab = struct.unpack_from("<%dH" % nn, font, 8 + 2 * nw)
    wg = 8 + 2 * nw + 2 * nn
    ng = wg + 32 * nw
    need = {ord(c) for c in dic if ord(c) >= 128}
    want = need | set(range(0x3041, 0x3097)) | set(range(0x30A1, 0x30FB)) | {0x30FC}
    rows = []
    for cp in sorted(want):
        if cp in wtab:
            i = wtab.index(cp)
            rows.append((cp, 1, font[wg + 32 * i:wg + 32 * i + 32]))
        elif cp in ntab:
            i = ntab.index(cp)
            rows.append((cp, 0, font[ng + 16 * i:ng + 16 * i + 16] + bytes(16)))
        elif cp in need:
            sys.exit("imefont: no glyph for U+%04X" % cp)
    print("/* Made by tools/imefont.py: the glyphs of user/bwmdict.h and the kana, 16 rows of a byte or two (the left cell first). */")
    print("static const struct { unsigned short cp; unsigned char wide; unsigned char g[32]; } ime_font[] = {")
    for cp, wide, g in rows:
        print("    { 0x%04X, %d, { %s } }," % (cp, wide, ", ".join(str(b) for b in g)))
    print("};")


main()
