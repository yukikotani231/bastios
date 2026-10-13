/* The AArch64 vDSO: so far only the signal return trampoline, exported as
 * glibc's unwinder expects (__kernel_rt_sigreturn). The kernel maps this
 * image, read-only, on one page of every Linux process and points
 * AT_SYSINFO_EHDR at it, so that the dynamic linker lists linux-vdso.so.1
 * and finds the image's symbols. The clocks are not here yet: a program
 * that finds no __kernel_clock_gettime makes the system call, which keeps
 * the same time.
 *
 * The kernel's own signal frames return through the copy of this
 * trampoline it writes at byte 0xF00 of the page (SIGTRAMP_AT); this one
 * is for code that looks the symbol up. */
__asm__(".text\n"
        ".globl __kernel_rt_sigreturn\n"
        ".type __kernel_rt_sigreturn, %function\n"
        "__kernel_rt_sigreturn:\n"
        "  mov x8, #139\n"
        "  svc #0\n");
