/*
 * BastiOS boot and trap entry.
 *
 * Multiboot 1 or 2, or PVH, entry in 32-bit protected mode, switch to 64-bit long mode,
 * set up the descriptor tables (platform.c), call the Bastio kernel
 * (`bst_kmain`), then power off. Also the only code that crosses between
 * user mode and the kernel: `bst_cpu_enter`, the trap stubs, `bst_cpu_halt`.
 *
 * Part of the trusted base (see README). Bastio code reaches it only through
 * the `Cpu` capability's builtins.
 */

#include "core.h"

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000007          /* page-align modules, give memory info, set a video mode */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.set KCODE, 0x08
.set KDATA, 0x10
.set UDATA, 0x1B                      /* 0x18 | RPL 3 */
.set UCODE, 0x23                      /* 0x20 | RPL 3 */

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM
.long 0, 0, 0, 0, 0                   /* load addresses: unused (flag 16 is off) */
.long 0                               /* a linear framebuffer, preferably */
.long 1024, 768, 32                   /* 1024x768, 32 bits per pixel */

/* The Multiboot 2 header, which Limine uses (boot/limine.conf): it passes the
 * ACPI RSDP as well, and boot/multiboot.c turns its information into the
 * Multiboot 1 form above. */
.set MB2_MAGIC,   0xE85250D6
.align 8
mb2_header:
.long MB2_MAGIC
.long 0                               /* i386 (32-bit protected mode) */
.long mb2_end - mb2_header
.long -(MB2_MAGIC + (mb2_end - mb2_header))
.align 8
.word 5, 1                            /* framebuffer, optional: */
.long 20
.long 1024, 768, 32                   /* 1024x768, 32 bits per pixel */
.align 8
.word 0, 0                            /* end */
.long 8
mb2_end:

/* The PVH entry (Xen's HVM direct boot, which Firecracker, Cloud Hypervisor
 * and QEMU's microvm machine use): an ELF note names a 32-bit entry point.
 * The loader enters in 32-bit protected mode with paging off and %ebx
 * pointing to an hvm_start_info; pvh_start tags it with a magic of our own
 * in %eax and joins the Multiboot path, where boot/multiboot.c converts
 * the information. */
.set PVH_TAG, 0x31485650              /* "PVH1" */
.section .note.pvh, "a", @note
.align 4
.long 4                               /* name size: "Xen\0" */
.long 4                               /* descriptor size: one 32-bit address */
.long 18                              /* XEN_ELFNOTE_PHYS32_ENTRY */
.asciz "Xen"
.long pvh_start

.section .bss
.align 4096
.global bst_kernel_pml4
bst_kernel_pml4:
pml4:   .skip 4096
pdpt:   .skip 4096
hipdpt: .skip 63 * 4096               /* 512 GiB - 2^46, right after pdpt */
pd:     .skip 4096
fwpd:   .skip 4096                    /* 1-2 GiB */
devpd:  .skip 8192                    /* 2-3 GiB, then 3-4 GiB */
.global bst_shadow_pml4
bst_shadow_pml4: .skip 4096 * MAX_CPUS /* each processor's table while it runs user code */
.align 16
stack_bottom:
        .skip 4096
stack_top:
/* The boot stack serves only until long mode: then each processor runs on
 * its own block (core.h). */
.align 8
mb_info:        .skip 8
mb_magic:       .skip 8

.section .data
.align 4096                           /* mapped on its own in the entry area (core.h) */
.global bst_gdt
bst_gdt:
        .quad 0                       /* null */
        .quad 0x00AF9A000000FFFF      /* 0x08: 64-bit code, ring 0 */
        .quad 0x00CF92000000FFFF      /* 0x10: data, ring 0 */
        .quad 0x00CFF2000000FFFF      /* 0x18: data, ring 3 */
        .quad 0x00AFFA000000FFFF      /* 0x20: 64-bit code, ring 3 */
        .fill 2 * MAX_CPUS, 8, 0      /* 0x28 + 16 i: processor i's TSS (smp.c) */
bst_gdt_end:
.align 4096

.global bst_gdt_ptr
bst_gdt_ptr:
        .word bst_gdt_end - bst_gdt - 1
        .quad bst_gdt

.section .text
.code32
pvh_start:
        mov     $PVH_TAG, %eax
        jmp     _start

