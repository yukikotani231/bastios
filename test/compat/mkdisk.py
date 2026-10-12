#!/usr/bin/env python3
"""Writes a BastiOS disk image (src/diskfs.bastio's format) holding files.

    mkdisk.py OUT.img FILE... [DEST=FILE]... [DEST=DIR]...

Each FILE is stored under its base name, so a program shows up in the guest
as /disk/NAME. DEST=FILE stores FILE as /disk/DEST, where DEST may name
directories (a/b/c.h), which are made as needed. DEST=DIR stores every file
under DIR, keeping its path below DIR, under /disk/DEST. The format ("BSTDISK3"),
in 4096-byte blocks: block 0 the superblock, its first 8 bytes the magic,
then at byte 8 * db the block holding directory block db for db = 4..63;
blocks 1..4 directory blocks 0..3. The directory holds up to 4096 entries
of 256 bytes, 16 to a block, entry 64 unused (byte 0 the kind, 1 a file; byte 1 the name length;
then little-endian u64s: at 8 the parent, 64 for the top directory, at 16
the size, at 24 the root map block, at 32 the mode, at 40 the modification
time; the name from byte 56); from block 5, data and maps. A root map is a
block of 512 block numbers of first-level maps, each a block of 512 block
numbers of data blocks; 0 is a hole.
"""
import os, struct, sys

BLOCK = 4096
DSLOTS = 4096
DENT = 256
DPB = BLOCK // DENT
DIR_FIXED = 4
DATA_B0 = 5
PTRS = 512
DTOP = 64
DNAME_MAX = 200
MODE = 0o755
DISK_BYTES = 33 << 20             # at least; more when the files need it
# room left for the guest to write (MKDISK_FREE_MB overrides)
FREE_BYTES = int(os.environ.get("MKDISK_FREE_MB", "16")) << 20
MAX_DISK = (1 << 28) * BLOCK      # src/diskfs.bastio's MAX_BLOCKS

def expand(args):
    """DEST=DIR arguments as one DEST/PATH=FILE argument per file under DIR."""
    for a in args:
        dest, src = a.split("=", 1) if "=" in a else (None, a)
        if dest is None or not os.path.isdir(src):
            yield a
            continue
        for root, dirs, files in os.walk(src):
            dirs.sort()
            for f in sorted(files):
                path = os.path.join(root, f)
                yield dest + "/" + os.path.relpath(path, src) + "=" + path

def main():
    out, args = sys.argv[1], list(expand(sys.argv[2:]))
    need = sum(os.path.getsize(a.split("=", 1)[-1]) for a in args)
    size = max(DISK_BYTES, min(MAX_DISK, (need * 101 // 100 + FREE_BYTES) // BLOCK * BLOCK))
    img = bytearray(size)
    img[0:8] = b"BSTDISK3"
    nxt = DATA_B0
    slots = 0
    dirs = {}                      # path of a directory -> its entry

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

    def entry(kind, name, parent, size, root):
        nonlocal slots
        if slots >= DSLOTS:
            sys.exit(f"mkdisk: more than {DSLOTS} files and directories, the disk holds {DSLOTS}")
        if not name or len(name) > DNAME_MAX:
            sys.exit(f"mkdisk: bad name: {name!r}")
        if slots == DTOP:
            slots += 1                 # its number means the top directory
        i = slots
        slots += 1
        db = i // DPB
        if db < DIR_FIXED:
            dblock = 1 + db
        else:
            dblock = struct.unpack_from("<Q", img, 8 * db)[0]
            if dblock == 0:
                dblock = alloc()
                img[8 * db:8 * db + 8] = struct.pack("<Q", dblock)
        e = dblock * BLOCK + (i % DPB) * DENT
        img[e] = kind
        img[e + 1] = len(name)
        img[e + 8:e + 48] = struct.pack("<QQQQQ", parent, size, root, MODE, 0)
        img[e + 56:e + 56 + len(name)] = name
        return i

    def parent_of(dest):
        parent, path = DTOP, ""
        for part in dest.split("/")[:-1]:
            path = path + "/" + part
            if path not in dirs:
                dirs[path] = entry(2, part.encode(), parent, 0, 0)
            parent = dirs[path]
        return parent

    for arg in args:
        dest, src = arg.split("=", 1) if "=" in arg else (os.path.basename(arg), arg)
        data = open(src, "rb").read()
        if len(data) > PTRS * PTRS * BLOCK:
            sys.exit(f"mkdisk: {src} is too large")
        parent = parent_of(dest)
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
        entry(1, dest.split("/")[-1].encode(), parent, len(data), root)
    with open(out, "wb") as f:
        f.write(img)

if __name__ == "__main__":
    main()
