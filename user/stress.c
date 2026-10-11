/*
 * A concurrency stress test: several processes, each with several threads,
 * make valid system calls at once and check every answer. Where sysfuzz
 * throws hostile arguments at the kernel from one process, this one looks
 * for what goes wrong only when calls overlap on several processors: two
 * processes given the same page, a page table changed under a running
 * thread, a register or flag the kernel forgets to save, a file that comes
 * back with the wrong bytes, a lost wake-up.
 *
 * usage: stress SEED SECONDS [PROCS [THREADS]]     (defaults 4 x 3)
 *        stress -x CODE                            (exits with CODE: an exec target)
 *
 * Each thread draws operations from its own generator, seeded from SEED,
 * its process and its thread, until SECONDS have passed. Every operation
 * checks what it can: memory reads back what was written (and fresh memory
 * is zero), files read back their bytes and sizes, children exit with the
 * status computed from the data they were sent, registers and the
 * direction flag survive system calls, preemption and signals, a signal
 * handler starts with the direction flag clear, and a mutex-guarded counter
 * adds up at the end. The first failure prints "stress: FAIL ..." and ends
 * the run with status 1; a clean run prints "stress: ok ...". The order in
 * which threads meet depends on the scheduler, so a seed does not replay a
 * run exactly; it replays what each thread does.
 *
 * Calls that run out of memory or of descriptors (ENOMEM, EAGAIN, EMFILE,
 * ENFILE, ENOSPC) are skipped, not failures: many processes share one
 * small machine. Anything else unexpected is a failure.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t u64;

#define MAX_THREADS 16
#define PAGE 4096ul

static u64 seed0;
static int secs, nprocs = 4, nthreads = 3;
static int wid;                         /* this worker process's number */
static const char *self;                /* argv[0], for exec */
static double deadline;
static int disk;                        /* /disk exists: some files go there */

/* Shared by all workers (MAP_SHARED, made before they fork). */
struct total { u64 ops; u64 skipped; u64 per_op[32]; };
static struct total *total;

/* Shared by the threads of one worker. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static u64 counter;
static u64 added[MAX_THREADS];
static int shfd = -1;                   /* a file all threads write their own slots of */
static pid_t tids[MAX_THREADS];

static __thread u64 rng;
static __thread int me;                 /* this thread's number in its worker */
static __thread const char *op_name = "start";
static __thread volatile u64 sig_seen;
static volatile int sig_df;             /* a handler saw the direction flag set */

static u64 next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static u64 pick(u64 n) { return next() % n; }
static u64 mix(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; return x ^ (x >> 33); }

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void fail(const char *fmt, ...) {
    char b[400];
    int n = snprintf(b, sizeof b, "stress: FAIL seed %llu worker %d thread %d, %s: ",
                     (unsigned long long)seed0, wid, me, op_name);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    write(2, b, n);
    syscall(SYS_exit_group, 1);
    for (;;) ;
}

/* Out of a resource the machine may legitimately run short of. */
static int short_of(int e) { return e == ENOMEM || e == EAGAIN || e == EMFILE || e == ENFILE || e == ENOSPC; }
static void skip(void) { __atomic_add_fetch(&total->skipped, 1, __ATOMIC_RELAXED); }

/* The byte at offset i of the data tagged t. */
static unsigned char byte_at(u64 t, u64 i) { return (unsigned char)(mix(t + i / 8) >> (8 * (i % 8))); }
static void fill(unsigned char *p, u64 n, u64 t, u64 from) { for (u64 i = 0; i < n; i++) p[i] = byte_at(t, from + i); }
static u64 check(const unsigned char *p, u64 n, u64 t, u64 from) {
    for (u64 i = 0; i < n; i++)
        if (p[i] != byte_at(t, from + i)) return i;
    return n;
}

static ssize_t full_write(int fd, const void *p, size_t n) {
    size_t d = 0;
    while (d < n) {
        ssize_t r = write(fd, (const char *)p + d, n - d);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return d ? (ssize_t)d : r;
        d += r;
    }
    return d;
}