.global _start
_start:
        cli
        mov     $stack_top, %esp
        mov     %ebx, mb_info
        mov     %eax, mb_magic        /* which Multiboot */

        /* Identity-map the first 1 GiB with 2 MiB pages, kernel-only (no U bit). */
        mov     $pdpt, %eax
        or      $3, %eax
        mov     %eax, pml4
        mov     $pd, %eax
        or      $3, %eax
        mov     %eax, pdpt
        xor     %ecx, %ecx
1:      mov     %ecx, %eax
        shl     $21, %eax
        or      $0x83, %eax           /* present | writable | huge */
        mov     %eax, pd(,%ecx,8)
        movl    $0, pd+4(,%ecx,8)
        inc     %ecx
        cmp     $512, %ecx
        jne     1b

        /* The second GiB, identity-mapped (2 MiB pages, kernel-only) for
         * one reader: boot/smp.c, which finds the ACPI tables there on
         * machines with 2 GiB or more below 4 GiB (firmware puts them at
         * the top of that memory). The page pool may use the RAM here too
         * (it spans every run of RAM), and Bastio's `mmio_claim` starts at
         * 2 GiB. */
        mov     $fwpd, %eax
        or      $3, %eax
        mov     %eax, pdpt+8
        xor     %ecx, %ecx
1:      mov     %ecx, %eax
        shl     $21, %eax
        add     $0x40000000, %eax
        or      $0x83, %eax           /* present | writable | huge */
        mov     %eax, fwpd(,%ecx,8)
        movl    $0, fwpd+4(,%ecx,8)
        inc     %ecx
        cmp     $512, %ecx
        jne     1b

        /* The device window: 2-4 GiB, where the local APIC, I/O APIC, HPET
         * and PCI devices (a UEFI framebuffer usually sits at 2 GiB) live,
         * identity-mapped uncached (PWT | PCD) with 2 MiB pages, kernel-only.
         * Bastio code reaches it only through claimed `Mmio` blocks. Where
         * the memory map says it is RAM instead, bst_map_ram (multiboot.c)
         * maps it write-back for the page pool, and mmio_claim refuses it. */
        mov     $devpd, %eax
        or      $3, %eax
        mov     %eax, pdpt+16
        add     $4096, %eax
        mov     %eax, pdpt+24
        xor     %ecx, %ecx
1:      mov     %ecx, %eax
        shl     $21, %eax
        add     $0x80000000, %eax
        or      $0x9B, %eax           /* present | writable | PWT | PCD | huge */
        mov     %eax, devpd(,%ecx,8)
        movl    $0, devpd+4(,%ecx,8)
        inc     %ecx
        cmp     $1024, %ecx
        jne     1b

        /* The rest of the device window, 4 GiB up to 2^46, where firmware
         * puts 64-bit PCI BARs (UEFI's often sit at 768 GiB): identity-mapped
         * uncached with 1 GiB pages, kernel-only. Entry n of the run that
         * starts at pdpt maps GiB n, so pdpt holds GiBs 4-511 and hipdpt the
         * 63 tables after it, which pml4 entries 1-63 point at. A user
         * address space shares only pml4 entry 0 (its own window is at
         * 512 GiB), and the kernel switches back to its own tables before
         * running Bastio code, so a claimed Mmio up here is always mapped.
         * A processor without 1 GiB pages gets none of this: an access
         * above 4 GiB faults instead. */
        mov     $0x80000000, %eax
        cpuid
        cmp     $0x80000001, %eax
        jb      3f
        mov     $0x80000001, %eax
        cpuid
        bt      $26, %edx             /* Page1GB */
        jnc     3f
        mov     $hipdpt, %eax
        or      $3, %eax
        mov     $1, %ecx
1:      mov     %eax, pml4(,%ecx,8)
        movl    $0, pml4+4(,%ecx,8)
        add     $4096, %eax
        inc     %ecx
        cmp     $64, %ecx
        jne     1b
        mov     $4, %ecx
2:      mov     %ecx, %eax
        shl     $30, %eax
        or      $0x9B, %eax           /* present | writable | PWT | PCD | huge */
        mov     %eax, pdpt(,%ecx,8)
        mov     %ecx, %eax
        shr     $2, %eax
        mov     %eax, pdpt+4(,%ecx,8)
        inc     %ecx
        cmp     $32768, %ecx
        jne     2b
