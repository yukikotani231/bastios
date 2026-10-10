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
bastios> fault

[pid 3 (fault): page fault at rip 0x0000008000000018, address 0x0000000000100000; killed]
[exit 142]
bastios> halt
bye
BastiOS: 14016 pages free at power off
```

The kernel (`src/`) runs user programs in ring 3, each in its own address space, and schedules them round robin with timer preemption. The shell is itself a user program (`user/sh.bastio`). None of the Bastio code has runtime checks; the compiler proves, before it emits anything:
- every buffer and string index is in bounds, and no arithmetic overflows or underflows;
- every physical page is owned by exactly one thing at a time: the allocator, or one slot of one page table. The allocator's bitmap is proved to agree with the pool, so a page is never handed out twice;
- a page table is empty before it is freed, so tearing down a process returns every page (the test checks that the count at power off equals the count at boot);
- table levels line up, user mappings never go into the kernel's slot of a top-level table, and the address space in CR3 is never freed;
- the kernel never reads memory through an address: it reads a user buffer by taking each table out of its parent and the page out of the last table, and putting them back.

Introduce an off-by-one, drop an ownership check, or free a table that still maps a page, and the build fails with a counterexample instead of producing a kernel.

## Layout

How the pieces fit together (boot, memory, processes, the scheduler, system calls, the proof boundary) is in [docs/architecture.md](docs/architecture.md).

| Path | What |
| --- | --- |
| `src/pages.bastio` | Physical page allocator (`Mem`: a bitmap proved to track the pool) |
| `src/space.bastio` | Building, walking and tearing down user address spaces |
| `src/elf.bastio` | Loading static Linux x86-64 ELF executables: header checks, segments, the initial stack (argv, envp, auxv) |
| `src/proc.bastio` | Kernel state (`Kernel` struct), console input queue, process table, program images, spawn and exit |
| `src/syscall.bastio` | System calls (BastiOS ABI, `int 0x80`) |
| `src/linux.bastio` | Linux system calls (the `syscall` instruction) |
| `src/main.bastio` | Boot, the scheduler loop, fault handling, power off |
| `user/*.bastio` | User programs: `sh`, `hello`, `spin` (never yields; shows preemption), `true` (exits at once), `bench` (times syscalls, spawn and write in timer ticks) |
| `user/fault.S` | A user program that reads kernel memory, to test fault isolation |
| `user/straddle.S` | A user program whose buffers cross page boundaries, to test how the kernel copies user memory |
| `user/mhello.c` | An ordinary C program built with musl libc (`musl-gcc -static-pie`) |
| `user/fpucheck.c` | Checks that two processes using SSE registers at once do not see each other's |
| `user/elfcheck.c`, `user/elfstart.S` | A freestanding C program linked as a static Linux ELF executable twice, at a fixed address (`elfexec`) and as a static PIE (`elfpie`); it checks its data, bss and initial stack, and a few Linux system calls. `elfbad` is a truncated copy the loader must refuse |

User programs are linked at `USER_BASE` (0x8000000000) behind a small header (magic, memory size, entry), flattened to raw images, and embedded in the kernel with `include_bytes`.

The kernel also loads static Linux x86-64 ELF executables, the first step toward running Linux programs: fixed-address executables whose segments lie in the user window, and static PIEs, which it places at `USER_BASE`. It starts them with the stack the Linux ABI promises (argc, argv, envp and the auxiliary vector). Every header field is untrusted, and the proof shows each read stays inside the file and each copied byte lands in the window below the stack. Dynamically linked executables are refused.

Such programs make Linux system calls with the `syscall` instruction (`src/linux.bastio`). So far there are `write` and `writev` (to stdout and stderr), `ioctl` (always `-ENOTTY`), `arch_prctl(ARCH_SET_FS)` (the thread pointer), `set_tid_address`, `exit`, `exit_group`, `getpid` and `getppid`; every other call prints a note on the console and returns `-ENOSYS`. That is enough for a C program built against musl libc as a static PIE (`user/mhello.c`) to start, print with `printf` and exit with a status. Each process has its own x87/SSE state, saved in a page of its own whenever it stops running. `int 0x80` keeps the BastiOS ABI, the one in bastiolang's user prelude.

## Build and run

Requirements:

| Tool | Tested with | Notes |
| --- | --- | --- |
| [bastiolang](https://github.com/yukikotani231/bastiolang) | `main` | Checked out next to this repository and built; it needs Rust and Z3 (see its README) |
| C compiler and binutils for x86-64 | GCC 13, binutils 2.42 | Called as `cc` |
| Z3 | 4.8.12, 5.1.0 | Every kernel and user source is proved at build time |
| QEMU | 8.2 | `qemu-system-x86_64` |
| Python 3 | 3.12, 3.13 | Only for the test driver |
| musl-gcc | musl 1.2.4 | Builds the musl test program (`musl-tools` on Ubuntu) |

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
| Platform setup | `boot/platform.c` | TSS, interrupt table, `syscall` MSRs, 8259 PICs, 100 Hz timer |
| C helpers | `boot/libc.c` | `memset` / `memcpy` / `memmove` / `memcmp`, which the C compiler may call |
| Runtime preludes | in bastiolang, `stage0/src/codegen.rs` | COM1 port I/O, the arena allocator, page-table entry encoding, CR3 loads, the system call stubs for user programs |
| User entry | `user/start.S`, `user/user.ld` | Image header; calls `main` and exits with its result |
| Toolchain | | Z3, the stage0 compiler, the C compiler, QEMU |

Count the lines yourself (`wc -l boot/* user/*.S`). Copied numbers go stale.

`bst_cpu_enter` takes only rip, rsp, the general registers and the arithmetic flags from the saved context; the code and stack segments, IOPL 0 and the interrupt flag are forced, and non-canonical addresses are replaced, so whatever the kernel stores there, user code cannot gain I/O privilege, disable interrupts or run with kernel segments.

## Status

v0.2: processes, preemptive round-robin scheduling, system calls and fault isolation. Not there yet:
- one CPU only; the kernel runs with interrupts off and polls the UART;
- all user pages are mapped writable and executable (the image has no page-aligned sections yet);
- a user window of 2 MiB per process, at most 15 processes, programs built into the kernel image (no file system);
- running out of memory when starting a program fails the spawn cleanly, but the kernel's own arena (for `Buf`) still halts the machine when it runs out.

The direction is in bastiolang's [ROADMAP.md](https://github.com/yukikotani231/bastiolang/blob/main/ROADMAP.md) under "v3: toward a kernel".

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <https://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or <https://opensource.org/licenses/MIT>)

at your option.
