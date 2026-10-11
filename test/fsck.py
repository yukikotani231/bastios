#!/usr/bin/env python3
"""Checks a BastiOS disk image (src/diskfs.bastio's BSTDISK3 format, or
the older BSTDISK2, which is the same with only the fixed directory blocks).

    fsck.py DISK.img

It reads the image the way a mount would and checks every rule the kernel
keeps, without changing anything:

  - the magic, and every entry's kind (0 free, 1 file, 2 directory,
    3 orphan, 4 hard link);
  - the directory's blocks past the fixed four: named in the superblock
    in order with no gap, each a block of the disk used by nothing else,
    and entry 64 (DTOP) never used;
  - names: 1 to 200 bytes, no "/" or NUL, nothing after the name, and no
    two names alike in one directory;
  - parents: the top (64) or a directory, reached without a cycle;
  - directories: size 0, no root map;
  - files and orphans: a size up to 1 GiB, every block number in a map 0
    (a hole) or a block of the disk, no block in two places (two files,
    or twice in one), no data block wholly past the end, and the bytes of
    the last block past the end all zero;
  - hard links: naming a file entry under another name.

Two states are what a power cut leaves and the next mount clears, so they
are reported but are not errors: an orphan (a file removed while open)
and a hard link whose file is gone or has taken its name
(disk_take_name). Prints "fsck: clean ..." and exits 0, or prints each
error and exits 1.
"""
import struct, sys

BLOCK = 4096
DIR_AT = BLOCK
DSLOTS = 1024
DENT = 256
DPB = BLOCK // DENT
DIR_FIXED = 4
DIR_BLOCKS = DSLOTS // DPB
DATA_B0 = 5
PTRS = 512
MAX_BLOCKS = 65536
DTOP = 64
DNAME_MAX = 200
FILE_MAX = PTRS * PTRS * BLOCK
E_NAME = 56
FILE, DIR, ORPHAN, LINK = 1, 2, 3, 4


def u64(b, at):
    return struct.unpack_from("<Q", b, at)[0]


