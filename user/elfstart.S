/*
 * Entry for the ELF test programs. A Linux kernel starts a static
 * executable with rsp pointing at argc, 16-byte aligned; hand that pointer
 * to `elf_main` before anything moves rsp, and exit with its result through
 * the BastiOS system call ABI (Linux system calls are not there yet).
 */
.section .text
.global _start
_start:
        movq    %rsp, %rdi
        testq   $15, %rsp
        jz      1f
        movq    $0, %rdi                /* misaligned: tell elf_main */
1:      andq    $-16, %rsp
        call    elf_main
        movq    %rax, %rdi
        xorl    %eax, %eax              /* SYS_EXIT */
        int     $0x80
        ud2

        .section .note.GNU-stack,"",@progbits
