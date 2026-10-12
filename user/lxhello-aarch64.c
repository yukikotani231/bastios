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

static void hex(unsigned long v) {
    char b[20];
    int i = 19;
    b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    say(b + i);
}

void _start(void) {
    say("hello from linux arm64, pid ");
    number((unsigned long)sys(172, 0, 0, 0));      /* getpid */
    say("\n");
    unsigned int st[32];
    for (int i = 0; i < 32; i++) st[i] = 0;
    sys(80, 1, (long)st, 0);                       /* fstat(1): the console, a character device */
    say("stdout mode 0x");
    hex(st[4]);                                    /* st_mode is at offset 16 */
    say(" nlink ");
    number(st[5]);
    say("\n");
    long fd = sys(56, -100, (long)"/etc/hosts", 0); /* openat(AT_FDCWD, "/etc/hosts", O_RDONLY) */
    char buf[10];
    for (int i = 0; i < 10; i++) buf[i] = 0;
    long n = sys(63, fd, (long)buf, 9);            /* read */
    sys(57, fd, 0, 0);                             /* close */
    say("read ");
    number((unsigned long)n);
    say(": ");
    say(buf);
    say("\n");
    sys(94, 3, 0, 0);                              /* exit_group(3) */
    for (;;) { }
}
