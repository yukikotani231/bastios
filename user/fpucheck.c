/*
 * Checks that the kernel keeps each process's SSE registers apart: puts a
 * value derived from its pid in xmm0 and checks it is still there across
 * enough work for the timer to preempt it many times. Run two at once
 * (`fpucheck &` then `fpucheck`): without per-process FPU state, each would
 * see the other's value. Freestanding, Linux system calls only.
 */
typedef unsigned long u64;

static u64 sys(u64 n, u64 a, u64 b, u64 c) {
    u64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
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
    sys(1, 1, (u64)"fpucheck: xmm0 kept\r\n", 21);
    return 0;
}
