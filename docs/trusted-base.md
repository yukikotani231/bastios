# The trusted base

The kernel and the user programs written in Bastio are proved memory safe. This page lists everything that proof rests on without being proved itself, says why each part is there, what it is trusted to do, and how it could shrink.

Sizes are not copied here, because copied numbers go stale. They are counted by a script, and the count is checked into the repository:

```sh
test/trusted_base.sh            # compare with test/trusted_base.txt (the first step of `make test`)
test/trusted_base.sh --update   # after a deliberate change: rewrite it, and review the diff
```

The hand-written C in `boot/` is compiled with `-Wall -Wextra -Werror`; the generated C is not, because it declares temporaries it may not use.

BastiOS also trusts everything bastiolang trusts: Z3, the Bastio compiler, the C compiler, the runtime preludes of the `kernel` and `user` targets, and the specification of every builtin. [bastiolang's docs/trusted-base.md](https://github.com/yukikotani231/bastiolang/blob/main/docs/trusted-base.md) lists those, and `bastioc0 --trusted-base` counts them.

## What the kernel trusts

| File | What it does | Why it is not Bastio |
| --- | --- | --- |
| `boot/boot.S` | Multiboot header (with its request for a 1024x768x32 framebuffer) and entry; identity map of the first 1 GiB and the uncached device window (2-4 GiB); long mode, SSE, `syscall` enabled; the GDT; entering user mode (`bst_cpu_enter`) and idling (`bst_cpu_wait`); the trap stubs, `trap_common` and the `syscall` entry | Mode switches, descriptor tables, `iretq` and saving registers are below any language |
| `boot/ap.S` | The real-mode trampoline that takes each application processor to long mode on the kernel's tables | 16- and 32-bit code |
| `boot/smp.c` | Finding the processors in the ACPI MADT, the local APIC (end of interrupt, timer calibration, startup IPIs), each processor's task state segment and `KERNEL_GS_BASE`, and handing parked processors work for `smp_run` | Firmware tables lie anywhere in the first 1 GiB, outside the device window that `Mmio` covers, and Bastio cannot read memory it was not given as a capability. The local APIC is in the device window, so it could move to `Mmio`; the SMP work plans that |
| `boot/platform.c` | The interrupt table and the `syscall` MSRs | `lidt` and `wrmsr` have no capability |
| `boot/core.h` | The per-processor block layout shared by the three files above | |
| `boot/libc.c` | `memset`, `memcpy`, `memmove` and `memcmp`, which the C compiler may emit calls to | Also linked into each user program |
| `boot/linker.ld` | Load address, and `_end`, where the page pool starts | |
| `boot/grub.cfg` | The GRUB menu for the ISO only (`make run` and the tests use QEMU's Multiboot loader); it loads the video drivers so GRUB can set the framebuffer the kernel asks for | |

What the kernel trusts these to do, beyond the obvious:
- **Entering user mode cannot raise privilege.** `bst_cpu_enter` takes only rip, rsp, the FS base, the general registers and the arithmetic flags from the context Bastio code saved. The code and stack segments, IOPL 0 and the interrupt flag are forced, and a rip, rsp or FS base outside the lower half of the address space is replaced by 0. So nothing the kernel stores in a `Cpu` context can give user code I/O ports, kernel segments or interrupts off.
- **Only one interrupt gate is open to ring 3** (`int 0x80`). The `syscall` entry uses vector 0x81, whose gate has DPL 0, so user code cannot fake it.
- **A trap from user mode always lands on that processor's empty trap stack** (TSS.rsp0), and saves the full context into the processor's block before returning to Bastio.
- **A fault in kernel mode halts the machine** with a report (`bst_kernel_fault`): it is a bug in the trusted base or the compiler.
- **The memory map.** Everything the identity map covers is RAM or the kernel image, the page pool starts at `_end`, and the device window holds only devices.
- **The framebuffer report.** The address, pitch and size GRUB reports for the framebuffer (read by the prelude's `boot_fb`) describe device memory and nothing else; the kernel only checks that the block lies in the device window and claims it there, so a false report would let it draw over another device in the window.

### Known gap: an NMI during the `syscall` entry

`bst_syscall_entry` runs its first three instructions in ring 0 on the user's stack before it switches to the trap stack. The interrupt table uses no IST stacks, so an NMI or a machine check that arrives in that window would push its frame, and `trap_common` its registers, at whatever address user code left in %rsp, which may be kernel memory. That processor then halts (vectors below 32 from kernel mode are faults), but the others keep running on the damaged memory. Linux avoids this by giving NMI, machine checks and debug exceptions their own IST stacks. BastiOS sends no NMIs and QEMU raises none unless asked, so this is trusted rather than ruled out. The fix is an IST stack per processor for vectors 1, 2 and 18 (set in `smp.c`'s `core_init` and `platform.c`'s gates).

## What the proofs of user programs trust

| File | What it does |
| --- | --- |
| `user/start.S` | The image header (magic, size, entry) and `_start`, which calls the program and exits with its result |
| `user/user.ld` | Links each program at USER_BASE behind its header |

The kernel trusts no user code. It checks each image's header at run time and isolates every process in its own address space, so a broken user program, written in Bastio or not, can only fault itself. The other files under `user/` (the C test programs, `fault.S`, `straddle.S`, `elfstart.S`) and BusyBox are such programs, and are not part of the trusted base.

## Devices and the machine

Drivers are Bastio and reach devices only through the ports, registers and lent pages they claimed, which the proofs check. The devices' own behaviour is trusted: a device writes only the pages a driver lent it, stops using them when the driver has seen its completion or reset it, and the addresses a driver programs into it are those of lent pages. The CPU, the firmware (its memory map and ACPI tables, which are still read defensively: every table access in `smp.c` is checked against its length and the identity map, and the prelude reads the memory map only inside the identity map) and QEMU are trusted throughout.

## How it shrinks

- The local APIC accesses in `smp.c` can move to Bastio through `Mmio`.
- Parsing the ACPI tables and the Multiboot memory map can move to Bastio once there is a read-only capability for boot-time firmware memory.
- The C compiler leaves when bastiolang's native backend covers the kernel and user targets. The assembly here stays.
