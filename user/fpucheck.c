/*
 * Checks that the kernel keeps each process's SSE registers apart: puts a
 * value derived from its pid in xmm0 and checks it is still there across
 * enough work for the timer to preempt it many times. Run two at once
 * (`fpucheck &` then `fpucheck`): without per-process FPU state, each would
 * see the other's value. It then touches 512 fresh pages of anonymous memory,
 * every one a page fault the kernel answers without putting the task back in
 * the round robin, and checks xmm0 and xmm9 after each. Last it calls stat,
 * open, read and close in a loop (the calls the kernel answers without saving
 * the task's FPU registers when nothing else needs the processor) and checks
 * them again. Freestanding, Linux system calls only.
 */
typedef unsigned long u64;

static u64 sys(u64 n, u64 a, u64 b, u64 c) {
    u64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static u64 sys6(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    u64 r;
    register u64 r10 __asm__("r10") = d;
    register u64 r8 __asm__("r8") = e;
    register u64 r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}

u64 elf_main(u64 *sp) {
    (void)sp;
    u64 want = sys(39, 0, 0, 0) * 0x0101010101010101ul;   /* getpid */
    __asm__ volatile("movq %0, %%xmm0" : : "r"(want) : "xmm0");
    for (u64 i = 0; i < 30000000; i++) {
        u64 got;
        __asm__ volatile("movq %%xmm0, %0" : "=r"(got));
        if (got != want) {
            sys(1, 1, (u64)"fpucheck: xmm0 changed\r\n", 24);
            return 1;
        }
    }
    u64 want9 = ~want;
    __asm__ volatile("movq %0, %%xmm9" : : "r"(want9) : "xmm9");
    volatile char *mem = (volatile char *)sys6(9, 0, 512 * 4096, 3, 0x22, (u64)-1, 0);   /* mmap, anonymous */
    if ((u64)mem > (u64)-4096) {
        sys(1, 1, (u64)"fpucheck: mmap failed\r\n", 23);
        return 1;
    }
    for (u64 p = 0; p < 512; p++) {
        mem[p * 4096] = 1;
        u64 got, got9;
        __asm__ volatile("movq %%xmm0, %0" : "=r"(got));
        __asm__ volatile("movq %%xmm9, %0" : "=r"(got9));
        if (got != want || got9 != want9) {
            sys(1, 1, (u64)"fpucheck: xmm changed by a page fault\r\n", 38);
            return 1;
        }
    }
    char buf[256] __attribute__((aligned(16)));
    for (u64 i = 0; i < 20000; i++) {
        u64 fd;
        sys(4, (u64)"/", (u64)buf, 0);                                   /* stat */
        fd = sys6(257, (u64)-100, (u64)"/proc/self/exe", 0, 0, 0, 0);    /* openat */
        if (fd < 4096) {
            sys(0, fd, (u64)buf, 4);                                     /* read */
            sys(3, fd, 0, 0);                                            /* close */
        }
        u64 got, got9;
        __asm__ volatile("movq %%xmm0, %0" : "=r"(got));
        __asm__ volatile("movq %%xmm9, %0" : "=r"(got9));
        if (got != want || got9 != want9) {
            sys(1, 1, (u64)"fpucheck: xmm changed by a file call\r\n", 38);
            return 1;
        }
    }
    sys(1, 1, (u64)"fpucheck: xmm0 kept\r\n", 21);
    return 0;
}
