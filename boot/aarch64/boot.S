/*
 * BastiOS on AArch64, QEMU virt machine: the entry code, the exception
 * vectors and the switch to user mode. Part of the trusted base, with
 * platform.c: it saves and restores registers and sets system registers,
 * nothing else. Everything the kernel decides is in Bastio.
 *
 * Contract with the compiler's AArch64 kernel prelude (bastiolang
 * docs/language.md, Targets): bst_kmain(memmap) is called with the MMU on
 * and the kernel's map in bst_kernel_root; bst_cpu_enter runs the saved
 * user context at EL0 and returns the kind of the next exception.
 */
        /* Per processor (struct pcpu in platform.c): TPIDR_EL1 points at it. */
        .equ    PCPU_CTX, 0             /* the saved user context, 37 words */
        .equ    PCPU_MMU3, 344          /* the user table bst_cpu_enter loads (word 3 of bst_platform_mmu_state) */
        .equ    PCPU_IRQ, 352           /* the interrupt that ended bst_cpu_wait */
        .equ    PCPU_ID, 360
        .equ    PCPU_SHADOW, 376        /* the table user code runs on */
        .equ    PCPU_TTBR, 384          /* shadow table | ASID << 48 (bst_user_ttbr) */
        .equ    PCPU_SIZE, 432
        .equ    CPU_STACK, 65536

        /* Firmware often hands over at EL2 (U-Boot, UEFI, QEMU with
         * virtualization=on): drop to EL1 with the setup EL2 owes it, and
         * carry on there. A no-op at EL1. */
        .macro  el2_to_el1
        mrs     x1, CurrentEL
        cmp     x1, #8                  /* EL2 */
        b.ne    .Lel1_\@
        mov     x1, #(1 << 31)          /* HCR_EL2.RW: EL1 is AArch64 */
        msr     hcr_el2, x1
        mov     x1, #3                  /* CNTHCTL_EL2: EL1 may read the counter and use the timer */
        msr     cnthctl_el2, x1
        msr     cntvoff_el2, xzr
        mov     x1, #0x33ff             /* CPTR_EL2: FP and SIMD do not trap */
        msr     cptr_el2, x1
        msr     hstr_el2, xzr
        mrs     x1, id_aa64pfr0_el1
        ubfx    x1, x1, #24, #4         /* the GIC system register interface? */
        cbz     x1, .Lsre_\@
        mrs     x1, icc_sre_el2
        orr     x1, x1, #0xf            /* SRE, DFB, DIB, Enable: EL1 may use ICC_SRE_EL1 */
        msr     icc_sre_el2, x1
        isb
.Lsre_\@:
        ldr     x1, =0x30d00800         /* SCTLR_EL1: its RES1 bits, MMU and caches off */
        msr     sctlr_el1, x1
        mov     x1, #0x3c5              /* SPSR_EL2: EL1h, DAIF masked */
        msr     spsr_el2, x1
        adr     x1, .Lel1_\@
        msr     elr_el2, x1
        eret
.Lel1_\@:
        .endm

        /* The Linux arm64 "Image" header (Documentation/arm64/booting.rst):
         * QEMU, U-Boot and UEFI loaders that see it pass the device tree's
         * address in x0 and load the image at its text offset. (QEMU gives
         * an ELF kernel no tree.) The image is bastios.bin, bastios.elf
         * with its headers taken off.
         *
         * It is also a PE/COFF file, as Linux's is: code0 spells "MZ", the
         * word at 0x3c points to the PE header below, and a UEFI firmware
         * loads the image anywhere it likes and enters efi_entry (efi.c),
         * which moves it to its link address and starts it as a loader
         * would. The linker script defines the sizes. */
        .section .text.boot
        .global _start
_start:
        add     x13, x18, #0x16         /* code0: "MZ", and harmless to run */
        b       start                   /* code1 */
        .quad   0x80000                 /* text_offset */
        .quad   __image_size            /* image_size, with the zeroed data */
        .quad   0                       /* flags: little-endian, 4 KiB pages */
        .quad   0, 0, 0                 /* reserved */
        .long   0x644d5241              /* magic "ARM\x64" */
        .long   .Lpe_header - _start    /* offset of the PE header */

