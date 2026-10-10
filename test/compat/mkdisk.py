#!/usr/bin/env python3
"""Writes a BastiOS disk image (src/diskfs.bastio's format) holding files.

    mkdisk.py OUT.img FILE...

Each FILE is stored under its base name, so a program shows up in the guest
as /disk/NAME. The format: sector 0 the magic "BSTDISK1"; sectors 1..4 a
directory of 32 entries of 64 bytes (byte 0 used, byte 1 the name length,
bytes 2..30 the name, bytes 32..40 the size, little-endian); slot i's data
at sector 8 + 2048 i, 1 MiB per slot.
"""
import os, struct, sys

SECTOR = 512
DSLOTS = 32
SLOT_SECTORS = 2048
SLOT_BYTES = SLOT_SECTORS * SECTOR
DATA0 = 8
DENT = 64
DNAME_MAX = 28

def main():
    out, files = sys.argv[1], sys.argv[2:]
    if len(files) > DSLOTS:
        sys.exit(f"mkdisk: {len(files)} files, the disk holds {DSLOTS}")
    img = bytearray((DATA0 + DSLOTS * SLOT_SECTORS) * SECTOR)
    img[0:8] = b"BSTDISK1"
    for i, path in enumerate(files):
        name = os.path.basename(path).encode()
        data = open(path, "rb").read()
        if len(name) > DNAME_MAX:
            sys.exit(f"mkdisk: name too long: {path}")
        if len(data) > SLOT_BYTES:
            sys.exit(f"mkdisk: {path} is {len(data)} bytes, a slot holds {SLOT_BYTES}")
        e = SECTOR + i * DENT
        img[e] = 1
        img[e + 1] = len(name)
        img[e + 2:e + 2 + len(name)] = name
        img[e + 32:e + 40] = struct.pack("<Q", len(data))
        at = (DATA0 + i * SLOT_SECTORS) * SECTOR
        img[at:at + len(data)] = data
    with open(out, "wb") as f:
        f.write(img)

if __name__ == "__main__":
    main()
