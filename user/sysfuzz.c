/*
 * A system call fuzzer. It makes random calls through both of the kernel's
 * entry points, the Linux ABI (`syscall`) and the native one (`int $0x80`),
 * with hostile arguments: kernel and non-canonical addresses, pointers at
 * the edges of the user window and of mapped pages, huge and negative
 * lengths, bad descriptors, and paths that run off a page. The kernel must
 * answer every call without faulting, hanging or losing a page;
 * test/fuzz_test.sh checks that from the outside.
 *
 * usage: sysfuzz SEED [CALLS] [v]     (v prints every call before making it)
 *
 * The same seed makes the same calls, so a failure can be replayed. Calls
 * that would end the run or wait for something that never comes are left
 * out or tamed (see `tame`): exit, execve (made in a child), signals to other
 * processes, waits (given a zero timeout or a nonblocking flag), a
 * read with nothing to read. Those are not where the kernel is weak, and a
 * run that stops early tests nothing.
 *
 * Freestanding (no libc): it talks to the kernel only through the two
 * instructions, so nothing between it and the kernel tidies the arguments.
 */
typedef unsigned long u64;
typedef long i64;

#define USER_BASE 0x8000000000ul
#define USER_SIZE (64ul << 20)

static u64 lx(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    register u64 r10 __asm__("r10") = d;
    register u64 r8 __asm__("r8") = e;
    register u64 r9 __asm__("r9") = f;
    u64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

/* fork, vfork and clone: the child must leave at once, without touching the
 * stack it may share with its parent (vfork, CLONE_VM), so it exits inside
 * the asm. */
static u64 lx_fork(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e) {
    register u64 r10 __asm__("r10") = d;
    register u64 r8 __asm__("r8") = e;
    u64 r;
    __asm__ volatile("syscall\n\t"
                     "test %%rax, %%rax\n\t"
                     "jnz 1f\n\t"
                     "mov $231, %%eax\n\t"
                     "xor %%edi, %%edi\n\t"
                     "syscall\n\t"
                     "ud2\n"
                     "1:"
                     : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

/* execve and execveat, in a child: fork, and the child makes call n with
 * the arguments (kept in registers the fork leaves alone) and exits if it
 * returns. Returns the child's pid, which the parent reaps. */
static u64 lx_fork_then(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e) {
    register u64 r12 __asm__("r12") = a;
    register u64 r13 __asm__("r13") = b;
    register u64 r14 __asm__("r14") = c;
    register u64 r15 __asm__("r15") = d;
    register u64 rbx __asm__("rbx") = e;
    u64 r;
    __asm__ volatile("mov $57, %%eax\n\t"
                     "syscall\n\t"
                     "test %%rax, %%rax\n\t"
                     "jnz 1f\n\t"
                     "mov %[n], %%rax\n\t"
                     "mov %%r12, %%rdi\n\t"
                     "mov %%r13, %%rsi\n\t"
                     "mov %%r14, %%rdx\n\t"
                     "mov %%r15, %%r10\n\t"
                     "mov %%rbx, %%r8\n\t"
                     "syscall\n\t"
                     "mov $231, %%eax\n\t"
                     "xor %%edi, %%edi\n\t"
                     "syscall\n\t"
                     "ud2\n"
                     "1:"
                     : "=&a"(r) : [n] "r"(n), "r"(r12), "r"(r13), "r"(r14), "r"(r15), "r"(rbx)
                     : "rcx", "r11", "rdi", "rsi", "rdx", "r10", "r8", "memory");
    return r;
}

static u64 native(u64 n, u64 a, u64 b, u64 c) {
    u64 r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
/* Through the native console call, which no file descriptor the fuzzing
 * closes or redirects can affect. */
static void say(const char *s) { native(1, (u64)s, slen(s), 0); }
static void say_hex(u64 v) {
    char d[19];
    d[0] = '0'; d[1] = 'x';
    for (int i = 0; i < 16; i++) d[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    d[18] = 0;
    say(d);
}
static void say_num(u64 v) {
    char d[21]; int n = 20;
    d[n] = 0;
    do { d[--n] = (char)('0' + v % 10); v /= 10; } while (v);
    say(d + n);
}

static u64 rng;
static u64 next(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}
static u64 pick(u64 n) { return next() % n; }


/* Strings the kernel may take as paths or program names. Some are real
 * files, so the calls reach deep into the file systems. */
static const char *paths[] = {
    "/motd", "/about", "/elfdemo", "/", ".", "..", "", "/bin", "/bin/cat", "fz", "fz/a",
    "/fz", "disk", "disk/f", "/nothere", "//", "/./motd", "../../..", "true", "hello",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "/dev/ptmx", "/dev/pts/0", "/dev/pts/1", "/dev/tty", "/dev/null", "/dev/zero", "/dev/loop0",
    "/dev/loop-control", "/proc/self/stat", "/proc/self/fd", "/etc/passwd",
};
#define NPATHS (sizeof paths / sizeof paths[0])

/* What execve may run: programs that end at once whatever their arguments
 * (one never waits for input, which would stall the run), a script with
 * an interpreter line (written at start), and paths that are no program. */
static const char *progs[] = {
    "/bin/mhello", "/bin/true", "/fzscript", "/fzscript2", "/motd", "/", "/nothere", "", "/bin",
};
#define NPROGS (sizeof progs / sizeof progs[0])

static u64 stack_lo, stack_hi;      /* around the stack the fuzzer runs on */
extern char __ehdr_start[], _end[];

static int overlaps(u64 a, u64 n, u64 lo, u64 hi) { return a < hi && (n > ~0ul - a || a + n > lo); }

/* Would a call on [a, a + n) touch the fuzzer's own code, data or stack? */
static int hits_self(u64 a, u64 n) {
    return overlaps(a, n, (u64)__ehdr_start & ~0xFFFul, ((u64)_end + 0xFFF) & ~0xFFFul)
        || overlaps(a, n, stack_lo, stack_hi);
}

static u64 any_pointer(void);

/* Four pages of scratch: the one buffer the fuzzer hands out as a valid
 * pointer. It is zeroed before every call, so a structure the kernel reads
 * from it (a sigaction, a timespec) is all zeros, apart from the path
 * strings `path` copies into its third page. It is mapped on its own with
 * nothing after it, so a length that runs past it ends at an unmapped page
 * rather than in the fuzzer's variables. */
#define BUF_SIZE (4 * 4096ul)
#define BUF_AT (USER_BASE + (32ul << 20))
static char *const buf = (char *)BUF_AT;
static u64 slots;

/* The same, for calls that unmap, move or protect memory: buf stays too. */
static int hits_mine(u64 a, u64 n) { return hits_self(a, n) || overlaps(a, n, BUF_AT, BUF_AT + BUF_SIZE); }

/* A copy of path string s in the third page of buf, which is zeroed before
 * each call's arguments are chosen. */
static u64 path(const char *s) {
    char *d = buf + 2 * 4096 + 512 * (slots++ % 8);
    for (u64 i = 0; s[i] && i < 511; i++) d[i] = s[i];
    return (u64)d;
}

/* A pointer the kernel may write through. Never into the fuzzer's own
 * code, data or stack except `buf`: a kernel that writes where it was told
 * to is right, and a fuzzer that wrecks itself tests nothing more. */
static u64 pointer(void) {
    u64 p = any_pointer();
    if (hits_self(p, 1)) return (u64)buf;
    return p;
}

static u64 any_pointer(void) {
    switch (pick(16)) {
    case 0: return 0;
    case 1: return 1 + pick(4096);
    case 2: return 0x100000 + pick(0x400000);                  /* the kernel image */
    case 3: return 0xFEE00000;                                 /* the local APIC */
    case 4: return USER_BASE - 1 - pick(8);                    /* just below the user window */
    case 5: return USER_BASE + USER_SIZE - pick(16);           /* its last bytes */
    case 6: return USER_BASE + USER_SIZE + pick(4096);         /* just past it */
    case 7: return 0x0000800000000000ul - pick(16);            /* the end of canonical */
    case 8: return 0xFFFF800000000000ul + (pick(4) << 12);     /* the kernel half */
    case 9: return ~0ul - pick(16);
    case 10: return (u64)buf + 4 * 4096 - 1 - pick(8);         /* runs off the end of buf */
    case 11: return (u64)buf + pick(4 * 4096);
    case 12: return path(paths[pick(NPATHS)]);
    case 13: return USER_BASE + pick(USER_SIZE);               /* anywhere in the window */
    case 14: return USER_BASE + (pick(USER_SIZE) & ~0xFFFul);  /* some page in the window */
    default: return (u64)buf;
    }
}

static u64 length(void) {
    static const u64 l[] = { 0, 1, 2, 7, 8, 64, 4095, 4096, 4097, 1 << 16, 1 << 20,
                             64ul << 20, 1ul << 31, 1ul << 32, 1ul << 63, ~0ul };
    return pick(3) == 0 ? pick(256) : l[pick(sizeof l / sizeof l[0])];
}

static u64 number(void) {
    static const u64 v[] = { 0, 1, 2, 3, 4, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 1ul << 32,
                             1ul << 63, ~0ul, ~0ul - 1, 0x100, 0x800, 0x80000, 0x10 };
    return pick(2) ? pick(64) : v[pick(sizeof v / sizeof v[0])];
}

static u64 fd(void) {
    static const u64 v[] = { ~0ul, 100, 1ul << 31, 1ul << 32, 0xFFFFFF9C /* AT_FDCWD */ };
    return pick(5) ? pick(12) : v[pick(sizeof v / sizeof v[0])];
}

static u64 arg(void) {
    switch (pick(4)) {
    case 0: return pointer();
    case 1: return length();
    case 2: return fd();
    default: return number();
    }
}

/* The Linux calls BastiOS implements (most of the fuzzing goes here), and
 * the rest of the table now and then. test/sysfuzz_calls.sh checks that
 * every call the kernel dispatches is here or in this list of calls that
 * `tame` leaves out:
 *
 * left out: 15 34 42 60 130 153 155 169 231
 */
static const unsigned short linux_calls[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33, 35, 36, 37, 38, 39, 40, 41, 43, 44, 45, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 58, 59, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74,
    75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
    98, 99, 100, 102, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117,
    118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 131, 132, 133, 135, 136, 137,
    138, 139, 140, 141, 142, 143, 144, 145, 146, 148, 149, 150, 151, 152, 157, 158, 159, 160,
    161, 162, 164, 165, 166, 170, 171, 186, 187, 200, 201, 202, 203, 204, 206, 213, 217, 218,
    220, 221, 222, 223, 224, 226, 227, 228, 229, 230, 232, 233, 234, 235, 240, 241, 242, 243,
    244, 245, 247, 253, 254, 255, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268,
    269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 280, 281, 282, 283, 284, 285, 286, 287,
    288, 289, 290, 291, 292, 293, 294, 295, 296, 299, 302, 305, 307, 309, 310, 311, 312, 314,
    315, 316, 318, 319, 322, 325, 326, 327, 328, 332, 334, 424, 434, 435, 438, 439, 441, 452,
};
#define NLINUX (sizeof linux_calls / sizeof linux_calls[0])


/* ioctl requests the kernel implements: terminals and pseudo-terminals,
 * descriptors, loop devices. Left out: those that wait for output to drain
 * (TCSETSW, TCSETSF, TCSBRK, TCSBRKP, TIOCSBRK, TCSETAW, TCSETAF). */
static const u64 ioctls[] = {
    0x5401, 0x5402, 0x5405, 0x5406, 0x540A, 0x540B, 0x540E, 0x540F, 0x5410, 0x5411,
    0x5412, 0x5413, 0x5414, 0x5415, 0x541B, 0x541C, 0x541D, 0x5420, 0x5421, 0x5422,
    0x5424, 0x5429, 0x5432, 0x5441, 0x5450, 0x5451, 0x80045430, 0x40045431, 0x80045432,
    0x80045439, 0x40045436, 0x80045438, 0x5437, 0x4C00, 0x4C01, 0x4C02, 0x4C03, 0x4C04,
    0x4C05, 0x4C06, 0x4C07, 0x4C08, 0x4C09, 0x4C0A, 0x4C80, 0x4C81, 0x4C82, 0x8008127A,
};
#define NIOCTLS (sizeof ioctls / sizeof ioctls[0])

/* The console the fuzzer shares with the shell that runs it: what a call
 * changes on it (its termios, its foreground group, its O_NONBLOCK, input
 * pushed into it) changes for the shell too, and a shell that reads EOF
 * or stops ends the test for the wrong reason. Known by fstat: up to three
 * (st_dev, st_ino, st_rdev) triples, from fd 0, /dev/console and /dev/tty. */
static u64 cons[3][3];
static int ncons;

static void note_console(u64 fd_or_path, int is_path) {
    u64 st[18];
    u64 r = is_path ? lx(4, fd_or_path, (u64)st, 0, 0, 0, 0) : lx(5, fd_or_path, (u64)st, 0, 0, 0, 0);
    if (r != 0 || ncons == 3) return;
    cons[ncons][0] = st[0]; cons[ncons][1] = st[1]; cons[ncons][2] = st[5];
    ncons++;
}

static int is_console(u64 f) {
    u64 st[18];
    if (lx(5, f, (u64)st, 0, 0, 0, 0) != 0) return 0;
    for (int i = 0; i < ncons; i++)
        if (st[0] == cons[i][0] && st[1] == cons[i][1] && st[5] == cons[i][2]) return 1;
    return 0;
}

/* poll(fd, events) with no timeout: whether a call on fd would go ahead
 * rather than wait. A bad descriptor goes ahead (it fails at once). */
static int ready(u64 f, short events) {
    struct { int fd; short events, revents; } p = { (int)f, events, 0 };
    i64 r = (i64)lx(7, (u64)&p, 1, 0, 0, 0, 0);
    return r != 0;
}

/* Turns a call that would end the run or wait forever into one that does
 * not, or rules it out (returns 0). */
static int tame(u64 n, u64 *a) {
    switch (n) {
    case 59: case 322: {                              /* execve(at), in a child: a program */
        u64 *v = (u64 *)(buf + 4096 + 256);           /* that ends at once; argv and envp */
        u64 p = path(progs[pick(NPROGS)]);            /* real, or a quarter of the time hostile */
        v[0] = p; v[1] = path("x=y"); v[2] = 0;
        if (n == 59) { a[0] = p; if (pick(4)) a[1] = (u64)v; if (pick(4)) a[2] = (u64)(v + 1); }
        else { a[1] = p; if (pick(4)) a[2] = (u64)v; if (pick(4)) a[3] = (u64)(v + 1); }
        return 1;
    }
    case 60: case 231: case 169:                      /* exit, exit_group, reboot */
    case 34: case 130:                                /* pause, rt_sigsuspend */
    case 15:                                          /* rt_sigreturn: registers from the stack */
    case 42:                                          /* connect: TCP waits for a SYN reply */
    case 37:                                          /* alarm: SIGALRM would end the run */
    case 153:                                         /* vhangup: would hang up the shell */
    case 155:                                         /* pivot_root: would move the shell's root */
        return 0;
    case 202:                                         /* futex: the ops that cannot wait for */
        switch (a[1] & 127) {                         /* ever, and a zero timeout (in buf) */
        case 0: case 9: a[3] = (u64)buf; return 1;    /* WAIT, WAIT_BITSET */
        case 1: case 3: case 4: case 5: case 10: return 1;   /* WAKE, REQUEUE, CMP_REQUEUE, WAKE_OP, WAKE_BITSET */
        }
        return 0;
    case 128:                                         /* rt_sigtimedwait: zero timeout */
        a[2] = (u64)buf; return 1;
    case 434:                                         /* pidfd_open: ourselves only, so */
        a[0] = lx(39, 0, 0, 0, 0, 0, 0); return 1;    /* pidfd_send_signal can reach no one else */
    case 424:                                         /* pidfd_send_signal: signal 0 */
        a[1] = 0; return 1;
    case 310: case 311:                               /* process_vm_readv/writev: ourselves, */
        a[0] = lx(39, 0, 0, 0, 0, 0, 0);              /* and no remote iovecs (zeros in buf), */
        a[3] = (u64)buf; return 1;                    /* so the fuzzer writes nothing of its own */
    case 129:                                         /* rt_sigqueueinfo: ourselves, signal 0 */
        a[0] = lx(39, 0, 0, 0, 0, 0, 0); a[1] = 0; return 1;
    case 38:                                          /* setitimer: only disarm */
        a[1] = (u64)buf; return 1;
    case 223:                                         /* timer_settime: only disarm */
        a[2] = (u64)buf; return 1;
    case 23:                                          /* select: zero timeout */
        a[4] = (u64)buf; return 1;
    case 270:                                         /* pselect6 */
        a[4] = (u64)buf; return 1;
    case 232: case 281:                               /* epoll_wait, epoll_pwait: no waiting */
        a[3] = 0; return 1;
    case 441:                                         /* epoll_pwait2 */
        a[3] = (u64)buf; return 1;
    case 242: case 243:                               /* mq_timedsend, mq_timedreceive: */
        a[4] = (u64)buf; return 1;                    /* a deadline long past */
    case 244:                                         /* mq_notify: never a signal */
        a[1] = 0; return 1;
    case 299:                                         /* recvmmsg: MSG_DONTWAIT, zero */
        a[3] |= 0x40; a[4] = (u64)buf;                /* timeout, and only if ready */
        return ready(a[0], 1);
    case 307:                                         /* sendmmsg */
        return ready(a[0], 4);
    case 247:                                         /* waitid: WNOHANG */
        a[3] |= 1; return 1;
    case 65: case 220: {                              /* semop, semtimedop: one op, */
        short *op = (short *)(buf + 4096);            /* IPC_NOWAIT, zero timeout */
        op[0] = (short)pick(4); op[1] = (short)(pick(5) - 2); op[2] = 04000;
        a[1] = (u64)op; a[2] = 1; a[3] = (u64)buf;
        return 1;
    }
    case 69:                                          /* msgsnd: IPC_NOWAIT */
        a[3] |= 04000; return 1;
    case 70:                                          /* msgrcv */
        a[4] |= 04000; return 1;
    case 73:                                          /* flock: LOCK_NB */
        a[1] |= 4; return 1;
    case 275:                                         /* splice: SPLICE_F_NONBLOCK, and */
        a[5] |= 2;                                    /* both ends ready (a pipe end may */
        return ready(a[0], 1) && ready(a[2], 4);      /* ignore the flag) */
    case 276:                                         /* tee */
        a[3] |= 2; return ready(a[0], 1) && ready(a[1], 4);
    case 278:                                         /* vmsplice */
        a[3] |= 2; return ready(a[0], 5);
    case 40: case 326:                                /* sendfile, copy_file_range */
        return ready(n == 40 ? a[1] : a[0], 1) && ready(n == 40 ? a[0] : a[2], 4);
    case 30:                                          /* shmat: never over ourselves (an */
        if (a[1] && hits_mine(a[1] & ~0xFFFul, 64ul << 20)) a[1] = 0;   /* address replaces */
        return 1;                                     /* what is mapped there) */
    case 435:                                         /* clone3: flags from buf, all zero */
        a[0] = (u64)buf; return 1;
    case 62: case 200:                                /* kill, tkill: only ourselves, signal 0 */
        a[0] = lx(39, 0, 0, 0, 0, 0, 0); a[1] = 0; return 1;
    case 234:                                         /* tgkill */
        a[0] = a[1] = lx(39, 0, 0, 0, 0, 0, 0); a[2] = 0; return 1;
    case 13:                                          /* rt_sigaction: never a handler */
        if (a[0] == 13 || a[0] == 17) return 0;       /* keep SIGPIPE ignored, SIGCHLD default */
        if (a[1] != 0) a[1] = (u64)buf;               /* all zeros: SIG_DFL */
        return 1;
    case 35:                                          /* nanosleep: zero time */
        a[0] = (u64)buf; return 1;
    case 230:                                         /* clock_nanosleep */
        a[1] = 0; a[2] = (u64)buf; return 1;
    case 7:                                           /* poll: no waiting */
        a[2] = 0; return 1;
    case 271:                                         /* ppoll */
        a[2] = (u64)buf; return 1;
    case 61:                                          /* wait4: WNOHANG */
        a[2] |= 1; return 1;
    case 0: case 17: case 19: case 43: case 45: case 47: case 288: case 295: case 327:   /* read-like: only if ready */
        return ready(a[0], 1);
    case 1: case 18: case 20: case 44: case 46: case 296: case 328:   /* write-like */
        if (a[0] <= 2 && a[2] > 256) a[2] = 256;      /* keep the console readable */
        return ready(a[0], 4);
    case 9:                                           /* mmap: never over ourselves */
        if ((a[3] & 0x100010) && hits_mine(a[0], a[1])) return 0;
        return 1;
    case 11: case 10: case 28: case 149: case 150: case 325:   /* munmap, mprotect, madvise, mlock(2) */
        return !hits_mine(a[0], a[1]);
    case 25:                                          /* mremap */
        return !hits_mine(a[0], a[1]) && !((a[3] & 2) && hits_mine(a[4], a[2]));
    case 158:                                         /* arch_prctl: the fuzzer has no TLS */
        return 1;
    case 16:                                          /* ioctl: half the time a request the */
        if (pick(2)) a[1] = ioctls[pick(NIOCTLS)];    /* kernel knows; on the console only */
        if (is_console(a[0]))                         /* the ones that change nothing */
            switch (a[1]) {
            case 0x5401: case 0x5413: case 0x540F: case 0x5429: case 0x541B: return 1;
            default: return 0;
            }
        return 1;
    case 72:                                          /* fcntl: F_SETFL not on the console */
        return !((a[1] & 0xFFFFFFFF) == 4 && is_console(a[0]));
    }
    return 1;
}

/* Native calls: SYS_EXIT, SYS_READ (waits for a key), SYS_WAIT (waits for a
 * child), SYS_POWEROFF and SYS_KILL of another process are left out. */
static int tame_native(u64 n, u64 *a) {
    switch (n) {
    case 0: case 2: case 10: case 25: return 0;     /* exit, read, power off, reboot */
    case 4: a[0] = 1000 + pick(1000); return 1;       /* wait: no such child */
    case 9: a[0] = pick(2) ? 0 : 1000 + pick(1000); return 1;
    case 3:                                           /* spawn: only "true" or "hello" */
        a[0] = path(paths[18 + pick(2)]); a[1] = slen((const char *)a[0]);
        return 1;
    }
    return 1;
}

u64 elf_main(u64 *sp) {
    if (!sp) return 1;
    u64 argc = sp[0];
    char **argv = (char **)(sp + 1);
    u64 seed = 0, calls = 2000, verbose = 0;
    if (argc >= 2) for (char *s = argv[1]; *s >= '0' && *s <= '9'; s++) seed = seed * 10 + (u64)(*s - '0');
    if (argc >= 3) { calls = 0; for (char *s = argv[2]; *s >= '0' && *s <= '9'; s++) calls = calls * 10 + (u64)(*s - '0'); }
    if (argc >= 4 && argv[3][0] == 'v') verbose = 1;
    rng = seed * 0x9E3779B97F4A7C15ul + 1;
    stack_hi = ((u64)sp + 0x10000) & ~0xFFFul;
    stack_lo = ((u64)sp - (1ul << 20)) & ~0xFFFul;

    /* SIGPIPE: ignored, so a write to a pipe nobody reads fails with EPIPE
     * instead of ending the run. */
    struct { u64 handler, flags, restorer, mask; } ign = { 1, 0, 0, 0 };
    lx(13, 13, (u64)&ign, 0, 8, 0, 0);

    if (lx(9, BUF_AT, BUF_SIZE, 3, 0x32, ~0ul, 0) != BUF_AT) {   /* MAP_PRIVATE|ANONYMOUS|FIXED */
        say("sysfuzz: cannot map the buffer\n");
        return 2;
    }
    note_console(0, 0);
    note_console((u64)"/dev/console", 1);
    note_console((u64)"/dev/tty", 1);

    /* Descriptors of many kinds, so the small numbers `fd` picks reach
     * more than the console: a pseudo-terminal pair, a pipe, a socket pair,
     * an eventfd, an epoll, a file. (Each may fail; the calls go ahead.) */
    u64 m = lx(2, path("/dev/ptmx"), 2, 0, 0, 0, 0);
    unsigned *pn = (unsigned *)(buf + 4096);
    if (lx(16, m, 0x40045431, (u64)pn, 0, 0, 0) == 0 && lx(16, m, 0x80045430, (u64)pn, 0, 0, 0) == 0) {
        char *d = buf + 4096 + 64;                    /* TIOCSPTLCK 0, TIOCGPTN */
        const char *pts = "/dev/pts/";
        u64 j = 0;
        while (pts[j]) { d[j] = pts[j]; j++; }
        char dig[11]; int nd = 0; u64 v = *pn;
        do { dig[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (nd) d[j++] = dig[--nd];
        d[j] = 0;
        lx(2, path(d), 0x102, 0, 0, 0, 0);            /* O_RDWR|O_NOCTTY */
    }
    lx(22, (u64)buf, 0, 0, 0, 0, 0);                  /* pipe */
    lx(53, 1, 1, 0, (u64)buf, 0, 0);                  /* socketpair(AF_UNIX, SOCK_STREAM) */
    lx(290, 0, 0, 0, 0, 0, 0);                        /* eventfd2 */
    lx(291, 0, 0, 0, 0, 0, 0);                        /* epoll_create1 */
    lx(2, path("/motd"), 0, 0, 0, 0, 0);

    /* Scripts for execve: one with an interpreter that ends at once, one
     * whose interpreter is the script itself. */
    static const char s1[] = "#!/bin/mhello an argument\n", s2[] = "#!/fzscript2\n";
    u64 f1 = lx(2, path("/fzscript"), 0x241, 0755, 0, 0, 0);   /* O_WRONLY|O_CREAT|O_TRUNC */
    lx(1, f1, (u64)s1, sizeof s1 - 1, 0, 0, 0);
    lx(3, f1, 0, 0, 0, 0, 0);
    u64 f2 = lx(2, path("/fzscript2"), 0x241, 0755, 0, 0, 0);
    lx(1, f2, (u64)s2, sizeof s2 - 1, 0, 0, 0);
    lx(3, f2, 0, 0, 0, 0, 0);

    say("sysfuzz: seed "); say_num(seed); say("\n");
    u64 done = 0, skipped = 0;
    for (u64 i = 0; i < calls; i++) {
        for (u64 k = 0; k < BUF_SIZE / 8; k++) ((volatile u64 *)buf)[k] = 0;
        u64 a[6];
        for (int k = 0; k < 6; k++) a[k] = arg();
        int nat = pick(8) == 0;
        u64 n;
        if (nat) n = pick(10) == 0 ? number() : pick(56);
        else if (pick(16) == 0) n = pick(460);
        else n = linux_calls[pick(NLINUX)];
        if (nat ? !tame_native(n, a) : !tame(n, a)) { skipped++; continue; }
        if (verbose) {
            say(nat ? "N " : "L "); say_num(n);
            for (int k = 0; k < (nat ? 3 : 6); k++) { say(" "); say_hex(a[k]); }
            say("\n");
        }
        if (nat) native(n, a[0], a[1], a[2]);
        else if (n == 59 || n == 322) {
            u64 c = lx_fork_then(n, a[0], a[1], a[2], a[3], a[4]);
            /* The program ends at once, unless what it inherited makes it
             * wait (a write to a full pipe nobody reads): then it is killed. */
            for (int t = 0; (i64)c > 0 && t < 100; t++) {
                if (lx(61, c, 0, 1, 0, 0, 0) != 0) c = 0;   /* wait4 WNOHANG */
                else lx(24, 0, 0, 0, 0, 0, 0);             /* sched_yield */
            }
            if ((i64)c > 0) { lx(62, c, 9, 0, 0, 0, 0); lx(61, c, 0, 0, 0, 0, 0); }
        }
        else if (n == 56 || n == 57 || n == 58 || n == 435) {
            if (n == 56) a[1] = 0;                    /* clone: the child keeps our stack */
            lx_fork(n, a[0], a[1], a[2], a[3], a[4]);
        } else lx(n, a[0], a[1], a[2], a[3], a[4], a[5]);
        done++;
        /* Reap the children that have ended (forks exit at once), or they
         * fill the process table and every later fork fails. */
        if (i % 8 == 0) while ((i64)lx(61, ~0ul, 0, 1, 0, 0, 0) > 0) {}
    }
    /* Reap the children (forks exit at once; spawned programs soon). */
    while ((i64)lx(61, ~0ul, 0, 0, 0, 0, 0) > 0) {}
    say("sysfuzz: seed "); say_num(seed); say(" made "); say_num(done);
    say(" calls, skipped "); say_num(skipped); say("\n");
    return 0;
}