.Lpe_header:
        .ascii  "PE\0\0"
        .short  0xaa64                  /* Machine: ARM64 */
        .short  1                       /* NumberOfSections */
        .long   0, 0, 0                 /* time stamp, no symbols */
        .short  .Lsection - .Loptional  /* SizeOfOptionalHeader */
        .short  0x0206                  /* executable, no line numbers, no debug info */
.Loptional:
        .short  0x20b                   /* PE32+ */
        .byte   0, 0                    /* linker version */
        .long   __file_size - 0x200     /* SizeOfCode */
        .long   0, 0                    /* SizeOfInitializedData, SizeOfUninitializedData */
        .long   __efi_entry_rva         /* AddressOfEntryPoint */
        .long   0x200                   /* BaseOfCode */
        .quad   0                       /* ImageBase: nothing to relocate */
        .long   0x1000                  /* SectionAlignment */
        .long   0x200                   /* FileAlignment */
        .short  0, 0, 0, 0              /* operating system and image versions */
        .short  0, 0                    /* subsystem version */
        .long   0                       /* Win32VersionValue */
        .long   __image_size            /* SizeOfImage */
        .long   0x200                   /* SizeOfHeaders */
        .long   0                       /* CheckSum */
        .short  10                      /* Subsystem: EFI application */
        .short  0                       /* DllCharacteristics */
        .quad   0, 0, 0, 0              /* stack and heap sizes */
        .long   0                       /* LoaderFlags */
        .long   16                      /* NumberOfRvaAndSizes */
        .fill   16 * 8, 1, 0            /* the data directories: none */
.Lsection:
        .ascii  ".text\0\0\0"
        .long   __image_size - 0x200    /* VirtualSize */
        .long   0x200                   /* VirtualAddress */
        .long   __file_size - 0x200     /* SizeOfRawData */
        .long   0x200                   /* PointerToRawData */
        .long   0, 0                    /* no relocations or line numbers */
        .short  0, 0
        .long   0xe0000020              /* code, executable, readable, writable */
        .balign 0x200
start:
        mov     x19, x0                 /* the device tree */
        el2_to_el1
        mrs     x1, mpidr_el1
        and     x1, x1, #0xff
        cbnz    x1, park                /* the others are started by PSCI (bst_secondary_entry) */
        ldr     x1, =stack_top
        mov     sp, x1
        ldr     x1, =__bss_start
        ldr     x2, =__bss_end
1:      cmp     x1, x2
        b.hs    2f
        str     xzr, [x1], #8
        b       1b
2:
        ldr     x1, =bst_pcpus          /* this is processor 0 */
        msr     tpidr_el1, x1
        /* FP/SIMD usable at EL0 (the kernel itself is built without it). */
        mov     x1, #(3 << 20)
        msr     cpacr_el1, x1
        ldr     x1, =VECTORS_VA
        msr     vbar_el1, x1

        /* The memory map and the kernel's translation table (platform.c),
         * built with the MMU off. */
        mov     x0, x19
        bl      bst_early

        mov     x1, #0xff               /* index 0 normal write-back, index 1 device-nGnRnE */
        msr     mair_el1, x1
        ldr     x1, =TCR
        msr     tcr_el1, x1
        ldr     x1, =bst_kernel_root
        msr     ttbr0_el1, x1
        ldr     x1, =bst_ttbr1_tables
        msr     ttbr1_el1, x1
        isb
        tlbi    vmalle1
        dsb     nsh
        isb
        mrs     x1, sctlr_el1
        orr     x1, x1, #1              /* M: the MMU */
        orr     x1, x1, #(1 << 2)       /* C: data cache */
        orr     x1, x1, #(1 << 12)      /* I: instruction cache */
        bic     x1, x1, #(1 << 19)      /* WXN off: a writable page may hold code */
        bic     x1, x1, #(1 << 1)       /* A off: unaligned accesses are fine */
        msr     sctlr_el1, x1
        isb

        bl      bst_gic_init            /* interrupt controller and timer */
        bl      bst_smp_boot            /* the other processors, parked */
        ldr     x0, =bst_memmap
        bl      bst_kmain
        /* Power off through PSCI SYSTEM_OFF. */
        bl      bst_psci_system_off
park:   wfe
        b       park

/*
 * PSCI CPU_ON enters here at EL1 with the MMU off and x0 = the processor's
 * number (the context id bst_smp_boot passed): the same setup as _start,
 * then bst_secondary_main on this processor's own stack.
 */
        .global bst_secondary_entry
