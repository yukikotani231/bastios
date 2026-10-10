# Changelog

Notable changes to BastiOS. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). BastiOS is
pre-release: before 1.0, any minor version may change the system call ABI,
the disk format or the build.

The version is the one the kernel prints at boot (`BastiOS X.Y` in
`src/main.bastio` and `user/sh.bastio`). A release is a tag `vX.Y.Z` on
`main`; `.github/workflows/release.yml` builds it (see
[docs/releasing.md](docs/releasing.md)).

## [Unreleased]

### Added

- The threads of a Linux process run on several processors at once. They
  used to take turns on one: four threads of pure computation now finish
  about twice as fast on four processors under QEMU.

- Files on the disk can grow to 1 GiB (as far as the disk allows) and in
  memory to 64 MiB; they were capped at 1 MiB.
- Paths can be up to 255 bytes; they were capped at 28.
- A write to a file on the disk lands whole or not at all across a power
  cut, even one that overwrites the middle of a file across two blocks.
- Each file and directory keeps permission bits and a modification time,
  in memory and on the disk. Linux `chmod`, `fchmod`, `fchmodat`,
  `utime`, `utimes`, `futimesat` and `utimensat` change them, and `stat`
  reports them with an inode number per file. BusyBox's `chmod`, `touch`
  and `stat` applets are built in.
- Removing a file that is open works as on Linux: the name goes at once
  and the data stays readable through the open descriptors until the
  last one is closed. On the disk such a file is an orphan entry, and a
  mount frees any a power cut left behind.
- Renaming files and directories: `vfs_rename`, system call 22, and the
  shell's `mv`. On the disk a rename is one sector write, so a power cut
  leaves the old name or the new one.
- A generated, read-only `/proc` for Linux programs: `mounts`,
  `meminfo`, `uptime`, a directory per process with `stat`, `status`,
  `cmdline` and `exe` (a link `readlink` reads), and `/proc/self`.
  BusyBox's `ps`, `free`, `df` and `uptime` work on it.
- Setting an open file's length (`vfs_truncate`, system call 23):
  shrinking frees what lies past the new end, growing reads as zeros.
  On the disk it is copy-on-write like a write, so a power cut leaves the
  old length or the new one.
- A console on the screen and a PS/2 keyboard when booted from the ISO:
  text drawn on the framebuffer GRUB sets up under BIOS or UEFI, keys read
  from the i8042, alongside the serial console.
- A USB keyboard on an xHCI controller's root port, for PCs without PS/2
  (no hubs).
- An NVMe drive as the disk (`src/nvme.bastio`): polled, one I/O queue,
  namespace 1 with 512-byte blocks, each write flushed. It is picked
  before virtio and ATA.
- A SATA disk on an AHCI controller (`src/ahci.bastio`): polled, command
  slot 0 of the first port with a disk, each write flushed. It is picked
  after virtio, before legacy ATA.
- PCI devices are found on every bus and function, not only the
  function-0 devices of bus 0 (NVMe drives sit behind root ports, and
  q35's AHCI controller is function 2).

### Changed

- The disk format is now `BSTDISK2`: 4 KiB blocks, 64 directory entries
  that each hold one path component and point at their parent, two levels
  of block maps per file, and copy-on-write for every write. A disk in the
  old `BSTDISK1` format is formatted afresh at boot (its files are lost).
- The ATA driver waits longer for the drive (a cache flush waits on the
  host's fsync).
- The ISO's GRUB loads the kernel with Multiboot 2 (QEMU's `-kernel` keeps
  Multiboot 1), so under UEFI the kernel gets the ACPI RSDP and starts
  every processor; it ran on one before.

- The device window is mapped from 2 GiB (it was 3 GiB), where UEFI
  firmware puts the framebuffer. Needs bastiolang with `boot_fb`.
- The device window now reaches 2^46: boot.S maps 4 GiB to 2^46 uncached
  with 1 GiB pages, so the xHCI and NVMe drivers take 64-bit BARs above
  4 GiB, where UEFI firmware puts them. Needs bastiolang with the wider
  window. QEMU targets pass `-cpu qemu64,+pdpe1gb`, since QEMU's default
  CPU model has no 1 GiB pages (on such a CPU the window stops at 4 GiB).
- `make run-iso` and `make run-uefi` open a QEMU window.

## [0.2.0] - not yet tagged

The first version: everything up to the bootable ISO.

### Kernel

- Boot from QEMU's Multiboot loader or from GRUB, as a hybrid BIOS and UEFI
  ISO (`make iso`).
- A physical page allocator whose bitmap is proved to agree with the pool,
  page tables with each page owned by exactly one slot, and user address
  spaces whose teardown returns every page (the tests compare the free page
  count at power off with the count at boot).
- User processes in ring 3 with preemptive round-robin scheduling, system
  calls, fault isolation, and a 64 MiB user window per process, mapped as
  programs ask for it. The scheduler runs on every processor, under one
  kernel lock (`Lock[Sys]`).
- Drivers written in Bastio, each reaching only the I/O ports or registers
  it claimed: the UART, the 8259 PICs, the PIT, the HPET, a legacy ATA disk,
  virtio-blk, virtio-net and Intel e1000 network cards.
- File systems: one in memory, whose data pages are owned by its files, and
  one on disk, under `disk/`, tested against power cuts during writes.
- Networking: Ethernet, ARP, IPv4, ICMP echo, UDP with sockets, a DHCP
  client and TCP. Every read of a frame is proved to stay inside it.
- A shell written in Bastio, with commands for processes, files and the
  network.

### Linux compatibility

- Static x86-64 Linux ELF executables (fixed-address and static PIE), with
  every header field treated as untrusted and every read proved in bounds.
- A Linux system call layer, written in Bastio: files, pipes, `dup`,
  `fork`/`execve`/`wait4`, `brk`/`mmap`, `poll` that waits, sleeps and
  clocks, signals (handlers, `kill`, Ctrl-C, SIGPIPE), and UDP and TCP
  sockets. The README lists every call.
- BusyBox 1.36.1 `ash`, built unmodified against musl, runs interactively,
  with `ls`, `cat`, `wc`, `head`, `rm`, `cp`, `kill` and `sleep`.

### Known limits

- Tried only in QEMU. Serial console only (COM1). One processor under UEFI.
- The README's "Status" section lists the rest.

[Unreleased]: https://github.com/yukikotani231/bastios/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/yukikotani231/bastios/releases/tag/v0.2.0
