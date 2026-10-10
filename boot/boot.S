/*
 * BastiOS boot and trap entry.
 *
 * Multiboot 1 or 2 entry in 32-bit protected mode, switch to 64-bit long mode,
 * set up the descriptor tables (platform.c), call the Bastio kernel
 * (`bst_kmain`), then power off. Also the only code that crosses between
 * user mode and the kernel: `bst_cpu_enter`, the trap stubs, `bst_cpu_wait`.
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

.section .bss
.align 4096
.global bst_kernel_pml4
bst_kernel_pml4:
pml4:   .skip 4096
pdpt:   .skip 4096
hipdpt: .skip 63 * 4096               /* 512 GiB - 2^46, right after pdpt */
pd:     .skip 4096
devpd:  .skip 8192                    /* 2-3 GiB, then 3-4 GiB */
.align 16
stack_bottom:
        .skip 4096
stack_top:
/* The boot stack serves only until long mode: then each processor runs on
 * its own block of bst_cores (core.h). */
.align 8
mb_info:        .skip 8
mb_magic:       .skip 8

.section .data
.align 16
.global bst_gdt
bst_gdt:
        .quad 0                       /* null */
        .quad 0x00AF9A000000FFFF      /* 0x08: 64-bit code, ring 0 */
        .quad 0x00CF92000000FFFF      /* 0x10: data, ring 0 */
        .quad 0x00CFF2000000FFFF      /* 0x18: data, ring 3 */
        .quad 0x00AFFA000000FFFF      /* 0x20: 64-bit code, ring 3 */
        .fill 2 * MAX_CPUS, 8, 0      /* 0x28 + 16 i: processor i's TSS (smp.c) */
bst_gdt_end:

.global bst_gdt_ptr
bst_gdt_ptr:
        .word bst_gdt_end - bst_gdt - 1
        .quad bst_gdt

.section .text
.code32
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

        /* The device window: 2-4 GiB, where the local APIC, I/O APIC, HPET
         * and PCI devices (a UEFI framebuffer usually sits at 2 GiB) live,
         * identity-mapped uncached (PWT | PCD) with 2 MiB pages, kernel-only.
         * Bastio code reaches it only through claimed `Mmio` blocks; nothing
         * else in the kernel uses these addresses, and the page pool stays
         * below 1 GiB. */
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
        or      $0x101, %eax          /* LME | SCE (the `syscall` instruction) */
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
        mov     $(bst_cores + CORE_KSTACK), %rsp

        mov     mb_magic, %edi        /* Multiboot 2 information to the form */
        mov     mb_info, %esi         /* the prelude reads (boot/multiboot.c) */
        call    bst_multiboot_info
        mov     %eax, mb_info
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
 * and the arithmetic flags are taken from it.
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
        lea     CORE_BLOCK(%rsi), %rsp          /* its trap stack, empty */
        mov     $0x00007FFFFFFFFFFF, %rcx /* non-canonical addresses would fault in ring 0 */

        push    $UDATA                  /* ss */
        mov     16*8(%rsi), %rax        /* rsp */
        cmp     %rcx, %rax
        jbe     1f
        xor     %eax, %eax
1:      push    %rax
        mov     17*8(%rsi), %rax        /* rflags: CF PF AF ZF SF DF OF only, plus IF */
        and     $0xCD5, %rax
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
3:      mov     %rax, %rdx
        shr     $32, %rdx
        mov     $0xC0000100, %ecx       /* FS_BASE */
        wrmsr

        mov     $UDATA, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     0*8(%rsi), %rax
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
        iretq

/* uint64_t bst_cpu_wait(void): sleep until an interrupt; return its vector. */
.global bst_cpu_wait
bst_cpu_wait:
        mov     %rsp, %rdx
        and     $-CORE_BLOCK, %rdx
        movq    $0, CORE_IDLE(%rdx)
        sti
        hlt
        cli
        mov     CORE_IDLE(%rdx), %rax
        ret

/*
 * Entry for the `syscall` instruction (Linux system calls). The CPU leaves
 * the user rip in rcx and rflags in r11, masks IF, DF, TF and AC (see
 * bst_platform_init) and does not switch stacks, so this moves to the trap
 * stack (empty while user code runs), builds the frame an interrupt from
 * ring 3 would have pushed, and joins trap_common as vector 0x81. No `int`
 * can raise 0x81 from ring 3 (its gate has DPL 0), so the vector means
 * `syscall`. The stack is this processor's trap stack, found through
 * KERNEL_GS_BASE: `swapgs` makes %gs point at its block for the few
 * instructions that need it, and the second `swapgs` gives the user's
 * %gs base back before anything can trap. The return to user mode is the usual
 * iretq in bst_cpu_enter, which sanitizes everything it restores.
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
        jmp     trap_common

/*
 * Trap stubs. Each pushes a dummy error code where the CPU pushes none, then
 * its vector, and joins trap_common.
 */
.macro stub n
        .align 16
stub_\n:
        .if (\n == 8) || (\n >= 10 && \n <= 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
        .else
        push    $0
        .endif
        push    $\n
        jmp     trap_common
.endm

.altmacro
.set i, 0
.rept 256
        stub %i
        .set i, i + 1
.endr

.macro stubaddr n
        .quad stub_\n
.endm
.section .rodata
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
 * Stack at trap_common, from %rsp after the pushes below:
 *   0 r15, 8 r14, 16 r13, 24 r12, 32 r11, 40 r10, 48 r9, 56 r8, 64 rbp,
 *   72 rdi, 80 rsi, 88 rdx, 96 rcx, 104 rbx, 112 rax, 120 vector, 128 error,
 *   136 rip, 144 cs, 152 rflags, 160 rsp, 168 ss
 */
trap_common:
        push    %rax
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
        mov     $0xC0000100, %ecx       /* FS_BASE */
        rdmsr
        shl     $32, %rdx
        or      %rdx, %rax;      mov %rax, 20*8(%rdi)
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
        /* The kernel runs with interrupts off except inside bst_cpu_wait, so
         * this is either the interrupt that wakes it, or a fault: a bug in the
         * trusted base or the compiler. */
        mov     120(%rsp), %rdi
        cmp     $32, %rdi
        jb      kernel_fault
        mov     %rsp, %rax
        and     $-CORE_BLOCK, %rax
        mov     %rdi, CORE_IDLE(%rax)
        /* Return with interrupts off. bst_cpu_wait reports one vector; a
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
