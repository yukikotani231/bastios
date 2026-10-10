# BastiOS

A small x86-64 kernel written in [Bastio](https://github.com/yukikotani231/bastiolang), a language whose memory safety is proved at compile time. It runs its own programs and unmodified Linux ones (BusyBox, Lua, sqlite, a C compiler), on several processors, with files on a USB stick or an NVMe, virtio, SATA or ATA disk and a TCP/IP stack on a virtio or e1000 network card. It boots under QEMU, or from a GRUB image on a USB stick under BIOS or UEFI.

<!-- guide-test: session -->
```text
bastios> hello
hello from user mode, pid 2
bastios> fault

[pid 2 (fault): page fault at rip 0x0000008000000018, address 0x0000000000100000; killed]
[exit 139]
bastios> ash
...
# echo hi there > /disk/notes; cat /disk/notes | wc -c
9
# exit
bastios> halt
bye
...
```

The kernel (`src/`) runs each user program in ring 3 in its own address space, preempted by the timer. The shell above is itself a user program in Bastio (`user/sh.bastio`); `fault` reads kernel memory and is killed for it; `ash` is BusyBox's shell, a Linux binary. None of the Bastio code has runtime checks; the compiler proves, before it emits anything:
- every buffer and string index is in bounds, and no arithmetic overflows or underflows;
- every physical page is owned by exactly one thing at a time: the allocator, or one slot of one page table. The allocator's bitmap is proved to agree with the pool, so a page is never handed out twice;
- a page table is empty before it is freed, so tearing down a process returns every page (the test checks that the count at power off equals the count at boot);
- table levels line up, user mappings never go into the kernel's slot of a top-level table, and the address space in CR3 is never freed;
- every device is driven through the I/O ports it claimed: the UART, the interrupt controllers and the timer each own their ports (claimed once, in `main`), no two of them can own the same port, and an access outside a driver's ports does not compile;
- the kernel never reads memory through an address: it reads a user buffer by taking each table out of its parent and the page out of the last table, and putting them back;
- a file's data pages are owned by the file, so removing a file (or powering off) gives each of them back exactly once, and a system call copies only bytes inside the user range it was given and inside the kernel's transfer buffer.

Introduce an off-by-one, drop an ownership check, or free a table that still maps a page, and the build fails with a counterexample instead of producing a kernel.

## Try it

On x86-64 Linux, with Rust, Z3, QEMU and musl-gcc installed ([Getting started](docs/getting-started.md) has the full list):

```sh
git clone https://github.com/yukikotani231/bastiolang
git clone https://github.com/yukikotani231/bastios
(cd bastiolang/stage0 && cargo build --release)
cd bastios && make run
```

The first build proves every source file and fetches BusyBox, so it takes a while. `make run` boots on your terminal; type `help`, and Ctrl-A X quits. [Getting started](docs/getting-started.md) goes on from there: writing a program in Bastio, running a Linux program, and booting from a USB stick.

## What it does

- **Processors.** Every processor runs the scheduler. The kernel's state is under one lock (`Lock[Sys]`) and the console's devices under another, while user code runs in parallel, including the threads of one process, which share its address space.
- **Programs.** BastiOS's own programs use its system call ABI (`int 0x80`). Linux x86-64 ELF executables use the Linux ABI (`syscall`): static or dynamic, against musl, or static against glibc. Much of the Linux system call interface is implemented: files, pipes, signals, threads and futexes, `poll` and `epoll`, timers, and sockets; [docs/linux.md](docs/linux.md) lists them. `make compat` runs a compatibility suite (`test/compat`).
- **Files.** An in-memory file system, and one on the disk under `disk/` that keeps every write whole across a power cut (copy-on-write). The disk is a partition on a USB stick (the one it booted from, with `make usb`), an NVMe drive, a virtio disk, a SATA disk on an AHCI controller or a legacy ATA disk; commands are queued behind a block cache and, except on ATA, finish by interrupt (MSI-X or MSI), so a process waiting for the disk sleeps.
- **Network.** A virtio or Intel e1000 card; DHCP, ARP, IPv4, ICMP, UDP and TCP (retransmission, out-of-order data, zero windows), loopback, and Unix sockets.
- **Console.** The first serial port, and with GRUB the screen (the framebuffer it sets up) with a PS/2 or USB keyboard.

How the pieces fit together, and what is proved where, is in [docs/architecture.md](docs/architecture.md).

## Build and test

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

Tested on x86-64 Linux (Ubuntu 24.04), which is what CI runs; other platforms are untested.

```sh
make            # build/bastios.elf
make run        # boot on this terminal with build/disk.img as the disk; Ctrl-A X quits
make disk FILES="..."   # put files from this machine on that disk
make iso        # build/bastios.iso: GRUB + the kernel, bootable under BIOS or UEFI
make test       # boot in QEMU, type commands over the serial line, check the replies
make fuzz       # fuzz the system calls with hostile arguments (SEED, SEEDS, CALLS, SMP, PAR)
make compat     # the Linux compatibility suite; writes build/compat/report.md
```

`make test` also checks the sessions in this README and in [Getting started](docs/getting-started.md) (`test/guide_test.py`). Set `BASTIOC=/path/to/bastioc0` if the compiler lives elsewhere.

The kernel is built by bastiolang's bootstrap compiler, `bastioc0`, through C: the compiler proves each file, writes C, and `cc` compiles it. bastiolang's self-hosted compiler also has a native backend that writes x86-64 assembly for the kernel and user targets, which takes the C compiler out of the trusted base for Bastio code; BastiOS built that way passes `make test`. The Makefile switches to it once the self-hosted compiler is the default.

## What is trusted

The proof covers everything in `src/` and `user/*.bastio`. Everything below it is trusted, and kept small on purpose:

| Part | Where | Role |
| --- | --- | --- |
| Boot loader | GRUB, configured by `boot/grub.cfg` (only for the ISO) | Loads the kernel as a Multiboot 2 image and passes the memory map, a copy of the ACPI RSDP and the framebuffer it set up (address, pitch, size, pixel layout: the screen console trusts these); `make run` and the tests use QEMU's own Multiboot loader instead |
| Boot and trap entry | `boot/boot.S` | Multiboot 1 and 2 headers and entry (`boot/multiboot.c` turns Multiboot 2 information into the Multiboot 1 form), identity map (the first 2 GiB, and the device window from 2 GiB to 2^46 uncached, with 1 GiB pages above 4 GiB), the Multiboot header's video request, long mode; GDT with user segments; the trap stubs and the `syscall` entry; enabling SSE; entering user mode (which also loads the FS base) (`bst_cpu_enter`) and idling (`bst_cpu_wait`) |
| Processor startup | `boot/smp.c`, `boot/ap.S` | Finding the ACPI tables (the processors in the MADT; any table, a byte at a time, for the kernel's `boot_acpi`), mapping the local APIC and calibrating its timer, INIT and startup IPIs; the real-mode trampoline that takes each application processor to long mode on the kernel's tables, where it checks in and parks until `smp_run` hands it work; each processor's TSS. C because the firmware tables lie outside the device window `Mmio` covers |
| Platform setup | `boot/platform.c` | Interrupt table, `syscall` MSRs |
| C helpers | `boot/libc.c` | `memset` / `memcpy` / `memmove` / `memcmp`, which the C compiler may call |
| Runtime preludes | in bastiolang, `stage0/src/codegen.rs` | one `in` / `out` instruction per port access, COM1 output for the prelude's own panic messages, reading the framebuffer fields out of the Multiboot information (`boot_fb`), the arena allocator, page-table entry encoding, CR3 loads, the system call stubs for user programs |
| User entry | `user/start.S`, `user/user.ld` | Image header; calls `main` and exits with its result |
| Toolchain | | Z3, the stage0 compiler, the C compiler, QEMU |

`test/trusted_base.sh` counts these files, and `make test` fails when the count differs from `test/trusted_base.txt`, so growth gets reviewed. [docs/trusted-base.md](docs/trusted-base.md) says what each part is trusted to do, and one known gap.

`bst_cpu_enter` takes only rip, rsp, the general registers and the arithmetic flags from the saved context; the code and stack segments, IOPL 0 and the interrupt flag are forced, and non-canonical addresses are replaced, so whatever the kernel stores there, user code cannot gain I/O privilege, disable interrupts or run with kernel segments.

## Status

v0.2, pre-release: the system call ABI, the disk format and the build can change. Known limits:
- one lock for the whole kernel state; system calls from different processors wait for each other;
- user pages are always executable: `mprotect` sets only whether a page is writable;
- a user window of 64 MiB per process (pages are mapped as a program asks for them, and `fork` copies every mapped page), at most 31 processes;
- 1024 files and directories in memory and 1023 under `disk/`, paths up to 255 bytes, files up to 64 MiB in memory and 1 GiB on disk, 16 open files per process; one disk, one command at a time; a write call returns once the disk has it, but work spread over several calls can be cut between them by a power cut;
- running out of memory when starting a program fails the spawn cleanly, but the kernel's own arena (for `Buf`) still halts the machine when it runs out;
- it has only been run in QEMU; [Getting started](docs/getting-started.md#5-boot-from-a-usb-stick) lists what real hardware it supports.

The direction is in bastiolang's [ROADMAP.md](https://github.com/yukikotani231/bastiolang/blob/main/ROADMAP.md).

## Layout

| Path | What |
| --- | --- |
| `src/main.bastio`, `proc.bastio`, `syscall.bastio` | Boot, the scheduler loop, faults, power off; processes and program images; BastiOS's system calls |
| `src/pages.bastio`, `space.bastio`, `usermem.bastio`, `elf.bastio` | The page allocator, address spaces, copying user memory, the ELF loader |
| `src/linux.bastio`, `src/lx*.bastio` | The Linux system calls ([docs/linux.md](docs/linux.md)) |
| `src/fs.bastio`, `ramfs.bastio`, `diskfs.bastio`, `diskq.bastio`, `bio.bastio` | Open files, the in-memory and disk file systems, the disk queue and block cache |
| `src/blk.bastio`, `nvme.bastio`, `virtio_blk.bastio`, `ahci.bastio`, `ata.bastio` | Disk drivers (USB sticks: `xhci.bastio`) |
| `src/net.bastio`, `tcp.bastio`, `card.bastio`, `virtio_net.bastio`, `e1000.bastio` | The network stack and card drivers |
| `src/console.bastio`, `uart.bastio`, `fb.bastio`, `kbd.bastio`, `xhci.bastio` | The console: serial port, screen, PS/2 and USB keyboards; `xhci.bastio` also drives USB sticks |
| `src/pic.bastio`, `pit.bastio`, `lapic.bastio`, `hpet.bastio`, `rtc.bastio`, `pci.bastio`, `virtq.bastio` | Interrupt controllers, clocks, PCI, virtqueues |
| `src/acpi.bastio` | ACPI: power off (S5), restart, the power button, the PM timer clock; the FADT and the DSDT's `\_S5` parsed in Bastio |
| `src/info.bastio` | The hardware report (boot option `diag`): processor, memory map, ACPI tables and registers, PCI devices; see [docs/real-hardware.md](docs/real-hardware.md) |
| `boot/` | The trusted base: boot, trap entry, processor startup (see above) |
| `user/*.bastio` | BastiOS programs: `sh`, `hello`, `spin`, `bench`, `fstest`, `crashme`, `diskbench` and others |
| `user/*.c`, `user/*.S` | Test programs in C and assembly: musl programs for the Linux system calls, freestanding ELF and fault tests, the system call fuzzer |
| `files/` | Files installed in memory at boot (built into the kernel image) |
| `test/` | `make test` and the benchmarks; `test/compat/` is the Linux compatibility suite |
| `docs/` | [Getting started](docs/getting-started.md), [architecture](docs/architecture.md), [Linux programs](docs/linux.md), [trusted base](docs/trusted-base.md), [releasing](docs/releasing.md) |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <https://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or <https://opensource.org/licenses/MIT>)

at your option.
