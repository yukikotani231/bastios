#!/usr/bin/env python3
"""Makes the FAT images test/shell_test.sh mounts (src/fat.bastio), with mkfs.fat and mcopy.

    fatimg.py OUTDIR

Writes f16.img (FAT16), f32.img (FAT32) and bad-fat.img (a FAT32 image with
its FAT cut short, which must be refused). The tree: HELLO.TXT, a long
mixed-case name, a file of numbered lines (L1 .. L12000) across many clusters, a directory
with a sub directory, and an empty file. Exits 77 if mkfs.fat or mcopy is
missing.
"""
import os, shutil, subprocess, sys

def main():
    out = sys.argv[1]
    if not (shutil.which("mkfs.fat") and shutil.which("mcopy") and shutil.which("mmd")):
        print("fatimg: mkfs.fat or mtools is not installed", file=sys.stderr)
        return 77
    os.makedirs(out, exist_ok=True)
    src = os.path.join(out, "src")
    os.makedirs(src, exist_ok=True)
    def w(name, data):
        with open(os.path.join(src, name), "wb") as f:
            f.write(data)
    w("hello.txt", b"hello fat\n")
    w("A Longer File Name.txt", b"long name\n")
    w("big.txt", "".join(f"L{i}\n" for i in range(1, 12001)).encode())
    w("deep.txt", b"deep file\n")
    w("empty", b"")
    for name, opts in (("f16.img", ["-F", "16", "-s", "2"]), ("f32.img", ["-F", "32", "-s", "1"])):
        path = os.path.join(out, name)
        if os.path.exists(path):
            os.unlink(path)
        subprocess.run(["mkfs.fat", *opts, "-C", path, "4096" if name == "f32.img" else "8192"], check=True, stdout=subprocess.DEVNULL)
        env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        def mt(*a):
            subprocess.run(list(a), check=True, env=env)
        for f in ("hello.txt", "A Longer File Name.txt", "big.txt", "empty"):
            mt("mcopy", "-i", path, os.path.join(src, f), "::" + f)
        mt("mmd", "-i", path, "::dir")
        mt("mmd", "-i", path, "::dir/sub")
        mt("mcopy", "-i", path, os.path.join(src, "deep.txt"), "::dir/sub/deep.txt")
    with open(os.path.join(out, "f32.img"), "rb") as f:
        good = bytearray(f.read())
    with open(os.path.join(out, "bad-fat.img"), "wb") as f:
        f.write(good[:20 * 512])
    return 0

sys.exit(main())
