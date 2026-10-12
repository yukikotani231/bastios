# BastiOS on AArch64

BastiOS builds for AArch64 and boots on QEMU's `virt` machine. This is the first stage of the port tracked in [bastiolang#119](https://github.com/yukikotani231/bastiolang/issues/119) and [#194](https://github.com/yukikotani231/bastios/issues/194): a kernel with the processor and platform parts done, running the native-ABI programs. The Linux layer, the file system, the network and more than one processor come stage by stage (see "What is not there yet").

```sh
sudo apt-get install z3 qemu-system-arm gcc-aarch64-linux-gnu   # on an x86-64 host
make ARCH=aarch64          # build/aarch64/bastios.elf
make ARCH=aarch64 run      # boot it on this terminal (Ctrl-A X quits)
make ARCH=aarch64 test     # boot it and drive the shell (test/aarch64_test.sh)
```

The build proves `src/aarch64/main.bastio` (and the shared `src/pages.bastio` and `src/space.bastio` it imports) with bastioc1's `--target kernel --arch aarch64`, translates it to C, and compiles it with `aarch64-linux-gnu-gcc` (plain `gcc` on an AArch64 machine). `make ARCH=aarch64` and `make` share `build/bastioc1`, the proof cache and `BASTIOC0`, `BASTIOLIB`.

## What runs

`make ARCH=aarch64 run` boots to the BastiOS shell. The native programs work: `hello`, `spin`, `true` and `sh` itself, which starts programs, waits for them, lists (`ps`) and kills them, reads the console and powers off (`halt`, through PSCI `SYSTEM_OFF`). The timer interrupt preempts a program that never makes a system call (`spin &`), and at power off every page is back in the pool.

The processes are set up as on x86-64: an address space of four-level translation tables (the shared code builds them), the image at `USER_BASE`, the stack at the top of the 512 GiB user window, system calls with `svc #0` (number in x8, arguments in x0-x2), results in x0. Programs are built with `--target user --arch aarch64` and the image header of `user/start-aarch64.S`.

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

- the Linux arm64 system-call ABI and AArch64 ELF programs ([#196](https://github.com/yukikotani231/bastios/issues/196));
- the file system, the network and the other drivers: `src/main.bastio` and its modules hold `Ports` through their types and need an architecture split first, as does the register layout the Linux layer reads (`F_REGS + R_*`);
- virtio-mmio drivers and more than one processor (PSCI `CPU_ON`) ([#195](https://github.com/yukikotani231/bastios/issues/195));
- real hardware ([#197](https://github.com/yukikotani231/bastios/issues/197)).