3:
        mov     $pml4, %eax
        mov     %eax, %cr3
        mov     %cr4, %eax
        or      $0x620, %eax          /* PAE | OSFXSR | OSXMMEXCPT: SSE for user code */
        mov     %eax, %cr4
        mov     $0xC0000080, %ecx     /* EFER */
        rdmsr
        or      $0x901, %eax          /* LME | SCE (the `syscall` instruction) | NXE (user pages the program may write are not executable) */
        wrmsr
        mov     %cr0, %eax
        and     $0xFFFFFFFB, %eax     /* clear EM: x87/SSE run natively */
        or      $0x80010003, %eax     /* PG | WP | MP | PE: the kernel cannot write read-only pages either */
        mov     %eax, %cr0

        lgdt    bst_gdt_ptr
        ljmp    $KCODE, $long_mode

.code64
long_mode:
        mov     $KDATA, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss
        xor     %ax, %ax
        mov     %ax, %fs
        mov     %ax, %gs
        mov     $(bst_core0 + CORE_KSTACK), %rsp

        mov     mb_magic, %edi        /* Multiboot 2 information to the form */
        mov     mb_info, %esi         /* the prelude reads (boot/multiboot.c) */
        call    bst_multiboot_info
        mov     %eax, mb_info
        call    bst_map_ram           /* RAM above 2 GiB write-back (boot/multiboot.c) */
        call    bst_platform_init
        call    bst_smp_init
        mov     mb_info, %edi
        call    bst_kmain

        /* Power off. Under QEMU with isa-debug-exit at 0xf4 this exits with
         * status (0x10 << 1) | 1 = 33; elsewhere we just halt. */
        mov     $0x10, %al
        out     %al, $0xf4
2:      cli
        hlt
        jmp     2b

/*
 * uint64_t bst_cpu_enter(void): run this processor's saved user context until
 * the next trap, then return its vector with the context saved back.
 * Whatever Bastio put in the context, the CPU enters ring 3 with the user
 * segments, interrupts on and IOPL 0: only rip, rsp, the general registers
 * and the arithmetic flags (and the trap flag) are taken from it.
 *
 * The process's top-level table (the prelude left it in CORE_UROOT) is not
 * what the CPU loads: this processor's shadow table has the process's window
 * (slot 1 of its table) in slot 0, at virtual address 0, and the entry area
 * (core.h) in the last slot, and nothing else. The kernel's memory stays out
 * of the user's address space. The frame for `iretq` goes on the trap stack
 * at its entry-area address, and the last instructions run in the entry area
 * too (bst_enter_tail): they load CR3 and leave.
 */
.global bst_cpu_enter
bst_cpu_enter:
        push    %rbx
        push    %rbp
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        mov     %rsp, %rsi
        and     $-CORE_BLOCK, %rsi              /* this processor's block */
        mov     %rsp, CORE_KRSP(%rsi)

        /* Once per processor: what a trap loads into CR3 (CORE_KCR3). With
         * PCIDs on (bst_pcid_on, platform.c) CR4.PCIDE comes on here, while CR3
         * still has PCID 0, the kernel's table is PCID 1 and loaded without
         * flushing (it never changes once the processors run: a trap no longer
         * empties the TLB of the kernel's translations), and the shadow table
         * is PCID 2, loaded with a flush every time (its slot 0 changes with
         * the process, and the process's table with every munmap and mprotect). */
        cmpq    $0, CORE_KCR3(%rsi)
        jne     4f
        cmpq    $0, bst_fsgsbase(%rip)
        je      6f
        mov     %cr4, %rdx
        bts     $16, %rdx                       /* FSGSBASE: the trap path uses rdfsbase and wrfsbase */
        mov     %rdx, %cr4
6:
        movabs  $bst_kernel_pml4, %rax
        cmpq    $0, bst_pcid_on(%rip)
        je      5f
        mov     %cr4, %rdx
        bts     $17, %rdx                       /* PCIDE */
        mov     %rdx, %cr4
        or      $1, %rax
        bts     $63, %rax                       /* do not flush */
        movq    $2, CORE_UPCID(%rsi)
