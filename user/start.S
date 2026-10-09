/*
 * Entry and header for BastiOS user programs. The kernel reads the header
 * (magic, bytes of memory the image needs including .bss, entry address),
 * maps the image at USER_BASE and starts at `_start` with a fresh stack.
 * When `main` returns, its result is the exit code.
 */
.section .header, "a"
        .quad 0x0A52455355545342      /* "BSTUSER\n" */
        .quad __image_end - __image_start
        .quad _start

.section .text
.global _start
_start:
        call    bst_umain
        mov     %rax, %rdi
        xor     %eax, %eax              /* SYS_EXIT */
        int     $0x80
        ud2

        .section .note.GNU-stack,"",@progbits
