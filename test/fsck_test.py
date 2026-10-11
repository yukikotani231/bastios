#!/usr/bin/env python3
"""Checks that test/fsck.py catches what it claims to: each case damages a
fresh image (made by test/compat/mkdisk.py) in one way and expects fsck to
say clean, clean with a note (a state the next mount clears), or an error.
"""
import os, struct, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
BLOCK, DENT, DIR_AT, DTOP = 4096, 256, 4096, 64


def entry(img, i):
    return DIR_AT + i * DENT


def put(img, at, v):
    img[at:at + 8] = struct.pack("<Q", v)


def get(img, at):
    return struct.unpack_from("<Q", img, at)[0]


def data_block(img, i, k=0):
    """Block k of entry i's file."""
    root = get(img, entry(img, i) + 24)
    l1 = get(img, root * BLOCK)
    return get(img, l1 * BLOCK + 8 * k)


def add_link(img, slot, name, target, parent=DTOP):
    e = entry(img, slot)
    img[e:e + DENT] = bytes(DENT)
    img[e] = 4
    img[e + 1] = len(name)
    put(img, e + 8, parent)
    put(img, e + 48, target)
    img[e + 56:e + 56 + len(name)] = name


def same_block(img):
    put(img, get(img, get(img, entry(img, 1) + 24) * BLOCK) * BLOCK, data_block(img, 0))


def past_end(img):
    put(img, entry(img, 0) + 16, 10)        # 5000 bytes become 10: block 1 is past the end


def tail(img):
    put(img, entry(img, 1) + 16, 2)         # "abc" becomes "ab": the "c" is past the end


def cycle(img):
    for i in (0, 1):
        e = entry(img, i)
        img[e] = 2
        put(img, e + 16, 0)
        put(img, e + 24, 0)
    put(img, entry(img, 0) + 8, 1)
    put(img, entry(img, 1) + 8, 0)


def dup_name(img):
    e0, e1 = entry(img, 0), entry(img, 1)
    img[e1 + 1] = img[e0 + 1]
    img[e1 + 56:e1 + DENT] = img[e0 + 56:e0 + DENT]


def off_disk(img):
    put(img, get(img, get(img, entry(img, 0) + 24) * BLOCK) * BLOCK, 1 << 40)


def dir_block(img, db):
    return get(img, 8 * db)


def gap(img):
    put(img, 8 * 6, dir_block(img, 4))      # block 6 named, block 4 not
    put(img, 8 * 4, 0)


def shared_dir_block(img):
    # A file's data block is also directory block 4.
    put(img, get(img, get(img, entry(img, 0) + 24) * BLOCK) * BLOCK, dir_block(img, 4))


def dtop_used(img):
    e = dir_block(img, 4) * BLOCK           # entry 64: the first of directory block 4
    img[e] = 1
    img[e + 1] = 1
    put(img, e + 8, DTOP)
    img[e + 56] = ord("q")


# On an image with 100 more files, so the directory has grown past its
# fixed blocks.
GROWN = [
    ("a grown directory", None, "clean"),
    ("a gap in the directory's blocks", gap, "error"),
    ("a directory block a file also uses", shared_dir_block, "error"),
    ("entry 64 in use", dtop_used, "error"),
]

CASES = [
    ("untouched", None, "clean"),
    ("a hard link", lambda img: add_link(img, 5, b"other", 0), "clean"),
    ("a link its file took the name of", lambda img: add_link(img, 5, b"big", 0), "note"),
    ("a link to a free entry", lambda img: add_link(img, 5, b"gone", 9), "note"),
    ("an orphan", lambda img: img.__setitem__(entry(img, 1), 3), "note"),
    ("two files sharing a block", same_block, "error"),
    ("a block past the end", past_end, "error"),
    ("bytes past the end", tail, "error"),
    ("a cycle of parents", cycle, "error"),
    ("two names alike", dup_name, "error"),
    ("a block off the disk", off_disk, "error"),
    ("an unknown kind", lambda img: img.__setitem__(entry(img, 0), 9), "error"),
    ("a slash in a name", lambda img: add_link(img, 5, b"x/y", 0), "error"),
]


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as t:
        big, small = os.path.join(t, "big"), os.path.join(t, "small")
        open(big, "wb").write(b"b" * 5000)
        open(small, "wb").write(b"abc")
        base = os.path.join(t, "base.img")
        subprocess.run([sys.executable, os.path.join(here, "compat", "mkdisk.py"), base, big, small], check=True)
        pristine = open(base, "rb").read()
        many = []
        for k in range(100):
            many.append(os.path.join(t, f"m{k}"))
            open(many[-1], "wb").write(b"m" * (k + 1))
        grown = os.path.join(t, "grown.img")
        subprocess.run([sys.executable, os.path.join(here, "compat", "mkdisk.py"), grown, big] + many, check=True)
        grown_img = open(grown, "rb").read()
        runs = [(n, d, w, pristine) for n, d, w in CASES] + [(n, d, w, grown_img) for n, d, w in GROWN]
        for name, damage, want, start in runs:
            img = bytearray(start)
            if damage:
                damage(img)
            path = os.path.join(t, "case.img")
            open(path, "wb").write(img)
            r = subprocess.run([sys.executable, os.path.join(here, "fsck.py"), path], capture_output=True, text=True)
            got = "error" if r.returncode else ("note" if "fsck: note" in r.stdout else "clean")
            ok = got == want
            print(("ok  " if ok else "FAIL"), f"fsck on {name}: {got}")
            if not ok:
                print(r.stdout)
            fails += not ok
    print(f"---- {fails} failed ----")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