5:      mov     %rax, CORE_KCR3(%rsi)
4:
        /* The shadow table: slot 0 is the window's level-3 table. */
        mov     CORE_UROOT(%rsi), %rax
        mov     CORE_INDEX(%rsi), %rdx
        shl     $12, %rdx
        movabs  $bst_shadow_pml4, %rcx
        add     %rcx, %rdx
        mov     %rdx, %rcx
        or      CORE_UPCID(%rsi), %rcx
        mov     %rcx, CORE_CR3(%rsi)
        mov     8(%rax), %rax                   /* slot 1 of the process's table */
        mov     %rax, (%rdx)
        movq    $1, CORE_UMODE(%rsi)

        mov     CORE_ALIAS(%rsi), %rax
        lea     CORE_BLOCK(%rax), %rsp          /* its trap stack, empty, as the entry area sees it */
        mov     $0x00007FFFFFFFFFFF, %rcx /* non-canonical addresses would fault in ring 0 */

        push    0*8(%rsi)               /* the user's rax, for bst_enter_tail */
        push    $UDATA                  /* ss */
        mov     16*8(%rsi), %rax        /* rsp */
        cmp     %rcx, %rax
        jbe     1f
        xor     %eax, %eax
1:      push    %rax
        mov     17*8(%rsi), %rax        /* rflags: CF PF AF ZF SF DF OF and TF only, plus IF */
        and     $0xDD5, %rax
        or      $0x202, %rax
        push    %rax
        push    $UCODE                  /* cs */
        mov     15*8(%rsi), %rax        /* rip */
        cmp     %rcx, %rax
        jbe     2f
        xor     %eax, %eax
2:      push    %rax

        /* FS base (word 20), for the user's thread pointer. */
        mov     20*8(%rsi), %rax
        cmp     %rcx, %rax
        jbe     3f
        xor     %eax, %eax
3:      cmpq    $0, bst_fsgsbase(%rip)
        je      7f
        wrfsbase %rax
        jmp     8f
7:      mov     %rax, %rdx
        shr     $32, %rdx
        mov     $0xC0000100, %ecx       /* FS_BASE */
        wrmsr
8:

        mov     $UDATA, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     1*8(%rsi), %rbx
        mov     2*8(%rsi), %rcx
        mov     3*8(%rsi), %rdx
        mov     5*8(%rsi), %rdi
        mov     6*8(%rsi), %rbp
        mov     7*8(%rsi), %r8
        mov     8*8(%rsi), %r9
        mov     9*8(%rsi), %r10
        mov     10*8(%rsi), %r11
        mov     11*8(%rsi), %r12
        mov     12*8(%rsi), %r13
        mov     13*8(%rsi), %r14
        mov     14*8(%rsi), %r15
        mov     4*8(%rsi), %rsi
        jmp     *enter_tail_ptr(%rip)           /* the rest runs in the entry area */

/* uint64_t *bst_platform_ctx(void): this processor's saved user context, the
 * start of its block. It touches only %rax, so the native backend's cpu_get and
 * cpu_set (a call each, for every word of a context switch) need no frame. */
.if CORE_CTX != 0
.error "bst_platform_ctx_mask needs the saved context at the start of the block"
.endif
.global bst_platform_ctx_mask
.set bst_platform_ctx_mask, -CORE_BLOCK     /* the native backend's inline cpu_get and cpu_set: %rsp & mask is the context */
.global bst_platform_ctx
bst_platform_ctx:
        mov     %rsp, %rax
        and     $-CORE_BLOCK, %rax
        add     $CORE_CTX, %rax
        ret

/* uint64_t bst_cpu_halt(void): sleep until an interrupt; return its vector.
 * The kernel's cpu_idle reaches it through bst_cpu_wait (smp.c). */
.global bst_cpu_halt
bst_cpu_halt:
        mov     %rsp, %rdx
        and     $-CORE_BLOCK, %rdx
        movq    $0, CORE_IDLE(%rdx)
        sti
        hlt
        cli
        mov     CORE_IDLE(%rdx), %rax
        ret

/*
 * The entry area's code (core.h): everything that runs while a user table is
 * in CR3. Position independent, mapped at ALIAS_BASE by smp.c.
 */
.section .tramp.text, "ax"
.balign 4096
.global bst_tramp_start
bst_tramp_start:

