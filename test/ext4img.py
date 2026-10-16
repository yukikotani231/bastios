#!/usr/bin/env python3
"""Makes the ext4 images user/mext4.c checks, with mke2fs -d.

    ext4img.py OUTDIR

Writes e4.img (ext4, 4 KiB blocks, extents, journal), e2.img (ext2, 1 KiB
blocks, block maps with indirect blocks), e4k.img (ext4, 1 KiB blocks, no
journal) and four that must be refused: bad-zero.img (no file system),
bad-short.img (e4.img cut off after 1 MiB), bad-recover.img (the "journal
needs recovery" feature set) and bad-inline.img (inline data, which is not
read). Exits 77 if mke2fs is not installed.
"""
import os, shutil, subprocess, sys

def main():
    out = sys.argv[1]
    if not shutil.which("mke2fs"):
        print("ext4img: mke2fs is not installed", file=sys.stderr)
        return 77
    os.makedirs(out, exist_ok=True)
    tree = os.path.join(out, "tree")
    shutil.rmtree(tree, ignore_errors=True)
    for d in ("dir/sub", "empty"):
        os.makedirs(os.path.join(tree, d))
    def w(name, data):
        with open(os.path.join(tree, name), "wb") as f:
            f.write(data)
    w("small", b"hello ext4\n")
    os.chmod(os.path.join(tree, "small"), 0o640)
    w("big", bytes((i * 7 + i // 251) % 256 for i in range(700000)))
    with open(os.path.join(tree, "sparse"), "wb") as f:
        f.truncate(3 << 20)
        f.seek(2 << 20)
        f.write(b"tail-data\n")
    w("dir/sub/deep", b"deep file\n")
    w("zero", b"")
    os.symlink("small", os.path.join(tree, "short"))
    os.symlink("dir/sub/" + "x" * 70 + "/../deep", os.path.join(tree, "long"))
    os.link(os.path.join(tree, "small"), os.path.join(tree, "dir/hard"))
    os.mkfifo(os.path.join(tree, "pipe"))
    w("dir/" + "n" * 200, b"long name\n")
    for i in range(300):
        w(f"dir/f{i:03}", f"{i}\n".encode())
    def mk(name, *opts):
        path = os.path.join(out, name)
        if os.path.exists(path):
            os.unlink(path)
        subprocess.run(["mke2fs", "-q", *opts, "-d", tree, path, "8M"], check=True)
    mk("e4.img", "-t", "ext4", "-b", "4096")
    mk("e2.img", "-t", "ext2", "-O", "^resize_inode", "-b", "1024")
    mk("e4k.img", "-t", "ext4", "-b", "1024", "-O", "^has_journal")
    with open(os.path.join(out, "e4.img"), "rb") as f:
        good = f.read()
    def bad(name, data):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
    bad("bad-zero.img", bytes(1 << 20))
    bad("bad-short.img", good[:1 << 20])
    for name, bit in (("bad-recover.img", 0x4), ("bad-inline.img", 0x8000)):
        img = bytearray(good)
        flags = int.from_bytes(img[1024 + 0x60:1024 + 0x64], "little") | bit
        img[1024 + 0x60:1024 + 0x64] = flags.to_bytes(4, "little")
        bad(name, bytes(img))
    shutil.rmtree(tree)
    return 0

sys.exit(main())
