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

| Path | What |
| --- | --- |
| `src/pages.bastio` | Physical page allocator (bitmap proved to track the pool) |
| `src/space.bastio` | Building, walking and tearing down user address spaces |
| `src/proc.bastio` | Process table, program images, spawn and exit |
| `src/syscall.bastio` | System calls and the console input queue |
| `src/main.bastio` | Boot, the scheduler loop, fault handling, power off |
| `user/*.bastio` | User programs: `sh`, `hello`, `spin` (never yields; shows preemption) |
| `user/fault.S` | A user program that reads kernel memory, to test fault isolation |

User programs are linked at `USER_BASE` (0x8000000000) behind a small header (magic, memory size, entry), flattened to raw images, and embedded in the kernel with `include_bytes`. The system call ABI (`int 0x80`) is the one in bastiolang's user prelude.

## Build and run

Requirements: a C compiler and binutils for x86-64, `z3`, `qemu-system-x86_64`, Python 3 (for the test driver), and a built [bastiolang](https://github.com/yukikotani231/bastiolang) checkout next to this one.

```sh
(cd ../bastiolang/stage0 && cargo build --release)
make          # build/bastios.elf
make run      # boot on this terminal; Ctrl-A X quits
make test     # boot in QEMU, type commands over the serial line, check the replies
```

Set `BASTIOC=/path/to/bastioc0` if the compiler lives elsewhere.

## What is trusted

The proof covers everything in `src/` and `user/*.bastio`. Everything below it is trusted, and kept small on purpose:

| Part | Where | Role |
| --- | --- | --- |
| Boot and trap entry | `boot/boot.S` | Multiboot entry, identity map, long mode; GDT with user segments; the trap stubs; entering user mode (`bst_cpu_enter`) and idling (`bst_cpu_wait`) |
| Platform setup | `boot/platform.c` | TSS, interrupt table, 8259 PICs, 100 Hz timer |
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
