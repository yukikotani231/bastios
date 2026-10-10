#!/usr/bin/env python3
"""Makes a USB stick image whose last GPT partition holds the BastiOS file
system, so the stick BastiOS boots from is also its disk.

    mkusb.py OUT.img --iso build/bastios.iso [--mib N]
    mkusb.py OUT.img --blank [--mib N]
    mkusb.py --find IMG

With --iso, the image is the hybrid ISO that `make iso` builds (GRUB and
the kernel, bootable under BIOS and UEFI, with a GPT that grub-mkrescue
wrote), then N MiB (default 64) for the BastiOS partition, then the GPT's
backup copy, moved to the new end. With --blank there is no boot image:
only the GPT and the partition, for a test that boots the kernel another
way. The partition is left zeroed; BastiOS formats it the first time it
boots from it.

The partition's type is e0c9ea2a-d5f1-45b3-a799-0a7bec83d701 (BASTIOS_PART
in src/xhci.bastio). BastiOS uses a USB disk only if it has a partition of
that type, and then only that partition.

--find prints the first sector and the sector count of the BastiOS
partition of IMG, or exits 1 if it has none.
"""
import struct, sys, uuid, zlib

SECTOR = 512
BASTIOS_TYPE = uuid.UUID("e0c9ea2a-d5f1-45b3-a799-0a7bec83d701")
ESIZE = 128


def header(buf, lba):
    h = buf[lba * SECTOR:lba * SECTOR + SECTOR]
    if h[:8] != b"EFI PART":
        return None
    return h


def entries(buf, h):
    elba, count, esz = struct.unpack_from("<QII", h, 72)
    return elba, count, esz, bytearray(buf[elba * SECTOR:elba * SECTOR + count * esz])


def find(buf):
    h = header(buf, 1)
    if h is None:
        return None
    elba, count, esz, ents = entries(buf, h)
    for i in range(count):
        e = ents[i * esz:(i + 1) * esz]
        if uuid.UUID(bytes_le=bytes(e[:16])) == BASTIOS_TYPE:
            first, last = struct.unpack_from("<QQ", e, 32)
            return first, last - first + 1
    return None


def gpt_header(cur, alt, first_use, last_use, disk_guid, elba, count, ents):
    h = bytearray(92)
    struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, 0, cur, alt,
                     first_use, last_use, disk_guid, elba, count, ESIZE, zlib.crc32(ents))
    struct.pack_into("<I", h, 16, zlib.crc32(h))
    return bytes(h) + bytes(SECTOR - 92)


def build(base, mib):
    """base: the boot image (empty for --blank). Returns the new image."""
    count = 128
    ents = bytearray(count * ESIZE)
    disk_guid = uuid.uuid4().bytes_le
    used_to = 34                                      # past the primary GPT
    pelba, first_use = 2, 34
    if base:
        h = header(base, 1)
        if h is None:
            sys.exit("mkusb: the boot image has no GPT")
        disk_guid = bytes(h[56:72])
        elba, count, esz, old = entries(base, h)
        if esz != ESIZE:
            sys.exit("mkusb: unexpected GPT entry size in the boot image")
        # The entries stay where they are (grub-mkrescue puts an Apple
        # partition map before them).
        pelba = elba
        first_use = struct.unpack_from("<Q", h, 40)[0]
        ents = old
        for i in range(count):
            e = ents[i * ESIZE:(i + 1) * ESIZE]
            if any(e[:16]):
                used_to = max(used_to, struct.unpack_from("<Q", e, 40)[0] + 1)
        used_to = max(used_to, len(base) // SECTOR)
    esec = (count * ESIZE + SECTOR - 1) // SECTOR
    first = (used_to + 2047) // 2048 * 2048              # 1 MiB aligned
    size = mib * 2048
    last = first + size - 1
    total = last + 1 + esec + 1                          # the backup entries and header
    slot = next((i for i in range(count) if not any(ents[i * ESIZE:i * ESIZE + 16])), None)
    if slot is None:
        sys.exit("mkusb: no free GPT entry")
    e = bytearray(ESIZE)
    e[0:16] = BASTIOS_TYPE.bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<QQQ", e, 32, first, last, 0)
    e[56:56 + 14] = "BastiOS".encode("utf-16le")
    ents[slot * ESIZE:(slot + 1) * ESIZE] = e
    img = bytearray(total * SECTOR)
    img[:len(base)] = base
    # The ISO's own backup GPT, now in the middle of the image, is wiped so
    # nothing takes it for the real one.
    if base:
        tail = len(base) // SECTOR - 1
        if header(base, tail) is not None:
            img[tail * SECTOR:(tail + 1) * SECTOR] = bytes(SECTOR)
    # A protective MBR (keeping a hybrid image's boot code), the primary
    # GPT, and the backup at the end.
    mbr = bytearray(img[:SECTOR])
    for i in range(4):
        mbr[446 + i * 16:462 + i * 16] = bytes(16)
    struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    img[:SECTOR] = mbr
    last_use = total - esec - 2
    img[SECTOR:2 * SECTOR] = gpt_header(1, total - 1, first_use, last_use, disk_guid, pelba, count, ents)
    img[pelba * SECTOR:pelba * SECTOR + len(ents)] = ents
    bel = total - 1 - esec
    img[bel * SECTOR:bel * SECTOR + len(ents)] = ents
    img[(total - 1) * SECTOR:] = gpt_header(total - 1, 1, first_use, last_use, disk_guid, bel, count, ents)
    return bytes(img)


def main():
    args = sys.argv[1:]
    if args[:1] == ["--find"] and len(args) == 2:
        p = find(open(args[1], "rb").read())
        if p is None:
            sys.exit(1)
        print(p[0], p[1])
        return
    mib = 64
    if "--mib" in args:
        i = args.index("--mib")
        mib = int(args[i + 1])
        del args[i:i + 2]
    if len(args) == 3 and args[1] == "--iso":
        base = open(args[2], "rb").read()
    elif len(args) == 2 and args[1] == "--blank":
        base = b""
    else:
        sys.exit(__doc__)
    img = build(base, mib)
    with open(args[0], "wb") as f:
        f.write(img)
    first, n = find(img)
    print(f"{args[0]}: BastiOS partition at sector {first}, {n} sectors ({n // 2048} MiB)")


if __name__ == "__main__":
    main()
