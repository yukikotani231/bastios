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

- Fixes from the LTP file-system tests: a path through a file fails with
  `ENOTDIR`, a loop of symbolic links with `ELOOP`, a too-long name with
  `ENAMETOOLONG`, and a bad directory descriptor with `EBADF`/`ENOTDIR`
  (they all said `ENOENT`). `renameat2` takes `RENAME_EXCHANGE`, `mknod`
  makes regular files, one `write` moves more than 4096 bytes, `"link/"`
  names what the link points to, `linkat` across file systems says
  `EXDEV`, `newfstatat` rejects unknown flags, a full in-memory table
  makes `mkdir` say `ENOSPC`, and `O_TMPFILE` is refused with
  `EOPNOTSUPP` (it opened the directory).
- `/proc/cpuinfo`, `/proc/filesystems`, `/proc/PID/mounts` (so
  `/proc/self/mounts`) and three read-only sysctls:
  `/proc/sys/kernel/tainted`, `pid_max` and `printk`.
- The in-memory file system holds 128 files and directories (it held 32).
- `sendmsg` and `recvmsg`, with `SCM_RIGHTS` passing a descriptor over an
  `AF_UNIX` socket (one per message, one in flight per direction);
  `MSG_PEEK` on TCP and unix sockets; `shutdown` on TCP (sends the FIN)
  and unix sockets; `MSG_NOSIGNAL`; `SO_SNDBUF` and `SO_RCVBUF`;
  `AF_UNIX` `SOCK_DGRAM` and `SOCK_SEQPACKET` socket pairs, which keep
  message boundaries; and `rt_sigqueueinfo` (`sigqueue`), whose value
  reaches the handler's siginfo. A new compat program, `t_sock2`, checks
  these.
- `signalfd` and `signalfd4`: read, poll and epoll on the signals in its
  set. A blocked signal is now kept pending even when its action ignores
  it, as on Linux, so a blocked `SIGCHLD` reaches a signalfd or `sigwait`.
- A Linux program blocked on a network socket (`accept`, `read`,
  `recvfrom`, a full TCP send buffer) sleeps instead of being restarted on
  every pass of the scheduler, so it no longer keeps a processor busy
  (`ps` shows it as `socket`). The network card wakes it by interrupt:
  MSI-X on virtio-net, the PCI interrupt line through the 8259 on the
  e1000, asked for only while someone sleeps. `make netbench` measures it.
- File locks are kept (`src/lxlock.bastio`): `fcntl` record locks
  (`F_GETLK`, `F_SETLK`, `F_SETLKW`), open file description locks
  (`F_OFD_*`) and `flock`, with Linux's owners and release rules. A
  conflicting `F_SETLKW` or blocking `flock` waits, and an `F_SETLKW` that
  would close a cycle of waits fails with `EDEADLK`. Checked by the new
  compat program `t_lock`.