bst_secondary_entry:
        mov     x19, x0
        el2_to_el1
        ldr     x1, =bst_pcpus
        mov     x2, #PCPU_SIZE
        madd    x1, x19, x2, x1
        msr     tpidr_el1, x1
        ldr     x1, =bst_cpu_stacks
        mov     x2, #CPU_STACK
        add     x3, x19, #1
        madd    x1, x3, x2, x1
        mov     sp, x1
        mov     x1, #(3 << 20)
        msr     cpacr_el1, x1
        ldr     x1, =VECTORS_VA
        msr     vbar_el1, x1
        mov     x1, #0xff
        msr     mair_el1, x1
        ldr     x1, =TCR
        msr     tcr_el1, x1
        ldr     x1, =bst_kernel_root
        msr     ttbr0_el1, x1
        ldr     x1, =bst_ttbr1_tables
        msr     ttbr1_el1, x1
        isb
        tlbi    vmalle1
        dsb     nsh
        isb
        mrs     x1, sctlr_el1
        orr     x1, x1, #1
        orr     x1, x1, #(1 << 2)
        orr     x1, x1, #(1 << 12)
        bic     x1, x1, #(1 << 19)
        bic     x1, x1, #(1 << 1)
        msr     sctlr_el1, x1
        isb
        bl      bst_secondary_main
        b       park

        /* 48-bit addresses from both TTBR0 and TTBR1 (T0SZ, T1SZ 16), 4 KiB
         * granules, write-back inner shareable walks, 40-bit physical. */
        .equ    TCR, 16 | (1 << 8) | (1 << 10) | (3 << 12) | (16 << 16) | (1 << 24) | (1 << 26) | (3 << 28) | (2 << 30) | (2 << 32)

/*
 * uint64_t bst_cpu_enter(void): run the saved user context at EL0 until the
 * next exception, save it back, and return its kind: 128 for SVC, 14 for an
 * abort, 32 + n for an interrupt, 6 for anything else. Of SPSR only the
 * condition flags are kept: EL0t, interrupts unmasked.
 */
        .text
        .global bst_cpu_enter