static pid_t wait_for(pid_t pid, int *st) {
    pid_t r;
    do r = waitpid(pid, st, 0); while (r < 0 && errno == EINTR);
    return r;
}

/* ------------------------------------------------------------ memory */

/* Pages each thread keeps for a while, checked whenever it comes back to them. */
#define KEEP 4
static __thread struct { u64 *p; u64 pages, tag; } kept[KEEP];

static void op_mem(void) {
    op_name = "mmap";
    u64 n = 1 + pick(16);
    u64 *p = mmap(0, n * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { if (short_of(errno)) { skip(); return; } fail("errno %d", errno); }
    for (u64 i = 0; i < n * PAGE / 8; i++)
        if (p[i]) fail("fresh page %llu word %llu is %#llx, not 0", i / 512, i % 512, p[i]);
    u64 tag = next();
    for (u64 i = 0; i < n * PAGE / 8; i++) p[i] = mix(tag + i);
    sched_yield();
    u64 c = pick(4);
    if (c == 0) {
        op_name = "mprotect";
        if (mprotect(p, n * PAGE, PROT_READ) && !short_of(errno)) fail("read-only: errno %d", errno);
        for (u64 i = 0; i < n * PAGE / 8; i++)
            if (p[i] != mix(tag + i)) fail("word %llu changed under PROT_READ", i);
        if (mprotect(p, n * PAGE, PROT_READ | PROT_WRITE)) fail("back to writable: errno %d", errno);
    } else if (c == 1) {
        op_name = "mremap";
        u64 m = n + 1 + pick(8);
        u64 *q = mremap(p, n * PAGE, m * PAGE, MREMAP_MAYMOVE);
        if (q == MAP_FAILED) {
            if (!short_of(errno)) fail("grow %llu -> %llu pages: errno %d", n, m, errno);
        } else {
            for (u64 i = 0; i < n * PAGE / 8; i++)
                if (q[i] != mix(tag + i)) fail("word %llu lost moving %llu -> %llu pages", i, n, m);
            for (u64 i = n * PAGE / 8; i < m * PAGE / 8; i++)
                if (q[i]) fail("grown word %llu is %#llx, not 0", i, q[i]);
                else q[i] = mix(tag + i);
            p = q; n = m;
        }
    } else if (c == 2 && n >= 3) {
        op_name = "munmap middle";
        u64 k = 1 + pick(n - 2);
        if (munmap(p + k * 512, PAGE)) fail("errno %d", errno);
        for (u64 i = 0; i < n * PAGE / 8; i++)
            if ((i < k * 512 || i >= (k + 1) * 512) && p[i] != mix(tag + i))
                fail("word %llu changed by unmapping page %llu", i, k);
        munmap(p, k * PAGE);
        munmap(p + (k + 1) * 512, (n - k - 1) * PAGE);
        return;
    }
    u64 s = pick(KEEP);
    if (kept[s].p) {
        op_name = "kept pages";
        for (u64 i = 0; i < kept[s].pages * PAGE / 8; i++)
            if (kept[s].p[i] != mix(kept[s].tag + i))
                fail("word %llu of %llu kept pages is %#llx, not %#llx", i, kept[s].pages, kept[s].p[i], mix(kept[s].tag + i));
        munmap(kept[s].p, kept[s].pages * PAGE);
    }
    kept[s].p = p; kept[s].pages = n; kept[s].tag = tag;
}

static void op_malloc(void) {
    op_name = "malloc";
    u64 n = 1 + pick(pick(4) ? 4096 : 300000);
    unsigned char *p = malloc(n);
    if (!p) { skip(); return; }
    u64 t = next();
    fill(p, n, t, 0);
    sched_yield();
    u64 bad = check(p, n, t, 0);
    if (bad < n) fail("byte %llu of %llu changed", bad, n);
    free(p);
}

/* A shared page written by a child is seen by the parent; a private page the
 * child writes stays as it was for the parent. */
static void op_fork_mem(void) {
    op_name = "fork memory";
    volatile u64 *sh = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) { if (short_of(errno)) { skip(); return; } fail("MAP_SHARED: errno %d", errno); }
    u64 *pv = mmap(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pv == MAP_FAILED) { munmap((void *)sh, PAGE); if (short_of(errno)) { skip(); return; } fail("errno %d", errno); }
    u64 t = next();
    for (u64 i = 0; i < 1024; i++) pv[i] = mix(t + i);
    pid_t pid = fork();
    if (pid < 0) { munmap((void *)sh, PAGE); munmap(pv, 2 * PAGE); if (short_of(errno)) { skip(); return; } fail("fork: errno %d", errno); }
    if (pid == 0) {
        int ok = 1;
        for (u64 i = 0; i < 1024; i++) if (pv[i] != mix(t + i)) ok = 0;
        for (u64 i = 0; i < 1024; i++) pv[i] = ~mix(t + i);
        sh[0] = t ^ 0x5555;
        _exit(ok ? 7 : 8);
    }
    int st;
    if (wait_for(pid, &st) != pid) fail("waitpid: errno %d", errno);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 7) fail("child saw the wrong private copy (status %#x)", st);
    if (sh[0] != (t ^ 0x5555)) fail("parent sees %#llx in the shared page, not the child's write", (u64)sh[0]);
    for (u64 i = 0; i < 1024; i++)
        if (pv[i] != mix(t + i)) fail("the child's write reached the parent's private word %llu", i);
    munmap((void *)sh, PAGE);
    munmap(pv, 2 * PAGE);
}

