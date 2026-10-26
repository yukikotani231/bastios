# Installing BastiOS on a disk

`make installer` builds `build/bastios-installer.iso`, a live image that
installs BastiOS and an Alpine Linux userland on a disk and makes the disk
bootable. The image boots by BIOS and by UEFI, from a CD or written to a USB
stick (`dd`). It needs `dosfstools`, `mtools` and `xorriso` on the build host.

## Using it

Boot the image. Limine's menu starts "Install BastiOS to a disk" after five
seconds ("Try BastiOS without installing" runs the usual live system). The
installer shows the disk it found and asks you to type `erase`; anything else
stops without a change. It then writes the disk, unpacks Alpine Linux, and
asks for Enter to restart. Remove the install media and the disk boots.

It installs to the one disk the kernel opened, shows its size, and erases all
of it. Use a machine with a single disk that you can erase, or a virtual one.

## What is written

A GPT with three partitions at fixed places:

| partition | start sector | size | holds |
| --- | --- | --- | --- |
| BIOS boot | 2048 | 1 MiB | Limine's second stage, for BIOS |
| EFI system (FAT32) | 4096 | 48 MiB | Limine (`/EFI/BOOT/BOOTX64.EFI`), its menu, `/boot/bastios.elf` |
| BastiOS | 102400 | the rest | the BastiOS file system, with Alpine Linux under `/a` |

The kernel finds the BastiOS partition by its type GUID
(`e0c9ea2a-d5f1-45b3-a799-0a7bec83d701`) in the GPT of an internal disk. A GPT
disk without such a partition is left alone, as before. From the installed
system, `ash` then `/disk/alpine` is Alpine Linux's shell.

## How it works

- The installer is the same kernel, started with the `install` boot word. In
  that mode no file system is mounted and root may read and write the raw
  sectors of the disk through a private system call (`bstdisk`,
  `user/binstall.c`); outside install mode it fails with ENODEV.
- Everything the installer carries is built into the kernel image at build
  time (`tools/mkinitextra.py`): `binstall`, a BusyBox, the payload and the
  Alpine root. The payload (`tools/mkinstall.py`) holds Limine's boot code and
  the non-zero blocks of the BIOS boot and EFI system partitions, which do not
  depend on the disk.
- `binstall write` erases the old tables first, writes the data, then the
  backup GPT, the primary GPT and the protective MBR last, and reads
  everything back. A disk interrupted before the MBR is written has no
  partition table, so it is never half-bootable.

## What is proved and what is trusted

The kernel's changes (the partition offset, the raw access) are inside the
existing proofs. The installer program, the boot loader, and the host tools
that build the payload are trusted. `bstdisk` is limited to root in install
mode, so a normal system gains no new way to touch a raw disk.

## Testing

`test/install_test.py [bios|uefi] [virtio|nvme|ahci]` runs the whole path in
QEMU: install onto a blank disk, check the GPT and the EFI system partition on
the host, then boot the disk alone and run Alpine's shell.