bst_cpu_enter:
        stp     x29, x30, [sp, #-96]!
        stp     x19, x20, [sp, #16]
        stp     x21, x22, [sp, #32]
        stp     x23, x24, [sp, #48]
        stp     x25, x26, [sp, #64]
        stp     x27, x28, [sp, #80]
        mrs     x30, tpidr_el1          /* PCPU_CTX is 0 */
        /* The processor's shadow table: slot 0 is the window's level-3 table,
         * slot 1 of the process's own. */
        ldr     x2, [x30, #PCPU_MMU3]
        ldr     x2, [x2, #8]
        ldr     x3, =bst_shadow_root
        ldr     x4, [x30, #PCPU_ID]
        add     x3, x3, x4, lsl #12
        str     x2, [x3]
        str     x3, [x30, #PCPU_SHADOW]
        ldr     x0, [x30, #PCPU_MMU3]
        mov     x1, x3
        bl      bst_user_ttbr           /* the table's ASID, and a TLB flush if entries went away */
        mrs     x30, tpidr_el1
        ldr     x1, [x30, #248]
        msr     sp_el0, x1
        ldr     x1, [x30, #256]
        msr     elr_el1, x1
        ldr     x1, [x30, #264]
        mov     x2, #0xf0000000         /* NZCV and PSTATE.SS: the single-step request of a ptrace */
        orr     x2, x2, #0x200000
        and     x1, x1, x2
        msr     spsr_el1, x1
        ubfx    x2, x1, #21, #1
        mrs     x3, mdscr_el1           /* the step exception is armed only while PSTATE.SS asks for one */
        bfi     x3, x2, #0, #1
        msr     mdscr_el1, x3
        ldr     x1, [x30, #288]
        msr     tpidr_el0, x1
        ldp     x0, x1, [x30, #0]
        msr     tpidrro_el0, x0         /* x0 waits there while the trampoline needs it */
        ldp     x2, x3, [x30, #16]
        ldp     x4, x5, [x30, #32]
        ldp     x6, x7, [x30, #48]
        ldp     x8, x9, [x30, #64]
        ldp     x10, x11, [x30, #80]
        ldp     x12, x13, [x30, #96]
        ldp     x14, x15, [x30, #112]
        ldp     x16, x17, [x30, #128]
        ldp     x18, x19, [x30, #144]
        ldp     x20, x21, [x30, #160]
        ldp     x22, x23, [x30, #176]
        ldp     x24, x25, [x30, #192]
        ldp     x26, x27, [x30, #208]
        ldp     x28, x29, [x30, #224]
        ldr     x30, [x30, #240]
        ldr     x0, =TRAMP_ENTER_VA
        br      x0                      /* the trampoline loads the user's table and erets */

/* From EL0: save the context, then return from bst_cpu_enter with the kind
 * in x0. SP_EL1 is still where bst_cpu_enter left it. */
.macro  user_trap next
        mrs     x0, tpidrro_el0         /* the trampoline stashed the user's x0 there */
        stp     x0, x1, [sp, #-16]!
        mrs     x0, tpidr_el1
        stp     x2, x3, [x0, #16]
        stp     x4, x5, [x0, #32]
        stp     x6, x7, [x0, #48]
        stp     x8, x9, [x0, #64]
        stp     x10, x11, [x0, #80]
        stp     x12, x13, [x0, #96]
        stp     x14, x15, [x0, #112]
        stp     x16, x17, [x0, #128]
        stp     x18, x19, [x0, #144]
        stp     x20, x21, [x0, #160]
        stp     x22, x23, [x0, #176]
        stp     x24, x25, [x0, #192]
        stp     x26, x27, [x0, #208]
        stp     x28, x29, [x0, #224]
        str     x30, [x0, #240]
        ldp     x2, x3, [sp], #16
        stp     x2, x3, [x0, #0]
        mrs     x1, sp_el0
        str     x1, [x0, #248]
        mrs     x1, elr_el1
        str     x1, [x0, #256]
        mrs     x1, spsr_el1
        str     x1, [x0, #264]
        mrs     x1, esr_el1
        str     x1, [x0, #272]
        mrs     x1, far_el1
        str     x1, [x0, #280]
        mrs     x1, tpidr_el0
        str     x1, [x0, #288]
        b       \next
.endm

/* Classifies a synchronous exception from EL0 by its class (ESR bits 26-31). */
user_sync_kind:
        mrs     x1, esr_el1
        lsr     x1, x1, #26
        mov     x0, #128
        cmp     x1, #0x15               /* SVC from AArch64 */
        b.eq    user_return
        mov     x0, #14
        cmp     x1, #0x20               /* instruction abort from EL0 */
        b.eq    user_return
        cmp     x1, #0x24               /* data abort from EL0 */
        b.eq    user_return
        mov     x0, #3
        cmp     x1, #0x3c               /* BRK: SIGTRAP */
        b.eq    user_return
        mov     x0, #1
        cmp     x1, #0x32               /* software step from EL0: SIGTRAP, a single step */
        b.eq    user_return
        mov     x0, #17
        cmp     x1, #0x22               /* PC alignment fault: SIGBUS */
        b.eq    user_return
        cmp     x1, #0x26               /* SP alignment fault */
        b.eq    user_return
        mov     x0, #6                  /* anything else (unknown instruction, ...): SIGILL */
        b       user_return
user_irq_kind:
        bl      bst_irq_ack             /* the kernel's vector for it */
user_return:
        ldp     x19, x20, [sp, #16]
        ldp     x21, x22, [sp, #32]
        ldp     x23, x24, [sp, #48]
        ldp     x25, x26, [sp, #64]
        ldp     x27, x28, [sp, #80]
        ldp     x29, x30, [sp], #96
        ret

el0_sync_k:
        user_trap user_sync_kind
el0_irq_k:
        user_trap user_irq_kind

/* The kernel's table is in TTBR0 again by the time these run (the vectors
 * below are in the trampoline page): from EL1, x0 is in TPIDRRO_EL0. */
el1_irq_k:
        mrs     x0, tpidrro_el0
        b       el1_irq

/* An interrupt taken at EL1 while the kernel waits in bst_cpu_wait: note
 * which one and go back to the instruction after the wfi. */
el1_irq:
        stp     x0, x1, [sp, #-176]!
        stp     x2, x3, [sp, #16]
        stp     x4, x5, [sp, #32]
        stp     x6, x7, [sp, #48]
        stp     x8, x9, [sp, #64]
        stp     x10, x11, [sp, #80]
        stp     x12, x13, [sp, #96]
        stp     x14, x15, [sp, #112]
        stp     x16, x17, [sp, #128]
        stp     x18, x30, [sp, #144]
        bl      bst_irq_ack
        mrs     x1, tpidr_el1
        str     x0, [x1, #PCPU_IRQ]
        ldp     x18, x30, [sp, #144]
        ldp     x16, x17, [sp, #128]
        ldp     x14, x15, [sp, #112]
        ldp     x12, x13, [sp, #96]
        ldp     x10, x11, [sp, #80]
        ldp     x8, x9, [sp, #64]
        ldp     x6, x7, [sp, #48]
        ldp     x4, x5, [sp, #32]
        ldp     x2, x3, [sp, #16]
        ldp     x0, x1, [sp], #176
        eret

/* Any other exception from EL1 itself: a bug in the trusted base or the
 * compiler. x0 is the vector's number. */
ktrap_k:
        mrs     x1, elr_el1
        mrs     x2, far_el1
        mrs     x3, esr_el1
        bl      bst_kernel_fault
        b       park

/*
 * The trampoline page: the exception vectors and the code that moves between
 * the kernel's table and the user's. User code runs on a table of its own
 * (TTBR0) with none of the kernel's memory in it, so the one page both
 * must see is mapped in TTBR1 (platform.c, at TRAMP_VA). It is
 * position-independent: its only absolute addresses are the literals at the
 * end. TPIDRRO_EL0 holds the one general register it needs while it runs.
 */
        .equ    TRAMP_VA, 0xffff000000000000
        .equ    VECTORS_VA, TRAMP_VA
        .equ    TRAMP_ENTER_VA, TRAMP_VA + (tramp_enter - tramp_page)
        .section .tramp, "ax"
        .balign 4096
        .global tramp_page
tramp_page:
.macro  from_el0 lit
        msr     tpidrro_el0, x0
        ldr     x0, lit_kernel_root
        msr     ttbr0_el1, x0           /* ASID 0: the kernel's translations are tagged, the user's are not hit */
        isb
        ldr     x0, \lit
        br      x0
.endm
.macro  from_el1 lit
        msr     tpidrro_el0, x0
        ldr     x0, \lit
        br      x0
.endm
.macro  ktrap vec
        mov     x0, #\vec
        ldr     x1, lit_ktrap
        br      x1
.endm
        .balign 2048
vectors:
        .balign 128
        ktrap 0
        .balign 128
        ktrap 1
        .balign 128
        ktrap 2
        .balign 128
        ktrap 3
        .balign 128
        ktrap 4
        .balign 128
        from_el1 lit_el1_irq
        .balign 128
        ktrap 6
        .balign 128
        ktrap 7
        .balign 128
        from_el0 lit_el0_sync
        .balign 128
        from_el0 lit_el0_irq
        .balign 128
        ktrap 10
        .balign 128
        ktrap 11
        .balign 128
        ktrap 12
        .balign 128
        ktrap 13
        .balign 128
        ktrap 14
        .balign 128
        ktrap 15

/* Enter user mode: the kernel's table is still loaded and the user's
 * context is in the registers, but x0 (in TPIDRRO_EL0) and a scratch. */
        .balign 16
tramp_enter:
        mrs     x0, tpidr_el1
        ldr     x0, [x0, #PCPU_TTBR]
        dsb     ish                     /* the table's entries are visible to the walker */
        msr     ttbr0_el1, x0           /* the user's ASID: nothing to invalidate */
        isb
        mrs     x0, tpidrro_el0
        eret

        .balign 8
lit_kernel_root: .quad bst_kernel_root
lit_el0_sync:   .quad el0_sync_k
lit_el0_irq:    .quad el0_irq_k
lit_el1_irq:    .quad el1_irq_k
lit_ktrap:      .quad ktrap_k
        .text

        .global bst_platform_ctx
bst_platform_ctx:
        mrs     x0, tpidr_el1
        ret

        .bss
        .balign 4096
        .global bst_kernel_root
bst_kernel_root: .skip 4096
        .global kernel_l1
kernel_l1:      .skip 4096

