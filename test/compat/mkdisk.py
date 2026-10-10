#!/usr/bin/env python3
"""Writes a BastiOS disk image (src/diskfs.bastio's format, "BSTDISK2")
holding files.

    mkdisk.py OUT.img FILE...

Each FILE is stored at the top of the disk under its base name, so a
program shows up in the guest as /disk/NAME. The format, in 4096-byte
blocks: block 0 the magic "BSTDISK2"; blocks 1..4 a directory of 64
entries of 256 bytes (byte 0 kind, 1 a file; byte 1 the name length; u64s
at 8 the parent, 64 for the top; 16 the size; 24 the root map block; 32 the
mode; 40 the modification time; the name at 56); then data and maps. A
file's root map is a block of 512 u64 block numbers, each naming a
first-level map of 512 more naming data blocks.
"""
import os, struct, sys, time

BLOCK = 4096
DSLOTS = 64
DENT = 256
DIR_B0 = 1
DATA_B0 = 5
PTRS = 512
DTOP = 64
DNAME_MAX = 200
MODE_FILE = 0o755


def main():
    out, files = sys.argv[1], sys.argv[2:]
    if len(files) > DSLOTS:
        sys.exit(f"mkdisk: {len(files)} files, the directory holds {DSLOTS}")
    blocks = {}                          # block number -> bytes
    nxt = DATA_B0

    def alloc(data):
        nonlocal nxt
        b = nxt
        nxt += 1
        blocks[b] = data
        return b

    dirent = bytearray(DSLOTS * DENT)
    for i, path in enumerate(files):
        name = os.path.basename(path).encode()
        data = open(path, "rb").read()
        if len(name) > DNAME_MAX:
            sys.exit(f"mkdisk: name too long: {path}")
        n = (len(data) + BLOCK - 1) // BLOCK
        if n > PTRS * PTRS:
            sys.exit(f"mkdisk: {path} is too big")
        root = 0
        if n:
            data_blocks = [alloc(data[j * BLOCK:(j + 1) * BLOCK].ljust(BLOCK, b"\0")) for j in range(n)]
            firsts = []
            for j in range(0, n, PTRS):
                part = data_blocks[j:j + PTRS]
                firsts.append(alloc(struct.pack(f"<{PTRS}Q", *(part + [0] * (PTRS - len(part))))))
            root = alloc(struct.pack(f"<{PTRS}Q", *(firsts + [0] * (PTRS - len(firsts)))))
        e = i * DENT
        dirent[e] = 1
        dirent[e + 1] = len(name)
        struct.pack_into("<QQQQQ", dirent, e + 8, DTOP, len(data), root, MODE_FILE, int(time.time()))
        dirent[e + 56:e + 56 + len(name)] = name
    # Room to spare for what the programs write.
    total = max(nxt + 2048, 8192)
    img = bytearray(total * BLOCK)
    img[0:8] = b"BSTDISK2"
    img[DIR_B0 * BLOCK:DIR_B0 * BLOCK + len(dirent)] = dirent
    for b, data in blocks.items():
        img[b * BLOCK:(b + 1) * BLOCK] = data
    with open(out, "wb") as f:
        f.write(img)


if __name__ == "__main__":
    main()
