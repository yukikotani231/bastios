/*
 * Application processor (AP) startup. Part of the trusted base.
 *
 * bst_smp_init (smp.c) copies the trampoline below to physical address
 * AP_TRAMPOLINE and points each AP at it with a startup IPI. The AP starts
 * in 16-bit real mode at that address, switches to protected mode, turns on
 * long mode with the kernel's own page tables (bst_kernel_pml4), loads the
 * kernel's GDT and IDT, takes the stack the BSP left in bst_ap_stack, checks
 * in by incrementing bst_cpus_online, and parks with interrupts off.
 *
 * APs are started one at a time, so bst_ap_stack is never shared. A parked
 * AP runs no Bastio code and touches no kernel state: it waits for later
 * work (a scheduler loop of its own, see bastiolang docs/concurrency.md).
 */

.set AP_TRAMPOLINE, 0x8000               /* must match smp.c; page-aligned, below 1 MiB */
#define T(x) ((x) - bst_ap_trampoline + AP_TRAMPOLINE)

.section .rodata
.global bst_ap_trampoline, bst_ap_trampoline_end
.code16
bst_ap_trampoline:
        cli
        cld
        xor     %ax, %ax
        mov     %ax, %ds
        lgdtl   T(tgdt_ptr)
        mov     %cr0, %eax
        or      $1, %eax                 /* PE */
        mov     %eax, %cr0
        ljmpl   $0x08, $T(ap32)

.code32
ap32:
        mov     $0x10, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss
        mov     %cr4, %eax
        or      $0x620, %eax             /* PAE | OSFXSR | OSXMMEXCPT, as on the BSP */
        mov     %eax, %cr4
        mov     $bst_kernel_pml4, %eax
        mov     %eax, %cr3
        mov     $0xC0000080, %ecx        /* EFER: LME | SCE, as on the BSP */
        rdmsr
        or      $0x101, %eax
        wrmsr
        mov     %cr0, %eax
        and     $0xFFFFFFFB, %eax        /* clear EM */
        or      $0x80010003, %eax        /* PG | WP | MP | PE */
        mov     %eax, %cr0
        ljmp    $0x18, $T(ap64)

.code64
ap64:
        movabs  $bst_ap_long, %rax       /* leave the low copy for the kernel image */
        jmp     *%rax

.align 8
tgdt:
        .quad 0
        .quad 0x00CF9A000000FFFF         /* 0x08: 32-bit code */
        .quad 0x00CF92000000FFFF         /* 0x10: data */
        .quad 0x00AF9A000000FFFF         /* 0x18: 64-bit code */
tgdt_end:
tgdt_ptr:
        .word tgdt_end - tgdt - 1
        .long T(tgdt)
bst_ap_trampoline_end:

.section .text
bst_ap_long:
        lgdt    bst_gdt_ptr
        mov     $0x10, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss
        xor     %ax, %ax
        mov     %ax, %fs
        mov     %ax, %gs
        mov     bst_ap_stack, %rsp
        pushq   $0x08                    /* reload CS from the kernel GDT */
        lea     1f(%rip), %rax
        push    %rax
        lretq
1:      call    bst_ap_main              /* smp.c: IDT, check in */
2:      cli
        hlt
        jmp     2b

        .section .note.GNU-stack,"",@progbits
