/*
 * Checks that the kernel keeps each process's SSE registers apart: puts a
 * value derived from its pid in xmm0 and checks it is still there across
 * enough work for the timer to preempt it many times, and then across
 * system calls of every kind of outcome: ones that return at once, ones that
 * give the processor away (sched_yield, nanosleep), and fork, whose child must
 * start with the same value. Run two at once (`fpucheck &` then `fpucheck`):
 * without per-process FPU state, each would see the other's value.
 * Freestanding, Linux system calls only.
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
    /* The same through system calls, with a new value in xmm0 for each, so a
     * page of FPU state that was not brought up to date shows. */
    u64 st = 0;
    for (u64 i = 0; i < 3000; i++) {
        u64 got, now = want + i;
        __asm__ volatile("movq %0, %%xmm0" : : "r"(now) : "xmm0");
        sys(110, 0, 0, 0);                              /* getppid */
        if (i % 16 == 0) {
            sys(24, 0, 0, 0);                           /* sched_yield */
        }
        if (i % 100 == 0) {
            volatile u64 ts[2];                         /* volatile: no SSE store to clobber xmm0 */
            ts[0] = 0;
            ts[1] = 1000000;
            sys(35, (u64)ts, 0, 0);                     /* nanosleep 1 ms */
        }
        if (i % 250 == 0) {
            u64 pid = sys(57, 0, 0, 0);                 /* fork */
            if (pid == 0) {
                __asm__ volatile("movq %%xmm0, %0" : "=r"(got));
                sys(60, got == now ? 0 : 1, 0, 0);      /* exit */
            }
            sys(61, pid, (u64)&st, 0);                  /* wait4 */
            if (st != 0) {
                sys(1, 1, (u64)"fpucheck: child changed xmm0\r\n", 30);
                return 1;
            }
        }
        __asm__ volatile("movq %%xmm0, %0" : "=r"(got));
        if (got != now) {
            sys(1, 1, (u64)"fpucheck: xmm0 changed\r\n", 24);
            return 1;
        }
    }
    sys(1, 1, (u64)"fpucheck: xmm0 kept\r\n", 21);
    return 0;
}
