/*
 * Hands the kernel user buffers that cross page boundaries (assembly, so
 * the layout is exact): a write whose bytes straddle two pages, a spawn
 * whose program name does, and a write that runs off the end of the image
 * into an unmapped page, which must stop short. A second name, "hellx",
 * agrees with "hello" on its first page only and must start nothing. Exits
 * with the count the last write returned (4) plus what that spawn returned
 * (0).
 */
.section .header, "a"
        .quad 0x0A52455355545342      /* "BSTUSER\n" */
        .quad __image_size
        .quad _start

.section .text
.global _start
_start:
        movl    $1, %eax                /* SYS_WRITE */
        leaq    msg(%rip), %rdi
        movl    $(msg_end - msg), %esi
        int     $0x80

        movl    $3, %eax                /* SYS_SPAWN "hello" */
        leaq    name(%rip), %rdi
        movl    $5, %esi
        int     $0x80
        movq    %rax, %rdi
        movl    $4, %eax                /* SYS_WAIT */
        int     $0x80

        movl    $3, %eax                /* SYS_SPAWN "hellx": no such program */
        leaq    bogus(%rip), %rdi
        movl    $5, %esi
        int     $0x80
        movq    %rax, %rbx

        movl    $1, %eax                /* SYS_WRITE, 100 bytes from the last 4 */
        leaq    tail(%rip), %rdi
        movl    $100, %esi
        int     $0x80

        leaq    (%rax, %rbx), %rdi
        xorl    %eax, %eax              /* SYS_EXIT */
        int     $0x80
        ud2

/* No .bss: the image ends right after `tail`, on a page boundary. */
.section .data
        .balign 4096
        .skip   4096 - 8
msg:    .ascii  "page-crossing write ok\r\n"
msg_end:
        .balign 4096
        .skip   4096 - 2
name:   .ascii  "hello"
        .balign 4096
        .skip   4096 - 2
bogus:  .ascii  "hellx"
        .balign 4096
        .skip   4096 - 4
tail:   .ascii  "END!"

        .section .note.GNU-stack,"",@progbits
