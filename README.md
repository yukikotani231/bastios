# BastiOS

A small x86-64 kernel written in [Bastio](https://github.com/yukikotani231/bastiolang), a language whose memory safety is proved at compile time. It boots under QEMU, or from a GRUB image under BIOS or UEFI, and runs a shell on the serial console and, when the boot loader sets up a framebuffer, on the screen with a PS/2 keyboard.

```
BastiOS 0.2: 14016 pages of memory

BastiOS 0.2 -- type `help`
bastios> hello
hello from user mode, pid 2
bastios> spin &
[2]
bastios> ps
  PID  PPID STATE    TICKS NAME
    1     0 ready       2 sh
    2     1 ready       2 spin
bastios> kill 2
bastios> echo hi there > notes
bastios> ls
      66 motd
     216 about
   13680 elfdemo
       9 notes
bastios> cat notes
hi there
bastios> fault

[pid 3 (fault): page fault at rip 0x0000008000000018, address 0x0000000000100000; killed]
[exit 142]
bastios> halt
bye
BastiOS: 14016 pages free at power off
```

The kernel (`src/`) runs user programs in ring 3, each in its own address space, and schedules them round robin with timer preemption. The shell is itself a user program (`user/sh.bastio`). Files live in an in-memory file system whose data pages come from the same allocator, or, under `disk/`, on an ATA disk where they survive power off; user programs open, read, write and remove them through system calls. None of the Bastio code has runtime checks; the compiler proves, before it emits anything:
- every buffer and string index is in bounds, and no arithmetic overflows or underflows;
- every physical page is owned by exactly one thing at a time: the allocator, or one slot of one page table. The allocator's bitmap is proved to agree with the pool, so a page is never handed out twice;
- a page table is empty before it is freed, so tearing down a process returns every page (the test checks that the count at power off equals the count at boot);
- table levels line up, user mappings never go into the kernel's slot of a top-level table, and the address space in CR3 is never freed;
- every device is driven through the I/O ports it claimed: the UART, the interrupt controllers and the timer each own their ports (claimed once, in `main`), no two of them can own the same port, and an access outside a driver's ports does not compile;
- the kernel never reads memory through an address: it reads a user buffer by taking each table out of its parent and the page out of the last table, and putting them back;
- a file's data pages are owned by the file, so removing a file (or powering off) gives each of them back exactly once, and a system call copies only bytes inside the user range it was given and inside the kernel's transfer buffer.

Introduce an off-by-one, drop an ownership check, or free a table that still maps a page, and the build fails with a counterexample instead of producing a kernel.

## Try it

On x86-64 Linux with QEMU (see [Build and run](#build-and-run) for the full list of tools):

```sh
git clone https://github.com/yukikotani231/bastiolang
git clone https://github.com/yukikotani231/bastios
(cd bastiolang/stage0 && cargo build --release)
cd bastios && make run
```

The first build proves every source file and fetches BusyBox, so it takes a while. `make run` boots the kernel on your terminal with two processors, 256 MiB, a virtio disk (`build/disk.img`, kept across boots) and a virtio network card on QEMU's user network. Some things to type:

| Command | What happens |
| --- | --- |
| `help` | the shell's commands |
| `hello`, `spin &`, `ps`, `kill 2` | user processes, preemption, killing |
| `echo hi > disk/note`, `cat disk/note` | a file on the disk; still there next boot |
| `ash` | BusyBox's shell, an unmodified Linux binary: try `ls /bin`, `echo hi \| wc -c`, then `exit` |
| `ifconfig`, `ping` | the address DHCP gave, and ICMP to the gateway |
| `tcpecho 7001` | then `nc 127.0.0.1 7001` on the host: what you type comes back |
| `halt` | power off; the kernel counts its pages back |

Ctrl-A X quits QEMU at any time.

To boot through a real boot loader, `make iso` builds `build/bastios.iso`. It is GRUB with the same kernel, as a hybrid image that boots from CD or USB under BIOS or UEFI. It needs `grub-mkrescue`, `xorriso` and `mtools`, plus GRUB's BIOS and UEFI files (`sudo apt-get install grub-pc-bin grub-efi-amd64-bin xorriso mtools`; add `ovmf` for UEFI in QEMU). `make run-iso` boots it in QEMU under BIOS and `make run-uefi` under UEFI firmware; both open a QEMU window, where the kernel draws its console on the 1024x768 framebuffer GRUB sets up and reads the PS/2 keyboard, while the serial console stays on your terminal. To write it to a USB stick, use `sudo dd if=build/bastios.iso of=/dev/sdX bs=4M` (check the device name first: dd overwrites it).

On real hardware, mind the limits. It has only been tried in QEMU. The console is on the first serial port (COM1, 115200 baud) and, booted from the image, on the screen, if GRUB can give it a 32-bit linear framebuffer between 2 GiB and the I/O APIC, which is where PC firmware puts it. Keys come from a PS/2 keyboard, which many PCs without the port emulate for a USB keyboard; there is no USB driver. It needs a virtio or Intel e1000 (82540EM) network card and a virtio or legacy ATA disk to use either. Under UEFI it runs on one processor, because it finds the ACPI tables only where BIOS firmware puts them.

## Layout

How the pieces fit together (boot, memory, processes, the scheduler, system calls, the proof boundary) is in [docs/architecture.md](docs/architecture.md).

| Path | What |
| --- | --- |
| `src/uart.bastio`, `src/pic.bastio`, `src/pit.bastio` | Device drivers: COM1, the 8259 interrupt controllers, the 100 Hz timer |
| `src/fb.bastio`, `src/kbd.bastio`, `src/console.bastio` | The console: text on the boot loader's framebuffer (8x16 cells from the public-domain font in `src/font8x8.bin`), the PS/2 keyboard, and the two joined with COM1 |
| `src/ata.bastio` | Disk driver: the first drive on the legacy ATA controller, in PIO mode |
| `src/pages.bastio` | Physical page allocator (`Mem`: a bitmap proved to track the pool) |
| `src/space.bastio` | Building, walking and tearing down user address spaces |
| `src/elf.bastio` | Loading static Linux x86-64 ELF executables: header checks, segments, the initial stack (argv, envp, auxv) |
| `src/ramfs.bastio` | The in-memory file system: a flat table of files, each a vector of pages (a page never written is a hole and reads as zeros) |
| `src/diskfs.bastio` | The file system on the disk (names under `disk/`): a superblock, a directory of 32 entries, and a 1 MiB slot per file |
| `src/fs.bastio` | The layer above both file systems: per-process open-file table, the transfer buffer, and dispatch by name (`disk/...` or memory) |
| `src/proc.bastio` | Kernel state (`Kernel` struct), console input queue, process table, program images, spawn and exit |
| `src/usermem.bastio` | Copying between user memory and kernel byte buffers |
| `src/syscall.bastio` | System calls (BastiOS ABI, `int 0x80`), including the file calls |
| `src/linux.bastio` | Linux system calls (the `syscall` instruction) |
| `src/main.bastio` | Boot, the scheduler loop, fault handling, power off |
| `user/*.bastio` | User programs: `sh`, `hello`, `spin` (never yields; shows preemption), `true` (exits at once), `bench` (times syscalls, spawn and write in timer ticks), `fstest` (exercises the file calls), `crashme` (checks the disk files, then keeps writing; the power-cut test kills it) |
| `user/lib/file.bastio` | The file system calls for user programs |
| `files/` | Files installed in the file system at boot (built into the kernel image) |
| `user/fault.S` | A user program that reads kernel memory, to test fault isolation |
| `user/straddle.S` | A user program whose buffers cross page boundaries, to test how the kernel copies user memory |
| `user/mhello.c` | An ordinary C program built with musl libc (`musl-gcc -static-pie`) |
| `user/busybox.config` | The BusyBox options for `ash`; the Makefile fetches BusyBox (GPL-2.0, not part of this repository) at a pinned tag and builds it against musl as a static PIE |
| `user/mtest.c` | A musl program using `malloc` (brk and mmap), a file through stdio, and a line read from the console |
| `user/fpucheck.c` | Checks that two processes using SSE registers at once do not see each other's |
| `user/mtcp.c` | A musl program that uses TCP through the Linux system calls: accepts a connection and answers it, then connects out and reads the reply |
| `user/mtime.c` | A musl program that checks clocks, `nanosleep`, and `poll` timing out on a pipe and waking when a child writes to it |
| `user/mudp.c` | A musl program that uses UDP sockets through the Linux system calls: binds a port, waits for a datagram and sends it back |
| `user/elfcheck.c`, `user/elfstart.S` | A freestanding C program linked as a static Linux ELF executable twice, at a fixed address (`elfexec`) and as a static PIE (`elfpie`); it checks its data, bss and initial stack, and a few Linux system calls. `elfbad` is a truncated copy the loader must refuse |

User programs are linked at `USER_BASE` (0x8000000000) behind a small header (magic, memory size, entry), flattened to raw images, and embedded in the kernel with `include_bytes`.

The kernel also loads static Linux x86-64 ELF executables, the first step toward running Linux programs: fixed-address executables whose segments lie in the user window, and static PIEs, which it places at `USER_BASE`. It starts them with the stack the Linux ABI promises (argc, argv, envp and the auxiliary vector). Every header field is untrusted, and the proof shows each read stays inside the file and each copied byte lands in the window below the stack. Dynamically linked executables are refused.

Such programs make Linux system calls with the `syscall` instruction (`src/linux.bastio`): `read` (the console, blocking, a file or a pipe), `write` and `writev` (the console, a file or a pipe), `pipe`, `pipe2`, `dup`, `dup2`, `dup3`, `fcntl` (duplicating and close-on-exec), `poll` and `ppoll` (waiting until the console has a line, a pipe has data or room, a socket has data, a connection to accept or room to send, or the timeout passes; ppoll's signal mask is not applied), `nanosleep` and `clock_nanosleep`, `clock_gettime`, `clock_getres`, `gettimeofday` and `time` (off the HPET; with no real-time clock, `CLOCK_REALTIME` counts from the epoch at boot), `open`, `openat`, `close`, `lseek` (from the start or the current offset), `unlink`, `unlinkat` (with `AT_REMOVEDIR` too), `mkdir`, `mkdirat`, `rmdir`, `chdir`, `brk`, anonymous `mmap`, `munmap` and `mprotect` (both accepted and ignored: pages stay mapped until exit), `ioctl` (`TCGETS` and `TIOCGWINSZ` on the console; setting the terminal is accepted and ignored), signals (`rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `kill`, `tkill`, `tgkill`; a handler runs on the user stack in the frame Linux builds, default actions end the process or ignore the signal, Ctrl-C on the console sends SIGINT to the Linux programs on it, and a write to a pipe with no reader sends SIGPIPE; there are no process groups, stopping or alternate signal stacks), `getuid` and friends (always 0), `getcwd`, `stat`/`lstat`/`access`/`newfstatat`/`fstat` (`/`, `/bin` and the file system's directories are directories, `/bin/NAME` a built-in program, anything else a file; an open descriptor reports the console, a pipe, a directory or a file), `getdents64` (listing `/`, `/bin` and any directory of the file system; paths are taken from the working directory, with `.` and `..`, and the `*at` calls ignore their directory descriptor), sockets over the kernel's UDP and TCP (`socket` for `AF_INET` datagrams and streams, `bind`, `listen`, `accept`, `accept4`, `connect`, `sendto`/`send`/`write`, `recvfrom`/`recv`/`read`, blocking or with `SOCK_NONBLOCK`/`MSG_DONTWAIT`, `getsockname`, `getpeername`; `setsockopt` and `shutdown` are accepted and ignored, and closing a stream sends its FIN), `fork`, `vfork` and `clone(SIGCHLD)` (the whole window is copied, and descriptors are shared), `execve` (built-in programs at `/bin/NAME`, and ELF files such as `/elfdemo` from memory or `disk/`, with argv and the environment), `wait4`, `gettid`, `arch_prctl(ARCH_SET_FS)` (the thread pointer), `set_tid_address`, `exit`, `exit_group`, `getpid` and `getppid`; every other call prints a note on the console and returns `-ENOSYS`. Linux fds 0 to 2 are the console, a terminal in canonical mode: the kernel echoes what is typed, handles Backspace, hands a read the line once Enter (or Ctrl-D) is pressed, and turns LF into CR LF on output. Each process has 16 descriptors (`src/lxfd.bastio`); each is the console or an open file description shared by `dup` and `fork`: a file in the file system below, one end of a pipe, a directory being listed, or a socket (`src/lxsock.bastio` and `src/lxtcp.bastio`, over the kernel's sockets in `src/net.bastio` and `src/tcp.bastio`). That is enough for C programs built against musl libc as static PIEs (`user/mhello.c`, `user/mtest.c`) to use `printf`, `malloc`, `fopen` and `fgets`, and for BusyBox's `ash`, built unmodified, to run as an interactive shell on the console and start other programs, with pipes and redirection. BusyBox's `ls`, `cat`, `wc`, `head`, `rm`, `cp`, `kill`, `sleep`, `mkdir`, `rmdir` and `pwd` are built in too, and `/bin/NAME` for each of them starts the BusyBox binary, which picks the applet from `argv[0]`. Each process has its own x87/SSE state, saved in a page of its own whenever it stops running. `int 0x80` keeps the BastiOS ABI, the one in bastiolang's user prelude. Calls 0 to 10 have named builtins there; the file calls (11 to 21, listed in `src/syscall.bastio`) go through its generic `sys_call` / `sys_send` / `sys_recv`, which lend the kernel a byte buffer.

## Build and run

Requirements:

| Tool | Tested with | Notes |
| --- | --- | --- |
| [bastiolang](https://github.com/yukikotani231/bastiolang) | `main` | Checked out next to this repository and built; it needs Rust and Z3 (see its README) |
| C compiler and binutils for x86-64 | GCC 13, binutils 2.42 | Called as `cc` |
| Z3 | 4.8.12, 5.1.0 | Every kernel and user source is proved at build time |
| QEMU | 8.2 | `qemu-system-x86_64` |
| Python 3 | 3.12, 3.13 | Only for the test driver |
| musl-gcc | musl 1.2.4 | Builds the musl test programs and BusyBox (`musl-tools` on Ubuntu) |
| git and network access | | The first build clones BusyBox 1.36.1 from github.com/mirror/busybox into `build/busybox` (set `BUSYBOX_GIT` for another mirror) |
| GRUB, xorriso, mtools, OVMF | GRUB 2.12, xorriso 1.5.6, OVMF 2024.02 | Only for `make iso` and `test/iso_test.sh` (which skips without them; UEFI is skipped without OVMF) |

Tested on x86-64 Linux (Ubuntu 24.04), which is what CI runs; other platforms are untested. On Ubuntu: `sudo apt-get install z3 build-essential qemu-system-x86 python3 musl-tools`, plus [rustup](https://rustup.rs) for bastiolang.

```sh
git clone https://github.com/yukikotani231/bastiolang
git clone https://github.com/yukikotani231/bastios
(cd bastiolang/stage0 && cargo build --release)
cd bastios
make          # build/bastios.elf
make run      # boot on this terminal with build/disk.img as the disk; Ctrl-A X quits
make iso      # build/bastios.iso: GRUB + the kernel, bootable under BIOS or UEFI
make test     # boot in QEMU, type commands over the serial line, check the replies
```

Set `BASTIOC=/path/to/bastioc0` if the compiler lives elsewhere.

## What is trusted

The proof covers everything in `src/` and `user/*.bastio`. Everything below it is trusted, and kept small on purpose:

| Part | Where | Role |
| --- | --- | --- |
| Boot loader | GRUB, configured by `boot/grub.cfg` (only for the ISO) | Loads the kernel as a Multiboot image and passes the memory map and the framebuffer it set up (address, pitch, size, pixel layout: the screen console trusts these); `make run` and the tests use QEMU's own Multiboot loader instead |
| Boot and trap entry | `boot/boot.S` | Multiboot entry, identity map (the first 1 GiB, and the 2-4 GiB device window uncached), the Multiboot header's video request, long mode; GDT with user segments; the trap stubs and the `syscall` entry; enabling SSE; entering user mode (which also loads the FS base) (`bst_cpu_enter`) and idling (`bst_cpu_wait`) |
| Processor startup | `boot/smp.c`, `boot/ap.S` | Finding the processors in the ACPI MADT, mapping the local APIC, INIT and startup IPIs; the real-mode trampoline that takes each application processor to long mode on the kernel's tables, where it checks in and parks until `smp_run` hands it work; each processor's TSS. C because the firmware tables lie outside the device window `Mmio` covers |
| Platform setup | `boot/platform.c` | Interrupt table, `syscall` MSRs |
| C helpers | `boot/libc.c` | `memset` / `memcpy` / `memmove` / `memcmp`, which the C compiler may call |
| Runtime preludes | in bastiolang, `stage0/src/codegen.rs` | one `in` / `out` instruction per port access, COM1 output for the prelude's own panic messages, reading the framebuffer fields out of the Multiboot information (`boot_fb`), the arena allocator, page-table entry encoding, CR3 loads, the system call stubs for user programs |
| User entry | `user/start.S`, `user/user.ld` | Image header; calls `main` and exits with its result |
| Toolchain | | Z3, the stage0 compiler, the C compiler, QEMU |

`test/trusted_base.sh` counts these files, and `make test` fails when the count differs from `test/trusted_base.txt`, so growth gets reviewed. [docs/trusted-base.md](docs/trusted-base.md) says what each part is trusted to do, and one known gap.

`bst_cpu_enter` takes only rip, rsp, the general registers and the arithmetic flags from the saved context; the code and stack segments, IOPL 0 and the interrupt flag are forced, and non-canonical addresses are replaced, so whatever the kernel stores there, user code cannot gain I/O privilege, disable interrupts or run with kernel segments.

## Status

v0.2: processes, preemptive round-robin scheduling, system calls, fault isolation, and files in memory and on disk. Not there yet:
- every processor runs the scheduler, but under one lock for the whole kernel state (`Lock[Sys]` in `src/main.bastio`); the kernel runs with interrupts off and handles each interrupt as an event in its scheduler loop;
- all user pages are mapped writable and executable (the image has no page-aligned sections yet);
- a user window of 64 MiB per process (pages are mapped as a program asks for them, and `fork` copies every mapped page), at most 15 processes;
- files are simple: flat directories of 32 files each (in memory, and under `disk/`), names up to 28 bytes, files up to 1 MiB, 8 open files per process. The disk driver polls one legacy ATA drive with 28-bit sector numbers, and writes go straight to the disk with no cache or journal, so a power cut in the middle of a write can leave that file half written;
- running out of memory when starting a program fails the spawn cleanly, but the kernel's own arena (for `Buf`) still halts the machine when it runs out.

The direction is in bastiolang's [ROADMAP.md](https://github.com/yukikotani231/bastiolang/blob/main/ROADMAP.md) under "v3: toward a kernel".

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <https://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or <https://opensource.org/licenses/MIT>)

at your option.