/* ------------------------------------------------------------- files */

static void tmp_path(char *b, size_t n, const char *what, int k) {
    const char *dir = disk && pick(4) == 0 ? "/disk" : "/tmp";
    snprintf(b, n, "%s/st%d.%d.%s%d", dir, wid, me, what, k);
}

static unsigned char fbuf[MAX_THREADS][65536];

static void op_file(void) {
    op_name = "file";
    char a[96], b[112];
    tmp_path(a, sizeof a, "f", (int)pick(4));
    snprintf(b, sizeof b, "%s.moved", a);
    unsigned char *buf = fbuf[me];
    int fd = open(a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) { if (short_of(errno)) { skip(); return; } fail("open %s: errno %d", a, errno); }
    u64 t = next(), n = 1 + pick(sizeof fbuf[0]);
    fill(buf, n, t, 0);
    ssize_t w = full_write(fd, buf, n);
    if (w < 0 && short_of(errno)) { close(fd); unlink(a); skip(); return; }
    if (w != (ssize_t)n) fail("write %llu bytes to %s: %zd, errno %d", n, a, w, errno);
    for (int k = 0; k < 3; k++) {
        u64 off = pick(n), len = 1 + pick(n - off);
        memset(buf, 0xAA, len);
        ssize_t r = pread(fd, buf, len, off);
        if (r != (ssize_t)len) fail("pread %llu at %llu of %llu: %zd, errno %d", len, off, n, r, errno);
        u64 bad = check(buf, len, t, off);
        if (bad < len) fail("%s byte %llu reads %#x, not %#x", a, off + bad, buf[bad], byte_at(t, off + bad));
    }
    struct stat s;
    u64 cut = pick(n + 1);
    if (ftruncate(fd, cut)) fail("ftruncate %llu: errno %d", cut, errno);
    if (fstat(fd, &s) || (u64)s.st_size != cut) fail("size %lld after truncating to %llu", (long long)s.st_size, cut);
    /* A write past the end leaves a hole that reads as zeros. */
    u64 gap = 1 + pick(9000);
    if (pwrite(fd, "x", 1, cut + gap) != 1) fail("pwrite past the end: errno %d", errno);
    memset(buf, 0xAA, gap + 1);
    if (pread(fd, buf, gap + 1, cut) != (ssize_t)(gap + 1)) fail("pread the hole: errno %d", errno);
    for (u64 i = 0; i < gap; i++) if (buf[i]) fail("hole byte %llu of %llu is %#x", i, gap, buf[i]);
    if (buf[gap] != 'x') fail("the byte after the hole is %#x", buf[gap]);
    if (pick(4) == 0 && fsync(fd)) fail("fsync: errno %d", errno);
    close(fd);
    op_name = "rename";
    if (rename(a, b)) fail("rename %s: errno %d", a, errno);
    if (stat(b, &s) || (u64)s.st_size != cut + gap + 1) fail("%s is %lld bytes after rename, not %llu", b, (long long)s.st_size, cut + gap + 1);
    if (stat(a, &s) == 0 || errno != ENOENT) fail("%s still there after rename", a);
    if (unlink(b)) fail("unlink %s: errno %d", b, errno);
    if (stat(b, &s) == 0 || errno != ENOENT) fail("%s still there after unlink", b);
}

