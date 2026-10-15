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
| `boot/boot.S` | Multiboot 1 and 2 headers (with their request for a 1024x768x32 framebuffer) and entry; identity map of the first 2 GiB (the second only for reading the ACPI tables) and the uncached device window (2 GiB to 2^46, with 1 GiB pages above 4 GiB when the CPU has them); long mode, SSE, `syscall` enabled; the GDT; entering user mode (`bst_cpu_enter`) and halting (`bst_cpu_halt`); the trap stubs, `trap_common` and the `syscall` entry | Mode switches, descriptor tables, `iretq` and saving registers are below any language |
| `boot/ap.S` | The real-mode trampoline that takes each application processor to long mode on the kernel's tables | 16- and 32-bit code |
| `boot/smp.c` | Finding the ACPI tables (the RSDP from the copy Multiboot 2 passed, or the BIOS areas), wherever the kernel's page tables map them (a walk of the tables before each read): the processors in the MADT, and any table a byte at a time for the kernel's `boot_acpi` (`bst_platform_acpi`, which must answer 256 past a table's end); answering the kernel's `boot_info` (`bst_platform_info`: the command line and memory map from `multiboot.c`, CPUID, what processor startup saw, the ACPI tables' list, and words from the processor's RDSEED or RDRAND, run only after CPUID says they exist), of which the kernel assumes nothing (the words only go into the random pool); the local APIC (its mode: xAPIC, or x2APIC when the firmware locked that mode or a processor has an id of 255 or more, with registers then reached as MSRs; end of interrupt, timer calibration against the HPET, else the ACPI PM timer, else PIT channel 2, else PIT channel 0, else the RTC, startup IPIs; an idle application processor's timer made one-shot around `hlt` in `bst_cpu_wait`), each processor's task state segment and `KERNEL_GS_BASE`, and handing parked processors work for `smp_run` | Firmware tables lie anywhere in the first 1 GiB, outside the device window that `Mmio` covers, and Bastio cannot read memory it was not given as a capability. The local APIC is in the device window, so part of it could move to `Mmio` (see "How it shrinks") |
| `boot/multiboot.c` | Copying the Multiboot 2 information, or the PVH `hvm_start_info` (command line, memory map, RSDP), the kernel uses (memory map, framebuffer, ACPI RSDP, command line) into the kernel image in the Multiboot 1 form, every read checked against the structure's size and the identity map | The prelude reads Multiboot 1 and runs after it; a Bastio parser would need the read-only firmware memory capability below |
| `boot/platform.c` | The interrupt table and the `syscall` MSRs; the entry area's page tables (the stubs' code, the GDT and IDT, each processor's block) | `lidt`, `lgdt` and `wrmsr` have no capability, and the area is what user mode's traps and `iretq` touch |
| `boot/core.h` | The per-processor block layout shared by the three files above | |
| `boot/libc.c` | `memset`, `memcpy`, `memmove` and `memcmp`, which the C compiler may emit calls to | Also linked into each user program |
| `boot/linker.ld` | Load address, and `_end`, where the page pool starts | |
| `boot/limine.conf` | The Limine menu for the ISO and the USB image only, loading the kernel with Multiboot 2 (`make run` and most tests use QEMU's Multiboot 1 loader) | |

### AArch64

The AArch64 port ([aarch64.md](aarch64.md)) has its own platform, listed by `test/trusted_base.sh` like the rest:

| File | What it does | Why it is not Bastio |
| --- | --- | --- |
| `boot/aarch64/boot.S` | The Linux arm64 Image header; entry at EL1 (and where PSCI starts another processor); the trampoline page (the exception vectors, and the switch of TTBR0 between the kernel's table and the user's, with one register parked in `TPIDRRO_EL0`); turning the MMU on; letting EL0 read the virtual counter (`CNTKCTL_EL1`, for the vDSO's `clock_gettime`); entering EL0 (`bst_cpu_enter`) and saving the user context on an exception; interrupts taken while the kernel waits | System registers, `eret` and saving registers are below any language |
| `boot/aarch64/platform.c` | The memory map from the device tree and a copy of the tree (up to 64 KiB) for `boot_info` query 10, which the kernel parses in Bastio; the kernel's translation table and the TTBR1 table that maps the trampoline page; each processor's shadow table for user code; the GICv3 and the generic timer; acknowledging interrupts; starting the other processors (PSCI `CPU_ON`) and parking them until `smp_run` gives them work | The device tree and the table are read and written before Bastio runs, and the controller registers are in the device window the kernel maps with 1 GiB blocks |
| `boot/aarch64/linker.ld` | Load address, and `_end`, where the page pool starts | |
| `user/start-aarch64.S` | The image header and `_start` of an AArch64 user program, as `user/start.S` | |

As on x86-64, entering user mode cannot raise privilege: of the saved processor state only the condition flags are kept, and the processor enters EL0t with interrupts unmasked. A fault in kernel mode halts the machine with a report (`bst_kernel_fault`). The kernel's map is the same 1 GiB blocks in top-level slot 0 of every address space, EL1 only and never executable at EL0.

What the kernel trusts these to do, beyond the obvious:
- **Entering user mode cannot raise privilege.** `bst_cpu_enter` takes only rip, rsp, the FS base, the general registers and the arithmetic flags from the context Bastio code saved. The code and stack segments, IOPL 0 and the interrupt flag are forced, and a rip, rsp or FS base outside the lower half of the address space is replaced by 0. So nothing the kernel stores in a `Cpu` context can give user code I/O ports, kernel segments or interrupts off.
- **Only one interrupt gate is open to ring 3** (`int 0x80`). The `syscall` entry uses vector 0x81, whose gate has DPL 0, so user code cannot fake it.
- **A trap from user mode always lands on that processor's empty trap stack** (TSS.rsp0), and saves the full context into the processor's block before returning to Bastio.
- **A fault in kernel mode halts the machine** with a report (`bst_kernel_fault`): it is a bug in the trusted base or the compiler.
- **The memory map.** Everything the identity map covers is RAM or the kernel image, the page pool starts at `_end`, and the device window holds only devices. Above 4 GiB the window may also cover RAM the kernel never uses (the pool stays below 1 GiB), mapped uncached. A user address space shares only the first 512 GiB of the kernel's map, and the kernel switches back to its own tables before any Bastio code runs, so a claimed `Mmio` is always mapped; on a CPU without 1 GiB pages nothing above 4 GiB is, and touching it faults.
- **The framebuffer report.** The address, pitch and size the boot loader reports for the framebuffer (read by the prelude's `boot_fb`) describe device memory and nothing else; the kernel only checks that the block lies in the device window and claims it there, so a false report would let it draw over another device in the window.

### Closed gap: an NMI during the `syscall` entry

`bst_syscall_entry` runs its first three instructions in ring 0 on the user's stack before it switches to the trap stack. An NMI or a machine check arriving in that window used to push its frame, and `trap_common` its registers, at whatever address user code left in %rsp. Now, as in Linux, the debug exception, NMI, double fault and machine check (vectors 1, 2, 8 and 18) run on their own stack: each processor has a 4 KiB IST1 stack above its task state segment (`core_init` in `smp.c`, the gates in `platform.c`). An NMI from kernel mode still halts that processor, but on its own stack, so it no longer writes where user code points. `test/nmi_test.sh` injects an NMI and checks that the halted processor's %rsp is inside its IST1 stack. The NMI lands while the kernel waits, not inside the three-instruction window, which cannot be hit on purpose; the test shows the gate switches stacks, which is what closes the window.

### Closed gap: kernel stack overflow

Bastio proves memory safety but not stack depth, so nothing stops kernel code from recursing past the bottom of its stack. The kernel stack used to sit right above the IST1 stack, the task state segment and the saved user context, all mapped, so an overflow wrote over them without a fault: a corrupted `rsp0` or `ist1`, or a user context with a forged rip. Now the page under each kernel stack is unmapped (`guard_block` in `smp.c` splits the 2 MiB page that holds each processor's block into 4 KiB ones and takes the guard out before that processor starts; the layout is in `core.h`). A push into the guard page faults; that fault finds no stack either and becomes a double fault, which runs on IST1 and halts the processor with cr2 in the guard. `test/stackguard_test.sh` moves the waiting processor's %rsp to just above the guard through QEMU's gdb stub and checks for that report; on a kernel without the guard it reports nothing. `test/stackdepth.py` adds up the frames along the deepest call chain of a native kernel (`build/kernel.s`) and fails when it passes 64 KiB of the 92 KiB, so a compiler change that makes frames grow cannot overflow the stack unseen; the recursion in the kernel (a hub inside a hub, a signal that posts one more) is counted at a fixed depth that the test lists, one of them an assumption and not a proof. Still trusted: the trap stack (one frame and `bst_lapic_ack`) and the IST1 stack (the fault report) have no guard, because what runs on them is short, fixed C and assembly, not Bastio code.

## What the proofs of user programs trust

| File | What it does |
| --- | --- |
| `user/start.S` | The image header (magic, size, entry) and `_start`, which calls the program and exits with its result |
| `user/user.ld` | Links each program at USER_BASE (address 0) behind its header |

| `user/vdso.c`, `user/vdso.ld`, `user/vdso.map` | The vDSO: `__vdso_clock_gettime` in user mode, with the kernel's clock arithmetic (`src/tsc.bastio`) written again in C. The kernel copies it, with the clock's parameters, onto a read-only page of every Linux process (`src/elf.bastio`) | It runs in user mode as part of each process, like libc, so it is not trusted by the kernel: a bug in it gives that process a wrong time and nothing more. It is listed here because every Linux program relies on it for the time |

The kernel trusts no user code. It checks each image's header at run time and isolates every process in its own address space, so a broken user program, written in Bastio or not, can only fault itself. The other files under `user/` (the C test programs, `fault.S`, `straddle.S`, `elfstart.S`) and BusyBox are such programs, and are not part of the trusted base.

## Devices and the machine

Drivers are Bastio and reach devices only through the ports, registers and lent pages they claimed, which the proofs check. The devices' own behaviour is trusted: a device writes only the pages a driver lent it, stops using them when the driver has seen its completion or reset it, and the addresses a driver programs into it are those of lent pages. A USB disk is used only inside a GPT partition of the BastiOS type, so whoever made that partition is trusted not to have kept anything else in it; the GPT itself is read defensively (the partition must lie on the disk). The time-stamp counter is trusted to count at a steady rate and to agree across processors (an invariant TSC; under QEMU's TCG every processor reads the host's): the proofs know nothing about its readings (`cpu_tsc`), and the kernel's clock only keeps itself from going back. The CPU, the firmware (its memory map and ACPI tables, which are still read defensively: every table access in `smp.c` is checked against its length and what `boot.S` maps, and the kernel's parsing of the FADT and DSDT is proved in bounds, and the prelude reads the memory map only inside the identity map) and QEMU are trusted throughout.

## How it shrinks

- The local APIC accesses in `smp.c` could move to Bastio through `Mmio`, but only part of them, and that part buys little. Sending INIT and startup IPIs and calibrating the timer against the PIT could move if `main` started the APs after claiming the APIC page, instead of `boot.S` starting them before `main`. Three uses have to stay in C: the end-of-interrupt write in the trap path, each AP starting its own timer before it runs any Bastio, and the parked APs, which rely on that timer to wake up and look for work. C would then still write the same page, so a Bastio claim on it would not be exclusive, and the proof would add little beyond the bounds check `map_lapic` already makes. About 30 lines would leave in exchange for reordering the boot. It is not worth doing on its own; it fits with a future change that already moves AP start-up or the park loop into Bastio. Since the disk interrupts (2026-10-10), the kernel does claim the page for what it adds: turning the bootstrap processor's APIC on, acknowledging `V_DISK` and `V_WAKE`, and sending `V_WAKE` (`src/lapic.bastio`). That claim is not exclusive, for the reason above, and nothing moved out of `smp.c`.
- Parsing the ACPI tables beyond finding them moved to Bastio with `boot_acpi` (2026-10-10: the FADT, `\_S5`, the HPET table, in `src/acpi.bastio`); the MADT walk and the timer calibration stay in C because they run before `main`. The Multiboot memory map can move once there is a read-only capability for boot-time firmware memory.
- The C compiler leaves when bastiolang's native backend covers the kernel and user targets. The assembly here stays.
