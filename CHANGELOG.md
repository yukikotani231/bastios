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

Nothing yet.

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
