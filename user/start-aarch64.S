/*
 * Entry and header for BastiOS user programs on AArch64: the same header as
 * start.S (magic, bytes of memory the image needs, entry address). The
 * kernel maps the image at USER_BASE and starts at `_start` with a fresh
 * stack. When `main` returns, its result is the exit code (system call 0).
 */
        .section .header, "a"
        .quad 0x0A52455355545342      /* "BSTUSER\n" */
        .quad __image_size
        .quad _start

        .section .text
        .global _start
_start:
        bl      bst_umain
        mov     x8, #0                  /* SYS_EXIT; the code is already in x0 */
        svc     #0
        brk     #0
