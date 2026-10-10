/*
 * BastiOS boot and trap entry.
 *
 * Multiboot 1 entry in 32-bit protected mode, switch to 64-bit long mode,
 * set up the descriptor tables (platform.c), call the Bastio kernel
 * (`bst_kmain`), then power off. Also the only code that crosses between
 * user mode and the kernel: `bst_cpu_enter`, the trap stubs, `bst_cpu_wait`.
 *
 * Part of the trusted base (see README). Bastio code reaches it only through
 * the `Cpu` capability's builtins.
 */

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000003          /* page-align modules, give memory info */
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

.section .bss
.align 4096
.global bst_kernel_pml4
bst_kernel_pml4:
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4096
.align 16
stack_bottom:
        .skip 65536
stack_top:
/* User-mode traps land here (TSS.rsp0); the stub moves to the kernel stack. */
trap_stack:
        .skip 16384
.global bst_trap_stack_top
bst_trap_stack_top:
.align 8
mb_info:        .skip 8
kernel_rsp:     .skip 8
user_rsp:       .skip 8
.global bst_idle_vector
bst_idle_vector: .skip 8
.global bst_cpu_ctx
bst_cpu_ctx:    .skip 20 * 8

.section .data
.align 16
.global bst_gdt
bst_gdt:
        .quad 0                       /* null */
        .quad 0x00AF9A000000FFFF      /* 0x08: 64-bit code, ring 0 */
        .quad 0x00CF92000000FFFF      /* 0x10: data, ring 0 */
        .quad 0x00CFF2000000FFFF      /* 0x18: data, ring 3 */
        .quad 0x00AFFA000000FFFF      /* 0x20: 64-bit code, ring 3 */
        .quad 0, 0                    /* 0x28: TSS (filled by platform.c) */
bst_gdt_end:
gdt_ptr:
        .word bst_gdt_end - bst_gdt - 1
        .quad bst_gdt

.section .text
.code32
.global _start
_start:
        cli
        mov     $stack_top, %esp
        mov     %ebx, mb_info

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

        mov     $pml4, %eax
        mov     %eax, %cr3
        mov     %cr4, %eax
        or      $0x20, %eax           /* PAE */
        mov     %eax, %cr4
        mov     $0xC0000080, %ecx     /* EFER */
        rdmsr
        or      $0x101, %eax          /* LME | SCE (the `syscall` instruction) */
        wrmsr
        mov     %cr0, %eax
        or      $0x80010001, %eax     /* PG | WP | PE: the kernel cannot write read-only pages either */
        mov     %eax, %cr0

        lgdt    gdt_ptr
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
        mov     $stack_top, %rsp

        call    bst_platform_init
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
 * uint64_t bst_cpu_enter(void): run the user context in bst_cpu_ctx until the
 * next trap, then return its vector with the context saved back.
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
        mov     %rsp, kernel_rsp
        mov     $bst_trap_stack_top, %rsp
        mov     $bst_cpu_ctx, %rsi
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
        movq    $0, bst_idle_vector
        sti
        hlt
        cli
        mov     bst_idle_vector, %rax
        ret

/*
 * Entry for the `syscall` instruction (Linux system calls). The CPU leaves
 * the user rip in rcx and rflags in r11, masks IF, DF, TF and AC (see
 * bst_platform_init) and does not switch stacks, so this moves to the trap
 * stack (empty while user code runs), builds the frame an interrupt from
 * ring 3 would have pushed, and joins trap_common as vector 0x81. No `int`
 * can raise 0x81 from ring 3 (its gate has DPL 0), so the vector means
 * `syscall`. With interrupts masked and one CPU, user_rsp cannot be
 * overwritten before it is pushed. The return to user mode is the usual
 * iretq in bst_cpu_enter, which sanitizes everything it restores.
 */
.global bst_syscall_entry
bst_syscall_entry:
        mov     %rsp, user_rsp
        mov     $bst_trap_stack_top, %rsp
        push    $UDATA                  /* ss */
        push    user_rsp                /* rsp */
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
        call    eoi
        testb   $3, 144(%rsp)
        jz      from_kernel

        /* From user mode: save the context and return from bst_cpu_enter. */
        mov     $bst_cpu_ctx, %rdi
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
        mov     120(%rsp), %rax
        mov     kernel_rsp, %rsp
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
        mov     %rdi, bst_idle_vector
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

/* Acknowledge a hardware interrupt (vectors 32-47) at the 8259 PICs. */
eoi:
        cmp     $32, %rdi
        jb      1f
        cmp     $48, %rdi
        jae     1f
        mov     $0x20, %al
        cmp     $40, %rdi
        jb      2f
        out     %al, $0xA0
2:      out     %al, $0x20
1:      ret

        .section .note.GNU-stack,"",@progbits