/* Every thread of a worker writes its own slot of one open file at once. */
static void op_shared_file(void) {
    op_name = "shared file";
    if (shfd < 0) return;
    unsigned char *buf = fbuf[me];
    u64 off = me * 16384 + pick(8192), n = 1 + pick(8192), t = next();
    fill(buf, n, t, 0);
    ssize_t w = pwrite(shfd, buf, n, off);
    if (w < 0 && short_of(errno)) { skip(); return; }
    if (w != (ssize_t)n) fail("pwrite %llu at %llu: %zd, errno %d", n, off, w, errno);
    sched_yield();
    memset(buf, 0, n);
    if (pread(shfd, buf, n, off) != (ssize_t)n) fail("pread: errno %d", errno);
    u64 bad = check(buf, n, t, 0);
    if (bad < n) fail("byte %llu of my slot reads %#x, not %#x", off + bad, buf[bad], byte_at(t, bad));
}

static void op_dir(void) {
    op_name = "directory";
    char d[96], f[128];
    tmp_path(d, sizeof d, "d", (int)pick(2));
    int made = mkdir(d, 0755);
    if (made && errno == EEXIST) {
        /* Left by an earlier run that failed. */
        for (int i = 0; i < 16; i++) { snprintf(f, sizeof f, "%s/e%d", d, i); unlink(f); }
        rmdir(d);
        made = mkdir(d, 0755);
    }
    if (made) { if (short_of(errno)) { skip(); return; } fail("mkdir %s: errno %d", d, errno); }
    int k = 1 + (int)pick(12);
    for (int i = 0; i < k; i++) {
        snprintf(f, sizeof f, "%s/e%d", d, i);
        int fd = open(f, O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (fd < 0) fail("create %s: errno %d", f, errno);
        close(fd);
    }
    DIR *dp = opendir(d);
    if (!dp) {
        if (!short_of(errno)) fail("opendir %s: errno %d", d, errno);
        skip();
        for (int i = 0; i < k; i++) { snprintf(f, sizeof f, "%s/e%d", d, i); unlink(f); }
        rmdir(d);
        return;
    }
    int seen = 0;
    struct dirent *e;
    while ((e = readdir(dp)))
        if (e->d_name[0] == 'e') seen++;
    closedir(dp);
    if (seen != k) fail("%s lists %d entries, not %d", d, seen, k);
    if (rmdir(d) == 0 || errno != ENOTEMPTY) fail("rmdir of non-empty %s: errno %d", d, errno);
    for (int i = 0; i < k; i++) {
        snprintf(f, sizeof f, "%s/e%d", d, i);
        if (unlink(f)) fail("unlink %s: errno %d", f, errno);
    }
    if (rmdir(d)) fail("rmdir %s: errno %d", d, errno);
}

/* A descriptor and its duplicate share one offset. */
static void op_dup(void) {
    op_name = "dup";
    char a[96];
    tmp_path(a, sizeof a, "u", 0);
    int fd = open(a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) { if (short_of(errno)) { skip(); return; } fail("open: errno %d", errno); }
    unlink(a);
    unsigned char b[64];
    u64 t = next();
    fill(b, 64, t, 0);
    if (write(fd, b, 64) != 64) fail("write: errno %d", errno);
    /* dup takes the lowest free descriptor, so the threads never pick the
     * same one; dup2 then reuses it, closing what is there first. */
    int hi = dup(fd);
    if (hi < 0) { close(fd); if (short_of(errno)) { skip(); return; } fail("dup: errno %d", errno); }
    if (pick(2) && dup2(fd, hi) != hi) fail("dup2 onto my own %d: errno %d", hi, errno);
    if (lseek(fd, 10, SEEK_SET) != 10) fail("lseek: errno %d", errno);
    memset(b, 0, 64);
    if (read(hi, b, 20) != 20) fail("read through the duplicate: errno %d", errno);
    u64 bad = check(b, 20, t, 10);
    if (bad < 20) fail("the duplicate read byte %llu wrong (offset not shared?)", 10 + bad);
    if (lseek(fd, 0, SEEK_CUR) != 30) fail("offset %lld after reading through the duplicate", (long long)lseek(fd, 0, SEEK_CUR));
    close(hi);
    close(fd);
}

/* ------------------------------------------------- processes and pipes */

static void op_pipe(void) {
    op_name = "pipe";
    int p[2];
    if (pipe(p)) { if (short_of(errno)) { skip(); return; } fail("errno %d", errno); }
    u64 n = 1 + pick(200000), t = next();
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); if (short_of(errno)) { skip(); return; } fail("fork: errno %d", errno); }
    if (pid == 0) {
        close(p[1]);
        unsigned char b[4096];
        u64 got = 0, bad = 0;
        /* Reads exactly n bytes rather than to the end: another thread's
         * child, forked meanwhile, may hold a copy of the write end. */
        while (got < n) {
            ssize_t r = read(p[0], b, n - got < sizeof b ? n - got : sizeof b);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            if (check(b, r, t, got) < (u64)r) bad = 1;
            got += r;
        }
        _exit(bad ? 3 : got == n ? 0 : 4);
    }
    close(p[0]);
    unsigned char *buf = fbuf[me];
    for (u64 d = 0; d < n; ) {
        u64 m = n - d < sizeof fbuf[0] ? n - d : sizeof fbuf[0];
        fill(buf, m, t, d);
        if (full_write(p[1], buf, m) != (ssize_t)m) fail("write to the pipe: errno %d", errno);
        d += m;
    }
    close(p[1]);
    int st;
    if (wait_for(pid, &st) != pid) fail("waitpid: errno %d", errno);
    if (!WIFEXITED(st) || WEXITSTATUS(st))
        fail("reader of %llu bytes: %s (status %#x)", n,
             WIFEXITED(st) && WEXITSTATUS(st) == 3 ? "wrong bytes" : WIFEXITED(st) && WEXITSTATUS(st) == 4 ? "wrong count" : "died", st);
}

