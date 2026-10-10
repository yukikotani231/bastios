/*
 * A deliberately broken user program (assembly, since a Bastio program
 * cannot express this): it reads kernel memory. The kernel's pages are
 * mapped without the user bit, so the CPU raises a page fault and the
 * kernel must end this process and carry on. Exercises fault isolation.
 */
.section .header, "a"
        .quad 0x0A52455355545342      /* "BSTUSER\n" */
        .quad __image_size
        .quad _start

.section .text
.global _start
_start:
        movq    0x100000, %rax          /* the kernel image */
        ud2

        .section .note.GNU-stack,"",@progbits