/* The end of bst_cpu_enter: CR3 takes the shadow table, and `iretq` takes
 * the frame bst_cpu_enter built. All registers but rax are the user's; the
 * user's rax is on the stack above the frame. */
bst_enter_tail:
        mov     %rsp, %rax
        and     $-CORE_BLOCK, %rax
        mov     CORE_CR3(%rax), %rax
        mov     %rax, %cr3
        mov     5*8(%rsp), %rax
        iretq

/*
 * Entry for the `syscall` instruction (Linux system calls). The CPU leaves
 * the user rip in rcx and rflags in r11, masks IF, DF, TF and AC (see
 * bst_platform_init) and does not switch stacks, so this moves to the trap
 * stack (empty while user code runs), builds the frame an interrupt from
 * ring 3 would have pushed, and joins trap_alias as vector 0x81. No `int`
 * can raise 0x81 from ring 3 (its gate has DPL 0), so the vector means
 * `syscall`. The stack is this processor's trap stack, found through
 * KERNEL_GS_BASE (the block's address in the entry area): `swapgs` makes %gs
 * point at it for the few instructions that need it, and the second `swapgs`
 * gives the user's %gs base back before anything can trap. The return to user
 * mode is the usual iretq in bst_enter_tail, which sanitizes everything it
 * restores.
 */
.global bst_syscall_entry
bst_syscall_entry:
        swapgs
        mov     %rsp, %gs:CORE_URSP
        mov     %gs:CORE_TRAP_TOP, %rsp
        push    $UDATA                  /* ss */
        pushq   %gs:CORE_URSP           /* rsp */
        swapgs
        push    %r11                    /* rflags */
        push    $UCODE                  /* cs */
        push    %rcx                    /* rip */
        push    $0                      /* error code */
        push    $0x81                   /* vector */
        jmp     trap_alias

/*
 * Every trap joins here, in the entry area, with the CPU's frame, the error
 * code and the vector on the stack. If the trap came from user mode, CR3
 * holds a shadow table: the kernel's replaces it before anything else runs
 * (CORE_UMODE says so, and a trap from the kernel finds it clear), and
 * trap_common, in the kernel's own memory, takes over with rax saved.
 */
trap_alias:
        push    %rax
        mov     %rsp, %rax
        and     $-CORE_BLOCK, %rax
        cmpq    $0, CORE_UMODE(%rax)
        je      1f
        movq    $0, CORE_UMODE(%rax)
        mov     CORE_KCR3(%rax), %rax
        mov     %rax, %cr3
1:      movabs  $trap_common, %rax
        jmp     *%rax

/*
 * Trap stubs. Each pushes a dummy error code where the CPU pushes none, then
 * its vector, and joins trap_alias.
 */
.macro stub n
        .align 16
