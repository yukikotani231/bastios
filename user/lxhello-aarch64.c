/*
 * A Linux arm64 program with no libc: the kernel's Linux system-call layer
 * as an AArch64 program sees it (number in x8, arguments in x0-x2, result in
 * x0). It is linked at the start of the user window (arch/aarch64/Makefile).
 */
static long sys(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static unsigned long len(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void say(const char *s) { sys(64, 1, (long)s, (long)len(s)); }

static void number(unsigned long v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    say(b + i);
}

void _start(void) {
    say("hello from linux arm64, pid ");
    number((unsigned long)sys(172, 0, 0, 0));      /* getpid */
    say("\n");
    sys(94, 3, 0, 0);                              /* exit_group(3) */
    for (;;) { }
}
