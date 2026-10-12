# BastiOS on AArch64

BastiOS builds for AArch64 and boots on QEMU's `virt` machine. This is the first stage of the port tracked in [bastiolang#119](https://github.com/yukikotani231/bastiolang/issues/119) and [#194](https://github.com/yukikotani231/bastios/issues/194): a kernel with the processor and platform parts done, running the native-ABI programs on the same process table, scheduler state, system calls and file system as x86-64. The Linux layer, the network and more than one processor came stage by stage; more than one processor is still to come (see "What is not there yet").

```sh
sudo apt-get install z3 qemu-system-arm gcc-aarch64-linux-gnu   # on an x86-64 host
make ARCH=aarch64          # build/aarch64/bastios.elf
make ARCH=aarch64 run      # boot it on this terminal (Ctrl-A X quits)
make ARCH=aarch64 test     # boot it and drive the shell (test/aarch64_test.sh)
```

The build proves `src/aarch64/main.bastio` and the shared kernel modules it imports (`src/proc.bastio`, `src/syscall.bastio`, `src/fs.bastio` and the rest, each choosing its device-specific parts from `src/aarch64/arch` through `--lib src/aarch64`) with bastioc1's `--target kernel --arch aarch64`, translates it to C, and compiles it with `aarch64-linux-gnu-gcc` (plain `gcc` on an AArch64 machine). `make ARCH=aarch64` and `make` share `build/bastioc1`, the proof cache and `BASTIOC0`, `BASTIOLIB`.

## What runs

`make ARCH=aarch64 run` boots to the BastiOS shell. The native programs work: `hello`, `spin`, `true` and `sh` itself, which starts programs, waits for them, lists (`ps`) and kills them, reads the console and powers off (`halt`, through PSCI `SYSTEM_OFF`). The timer interrupt preempts a program that never makes a system call (`spin &`), and at power off every page is back in the pool.

`src/aarch64/main.bastio` is a single-processor scheduler over the shared `Kernel` (`src/kernel.bastio`); the architecture parts are in `src/aarch64/arch`: the register layout (`regs.bastio`: 37 saved words, 16 of them past the usual process row), the program images, the files the file system starts with, the block layer (a virtio-mmio disk, else a loop device in memory), the network card (virtio-mmio) and an empty sound card. The device window starts at 0 here (`src/aarch64/arch/mmiowin.bastio`). The kernel reads the device tree the platform copied at boot (`boot_info` query 10, byte by byte; `src/aarch64/dt.bastio` walks it in Bastio, every offset checked against the tree's own size), takes the row of `virtio,mmio` nodes it lists (QEMU virt: 32 slots from `0x0a000000`, 512 bytes apart; the same row if there is no tree) and claims the slots one at a time; a slot is kept only if it holds the wanted device. The drivers, the virtqueue code and the slot scan are the ones x86-64 uses on Firecracker (`src/virtio_blk.bastio`, `src/virtio_net.bastio`, `src/virtio_mmio.bastio`); only the transport layer (`src/<arch>/arch/vt.bastio`) differs, because AArch64 has no I/O ports. The clock follows the 100 Hz timer. The file system lives in memory, with the disk's files under `disk/` (`echo text > disk/file` survives a reboot).

The processes are set up as on x86-64: an address space of four-level translation tables (the shared code builds them), the image at `USER_BASE`, the stack at the top of the 512 GiB user window, system calls with `svc #0` (number in x8, arguments in x0-x2), results in x0. Programs are built with `--target user --arch aarch64` and the image header of `user/start-aarch64.S`.

## Linux arm64 programs

An AArch64 ELF executable (`user/lxhello-aarch64.c` is `mhello`, `user/lxtest-aarch64.c` is `mtest`, a static glibc program) runs under the same Linux layer as on x86-64. A process loaded from an ELF file carries the Linux ABI flag (`F_ABI`): its `svc #0` is a Linux call with the number in x8 and the arguments in x0-x5. `src/aarch64/lxnum.bastio` turns the asm-generic numbers into the Linux layer's own (those with the same meaning on x86-64; the rest answer ENOSYS). What differs between the two ABIs is taken from `src/<arch>/arch`: register names and the call-number word (`regs.bastio`), struct stat and statx (`statfmt.bastio`), the signal frame (`sigframe.bastio`; arm64 has no restorer in sigaction, so the return trampoline sits in the vDSO page), the sigaction layout, clone's tls/ctid order, `struct epoll_event` (16 bytes here, packed 12 on x86-64) and uname's machine. `test/aarch64_test.sh` runs both programs. musl-linked programs, BusyBox and the compat suite are not tried yet.

## The platform

The platform is the trusted base besides the compiler's AArch64 preludes ([trusted-base.md](trusted-base.md)):

| File | What it does |
| --- | --- |
| `boot/aarch64/boot.S` | Entry at EL1 with the device tree in x0; the exception vectors; enabling the MMU; `bst_cpu_enter` (the `eret` to EL0 and the register save on the way back) |
| `boot/aarch64/platform.c` | Reading the memory map from the device tree; the kernel's translation table (1 GiB blocks: devices in the first, RAM above); the GICv3 and the generic timer (100 ticks a second); acknowledging interrupts; `bst_cpu_wait` (`wfi`) |
| `boot/aarch64/linker.ld` | Load address (`0x40080000`, where QEMU puts a kernel) and `_end`, where the page pool starts |

The interrupt controller's addresses are QEMU `virt`'s own (`-M virt,gic-version=3`); the walk of the device tree for them comes with real boards.

## What is not there yet

Tracked in [#194](https://github.com/yukikotani231/bastios/issues/194) and its siblings:

- musl, BusyBox and the compat suite on arm64, ptrace register sets ([#196](https://github.com/yukikotani231/bastios/issues/196));
- the interrupt controller and the UART from the device tree too (the platform still uses QEMU virt's addresses for them), the other drivers, and the clock from the generic timer's counter instead of the tick count;
- more than one processor (PSCI `CPU_ON`) ([#195](https://github.com/yukikotani231/bastios/issues/195));
- real hardware ([#197](https://github.com/yukikotani231/bastios/issues/197)).
