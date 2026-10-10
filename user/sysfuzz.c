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
 *        sysfuzz e SEED [RUNS] [v]    (hostile executables; see elf_fuzz)
 *
 * The same seed makes the same calls, so a failure can be replayed. Calls
 * that would end the run or wait for something that never comes are left
 * out or tamed (see `tame`): exit, execve, signals to other processes, a
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
 * the asm. With `exit`, not `exit_group`: a child made with CLONE_THREAD
 * would otherwise end the fuzzer too, part way through a line, and the run
 * would test nothing more (t_thread in test/compat checks exit_group from
 * a thread). */
static u64 lx_fork(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e) {
    register u64 r10 __asm__("r10") = d;
    register u64 r8 __asm__("r8") = e;
    u64 r;
    __asm__ volatile("syscall\n\t"
                     "test %%rax, %%rax\n\t"
                     "jnz 1f\n\t"
                     "mov $60, %%eax\n\t"
                     "xor %%edi, %%edi\n\t"
                     "syscall\n\t"
                     "ud2\n"
                     "1:"
                     : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
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
static u64 pick(u64 n) { return n ? next() % n : 0; }


/* Strings the kernel may take as paths or program names. Some are real
 * files, so the calls reach deep into the file systems. */
static const char *paths[] = {
    "/motd", "/about", "/elfdemo", "/", ".", "..", "", "/bin", "/bin/cat", "fz", "fz/a",
    "/fz", "disk", "disk/f", "/nothere", "//", "/./motd", "../../..", "true", "hello",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
};
#define NPATHS (sizeof paths / sizeof paths[0])

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
 * the rest of the table now and then. */
static const unsigned short linux_calls[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20, 21, 22, 24, 25, 28,
    32, 33, 35, 39, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 54, 56, 57, 58, 61, 63, 72, 74, 75,
    79, 80, 81, 83, 84, 87, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 102, 104, 105, 106, 107,
    108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 124, 132, 137, 138,
    140, 141, 149, 150, 157, 158, 160, 162, 186, 201, 204, 217, 218, 228, 229, 230, 235, 257,
    258, 260, 261, 262, 263, 268, 269, 271, 280, 288, 292, 293, 302, 318, 439,
    /* Added once BastiOS implemented them. */
    23, 26, 27, 29, 30, 31, 36, 37, 40, 46, 47, 53, 55, 64, 65, 66, 67, 68, 69, 70, 71, 73, 76,
    77, 82, 86, 88, 89, 100, 122, 123, 127, 129, 131, 133, 135, 151, 152, 159, 164, 165, 166,
    170, 171, 213, 220, 221, 222, 223, 224, 225, 226, 227, 232, 233, 247, 259, 264, 265, 266,
    267, 270, 273, 275, 276, 278, 281, 282, 283, 284, 285, 286, 287, 289, 290, 291, 295, 296,
    305, 309, 316, 319, 326, 332, 334, 441,
};
#define NLINUX (sizeof linux_calls / sizeof linux_calls[0])


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
    case 59: case 60: case 231: case 322: case 169:    /* execve, exit, exit_group, execveat, reboot */
    case 34: case 128: case 130:                      /* pause, rt_sigtimedwait, rt_sigsuspend */
    case 15:                                          /* rt_sigreturn: registers from the stack */
    case 202:                                         /* futex: waits on a word */
    case 42:                                          /* connect: TCP waits for a SYN reply */
        return 0;
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
    case 0: case 17: case 19: case 43: case 45: case 288:   /* read-like: only if ready */
        return ready(a[0], 1);
    case 1: case 18: case 20: case 44:                /* write-like */
        if (a[0] <= 2 && a[2] > 256) a[2] = 256;      /* keep the console readable */
        return ready(a[0], 4);
    case 9:                                           /* mmap: never over ourselves */
        if ((a[3] & 0x100010) && hits_mine(a[0], a[1])) return 0;
        return 1;
    case 11: case 10: case 28: case 149: case 150:    /* munmap, mprotect, madvise, mlock */
        return !hits_mine(a[0], a[1]);
    case 25:                                          /* mremap */
        return !hits_mine(a[0], a[1]) && !((a[3] & 2) && hits_mine(a[4], a[2]));
    case 158:                                         /* arch_prctl: the fuzzer has no TLS */
        return 1;
    case 435:                                         /* clone3: a child would go on fuzzing */
    case 38: case 223:                                /* setitimer, timer_settime: SIGALRM ends us */
    case 65:                                          /* semop: waits on a semaphore */
        return 0;
    case 37:                                          /* alarm: only cancel one */
        a[0] = 0; return 1;
    case 129:                                         /* rt_sigqueueinfo: ourselves, signal 0 */
        a[0] = lx(39, 0, 0, 0, 0, 0, 0); a[1] = 0; return 1;
    case 23:                                          /* select: zero timeout */
        a[4] = (u64)buf; return 1;
    case 270:                                         /* pselect6 */
        a[4] = (u64)buf; return 1;
    case 232: case 281:                               /* epoll_wait, epoll_pwait: no waiting */
        a[3] = 0; return 1;
    case 441:                                         /* epoll_pwait2 */
        a[3] = (u64)buf; return 1;
    case 220:                                         /* semtimedop: zero timeout */
        a[3] = (u64)buf; return 1;
    case 69:                                          /* msgsnd: IPC_NOWAIT */
        a[3] |= 04000; return 1;
    case 70:                                          /* msgrcv: IPC_NOWAIT */
        a[4] |= 04000; return 1;
    case 247:                                         /* waitid: WNOHANG */
        a[3] |= 1; return 1;
    case 73:                                          /* flock: LOCK_NB */
        a[1] |= 4; return 1;
    case 30:                                          /* shmat: wherever the kernel likes */
        a[1] = 0; return 1;
    case 47: case 295:                                /* recvmsg, preadv */
        return ready(a[0], 1);
    case 46: case 296: case 278:                      /* sendmsg, pwritev, vmsplice */
        return ready(a[0], 4);
    case 40:                                          /* sendfile(out, in) */
        return ready(a[1], 1) && ready(a[0], 4);
    case 275: case 326:                               /* splice, copy_file_range */
        return ready(a[0], 1) && ready(a[2], 4);
    case 276:                                         /* tee */
        return ready(a[0], 1) && ready(a[1], 4);
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

/*
 * Hostile executables. Each run takes one of three real ELF programs built
 * into the fuzzer (fixed-address, PIE, and dynamically linked, which names
 * an interpreter),
 * damages its headers, writes it to /fzelfSEED and has a child execve it.
 * The damage aims at what the loader checks: the file header's type, entry,
 * program header offset, count and size; each program header's type,
 * offset, address and sizes, with values at the edges of the user window
 * and of the file, and past 2^63; the interpreter's path (another program,
 * the file itself, a missing one, an unterminated one); and the file's
 * length. A child whose image the kernel accepts runs whatever it was
 * given, so it is killed if it has not ended within about 200 ms. The
 * kernel must refuse or run each one without faulting or losing a page.
 */
#define IMG_MAX (256 * 1024ul)
static unsigned char bad[IMG_MAX];
static const char *elf_sources[] = { "elfexec", "elfpie", "mdyn" };
#define NSOURCES (sizeof elf_sources / sizeof elf_sources[0])
#define IMAGE(n) __asm__(".section .rodata\n.balign 8\n" #n "_start: .incbin \"build/user/" #n ".elf\"\n" #n "_end:\n.previous")
IMAGE(elfexec);
IMAGE(elfpie);
IMAGE(mdyn);
extern const unsigned char elfexec_start[], elfexec_end[], elfpie_start[], elfpie_end[], mdyn_start[], mdyn_end[];

static u64 get(const unsigned char *p, int n) { u64 v = 0; for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i]; return v; }
static void put(unsigned char *p, int n, u64 v) { for (int i = 0; i < n; i++) p[i] = (unsigned char)(v >> (8 * i)); }

/* A value for a field that held `was`, in a file of `size` bytes. */
static u64 hostile(u64 was, u64 size) {
    static const u64 v[] = { 0, 1, 0xFFF, 0x1000, 0x1001, 0x400000, USER_BASE - 0x1000, USER_BASE,
                             USER_BASE + USER_SIZE - 0x1000, USER_BASE + USER_SIZE, USER_SIZE, USER_SIZE + 1,
                             1ul << 32, 1ul << 63, ~0ul, ~0ul - 0xFFF };
    switch (pick(6)) {
    case 0: return v[pick(sizeof v / sizeof v[0])];
    case 1: return was + pick(64) - 32;
    case 2: return was ^ (1ul << pick(64));
    case 3: return size - pick(64);
    case 4: return was * (2 + pick(4));
    default: return pick(size + 1);
    }
}

static void damage(u64 n, u64 *size, u64 self) {
    u64 phoff = get(bad + 32, 8), phnum = get(bad + 56, 2);
    switch (pick(8)) {
    case 0: {                                          /* a file header field */
        static const unsigned char at[] = { 16, 24, 32, 54, 56, 4, 5, 18 };
        static const unsigned char w[] = { 2, 8, 8, 2, 2, 1, 1, 2 };
        u64 k = pick(sizeof at);
        put(bad + at[k], w[k], hostile(get(bad + at[k], w[k]), n));
        break;
    }
    case 1: case 2: case 3: case 4: {                 /* a program header field */
        if (phnum == 0 || phoff >= n) break;
        u64 h = phoff + 56 * pick(phnum);
        static const unsigned char at[] = { 0, 8, 16, 32, 40, 4 };
        static const unsigned char w[] = { 4, 8, 8, 8, 8, 4 };
        u64 k = pick(sizeof at);
        if (h + at[k] + w[k] > n) break;
        u64 val = k == 0 && pick(2) ? (pick(2) ? 1 : 3) : hostile(get(bad + h + at[k], w[k]), n);
        put(bad + h + at[k], w[k], val);
        break;
    }
    case 5: {                                          /* the interpreter's path */
        for (u64 i = 0; i < phnum && phoff + 56 * i + 56 <= n; i++) {
            unsigned char *h = bad + phoff + 56 * i;
            if (get(h, 4) != 3) continue;
            u64 at = get(h + 8, 8), len = get(h + 32, 8);
            if (at >= n || len > n - at || len == 0) break;
            static const char *to[] = { "/bin/elfpie", "/bin/mdyn", "/bin/elfexec", "/nothere", "", "/bin" };
            const char *t = pick(4) ? to[pick(sizeof to / sizeof to[0])] : 0;
            char me[16] = "/fzelf";
            u64 m = 6;
            { char d[8]; int k = 0; u64 x = self; do { d[k++] = (char)('0' + x % 10); x /= 10; } while (x); while (k) me[m++] = d[--k]; me[m] = 0; }
            if (!t) t = me;
            u64 l = slen(t);
            if (l + 1 <= len) {
                for (u64 j = 0; j < len; j++) bad[at + j] = j < l ? (unsigned char)t[j] : 0;
                if (pick(4) == 0) put(h + 32, 8, l + 1);
            } else {
                for (u64 j = 0; j < len; j++) bad[at + j] = 'x';   /* no NUL */
            }
            break;
        }
        break;
    }
    case 6:                                            /* cut the file short */
        *size = pick(n + 1);
        break;
    default:                                           /* some bytes anywhere in the headers */
        for (u64 k = 1 + pick(8); k; k--) bad[pick(phoff + 56 * phnum < n ? phoff + 56 * phnum : n)] = (unsigned char)next();
    }
}

static int elf_fuzz(u64 seed, u64 runs, u64 verbose) {
    const unsigned char *copy[NSOURCES] = { elfexec_start, elfpie_start, mdyn_start };
    u64 size[NSOURCES] = { (u64)(elfexec_end - elfexec_start), (u64)(elfpie_end - elfpie_start), (u64)(mdyn_end - mdyn_start) };
    for (u64 s = 0; s < NSOURCES; s++)
        if (size[s] < 64 || size[s] > IMG_MAX) { say("sysfuzz: bad built-in image\n"); return 2; }
    char me[16] = "/fzelf";
    { u64 m = 6; char d[8]; int k = 0; u64 x = seed; do { d[k++] = (char)('0' + x % 10); x /= 10; } while (x); while (k) me[m++] = d[--k]; me[m] = 0; }
    u64 refused = 0, ran = 0, killed = 0;
    say("sysfuzz: elf seed "); say_num(seed); say("\n");
    for (u64 r = 0; r < runs; r++) {
        u64 s = pick(NSOURCES), n = size[s], len = n;
        for (u64 i = 0; i < n; i++) bad[i] = copy[s][i];
        for (u64 k = 1 + pick(4); k; k--) damage(n, &len, seed);
        if (verbose) { say("E "); say_num(r); say(" "); say(elf_sources[s]); say("\n"); }
        i64 f = (i64)lx(2, path(me), 01 | 0100 | 01000, 0755, 0, 0, 0);   /* O_WRONLY|O_CREAT|O_TRUNC */
        if (f < 0) { say("sysfuzz: cannot write "); say(me); say("\n"); return 2; }
        for (u64 at = 0; at < len; ) {
            i64 w = (i64)lx(1, (u64)f, (u64)bad + at, len - at, 0, 0, 0);
            if (w <= 0) break;
            at += (u64)w;
        }
        lx(3, (u64)f, 0, 0, 0, 0, 0);
        i64 pid = (i64)lx(57, 0, 0, 0, 0, 0, 0);
        if (pid == 0) {
            lx(3, 1, 0, 0, 0, 0, 0);                  /* whatever it prints goes nowhere */
            lx(3, 2, 0, 0, 0, 0, 0);
            u64 argv[2] = { path(me), 0 }, envp[1] = { 0 };
            lx(59, argv[0], (u64)argv, (u64)envp, 0, 0, 0);
            lx(60, 127, 0, 0, 0, 0, 0);               /* execve refused it */
        }
        if (pid < 0) { refused++; continue; }
        int status = 0;
        u64 t;
        for (t = 0; t < 20; t++) {
            if ((i64)lx(61, (u64)pid, (u64)&status, 1, 0, 0, 0) == pid) break;   /* WNOHANG */
            struct { u64 s, ns; } ts = { 0, 10000000 };
            lx(35, (u64)&ts, 0, 0, 0, 0, 0);
        }
        if (t == 20) {
            lx(62, (u64)pid, 9, 0, 0, 0, 0);
            lx(61, (u64)pid, (u64)&status, 0, 0, 0, 0);
            killed++;
        } else if ((status & 0x7F) == 0 && ((status >> 8) & 0xFF) == 127) {
            refused++;
        } else {
            ran++;
        }
    }
    lx(87, path(me), 0, 0, 0, 0, 0);                  /* unlink */
    say("sysfuzz: elf seed "); say_num(seed); say(" ran "); say_num(runs);
    say(" images: "); say_num(refused); say(" refused, "); say_num(ran); say(" ended, ");
    say_num(killed); say(" killed\n");
    return 0;
}

u64 elf_main(u64 *sp) {
    if (!sp) return 1;
    u64 argc = sp[0];
    char **argv = (char **)(sp + 1);
    u64 seed = 0, calls = 2000, verbose = 0, elf = 0;
    if (argc >= 2 && argv[1][0] == 'e') { elf = 1; argv++; argc--; }
    if (argc >= 2) for (char *s = argv[1]; *s >= '0' && *s <= '9'; s++) seed = seed * 10 + (u64)(*s - '0');
    if (argc >= 3) { calls = 0; for (char *s = argv[2]; *s >= '0' && *s <= '9'; s++) calls = calls * 10 + (u64)(*s - '0'); }
    if (argc >= 4 && argv[3][0] == 'v') verbose = 1;
    rng = seed * 0x9E3779B97F4A7C15ul + 1;
    stack_hi = ((u64)sp + 0x10000) & ~0xFFFul;
    stack_lo = ((u64)sp - (1ul << 20)) & ~0xFFFul;
    if (elf) {
        if (lx(9, BUF_AT, BUF_SIZE, 3, 0x32, ~0ul, 0) != BUF_AT) return 2;
        return (u64)elf_fuzz(seed, argc >= 3 ? calls : 50, verbose);
    }

    /* SIGPIPE: ignored, so a write to a pipe nobody reads fails with EPIPE
     * instead of ending the run. */
    struct { u64 handler, flags, restorer, mask; } ign = { 1, 0, 0, 0 };
    lx(13, 13, (u64)&ign, 0, 8, 0, 0);

    if (lx(9, BUF_AT, BUF_SIZE, 3, 0x32, ~0ul, 0) != BUF_AT) {   /* MAP_PRIVATE|ANONYMOUS|FIXED */
        say("sysfuzz: cannot map the buffer\n");
        return 2;
    }
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
        else if (n == 56 || n == 57 || n == 58) {
            if (n == 56) a[1] = 0;                    /* clone: the child keeps our stack */
            lx_fork(n, a[0], a[1], a[2], a[3], a[4]);
        } else lx(n, a[0], a[1], a[2], a[3], a[4], a[5]);
        done++;
    }
    /* Reap the children (forks exit at once; spawned programs soon). */
    while ((i64)lx(61, ~0ul, 0, 0, 0, 0, 0) > 0) {}
    say("sysfuzz: seed "); say_num(seed); say(" made "); say_num(done);
    say(" calls, skipped "); say_num(skipped); say("\n");
    return 0;
}
