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
| `boot/boot.S` | Multiboot 1 and 2 headers (with their request for a 1024x768x32 framebuffer) and entry; identity map of the first 1 GiB and the uncached device window (2 GiB to 2^46, with 1 GiB pages above 4 GiB when the CPU has them); long mode, SSE, `syscall` enabled; the GDT; entering user mode (`bst_cpu_enter`) and idling (`bst_cpu_wait`); the trap stubs, `trap_common` and the `syscall` entry | Mode switches, descriptor tables, `iretq` and saving registers are below any language |
| `boot/ap.S` | The real-mode trampoline that takes each application processor to long mode on the kernel's tables | 16- and 32-bit code |
| `boot/smp.c` | Finding the ACPI tables (the RSDP from the copy Multiboot 2 passed, or the BIOS areas): the processors in the MADT, and any table a byte at a time for the kernel's `boot_acpi` (`bst_platform_acpi`, which must answer 256 past a table's end); the local APIC (end of interrupt, timer calibration against the HPET, else the ACPI PM timer, else the PIT, startup IPIs), each processor's task state segment and `KERNEL_GS_BASE`, and handing parked processors work for `smp_run` | Firmware tables lie anywhere in the first 1 GiB, outside the device window that `Mmio` covers, and Bastio cannot read memory it was not given as a capability. The local APIC is in the device window, so part of it could move to `Mmio` (see "How it shrinks") |
| `boot/multiboot.c` | Copying the Multiboot 2 information the kernel uses (memory map, framebuffer, ACPI RSDP) into the kernel image in the Multiboot 1 form, every read checked against the structure's size and the identity map | The prelude reads Multiboot 1 and runs after it; a Bastio parser would need the read-only firmware memory capability below |
| `boot/platform.c` | The interrupt table and the `syscall` MSRs | `lidt` and `wrmsr` have no capability |
| `boot/core.h` | The per-processor block layout shared by the three files above | |
| `boot/libc.c` | `memset`, `memcpy`, `memmove` and `memcmp`, which the C compiler may emit calls to | Also linked into each user program |
| `boot/linker.ld` | Load address, and `_end`, where the page pool starts | |
| `boot/grub.cfg` | The GRUB menu for the ISO only, loading the kernel with Multiboot 2 (`make run` and most tests use QEMU's Multiboot 1 loader); it loads the video drivers so GRUB can set the framebuffer the kernel asks for | |

What the kernel trusts these to do, beyond the obvious:
- **Entering user mode cannot raise privilege.** `bst_cpu_enter` takes only rip, rsp, the FS base, the general registers and the arithmetic flags from the context Bastio code saved. The code and stack segments, IOPL 0 and the interrupt flag are forced, and a rip, rsp or FS base outside the lower half of the address space is replaced by 0. So nothing the kernel stores in a `Cpu` context can give user code I/O ports, kernel segments or interrupts off.
- **Only one interrupt gate is open to ring 3** (`int 0x80`). The `syscall` entry uses vector 0x81, whose gate has DPL 0, so user code cannot fake it.
- **A trap from user mode always lands on that processor's empty trap stack** (TSS.rsp0), and saves the full context into the processor's block before returning to Bastio.
- **A fault in kernel mode halts the machine** with a report (`bst_kernel_fault`): it is a bug in the trusted base or the compiler.
- **The memory map.** Everything the identity map covers is RAM or the kernel image, the page pool starts at `_end`, and the device window holds only devices. Above 4 GiB the window may also cover RAM the kernel never uses (the pool stays below 1 GiB), mapped uncached. A user address space shares only the first 512 GiB of the kernel's map, and the kernel switches back to its own tables before any Bastio code runs, so a claimed `Mmio` is always mapped; on a CPU without 1 GiB pages nothing above 4 GiB is, and touching it faults.
- **The framebuffer report.** The address, pitch and size GRUB reports for the framebuffer (read by the prelude's `boot_fb`) describe device memory and nothing else; the kernel only checks that the block lies in the device window and claims it there, so a false report would let it draw over another device in the window.

### Closed gap: an NMI during the `syscall` entry

`bst_syscall_entry` runs its first three instructions in ring 0 on the user's stack before it switches to the trap stack. An NMI or a machine check arriving in that window used to push its frame, and `trap_common` its registers, at whatever address user code left in %rsp. Now, as in Linux, the debug exception, NMI, double fault and machine check (vectors 1, 2, 8 and 18) run on their own stack: each processor has a 4 KiB IST1 stack above its task state segment (`core_init` in `smp.c`, the gates in `platform.c`). An NMI from kernel mode still halts that processor, but on its own stack, so it no longer writes where user code points. `test/nmi_test.sh` injects an NMI and checks that the halted processor's %rsp is inside its IST1 stack. The NMI lands while the kernel waits, not inside the three-instruction window, which cannot be hit on purpose; the test shows the gate switches stacks, which is what closes the window.

## What the proofs of user programs trust

| File | What it does |
| --- | --- |
| `user/start.S` | The image header (magic, size, entry) and `_start`, which calls the program and exits with its result |
| `user/user.ld` | Links each program at USER_BASE behind its header |

The kernel trusts no user code. It checks each image's header at run time and isolates every process in its own address space, so a broken user program, written in Bastio or not, can only fault itself. The other files under `user/` (the C test programs, `fault.S`, `straddle.S`, `elfstart.S`) and BusyBox are such programs, and are not part of the trusted base.

## Devices and the machine

Drivers are Bastio and reach devices only through the ports, registers and lent pages they claimed, which the proofs check. The devices' own behaviour is trusted: a device writes only the pages a driver lent it, stops using them when the driver has seen its completion or reset it, and the addresses a driver programs into it are those of lent pages. A USB disk is used only inside a GPT partition of the BastiOS type, so whoever made that partition is trusted not to have kept anything else in it; the GPT itself is read defensively (the partition must lie on the disk). The CPU, the firmware (its memory map and ACPI tables, which are still read defensively: every table access in `smp.c` is checked against its length and what `boot.S` maps, and the kernel's parsing of the FADT and DSDT is proved in bounds, and the prelude reads the memory map only inside the identity map) and QEMU are trusted throughout.

## How it shrinks

- The local APIC accesses in `smp.c` could move to Bastio through `Mmio`, but only part of them, and that part buys little. Sending INIT and startup IPIs and calibrating the timer against the PIT could move if `main` started the APs after claiming the APIC page, instead of `boot.S` starting them before `main`. Three uses have to stay in C: the end-of-interrupt write in the trap path, each AP starting its own timer before it runs any Bastio, and the parked APs, which rely on that timer to wake up and look for work. C would then still write the same page, so a Bastio claim on it would not be exclusive, and the proof would add little beyond the bounds check `map_lapic` already makes. About 30 lines would leave in exchange for reordering the boot. It is not worth doing on its own; it fits with a future change that already moves AP start-up or the park loop into Bastio. Since the disk interrupts (2026-10-10), the kernel does claim the page for what it adds: turning the bootstrap processor's APIC on, acknowledging `V_DISK` and `V_WAKE`, and sending `V_WAKE` (`src/lapic.bastio`). That claim is not exclusive, for the reason above, and nothing moved out of `smp.c`.
- Parsing the ACPI tables beyond finding them moved to Bastio with `boot_acpi` (2026-10-10: the FADT, `\_S5`, the HPET table, in `src/acpi.bastio`); the MADT walk and the timer calibration stay in C because they run before `main`. The Multiboot memory map can move once there is a read-only capability for boot-time firmware memory.
- The C compiler leaves when bastiolang's native backend covers the kernel and user targets. The assembly here stays.