def check(img):
    errors, notes = [], []
    if img[0:8] not in (b"BSTDISK2", b"BSTDISK3"):
        return ["no BSTDISK3 or BSTDISK2 magic (a blank disk is formatted at mount)"], notes, {}
    blocks = min(len(img) // BLOCK, MAX_BLOCKS)
    # Where each directory block is: the fixed ones, then the superblock's.
    dblocks = [1 + db for db in range(DIR_FIXED)]
    if img[0:8] == b"BSTDISK3":
        ended = False
        for db in range(DIR_FIXED, DIR_BLOCKS):
            b = u64(img, 8 * db)
            if b == 0:
                ended = True
            elif ended:
                errors.append(f"superblock: directory block {db} ({b}) is named past a gap")
            elif not DATA_B0 <= b < blocks:
                errors.append(f"superblock: directory block {db} ({b}) is off the disk")
                ended = True
            else:
                dblocks.append(b)
    slots = DPB * len(dblocks)
    ents = []
    for i in range(DSLOTS):
        if i < slots:
            at = dblocks[i // DPB] * BLOCK + (i % DPB) * DENT
            e = img[at:at + DENT]
        else:
            e = bytes(DENT)
        ents.append({
            "kind": e[0], "nlen": e[1], "parent": u64(e, 8), "size": u64(e, 16),
            "root": u64(e, 24), "mode": u64(e, 32), "link": u64(e, 48),
            "name": bytes(e[E_NAME:E_NAME + e[1]]), "tail": bytes(e[E_NAME + e[1]:]),
        })

    def where(i):
        return f"entry {i} ({ents[i]['name'].decode(errors='replace')!r})"

    named = (FILE, DIR, LINK)
    if ents[DTOP]["kind"]:
        errors.append(f"entry {DTOP}: in use, but its number means the top directory")
        ents[DTOP]["kind"] = 0
    for i, e in enumerate(ents):
        if e["kind"] > LINK:
            errors.append(f"entry {i}: unknown kind {e['kind']}")
    # Names and parents.
    for i, e in enumerate(ents):
        if e["kind"] not in named:
            continue
        if not 1 <= e["nlen"] <= DNAME_MAX:
            errors.append(f"{where(i)}: name length {e['nlen']}")
        elif b"/" in e["name"] or b"\0" in e["name"]:
            errors.append(f"{where(i)}: '/' or NUL in the name")
        if any(e["tail"]):
            errors.append(f"{where(i)}: bytes after the name")
        p, steps = e["parent"], 0
        while p != DTOP:
            if p >= DSLOTS or ents[p]["kind"] != DIR:
                errors.append(f"{where(i)}: parent {p} is not a directory")
                break
            steps += 1
            if steps > DSLOTS:
                errors.append(f"{where(i)}: its parents form a cycle")
                break
            p = ents[p]["parent"]
    seen = {}
    for i, e in enumerate(ents):
        if e["kind"] not in named:
            continue
        key = (e["parent"], e["name"])
        if key in seen:
            j = seen[key]
            pair = {ents[i]["kind"], ents[j]["kind"]}
            link, file = (i, j) if ents[i]["kind"] == LINK else (j, i)
            if pair == {LINK, FILE} and ents[link]["link"] == file:
                pass                    # reported with the links below
            else:
                errors.append(f"{where(i)} and entry {j} have the same name")
        else:
            seen[key] = i
    # Directories and links.
    stats = {"files": 0, "dirs": 0, "links": 0, "orphans": 0, "blocks": 0}
    for i, e in enumerate(ents):
        if e["kind"] == DIR:
            stats["dirs"] += 1
            if e["size"] or e["root"]:
                errors.append(f"{where(i)}: a directory with a size or a root map")
        elif e["kind"] == LINK:
            stats["links"] += 1
            t = e["link"]
            if t >= DSLOTS or ents[t]["kind"] != FILE:
                notes.append(f"{where(i)}: a hard link to no file (the mount drops it)")
            elif (ents[t]["parent"], ents[t]["name"]) == (e["parent"], e["name"]):
                notes.append(f"{where(i)}: a hard link its file took the name of (the mount drops it)")
    # Files: maps and blocks; the directory's own blocks first.
    owner = {}
    for db, b in enumerate(dblocks[DIR_FIXED:], DIR_FIXED):
        owner[b] = f"directory block {db}"
        stats["blocks"] += 1

    def claim(b, i, what):
        if b in owner:
            errors.append(f"{where(i)}: block {b} ({what}) is also used by {owner[b]}")
        else:
            owner[b] = f"entry {i}"
            stats["blocks"] += 1

    def block_ok(b, i, what):
        if b == 0:
            return False
        if not DATA_B0 <= b < blocks:
            errors.append(f"{where(i)}: {what} block {b} is off the disk")
            return False
        return True

    for i, e in enumerate(ents):
        if e["kind"] not in (FILE, ORPHAN):
            continue
        stats["files" if e["kind"] == FILE else "orphans"] += 1
        if e["kind"] == ORPHAN:
            notes.append(f"{where(i)}: an orphan (the mount frees it)")
        size = e["size"]
        if size > FILE_MAX:
            errors.append(f"{where(i)}: size {size} is past the limit")
            continue
        if not block_ok(e["root"], i, "root map"):
            continue                    # no root map: all holes
        claim(e["root"], i, "root map")
        nblocks = (size + BLOCK - 1) // BLOCK
        root = img[e["root"] * BLOCK:(e["root"] + 1) * BLOCK]
        for m in range(PTRS):
            l1 = u64(root, 8 * m)
            if not block_ok(l1, i, "map"):
                continue
            claim(l1, i, "map")
            mp = img[l1 * BLOCK:(l1 + 1) * BLOCK]
            for k in range(PTRS):
                b = u64(mp, 8 * k)
                if not block_ok(b, i, "data"):
                    continue
                claim(b, i, "data")
                idx = m * PTRS + k
                if idx >= nblocks:
                    errors.append(f"{where(i)}: data block {b} (block {idx} of the file) is wholly past the end ({size} bytes)")
                elif idx == nblocks - 1 and size % BLOCK:
                    tail = img[b * BLOCK + size % BLOCK:(b + 1) * BLOCK]
                    if any(tail):
                        errors.append(f"{where(i)}: bytes past the end ({size}) in its last block are not zero")
    return errors, notes, stats


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: fsck.py DISK.img")
    img = open(sys.argv[1], "rb").read()
    errors, notes, stats = check(img)
    for n in notes:
        print("fsck: note:", n)
    for e in errors:
        print("fsck: error:", e)
    if errors:
        print(f"fsck: {len(errors)} error(s)")
        sys.exit(1)
    print("fsck: clean, {files} files, {dirs} directories, {links} hard links, {orphans} orphans, {blocks} blocks in use".format(**stats))


if __name__ == "__main__":
    main()
