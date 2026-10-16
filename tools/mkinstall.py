#!/usr/bin/env python3
"""Makes the installer's payload: the parts of a new BastiOS disk that do not
depend on the disk (docs/install.md).

    mkinstall.py OUT.pay --kernel bastios.elf --limine DIR --conf limine.conf

The installed disk has a GPT with three partitions at fixed places:

    sector 2048   1 MiB   BIOS boot       Limine's second stage, for BIOS
    sector 4096  48 MiB   EFI system      FAT32: Limine, its menu, the kernel
    sector 102400 rest    BastiOS         the file system (type below)

The installer (user/binstall.c) writes the GPT itself, because it depends
on the disk's size; this file holds the rest: Limine's first stage (the 440
bytes of boot code in sector 0), and the 4 KiB blocks of the BIOS boot and
EFI system partitions that are not zero. The format, little-endian:

    header, 512 bytes: "BSTIPAY1", u64 bios_lba, bios_sectors, esp_lba,
        esp_sectors, os_lba; u32 extents; u32 0; 440 bytes of boot code
    extents: u64 lba, u64 blocks (of 4096 bytes), then the blocks

The BIOS boot code is made the way `limine bios-install` makes it for a
GPT disk: this script runs it on a scratch disk with the same layout, and
takes what changed. Needs mkfs.fat and mcopy (dosfstools, mtools).
"""
import os, shutil, struct, subprocess, sys, tempfile, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkusb

SECTOR = 512
BLOCK = 4096
BIOS_LBA, BIOS_N = 2048, 2048
ESP_LBA, ESP_N = 4096, 98304
OS_LBA = ESP_LBA + ESP_N
ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")


def scratch_disk(path):
    """A disk with the installed layout, a small BastiOS partition, and a
    valid GPT, for `limine bios-install`."""
    os_n = 2048
    total = OS_LBA + os_n + 34                      # the backup entries and header at the end
    ents = bytearray(128 * mkusb.ESIZE)
    mkusb.add_entry(ents, 128, mkusb.BIOS_BOOT_TYPE, BIOS_LBA, BIOS_LBA + BIOS_N - 1, "BIOS boot")
    mkusb.add_entry(ents, 128, ESP_TYPE, ESP_LBA, ESP_LBA + ESP_N - 1, "EFI system")
    mkusb.add_entry(ents, 128, mkusb.BASTIOS_TYPE, OS_LBA, OS_LBA + os_n - 1, "BastiOS")
    img = bytearray(total * SECTOR)
    guid = uuid.uuid4().bytes_le
    esec = 32
    last_use = total - esec - 2
    img[SECTOR:2 * SECTOR] = mkusb.gpt_header(1, total - 1, 34, last_use, guid, 2, 128, ents)
    img[2 * SECTOR:2 * SECTOR + len(ents)] = ents
    bel = total - 1 - esec
    img[bel * SECTOR:bel * SECTOR + len(ents)] = ents
    img[(total - 1) * SECTOR:] = mkusb.gpt_header(total - 1, 1, 34, last_use, guid, bel, 128, ents)
    img[510:512] = b"\x55\xaa"
    with open(path, "wb") as f:
        f.write(img)


def extents(buf, lba0):
    """The 4 KiB blocks of buf that are not all zero, as (lba, bytes) runs."""
    runs = []
    for at in range(0, len(buf), BLOCK):
        blk = buf[at:at + BLOCK]
        if not any(blk):
            continue
        lba = lba0 + at // SECTOR
        if runs and runs[-1][0] + len(runs[-1][1]) // SECTOR == lba:
            runs[-1][1] += blk
        else:
            runs.append([lba, bytearray(blk)])
    return runs


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def main():
    args = sys.argv[1:]
    if len(args) != 7 or args[1:2] != ["--kernel"] or args[3] != "--limine" or args[5] != "--conf":
        sys.exit(__doc__)
    out, kernel, limine, conf = args[0], args[2], args[4], args[6]
    for tool in ("mkfs.fat", "mcopy", "mmd"):
        if not shutil.which(tool):
            sys.exit(f"mkinstall: {tool} is missing (apt-get install dosfstools mtools)")
    with tempfile.TemporaryDirectory() as tmp:
        # The EFI system partition.
        esp = os.path.join(tmp, "esp.img")
        with open(esp, "wb") as f:
            f.truncate(ESP_N * SECTOR)
        run("mkfs.fat", "-F", "32", "-s", "1", "-S", "512", "-n", "BASTIOS", "-i", "BA571005", esp,
            stdout=subprocess.DEVNULL)
        env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        for d in ("::/EFI", "::/EFI/BOOT", "::/boot", "::/boot/limine"):
            run("mmd", "-i", esp, d, env=env)
        for src, dst in ((os.path.join(limine, "BOOTX64.EFI"), "::/EFI/BOOT/BOOTX64.EFI"),
                         (conf, "::/boot/limine/limine.conf"),
                         (os.path.join(limine, "limine-bios.sys"), "::/boot/limine/limine-bios.sys"),
                         (kernel, "::/boot/bastios.elf")):
            run("mcopy", "-i", esp, src, dst, env=env)
        esp_buf = open(esp, "rb").read()

        # Limine's BIOS stages, as they are on a disk with this layout.
        disk = os.path.join(tmp, "scratch.img")
        scratch_disk(disk)
        run(os.path.join(limine, "limine"), "bios-install", "--quiet", disk, stdout=subprocess.DEVNULL)
        d = open(disk, "rb").read()
        code = d[:440]
        bios_buf = d[BIOS_LBA * SECTOR:(BIOS_LBA + BIOS_N) * SECTOR]

    runs = extents(bios_buf, BIOS_LBA) + extents(esp_buf, ESP_LBA)
    hdr = struct.pack("<8s5QII440s", b"BSTIPAY1", BIOS_LBA, BIOS_N, ESP_LBA, ESP_N, OS_LBA, len(runs), 0, code)
    hdr += bytes(SECTOR - len(hdr))
    with open(out, "wb") as f:
        f.write(hdr)
        for lba, data in runs:
            f.write(struct.pack("<QQ", lba, len(data) // BLOCK))
            f.write(data)
    print(f"{out}: {len(runs)} extents, {sum(len(r[1]) for r in runs) >> 10} KiB")


if __name__ == "__main__":
    main()
