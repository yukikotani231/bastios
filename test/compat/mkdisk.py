#!/usr/bin/env python3
"""Writes a BastiOS disk image (src/diskfs.bastio's format) holding files.

    mkdisk.py OUT.img FILE...

Each FILE is stored under its base name, so a program shows up in the guest
as /disk/NAME. The format ("BSTDISK2"), in 4096-byte blocks: block 0 the
superblock, its first 8 bytes the magic; blocks 1..4 a directory of 64
entries of 256 bytes (byte 0 the kind, 1 a file; byte 1 the name length;
then little-endian u64s: at 8 the parent, 64 for the top directory, at 16
the size, at 24 the root map block, at 32 the mode, at 40 the modification
time; the name from byte 56); from block 5, data and maps. A root map is a
block of 512 block numbers of first-level maps, each a block of 512 block
numbers of data blocks; 0 is a hole.
"""
import os, struct, sys

BLOCK = 4096
DSLOTS = 64
DENT = 256
DATA_B0 = 5
PTRS = 512
DTOP = 64
DNAME_MAX = 200
MODE = 0o755
DISK_BYTES = 33 << 20

def main():
    out, files = sys.argv[1], sys.argv[2:]
    if len(files) > DSLOTS:
        sys.exit(f"mkdisk: {len(files)} files, the disk holds {DSLOTS}")
    img = bytearray(DISK_BYTES)
    img[0:8] = b"BSTDISK2"
    nxt = DATA_B0

    def alloc():
        nonlocal nxt
        b = nxt
        nxt += 1
        if (nxt + 1) * BLOCK > len(img):
            sys.exit("mkdisk: the files do not fit on the disk")
        return b

    def put_ptr(block, j, p):
        at = block * BLOCK + 8 * j
        img[at:at + 8] = struct.pack("<Q", p)

    for i, path in enumerate(files):
        name = os.path.basename(path).encode()
        data = open(path, "rb").read()
        if not name or len(name) > DNAME_MAX:
            sys.exit(f"mkdisk: bad name: {path}")
        if len(data) > PTRS * PTRS * BLOCK:
            sys.exit(f"mkdisk: {path} is too large")
        root = 0
        if data:
            root = alloc()
            maps = {}
            for k in range(0, len(data), BLOCK):
                b = k // BLOCK
                if b // PTRS not in maps:
                    maps[b // PTRS] = alloc()
                    put_ptr(root, b // PTRS, maps[b // PTRS])
                d = alloc()
                put_ptr(maps[b // PTRS], b % PTRS, d)
                chunk = data[k:k + BLOCK]
                img[d * BLOCK:d * BLOCK + len(chunk)] = chunk
        e = BLOCK + i * DENT
        img[e] = 1
        img[e + 1] = len(name)
        img[e + 8:e + 48] = struct.pack("<QQQQQ", DTOP, len(data), root, MODE, 0)
        img[e + 56:e + 56 + len(name)] = name
    with open(out, "wb") as f:
        f.write(img)

if __name__ == "__main__":
    main()
