# BastiOS

A small x86-64 kernel written in [Bastio](https://github.com/yukikotani231/bastiolang), a language whose memory safety is proved at compile time. It boots under QEMU and runs a shell on the serial console.

```
BastiOS 0.1 -- type `help`
bastios> add 2 40
42
bastios> mul 4294967296 4294967296
overflow: the product does not fit in 64 bits
bastios> halt
bye
```

The shell (`src/main.bastio`) has no runtime checks of any kind. The Bastio compiler proves that:
- every buffer index and string index is in bounds;
- no arithmetic overflows or underflows;
- every byte sent to the serial port fits in 8 bits;
- the line buffer is freed exactly once and the console capability is never lost.

Introduce an off-by-one and the build fails with a counterexample instead of producing a kernel.

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

The proof covers `src/main.bastio`. Everything below it is trusted, and kept small on purpose:

| Part | Where | Lines | Role |
| --- | --- | --- | --- |
| Boot code | `boot/boot.S` | ~100 | Multiboot entry, identity-mapped page tables, switch to long mode, call `bst_kmain` |
| C helpers | `boot/libc.c` | ~40 | `memset` / `memcpy` / `memmove` / `memcmp`, which the C compiler may call |
| Kernel prelude | in bastiolang, `stage0/src/codegen.rs` | ~70 | COM1 port I/O, the 8 MiB arena allocator |
| Toolchain | | | Z3, the stage0 compiler, the C compiler, QEMU |

Count the lines yourself (`wc -l boot/*`). Copied numbers go stale.

## Status

v0: a single-task kernel with one serial shell. No interrupts, no paging beyond the boot identity map, no user mode. Running out of memory halts the machine. The direction is in bastiolang's [ROADMAP.md](https://github.com/yukikotani231/bastiolang/blob/main/ROADMAP.md) under "v3: toward a kernel".
