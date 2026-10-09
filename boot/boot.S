/*
 * BastiOS boot: Multiboot 1 entry in 32-bit protected mode, switch to
 * 64-bit long mode, call the Bastio kernel (`bst_kmain`), then power off.
 *
 * This file is part of the trusted base (see README): it is the only
 * hand-written assembly in BastiOS.
 */

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000003          /* page-align modules, give memory info */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .bss
.align 4096
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4096
.align 16
stack_bottom:
        .skip 65536
stack_top:

.section .rodata
.align 8
gdt64:
        .quad 0                       /* null */
        .quad 0x00AF9A000000FFFF      /* 0x08: 64-bit code, ring 0 */
        .quad 0x00CF92000000FFFF      /* 0x10: data, ring 0 */
gdt64_end:
gdt64_ptr:
        .word gdt64_end - gdt64 - 1
        .quad gdt64

.section .text
.code32
.global _start
_start:
        cli
        mov     $stack_top, %esp

        /* Identity-map the first 1 GiB with 2 MiB pages. */
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
        or      $0x100, %eax          /* LME */
        wrmsr
        mov     %cr0, %eax
        or      $0x80000001, %eax     /* PG | PE */
        mov     %eax, %cr0

        lgdt    gdt64_ptr
        ljmp    $0x08, $long_mode

.code64
long_mode:
        mov     $0x10, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss
        mov     %ax, %fs
        mov     %ax, %gs
        mov     $stack_top, %rsp
        and     $-16, %rsp

        call    bst_kmain

        /* Power off. Under QEMU with isa-debug-exit at 0xf4 this exits with
         * status (0x10 << 1) | 1 = 33; elsewhere we just halt. */
        mov     $0x10, %al
        out     %al, $0xf4
2:      cli
        hlt
        jmp     2b

        .section .note.GNU-stack,"",@progbits