- C can be built on BastiOS: tcc and GNU make 4.4.1, built as static PIEs,
  compile and link programs (dynamic PIEs against musl's `libc.so`) and
  run makefiles, `-j` included. `make compat` boots a second disk with
  them and a small C runtime and runs `test/compat/dev.txt`.
- `ppoll` and `pselect6` wait with the signal mask they are given (GNU
  make `-j` hung waiting for SIGCHLD).

### Fixed

- Modification times were seconds since boot; they are wall-clock times
  now, as `stat` reports them.
- A file created by `open` with `O_CREAT` gets the mode asked for, less a
  umask of 022 (it was always 0644, so programs tcc linked were not
  executable by mode).
- Real programs in `make compat` (`test/compat/apps.txt`): Lua 5.4.7,
  QuickJS and the sqlite3 3.52 shell, built from source as static PIEs,
  run their files, pipes, timers, WAL databases and a 20000-row index.
- The console takes lines of up to 4095 bytes (it cut them at 255, which
  left a long pasted command hanging in ash).
- The threads of a Linux process run on several processors at once. They
  used to take turns on one: four threads of pure computation now finish
  about twice as fast on four processors under QEMU.
- `sendfile`, `copy_file_range`, `preadv`, `pwritev`, `statx`,
  `fallocate` (growing), `fadvise64`, `memfd_create`, `getcpu`, and
  `FIONREAD` on a pipe. A new compat program, `t_more`, checks these and other calls
  real programs make.
- `msync` writes a `MAP_SHARED` file mapping back to the file, and so do
  `munmap`, exit and `execve` (up to four such mappings per process; the
  mapping is still a copy, so others see the bytes only after a
  write-back).
- `AF_INET6` sockets, TCP and UDP, over the IPv4 stack: `::`, `::1` and
  `::ffff:a.b.c.d` work, and names come back as `sockaddr_in6`. There is
  no IPv6 on the wire.
- `munmap`, `mprotect` and `mremap` work: unmapped pages fault, read-only
  pages fault on a write (and stay read-only across `fork`), and `mremap`
  grows a mapping by moving its pages. A user fault now kills the process
  with `SIGSEGV` (or `SIGFPE`, `SIGILL`, `SIGTRAP`), as Linux does, so
  `wait4` and a shell report the signal.
- POSIX timers: `timer_create` (SIGEV_SIGNAL and SIGEV_NONE), `timer_settime`
  (relative or absolute), `timer_gettime`, `timer_getoverrun` and
  `timer_delete`, four per process.
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
- Symbolic links: `symlink`/`symlinkat` make them in memory or on the
  disk, every path a Linux program passes is followed through them (at
  most 8 links), and `lstat`, `readlink`, `unlink` and `rename` act on
  the link itself. BusyBox's `ln` and `readlink` are built in.
- Hard links: `link`/`linkat` give a file another name on the same file
  system (not a directory), `st_nlink` counts the names, and the file
  lives until its last name goes. On the disk a link is a new entry kind
  (4) naming the file's entry; a mount drops any link whose file is gone.
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
  namespace 1 with 512-byte or 4 KiB blocks, each write flushed. It is picked
  before virtio and ATA.
- A SATA disk on an AHCI controller (`src/ahci.bastio`): polled, command
  slot 0 of the first port with a disk, each write flushed. It is picked
  after virtio, before legacy ATA.
- PCI devices are found on every bus and function, not only the
  function-0 devices of bus 0 (NVMe drives sit behind root ports, and
  q35's AHCI controller is function 2).
- Every disk driver moves a 4 KiB file system block with one command and
  one cache flush instead of eight: writing a 2 MiB file is 3.4 to 3.9
  times faster on NVMe, virtio and AHCI, 1.5 times on ATA (numbers in
  docs/architecture.md; `make diskbench` measures them).
- A process waiting for the disk sleeps, and the others run. File data
  goes through a 64-block cache and an ordered queue of disk commands
  (`src/diskq.bastio`). A read that misses queues its blocks and read-ahead
  and restarts when they arrive. A write queues its blocks, a flush, its
  directory block and a flush, and returns once they are done. While a
  disk writer runs, `bench`'s getpid takes 120-183 ticks instead of
  240-260 (`make iobench`).
- Disk completions arrive by interrupt: MSI-X on NVMe and virtio, MSI on
  AHCI, to the bootstrap processor's local APIC, which the kernel now
  turns on. A processor with nothing to run halts instead of polling the
  disk, so `bench`'s getpid beside a disk writer takes 97-137 ticks
  instead of 122-191. Under QEMU's TCG, disk throughput drops 10-20%
  (numbers in docs/architecture.md).
- NVMe namespaces with 4 KiB logical blocks are used; they were skipped.
  AHCI disks with sectors longer than 512 bytes are now recognized and
  left alone instead of being misdriven.

### Changed

- Removing the name a disk file was made under while a hard link remains
  is committed by one sector write (`disk_take_name`); a rename between
  two names of one file does nothing, as on Linux.
- The power-cut test cuts power fifteen times and also truncates,
  renames and links; `test/fsck.py` checks the disk image after every
  cut (`test/fsck_test.py` shows it catches damage). Native call 24
  (`SYS_LINK`, `f_link`) makes a hard link.
- Named pipes live in the file system: `mknod`/`mkfifo` make a file with
  the FIFO type bits, so a FIFO is listed (type `p`), renamed, linked and
  removed like any file, and on the disk it survives a reboot. Only four
  can be open at once; any number can exist. BusyBox's `mkfifo` is built
  in.
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