static void op_exec(void) {
    op_name = "exec";
    int code = 1 + (int)pick(100);
    char c[8];
    snprintf(c, sizeof c, "%d", code);
    pid_t pid = fork();
    if (pid < 0) { if (short_of(errno)) { skip(); return; } fail("fork: errno %d", errno); }
    if (pid == 0) {
        char *argv[] = { (char *)self, "-x", c, 0 };
        char *envp[] = { 0 };
        execve(self, argv, envp);
        _exit(errno == ENOMEM || errno == EAGAIN ? 125 : 126);
    }
    int st;
    if (wait_for(pid, &st) != pid) fail("waitpid: errno %d", errno);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 125) { skip(); return; }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != code) fail("%s -x %d ended with status %#x", self, code, st);
}

/* --------------------------------------------------- threads and locks */

static void *child_thread(void *a) {
    u64 t = (u64)a;
    rng = t | 1;
    u64 *p = mmap(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return (void *)(t ^ 1);
    for (u64 i = 0; i < 1024; i++) p[i] = t + i;
    sched_yield();
    u64 s = 0;
    for (u64 i = 0; i < 1024; i++) s += p[i] - (t + i);
    munmap(p, 2 * PAGE);
    return (void *)(t + s);
}

static void op_thread(void) {
    op_name = "thread";
    pthread_t th;
    u64 t = next() & ~1ull;
    int e = pthread_create(&th, 0, child_thread, (void *)t);
    if (e) { if (short_of(e)) { skip(); return; } fail("pthread_create: %d", e); }
    void *r;
    if ((e = pthread_join(th, &r))) fail("pthread_join: %d", e);
    if ((u64)r == (t ^ 1)) { skip(); return; }
    if ((u64)r != t) fail("thread returned %#llx, not %#llx", (u64)r, t);
}

static void op_lock(void) {
    op_name = "mutex";
    int k = 1 + (int)pick(20);
    for (int i = 0; i < k; i++) {
        pthread_mutex_lock(&mu);
        u64 c = counter;
        if (pick(4) == 0) sched_yield();
        counter = c + 1;
        pthread_mutex_unlock(&mu);
    }
    added[me] += k;
}

/* ------------------------------------------------ registers and signals */

static void on_signal(int s) {
    (void)s;
    u64 fl;
    __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    if (fl & 0x400) sig_df = 1;           /* the ABI: a handler starts with DF clear */
    sig_seen++;
}

/* Registers the kernel must keep across system calls (all but rax, rcx and
 * r11), interrupts and preemption, the direction flag among them, with
 * the processor spinning with DF set in between: the kernel must not run
 * with the flag it finds (bastios #164). */
static struct regs_ctx {
    unsigned char xin[256], xout[256];      /* 0, 256 */
    u64 gin[4], gout[4];                    /* 512, 544 */
    u64 pid, tid, spin;                     /* 576, 584, 592 */
} rctx[MAX_THREADS];

static void op_regs(void) {
    op_name = "registers";
    struct regs_ctx *c = &rctx[me];
    u64 n = 1 + pick(32), fl, seen = sig_seen;
    for (int i = 0; i < 4; i++) c->gin[i] = next();
    for (int i = 0; i < 32; i++) ((u64 *)c->xin)[i] = next();
    memset(c->xout, 0, sizeof c->xout);
    memset(c->gout, 0, sizeof c->gout);
    c->pid = getpid(); c->tid = gettid(); c->spin = pick(20000);
    __asm__ volatile(
        "movdqu 0(%[c]), %%xmm0\n\t"    "movdqu 16(%[c]), %%xmm1\n\t"
        "movdqu 32(%[c]), %%xmm2\n\t"   "movdqu 48(%[c]), %%xmm3\n\t"
        "movdqu 64(%[c]), %%xmm4\n\t"   "movdqu 80(%[c]), %%xmm5\n\t"
        "movdqu 96(%[c]), %%xmm6\n\t"   "movdqu 112(%[c]), %%xmm7\n\t"
        "movdqu 128(%[c]), %%xmm8\n\t"  "movdqu 144(%[c]), %%xmm9\n\t"
        "movdqu 160(%[c]), %%xmm10\n\t" "movdqu 176(%[c]), %%xmm11\n\t"
        "movdqu 192(%[c]), %%xmm12\n\t" "movdqu 208(%[c]), %%xmm13\n\t"
        "movdqu 224(%[c]), %%xmm14\n\t" "movdqu 240(%[c]), %%xmm15\n\t"
        "mov 512(%[c]), %%r12\n\t" "mov 520(%[c]), %%r13\n\t"
        "mov 528(%[c]), %%r14\n\t" "mov 536(%[c]), %%r15\n\t"
        "std\n\t"
        "mov 576(%[c]), %%rdi\n\t" "mov 584(%[c]), %%rsi\n\t"
        "mov $10, %%edx\n\t" "mov $234, %%eax\n\t" "syscall\n"   /* tgkill(me, SIGUSR1) with DF set */
        "1:\n\t"
        "mov $39, %%eax\n\t"  "syscall\n\t"          /* getpid: answered without the lock */
        "mov $110, %%eax\n\t" "syscall\n\t"          /* getppid: through the lock */
        "mov $24, %%eax\n\t"  "syscall\n\t"          /* sched_yield */
        "mov 592(%[c]), %%rcx\n"
        "2:\n\t"
        "test %%rcx, %%rcx\n\t" "jz 3f\n\t" "dec %%rcx\n\t" "jmp 2b\n"
        "3:\n\t"
        "dec %[n]\n\t" "jnz 1b\n\t"
        "pushfq\n\t" "pop %[fl]\n\t" "cld\n\t"
        "movdqu %%xmm0, 256(%[c])\n\t"  "movdqu %%xmm1, 272(%[c])\n\t"
        "movdqu %%xmm2, 288(%[c])\n\t"  "movdqu %%xmm3, 304(%[c])\n\t"
        "movdqu %%xmm4, 320(%[c])\n\t"  "movdqu %%xmm5, 336(%[c])\n\t"
        "movdqu %%xmm6, 352(%[c])\n\t"  "movdqu %%xmm7, 368(%[c])\n\t"
        "movdqu %%xmm8, 384(%[c])\n\t"  "movdqu %%xmm9, 400(%[c])\n\t"
        "movdqu %%xmm10, 416(%[c])\n\t" "movdqu %%xmm11, 432(%[c])\n\t"
        "movdqu %%xmm12, 448(%[c])\n\t" "movdqu %%xmm13, 464(%[c])\n\t"
        "movdqu %%xmm14, 480(%[c])\n\t" "movdqu %%xmm15, 496(%[c])\n\t"
        "mov %%r12, 544(%[c])\n\t" "mov %%r13, 552(%[c])\n\t"
        "mov %%r14, 560(%[c])\n\t" "mov %%r15, 568(%[c])\n\t"
        : [n] "+r"(n), [fl] "=&r"(fl)
        : [c] "r"(c)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r11", "r12", "r13", "r14", "r15",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
    if (!(fl & 0x400)) fail("the direction flag was cleared under the program");
    for (int i = 0; i < 4; i++)
        if (c->gout[i] != c->gin[i]) fail("r%d is %#llx, not %#llx", 12 + i, c->gout[i], c->gin[i]);
    for (int i = 0; i < 16; i++)
        if (memcmp(c->xin + 16 * i, c->xout + 16 * i, 16)) fail("xmm%d changed", i);
    if (sig_seen == seen) fail("the signal sent with the direction flag set never arrived");
    if (sig_df) fail("a signal handler started with the direction flag set");
}

static void op_signal(void) {
    op_name = "signal";
    u64 before = sig_seen;
    if (syscall(SYS_tgkill, getpid(), gettid(), SIGUSR1)) fail("tgkill self: errno %d", errno);
    for (int i = 0; i < 100 && sig_seen == before; i++) sched_yield();
    if (sig_seen == before) fail("a signal to myself never arrived");
    if (sig_df) fail("a signal handler started with the direction flag set");
    /* And one to a sibling thread, which may be anywhere. */
    int to = (int)pick(nthreads);
    pid_t t = __atomic_load_n(&tids[to], __ATOMIC_RELAXED);
    if (t > 0 && syscall(SYS_tgkill, getpid(), t, SIGUSR2) && errno != ESRCH) fail("tgkill thread %d: errno %d", to, errno);
}

/* ------------------------------------------------------------ the loop */

static struct { void (*f)(void); int weight; } ops[] = {
    { op_mem, 6 }, { op_malloc, 3 }, { op_fork_mem, 2 }, { op_file, 4 }, { op_shared_file, 3 },
    { op_dir, 2 }, { op_dup, 2 }, { op_pipe, 2 }, { op_exec, 1 }, { op_thread, 2 },
    { op_lock, 3 }, { op_regs, 3 }, { op_signal, 2 },
};
#define NOPS (int)(sizeof ops / sizeof ops[0])

static void *worker_thread(void *a) {
    me = (int)(long)a;
    rng = mix(seed0 * 1000003 + wid * 1009 + me) | 1;
    __atomic_store_n(&tids[me], gettid(), __ATOMIC_RELAXED);
    int sum = 0;
    for (int i = 0; i < NOPS; i++) sum += ops[i].weight;
    while (now() < deadline) {
        int r = (int)pick(sum), i = 0;
        while (r >= ops[i].weight) r -= ops[i++].weight;
        ops[i].f();
        __atomic_add_fetch(&total->ops, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&total->per_op[i], 1, __ATOMIC_RELAXED);
    }
    op_name = "kept pages at the end";
    for (int s = 0; s < KEEP; s++)
        if (kept[s].p) {
            for (u64 i = 0; i < kept[s].pages * PAGE / 8; i++)
                if (kept[s].p[i] != mix(kept[s].tag + i)) fail("word %llu changed", i);
            munmap(kept[s].p, kept[s].pages * PAGE);
        }
    __atomic_store_n(&tids[me], 0, __ATOMIC_RELAXED);
    return 0;
}

static int worker(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, 0);
    sigaction(SIGUSR2, &sa, 0);
    char p[64];
    snprintf(p, sizeof p, "/tmp/st%d.shared", wid);
    shfd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0644);
    pthread_t th[MAX_THREADS];
    int started = 1;
    for (int i = 1; i < nthreads; i++) {
        if (pthread_create(&th[i], 0, worker_thread, (void *)(long)i)) break;
        started++;
    }
    worker_thread(0);
    for (int i = 1; i < started; i++) pthread_join(th[i], 0);
    op_name = "mutex count at the end";
    u64 want = 0;
    for (int i = 0; i < MAX_THREADS; i++) want += added[i];
    if (counter != want) fail("counter %llu, threads added %llu", counter, want);
    if (shfd >= 0) { close(shfd); unlink(p); }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "-x")) return atoi(argv[2]);
    if (argc < 3) {
        fprintf(stderr, "usage: stress SEED SECONDS [PROCS [THREADS]]\n");
        return 2;
    }
    seed0 = strtoull(argv[1], 0, 0);
    secs = atoi(argv[2]);
    if (argc > 3) nprocs = atoi(argv[3]);
    if (argc > 4) nthreads = atoi(argv[4]);
    if (nprocs < 1) nprocs = 1;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > MAX_THREADS) nthreads = MAX_THREADS;
    self = argv[0];
    struct stat s;
    disk = stat("/disk", &s) == 0 && S_ISDIR(s.st_mode);
    total = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (total == MAP_FAILED) { perror("stress: mmap"); return 1; }
    double t0 = now();
    deadline = t0 + secs;
    pid_t pids[64];
    if (nprocs > 64) nprocs = 64;
    for (int i = 0; i < nprocs; i++) {
        pids[i] = fork();
        if (pids[i] < 0) { perror("stress: fork"); return 1; }
        if (pids[i] == 0) { wid = i; _exit(worker()); }
    }
    int bad = 0;
    for (int i = 0; i < nprocs; i++) {
        int st;
        if (wait_for(pids[i], &st) != pids[i]) { fprintf(stderr, "stress: waitpid: %s\n", strerror(errno)); bad = 1; continue; }
        if (!WIFEXITED(st) || WEXITSTATUS(st)) {
            if (WIFSIGNALED(st))
                fprintf(stderr, "stress: FAIL seed %llu worker %d killed by signal %d\n", (unsigned long long)seed0, i, WTERMSIG(st));
            bad = 1;
        }
    }
    printf("stress: %s seed %llu, %d processes x %d threads, %llu operations (%llu skipped) in %.1f s\n",
           bad ? "FAIL" : "ok", (unsigned long long)seed0, nprocs, nthreads,
           (unsigned long long)total->ops, (unsigned long long)total->skipped, now() - t0);
    printf("stress: per operation:");
    for (int i = 0; i < NOPS; i++) printf(" %llu", (unsigned long long)total->per_op[i]);
    printf("\n");
    return bad;
}