stub_\n:
        .if (\n == 8) || (\n >= 10 && \n <= 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
        .else
        push    $0
        .endif
        push    $\n
        jmp     trap_alias
.endm

.altmacro
.set i, 0
.rept 256
        stub %i
        .set i, i + 1
.endr

.balign 4096
.global bst_tramp_end
bst_tramp_end:

.macro stubaddr n
        .quad stub_\n
.endm
.section .rodata
.align 8
enter_tail_ptr:
        .quad ALIAS_BASE + (bst_enter_tail - bst_tramp_start)
.align 8
.global bst_trap_stubs
bst_trap_stubs:
.set i, 0
.rept 256
        stubaddr %i
        .set i, i + 1
.endr
.noaltmacro

.section .text
/*
 * Stack at trap_common (rax was pushed by trap_alias), from %rsp after the pushes below:
 *   0 r15, 8 r14, 16 r13, 24 r12, 32 r11, 40 r10, 48 r9, 56 r8, 64 rbp,
 *   72 rdi, 80 rsi, 88 rdx, 96 rcx, 104 rbx, 112 rax, 120 vector, 128 error,
 *   136 rip, 144 cs, 152 rflags, 160 rsp, 168 ss
 */
trap_common:
        push    %rbx
        push    %rcx
        push    %rdx
        push    %rsi
        push    %rdi
        push    %rbp
        push    %r8
        push    %r9
        push    %r10
        push    %r11
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        /* An interrupt or exception gate leaves RFLAGS.DF as it was, and a
         * user program may be stopped inside a backward copy (musl's memmove
         * runs std; rep movsq; cld). The C code below, like every SysV
         * function, assumes DF clear: gcc's rep stos/movs for struct copies
         * and fills, and bst_copy_fwd, would run backwards over the memory
         * below their target. iretq gives the user its own DF back from the
         * saved RFLAGS. (syscall clears DF through FMASK already.) */
        cld
        mov     $KDATA, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     120(%rsp), %rdi
        call    bst_lapic_ack           /* smp.c: end-of-interrupt for local APIC vectors */
        testb   $3, 144(%rsp)
        jz      from_kernel

        /* From user mode: save the context and return from bst_cpu_enter. */
        mov     %rsp, %rdi
        and     $-CORE_BLOCK, %rdi      /* the context is at the bottom of the block */
        mov     112(%rsp), %rax; mov %rax, 0*8(%rdi)
        mov     104(%rsp), %rax; mov %rax, 1*8(%rdi)
        mov     96(%rsp), %rax;  mov %rax, 2*8(%rdi)
        mov     88(%rsp), %rax;  mov %rax, 3*8(%rdi)
        mov     80(%rsp), %rax;  mov %rax, 4*8(%rdi)
        mov     72(%rsp), %rax;  mov %rax, 5*8(%rdi)
        mov     64(%rsp), %rax;  mov %rax, 6*8(%rdi)
        mov     56(%rsp), %rax;  mov %rax, 7*8(%rdi)
        mov     48(%rsp), %rax;  mov %rax, 8*8(%rdi)
        mov     40(%rsp), %rax;  mov %rax, 9*8(%rdi)
        mov     32(%rsp), %rax;  mov %rax, 10*8(%rdi)
        mov     24(%rsp), %rax;  mov %rax, 11*8(%rdi)
        mov     16(%rsp), %rax;  mov %rax, 12*8(%rdi)
        mov     8(%rsp), %rax;   mov %rax, 13*8(%rdi)
        mov     0(%rsp), %rax;   mov %rax, 14*8(%rdi)
        mov     136(%rsp), %rax; mov %rax, 15*8(%rdi)
        mov     160(%rsp), %rax; mov %rax, 16*8(%rdi)
        mov     152(%rsp), %rax; mov %rax, 17*8(%rdi)
        mov     128(%rsp), %rax; mov %rax, 18*8(%rdi)
        mov     %cr2, %rax;      mov %rax, 19*8(%rdi)
        cmpq    $0, bst_fsgsbase(%rip)
        je      9f
        rdfsbase %rax
        jmp     10f
9:      mov     $0xC0000100, %ecx       /* FS_BASE */
        rdmsr
        shl     $32, %rdx
        or      %rdx, %rax
10:     mov     %rax, 20*8(%rdi)
        mov     120(%rsp), %rax
        mov     CORE_KRSP(%rdi), %rsp
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %rbp
        pop     %rbx
        ret

from_kernel:
        /* The kernel runs with interrupts off except inside bst_cpu_halt, so
         * this is either the interrupt that wakes it, or a fault: a bug in the
         * trusted base or the compiler. */
        mov     120(%rsp), %rdi
        cmp     $32, %rdi
        jb      kernel_fault
        mov     %rsp, %rax
        and     $-CORE_BLOCK, %rax
        mov     %rdi, CORE_IDLE(%rax)
        /* Return with interrupts off. bst_cpu_halt reports one vector; a
         * second interrupt taken between its `hlt` and `cli` would overwrite
         * it, and an 8259 interrupt lost that way is never acknowledged,
         * which blocks its line and every lower-priority one for good. */
        andq    $~0x200, 152(%rsp)      /* IF in the saved rflags */
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %r11
        pop     %r10
        pop     %r9
        pop     %r8
        pop     %rbp
        pop     %rdi
        pop     %rsi
        pop     %rdx
        pop     %rcx
        pop     %rbx
        pop     %rax
        add     $16, %rsp
        iretq

kernel_fault:
        mov     136(%rsp), %rsi
        mov     %cr2, %rdx
        mov     128(%rsp), %rcx
        call    bst_kernel_fault
3:      cli
        hlt
        jmp     3b

        .section .note.GNU-stack,"",@progbits
