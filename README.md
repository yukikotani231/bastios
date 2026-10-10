# BastiOS

A small x86-64 kernel written in [Bastio](https://github.com/yukikotani231/bastiolang), a language whose memory safety is proved at compile time. It boots under QEMU and runs a shell on the serial console.

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

The kernel (`src/`) runs user programs in ring 3, each in its own address space, and schedules them round robin with timer preemption. The shell is itself a user program (`user/sh.bastio`). Files live in an in-memory file system whose data pages come from the same allocator, and user programs open, read, write and remove them through system calls. None of the Bastio code has runtime checks; the compiler proves, before it emits anything:
- every buffer and string index is in bounds, and no arithmetic overflows or underflows;
- every physical page is owned by exactly one thing at a time: the allocator, or one slot of one page table. The allocator's bitmap is proved to agree with the pool, so a page is never handed out twice;
- a page table is empty before it is freed, so tearing down a process returns every page (the test checks that the count at power off equals the count at boot);
- table levels line up, user mappings never go into the kernel's slot of a top-level table, and the address space in CR3 is never freed;
- the kernel never reads memory through an address: it reads a user buffer by taking each table out of its parent and the page out of the last table, and putting them back;
- a file's data pages are owned by the file, so removing a file (or powering off) gives each of them back exactly once, and a system call copies only bytes inside the user range it was given and inside the kernel's transfer buffer.

Introduce an off-by-one, drop an ownership check, or free a table that still maps a page, and the build fails with a counterexample instead of producing a kernel.

## Layout

How the pieces fit together (boot, memory, processes, the scheduler, system calls, the proof boundary) is in [docs/architecture.md](docs/architecture.md).

| Path | What |
| --- | --- |
| `src/pages.bastio` | Physical page allocator (`Mem`: a bitmap proved to track the pool) |
| `src/space.bastio` | Building, walking and tearing down user address spaces |
| `src/elf.bastio` | Loading static Linux x86-64 ELF executables: header checks, segments, the initial stack (argv, envp, auxv) |
| `src/ramfs.bastio` | The in-memory file system: a flat table of files, each a vector of pages (a page never written is a hole and reads as zeros) |
| `src/fs.bastio` | The layer above file systems: per-process open-file table and the transfer buffer; a later file system on a block device slots in here |
| `src/proc.bastio` | Kernel state (`Kernel` struct), console input queue, process table, program images, spawn and exit |
| `src/usermem.bastio` | Copying between user memory and kernel byte buffers |
| `src/syscall.bastio` | System calls (BastiOS ABI, `int 0x80`), including the file calls |
| `src/linux.bastio` | Linux system calls (the `syscall` instruction) |
| `src/main.bastio` | Boot, the scheduler loop, fault handling, power off |
| `user/*.bastio` | User programs: `sh`, `hello`, `spin` (never yields; shows preemption), `true` (exits at once), `bench` (times syscalls, spawn and write in timer ticks), `fstest` (exercises the file calls) |
| `user/lib/file.bastio` | The file system calls for user programs |
| `files/` | Files installed in the file system at boot (built into the kernel image) |
| `user/fault.S` | A user program that reads kernel memory, to test fault isolation |
| `user/straddle.S` | A user program whose buffers cross page boundaries, to test how the kernel copies user memory |
| `user/mhello.c` | An ordinary C program built with musl libc (`musl-gcc -static-pie`) |
| `user/busybox.config` | The BusyBox options for `ash`; the Makefile fetches BusyBox (GPL-2.0, not part of this repository) at a pinned tag and builds it against musl as a static PIE |
| `user/mtest.c` | A musl program using `malloc` (brk and mmap), a file through stdio, and a line read from the console |
| `user/fpucheck.c` | Checks that two processes using SSE registers at once do not see each other's |
| `user/elfcheck.c`, `user/elfstart.S` | A freestanding C program linked as a static Linux ELF executable twice, at a fixed address (`elfexec`) and as a static PIE (`elfpie`); it checks its data, bss and initial stack, and a few Linux system calls. `elfbad` is a truncated copy the loader must refuse |

User programs are linked at `USER_BASE` (0x8000000000) behind a small header (magic, memory size, entry), flattened to raw images, and embedded in the kernel with `include_bytes`.

The kernel also loads static Linux x86-64 ELF executables, the first step toward running Linux programs: fixed-address executables whose segments lie in the user window, and static PIEs, which it places at `USER_BASE`. It starts them with the stack the Linux ABI promises (argc, argv, envp and the auxiliary vector). Every header field is untrusted, and the proof shows each read stays inside the file and each copied byte lands in the window below the stack. Dynamically linked executables are refused.

Such programs make Linux system calls with the `syscall` instruction (`src/linux.bastio`): `read` (the console, blocking, or a file), `write` and `writev` (the console or a file), `open`, `openat`, `close`, `lseek` (from the start or the current offset), `unlink`, `unlinkat`, `brk`, anonymous `mmap`, `munmap` and `mprotect` (both accepted and ignored: pages stay mapped until exit), `ioctl` (`TCGETS` and `TIOCGWINSZ` on the console; setting the terminal is accepted and ignored), `rt_sigaction` and `rt_sigprocmask` (accepted; no signal is ever delivered), `getuid` and friends (always 0), `getcwd` (always `/`), `stat`/`lstat`/`access`/`newfstatat` (always `-ENOENT` for now), `arch_prctl(ARCH_SET_FS)` (the thread pointer), `set_tid_address`, `exit`, `exit_group`, `getpid` and `getppid`; every other call prints a note on the console and returns `-ENOSYS`. Linux fds 0 to 2 are the console, a terminal in canonical mode: the kernel echoes what is typed, handles Backspace, hands a read the line once Enter (or Ctrl-D) is pressed, and turns LF into CR LF on output. Fd 3 and up are the process's open files in the file system below. That is enough for C programs built against musl libc as static PIEs (`user/mhello.c`, `user/mtest.c`) to use `printf`, `malloc`, `fopen` and `fgets`, and for BusyBox's `ash`, built unmodified, to run as an interactive shell on the console (built-in commands only, as there is no `fork`/`execve` yet). Each process has its own x87/SSE state, saved in a page of its own whenever it stops running. `int 0x80` keeps the BastiOS ABI, the one in bastiolang's user prelude. Calls 0 to 10 have named builtins there; the file calls (11 to 18, listed in `src/syscall.bastio`) go through its generic `sys_call` / `sys_send` / `sys_recv`, which lend the kernel a byte buffer.

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

Tested on x86-64 Linux (Ubuntu 24.04), which is what CI runs; other platforms are untested. On Ubuntu: `sudo apt-get install z3 build-essential qemu-system-x86 python3 musl-tools`, plus [rustup](https://rustup.rs) for bastiolang.

```sh
git clone https://github.com/yukikotani231/bastiolang
git clone https://github.com/yukikotani231/bastios
(cd bastiolang/stage0 && cargo build --release)
cd bastios
make          # build/bastios.elf
make run      # boot on this terminal; Ctrl-A X quits
make test     # boot in QEMU, type commands over the serial line, check the replies
```

Set `BASTIOC=/path/to/bastioc0` if the compiler lives elsewhere.

## What is trusted

The proof covers everything in `src/` and `user/*.bastio`. Everything below it is trusted, and kept small on purpose:

| Part | Where | Role |
| --- | --- | --- |
| Boot and trap entry | `boot/boot.S` | Multiboot entry, identity map, long mode; GDT with user segments; the trap stubs and the `syscall` entry; enabling SSE; entering user mode (which also loads the FS base) (`bst_cpu_enter`) and idling (`bst_cpu_wait`) |
| Processor startup | `boot/smp.c`, `boot/ap.S` | Finding the processors in the ACPI MADT, mapping the local APIC, INIT and startup IPIs; the real-mode trampoline that takes each application processor to long mode on the kernel's tables, where it checks in and parks. C until Bastio has an `Mmio` capability |
| Platform setup | `boot/platform.c` | TSS, interrupt table, `syscall` MSRs, 8259 PICs, 100 Hz timer |
| C helpers | `boot/libc.c` | `memset` / `memcpy` / `memmove` / `memcmp`, which the C compiler may call |
| Runtime preludes | in bastiolang, `stage0/src/codegen.rs` | COM1 port I/O, the arena allocator, page-table entry encoding, CR3 loads, the system call stubs for user programs |
| User entry | `user/start.S`, `user/user.ld` | Image header; calls `main` and exits with its result |
| Toolchain | | Z3, the stage0 compiler, the C compiler, QEMU |

Count the lines yourself (`wc -l boot/* user/*.S`). Copied numbers go stale.

`bst_cpu_enter` takes only rip, rsp, the general registers and the arithmetic flags from the saved context; the code and stack segments, IOPL 0 and the interrupt flag are forced, and non-canonical addresses are replaced, so whatever the kernel stores there, user code cannot gain I/O privilege, disable interrupts or run with kernel segments.

## Status

v0.2: processes, preemptive round-robin scheduling, system calls, fault isolation, and an in-memory file system. Not there yet:
- one CPU runs the kernel: the other processors are started at boot (`cpus` in the shell counts them) but park until Bastio has locks and `smp_start` (bastiolang docs/concurrency.md); the kernel runs with interrupts off and polls the UART;
- all user pages are mapped writable and executable (the image has no page-aligned sections yet);
- a user window of 2 MiB per process, at most 15 processes, programs built into the kernel image (the file system cannot hold programs to run yet);
- the file system lives in memory only: a flat directory of 32 files, names up to 28 bytes, files up to 1 MiB, 8 open files per process, and nothing survives power off (there is no block device driver yet);
- running out of memory when starting a program fails the spawn cleanly, but the kernel's own arena (for `Buf`) still halts the machine when it runs out.

The direction is in bastiolang's [ROADMAP.md](https://github.com/yukikotani231/bastiolang/blob/main/ROADMAP.md) under "v3: toward a kernel".

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <https://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or <https://opensource.org/licenses/MIT>)

at your option.
