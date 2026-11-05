/* Memory: anonymous and file mappings, protection, remapping, advice. */
#include "compat.h"
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_OP(op, oparg, cmp, cmparg) \
    ((unsigned)(((op) & 0xf) << 28 | ((cmp) & 0xf) << 24 | ((oparg) & 0xfff) << 12 | ((cmparg) & 0xfff)))
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/wait.h>
#include <ftw.h>

static long memfree_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char l[128];
    long v = -1;
    while (f && fgets(l, sizeof l, f))
        if (!strncmp(l, "MemFree:", 8)) { v = atol(l + 8); break; }
    if (f) fclose(f);
    return v;
}
/* Reads every file under /disk, up to `left` bytes, so the page cache
 * fills with pages read once and its clock passes over every slot. */
static long fill_left;
static int fill_one(const char *path, const struct stat *st, int type, struct FTW *w) {
    (void)w;
    if (type != FTW_F || !S_ISREG(st->st_mode)) return 0;
    int fd = open(path, O_RDONLY);
    static char buf[65536];
    long n;
    while (fd >= 0 && fill_left > 0 && (n = read(fd, buf, sizeof buf)) > 0) fill_left -= n;
    if (fd >= 0) close(fd);
    return fill_left <= 0;
}
static char *cow_buf;
static volatile int fast_phase;
static volatile int spin_stop;
static void *spin_thread(void *a) {      /* keeps a second processor on the process's space */
    (void)a;
    while (!spin_stop) { }
    return 0;
}

/* Runs one `ret` written to page p (fresh, never touched before). */
static void run_ret(unsigned char *p) {
#if defined(__x86_64__)
    p[0] = 0xc3;
#else
    unsigned int ret = 0xd65f03c0;
    memcpy(p, &ret, 4);
    __builtin___clear_cache((char *)p, (char *)p + 4);
#endif
    ((void (*)(void))p)();
}

static char *fast_page;
/* Runs only system calls the kernel answers without its lock (getpid), so the
 * processor keeps its translations between them, until the other thread has
 * unmapped the page. */
static void *fast_thread(void *a) {
    fast_page[0] = 1;
    fast_phase = 1;
    while (fast_phase < 2) syscall(SYS_getpid);
    fast_page[0] = 2;
    return a;
}
static void *cow_thread(void *a) { cow_buf[4096 * 3] = 7; return a; }

int main(void) {
    setup();
    char *p = MAP_FAILED, *q = MAP_FAILED;
    int fd = -1, st;

    EXPECT("mmap anonymous", (p = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           (p[0] = 1, p[(1 << 20) - 1] = 2, p[0] + p[(1 << 20) - 1] == 3));
    EXPECT("mmap is zeroed", (q = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           q[0] == 0 && q[8191] == 0);
    EXPECT("munmap", munmap(q, 8192) == 0);
    EXPECT("mmap MAP_FIXED", (q = mmap(p + 65536, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0)) == p + 65536 &&
           q[0] == 0);
    EXPECT("mremap grow", (q = mremap(p, 1 << 20, 2 << 20, MREMAP_MAYMOVE)) != MAP_FAILED && q[0] == 1);
    if (q != MAP_FAILED) p = q;
    EXPECT("madvise", madvise(p, 4096, MADV_DONTNEED) == 0);
    EXPECT("madvise on a PROT_NONE reservation (V8 discards pages inside one)", (q = mmap(0, 1 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)) != MAP_FAILED &&
           madvise(q + 65536, 65536, MADV_DONTNEED) == 0 && mprotect(q + 65536, 65536, PROT_READ | PROT_WRITE) == 0 &&
           (q[65536] = 5, madvise(q, 1 << 20, MADV_DONTNEED) == 0) && q[65536] == 0 && munmap(q, 1 << 20) == 0);
    EXPECT("mprotect read-only faults on write", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           mprotect(q, 4096, PROT_READ) == 0 && ({ pid_t c = fork(); c == 0 ? (q[0] = 1, _exit(0), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));
    EXPECT("munmap faults on touch", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           munmap(q, 4096) == 0 && ({ pid_t c = fork(); c == 0 ? (q[0] = 1, _exit(0), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));
    // A translation cached while the page was mapped must not outlive a
    // system call that changes the mapping (what a TLB tagged with process
    // identifiers must still honour): touch first, change, touch again.
    EXPECT("a page touched before munmap faults after it", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           ({ pid_t c = fork(); c == 0 ? (q[0] = 1, munmap(q, 4096), q[0] = 2, _exit(0), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));
    EXPECT("a page written before mprotect(PROT_READ) faults on the next write", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           ({ pid_t c = fork(); c == 0 ? (q[0] = 1, mprotect(q, 4096, PROT_READ), q[0] = 2, _exit(0), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));
    EXPECT("a page another thread unmaps while this one makes quick system calls faults on its next touch", (fast_page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           ({ pid_t c = fork(); c == 0 ? ({ pthread_t t; fast_phase = 0;
              if (pthread_create(&t, 0, fast_thread, 0)) _exit(1);
              while (fast_phase < 1) sched_yield();
              for (int i = 0; i < 1000; i++) syscall(SYS_getpid);
              munmap(fast_page, 4096);
              fast_phase = 2;
              pthread_join(t, 0); _exit(0); 0; }) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));
    EXPECT("a page read before munmap and mapped again at its address is a fresh zero page", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           ({ q[0] = 'P'; pid_t c = fork(); c == 0 ? ({ char r = q[0]; munmap(q, 4096);
              char *n = mmap(q, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
              _exit(r == 'P' && n == q && q[0] == 0 ? 0 : 1); 0; }) :
           c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0 && q[0] == 'P'; }));
    {
        int ro = open("cm_ro", O_RDWR | O_CREAT | O_TRUNC, 0644), wo = -1, bad;
        if (ro >= 0) { write(ro, "x", 1); close(ro); }
        ro = open("cm_ro", O_RDONLY);
        wo = open("cm_ro", O_WRONLY);
        EXPECT("mmap of a file opened write-only is EACCES", wo >= 0 &&
               mmap(0, 4096, PROT_READ, MAP_PRIVATE, wo, 0) == MAP_FAILED && errno == EACCES);
        EXPECT("writable MAP_SHARED of a read-only file is EACCES", ro >= 0 &&
               mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == MAP_FAILED && errno == EACCES);
        bad = dup(ro);
        close(bad);
        EXPECT("mmap of a closed descriptor is EBADF", bad >= 0 &&
               mmap(0, 4096, PROT_WRITE, MAP_SHARED, bad, 0) == MAP_FAILED && errno == EBADF);
        close(ro); close(wo);
        unlink("cm_ro");
    }
    EXPECT("file for mapping", (fd = open("cm_f", O_RDWR | O_CREAT | O_TRUNC, 0644)) >= 0 && write(fd, "mapped!", 7) == 7);
    EXPECT("mmap a file MAP_PRIVATE", (q = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0)) != MAP_FAILED && memcmp(q, "mapped!", 7) == 0);
    EXPECT("mmap a file MAP_SHARED writes back", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED &&
           (q[0] = 'M', msync(q, 4096, MS_SYNC) == 0) && ({ char c = 0; pread(fd, &c, 1, 0) == 1 && c == 'M'; }));
    EXPECT("MAP_SHARED write-back at munmap", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED &&
           (q[1] = 'U', munmap(q, 4096) == 0) && ({ char c = 0; pread(fd, &c, 1, 1) == 1 && c == 'U'; }));
    EXPECT("MAP_SHARED write-back at exit, after close", ({ pid_t c = fork(); c == 0 ? ({ char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
           close(fd); m != MAP_FAILED ? (m[2] = 'X', _exit(0), 0) : (_exit(1), 0); }) :
           c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0; }) && ({ char c = 0; pread(fd, &c, 1, 2) == 1 && c == 'X'; }));
    EXPECT("MAP_SHARED file mapping across fork", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED &&
           ({ pid_t c = fork(); c == 0 ? (q[3] = 'F', _exit(0), 0) : c > 0 && waitpid(c, &st, 0) == c && q[3] == 'F'; }));
    EXPECT("a writable MAP_SHARED mapping is written back after seventy read-only ones", ({
               int ok = 1;
               for (int i = 0; i < 70; i++) {
                   int r = open("cm_f", O_RDONLY);
                   ok &= r >= 0 && mmap(0, 4096, PROT_READ, MAP_SHARED, r, 0) != MAP_FAILED;
                   if (r >= 0) close(r);
               }
               int g = open("cm_g", O_RDWR | O_CREAT | O_TRUNC, 0644);
               char *m = g >= 0 && ftruncate(g, 4096) == 0 ? mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, g, 0) : MAP_FAILED;
               ok &= m != MAP_FAILED;
               if (m != MAP_FAILED) { m[5] = 'W'; ok &= munmap(m, 4096) == 0; }
               char c = 0;
               ok &= g >= 0 && pread(g, &c, 1, 5) == 1 && c == 'W';
               if (g >= 0) { close(g); unlink("cm_g"); }
               ok; }));
    EXPECT("a MAP_SHARED mapping from a read-only descriptor sees what is written to the file later, in another process too, and cannot become writable", ({
               /* Firefox's shared memory: a memfd, reopened read-only through
                  /proc/self/fd, mapped by a child before the parent writes */
               int ok = 1;
               int w = memfd_create("ro_shared", MFD_CLOEXEC);
               char path[64];
               snprintf(path, sizeof path, "/proc/self/fd/%d", w);
               int r = w >= 0 && ftruncate(w, 8192) == 0 ? open(path, O_RDONLY | O_CLOEXEC) : -1;
               char *pw = r >= 0 ? mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, w, 0) : MAP_FAILED;
               volatile char *pr = r >= 0 ? mmap(0, 8192, PROT_READ, MAP_SHARED, r, 0) : MAP_FAILED;
               int go[2], back[2];
               ok &= pw != MAP_FAILED && pr != MAP_FAILED && pipe(go) == 0 && pipe(back) == 0;
               if (ok) {
                   ok &= pr[0] == 0 && pr[4096] == 0;          /* both pages there before the writes */
                   pid_t c = fork();
                   if (c == 0) {
                       char x;
                       volatile char *q = mmap(0, 8192, PROT_READ, MAP_SHARED, r, 0);
                       int good = q != MAP_FAILED && q[0] == 0 && q[4096] == 0;
                       write(back[1], "m", 1);
                       good &= read(go[0], &x, 1) == 1 && q[0] == 'a' && q[4096] == 'b';
                       good &= mprotect((void *)q, 8192, PROT_READ | PROT_WRITE) == -1 && errno == EACCES;
                       _exit(good ? 0 : 1);
                   }
                   char x;
                   ok &= c > 0 && read(back[0], &x, 1) == 1;
                   pw[0] = 'a';
                   pw[4096] = 'b';
                   ok &= write(go[1], "w", 1) == 1;
                   ok &= c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
                   ok &= pr[0] == 'a' && pr[4096] == 'b';
                   ok &= mprotect((void *)pr, 8192, PROT_READ | PROT_WRITE) == -1 && errno == EACCES;
                   close(go[0]); close(go[1]); close(back[0]); close(back[1]);
               }
               if (pw != MAP_FAILED) munmap(pw, 8192);
               if (pr != MAP_FAILED) munmap((void *)pr, 8192);
               if (r >= 0) close(r);
               if (w >= 0) close(w);
               ok; }));
    EXPECT("a memfd mapped MAP_SHARED keeps what was written through the mapping when the caches are dropped, for a later mapping in another process", ({
               /* Firefox's shared font list: the parent writes a memfd through
                  its mapping, memory runs short (the page cache is shrunk),
                  and a content process started later maps it */
               int ok = 1, st = 0;
               int w = memfd_create("kept_shared", MFD_CLOEXEC);
               char *pw = w >= 0 && ftruncate(w, 16384) == 0 ? mmap(0, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, w, 0) : MAP_FAILED;
               ok &= pw != MAP_FAILED;
               if (ok) {
                   for (int i = 0; i < 4; i++) pw[i * 4096 + 7] = 'A' + i;
                   int f = open("/proc/sys/vm/drop_caches", O_WRONLY);
                   ok &= f >= 0 && write(f, "3", 1) == 1;
                   if (f >= 0) close(f);
                   pid_t c = fork();
                   if (c == 0) {
                       volatile char *q = mmap(0, 16384, PROT_READ, MAP_SHARED, w, 0);
                       int good = q != MAP_FAILED;
                       for (int i = 0; good && i < 4; i++) good = q[i * 4096 + 7] == 'A' + i;
                       pw[7] = 'z';                         /* and it is still the same memory */
                       good &= q != MAP_FAILED && q[7] == 'z';
                       _exit(good ? 0 : 1);
                   }
                   ok &= c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
                   munmap(pw, 16384);
               }
               if (w >= 0) close(w);
               ok; }));
    EXPECT("seventy read-only MAP_SHARED mappings still read their file after a writable mapping takes a record", ({
               int ok = 1;
               char *r[70];
               for (int i = 0; i < 70; i++) {
                   int f = open("cm_f", O_RDONLY);
                   r[i] = f >= 0 ? mmap(0, 4096, PROT_READ, MAP_SHARED, f, 0) : MAP_FAILED;
                   ok &= r[i] != MAP_FAILED;
                   if (f >= 0) close(f);
               }
               int g = open("cm_g", O_RDWR | O_CREAT | O_TRUNC, 0644);
               char *m = g >= 0 && ftruncate(g, 4096) == 0 ? mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, g, 0) : MAP_FAILED;
               ok &= m != MAP_FAILED;
               if (m != MAP_FAILED) { m[0] = 'w'; munmap(m, 4096); }
               char c = 0;
               ok &= g >= 0 && pread(g, &c, 1, 0) == 1 && c == 'w';
               for (int i = 0; i < 70; i++)
                   if (r[i] != MAP_FAILED) { ok &= r[i][0] == 'M' && r[i][1] == 'U'; munmap(r[i], 4096); }
               if (g >= 0) { close(g); unlink("cm_g"); }
               ok; }));
    EXPECT("forty writable MAP_SHARED mappings are each written back", ({
               int ok = 1;
               char *m[40];
               int g[40];
               char name[16];
               for (int i = 0; i < 40; i++) {
                   snprintf(name, sizeof name, "cm_w%d", i);
                   g[i] = open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
                   m[i] = g[i] >= 0 && ftruncate(g[i], 4096) == 0 ? mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, g[i], 0) : MAP_FAILED;
                   ok &= m[i] != MAP_FAILED;
                   if (m[i] != MAP_FAILED) m[i][i] = 'a' + i % 26;
               }
               for (int i = 0; i < 40; i++) {
                   if (m[i] != MAP_FAILED) ok &= munmap(m[i], 4096) == 0;
                   char c = 0;
                   ok &= g[i] >= 0 && pread(g[i], &c, 1, i) == 1 && c == 'a' + i % 26;
                   if (g[i] >= 0) close(g[i]);
                   snprintf(name, sizeof name, "cm_w%d", i);
                   unlink(name);
               }
               ok; }));
    {
        /* Two processes that map the file on their own share its pages,
           and read(2) and write(2) agree with the mapping. */
        pid_t c = fork();
        if (c == 0) {
            int g = open("cm_f", O_RDWR);
            char *m = g >= 0 ? mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, g, 0) : MAP_FAILED;
            if (m == MAP_FAILED) _exit(1);
            m[4] = 'C';
            for (int i = 0; i < 3000000 && m[5] != 'P'; i++) sched_yield();
            _exit(0);
        }
        char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        int ok = c > 0 && m != MAP_FAILED;
        for (int i = 0; ok && i < 3000000 && m[4] != 'C'; i++) sched_yield();
        EXPECT("MAP_SHARED file mapped by two processes", ok && m[4] == 'C');
        if (ok) m[5] = 'P';
        ok = ok && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        char b = 0;
        EXPECT("read(2) sees a store into a MAP_SHARED mapping", ok && (m[6] = 'R', pread(fd, &b, 1, 6) == 1) && b == 'R');
        EXPECT("a MAP_SHARED mapping sees write(2)", ok && pwrite(fd, "W", 1, 7) == 1 && m[7] == 'W');
        if (m != MAP_FAILED) munmap(m, 4096);
    }
    close(fd);
    unlink("cm_f");
    EXPECT("MAP_SHARED anonymous across fork", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           ({ pid_t c = fork(); c == 0 ? (q[0] = 9, _exit(0), 0) : c > 0 && waitpid(c, &st, 0) == c && q[0] == 9; }));
    {
        /* The child sleeps on a futex in a MAP_SHARED page; the parent's
           FUTEX_WAKE through its own mapping of the page must reach it. */
        volatile int *w = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int ok = w != MAP_FAILED;
        if (ok) {
            w[0] = 0; w[1] = 0;
            pid_t c = fork();
            if (c == 0) {
                struct timespec ts = {3, 0};
                w[1] = 1;
                long r = syscall(SYS_futex, (int *)w, FUTEX_WAIT, 0, &ts, 0, 0);
                _exit(r == 0 || (r < 0 && errno == EAGAIN && w[0] == 1) ? 0 : 1);
            }
            while (c > 0 && w[1] == 0) sched_yield();
            struct timespec d = {0, 100000000};
            nanosleep(&d, 0);
            w[0] = 1;
            syscall(SYS_futex, (int *)w, FUTEX_WAKE, 1, 0, 0, 0);
            ok = c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        }
        EXPECT("futex across fork on a MAP_SHARED page", ok);
    }
    {
        /* Timed waiters in four processes: one woken, three requeued to
           another word, and those three found there after they have each
           polled their timeout again. */
        volatile int *w = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int ok = w != MAP_FAILED, kids = 0;
        pid_t c[4];
        if (ok) {
            w[0] = 0; w[1] = 0; w[2] = 0;
            for (int i = 0; i < 4; i++) {
                c[i] = fork();
                if (c[i] == 0) {
                    struct timespec ts = {5, 0};
                    __sync_fetch_and_add(&w[2], 1);
                    long r = syscall(SYS_futex, (int *)w, FUTEX_WAIT, 0, &ts, 0, 0);
                    _exit(r == 0 ? 0 : 1);
                }
                kids += c[i] > 0;
            }
            for (int i = 0; i < 3000000 && w[2] < kids; i++) sched_yield();
            struct timespec d = {0, 200000000};
            nanosleep(&d, 0);
            long a = syscall(SYS_futex, (int *)w, FUTEX_CMP_REQUEUE, 1, 10, (int *)w + 1, 0);
            nanosleep(&d, 0);
            long b = syscall(SYS_futex, (int *)w + 1, FUTEX_WAKE, 10, 0, 0, 0);
            ok = kids == 4 && a == 4 && b == 3;
            for (int i = 0; i < 4; i++)
                ok &= c[i] > 0 && waitpid(c[i], &st, 0) == c[i] && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        }
        EXPECT("FUTEX_CMP_REQUEUE moves timed waiters across processes", ok);
        EXPECT("FUTEX_CMP_REQUEUE with a negative count is EINVAL", ok &&
               syscall(SYS_futex, (int *)w, FUTEX_CMP_REQUEUE, 1, -1, (int *)w + 1, 0) == -1 && errno == EINVAL);
        int *gone = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        EXPECT("FUTEX_WAKE on an unmapped word is EFAULT", gone != MAP_FAILED && munmap(gone, 4096) == 0 &&
               syscall(SYS_futex, gone, FUTEX_WAKE, 1, 0, 0, 0) == -1 && errno == EFAULT);
    }
    {
        /* FUTEX_WAKE_OP, the call musl's condition variables and Qt use:
           it changes the word at addr2, then wakes waiters of addr and, if
           the old word compares, of addr2. */
        volatile int *w = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int ok = w != MAP_FAILED;
        if (ok) {
            w[0] = 0; w[1] = 5; w[2] = 0;
            long r1 = syscall(SYS_futex, (int *)w, FUTEX_WAKE_OP, 1, 1, (int *)w + 1, FUTEX_OP(1, 3, 0, 5));  /* w1 += 3 */
            ok = r1 == 0 && w[1] == 8;
            long r2 = syscall(SYS_futex, (int *)w, FUTEX_WAKE_OP, 1, 1, (int *)w + 1, FUTEX_OP(2 | 8, 4, 1, 0));  /* w1 |= 1 << 4 */
            ok = ok && r2 == 0 && w[1] == 24;
            long r3 = syscall(SYS_futex, (int *)w, FUTEX_WAKE_OP, 1, 1, (int *)w + 1, FUTEX_OP(3, 8, 0, 0));  /* w1 &= ~8 */
            ok = ok && r3 == 0 && w[1] == 16;
            long r4 = syscall(SYS_futex, (int *)w, FUTEX_WAKE_OP, 1, 1, (int *)w + 1, FUTEX_OP(4, -1, 5, -1));  /* w1 ^= -1 */
            ok = ok && r4 == 0 && w[1] == ~16;
        }
        EXPECT("FUTEX_WAKE_OP changes the second word", ok);
        EXPECT("FUTEX_WAKE_OP with an unknown operation is ENOSYS", ok &&
               syscall(SYS_futex, (int *)w, FUTEX_WAKE_OP, 1, 1, (int *)w + 1, FUTEX_OP(7, 1, 0, 0)) == -1 && errno == ENOSYS);
        pid_t c = -1;
        long a = -1, b = -1;
        if (ok) {
            w[0] = 0; w[1] = 0; w[2] = 0;
            c = fork();
            if (c == 0) {
                struct timespec ts = {5, 0};
                __sync_fetch_and_add(&w[2], 1);
                long r = syscall(SYS_futex, (int *)w + 1, FUTEX_WAIT, 0, &ts, 0, 0);
                _exit(r == 0 ? 0 : 1);
            }
            for (int i = 0; i < 3000000 && w[2] < 1; i++) sched_yield();
            struct timespec d = {0, 200000000};
            nanosleep(&d, 0);
            /* wake nobody on w[0]; set w[1] to 1 and, as the old value is
               0 == 0, wake the child that waits on w[1] */
            a = syscall(SYS_futex, (int *)w, FUTEX_WAKE_OP, 1, 1, (int *)w + 1, FUTEX_OP(0, 1, 0, 0));
            b = c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        }
        EXPECT("FUTEX_WAKE_OP wakes a waiter of the second word when the old value compares", c > 0 && a == 1 && b == 1 && w[1] == 1);
    }
#ifdef __x86_64__
    {
        /* arch_prctl(ARCH_SET_GS), which wasm2c's "segue" (Firefox's sandboxed libraries) uses: the base
           %gs addresses with is kept per process, across switches to a process with another one. */
        static long mine[2] = {0x1234, 0}, other[2] = {0x5678, 0};
        unsigned long got = 0;
        long v = 0, w = 0;
        long r = syscall(SYS_arch_prctl, 0x1001, mine);
        __asm__ volatile ("movq %%gs:0, %0" : "=r"(v));
        pid_t c = fork();
        if (c == 0) {
            syscall(SYS_arch_prctl, 0x1001, other);
            long x = 0;
            for (int i = 0; i < 50; i++) sched_yield();
            __asm__ volatile ("movq %%gs:0, %0" : "=r"(x));
            _exit(x == 0x5678 ? 0 : 1);
        }
        for (int i = 0; i < 50; i++) sched_yield();
        __asm__ volatile ("movq %%gs:0, %0" : "=r"(w));
        int cst = 1;
        int ok = c > 0 && waitpid(c, &cst, 0) == c && WIFEXITED(cst) && WEXITSTATUS(cst) == 0;
        EXPECT("ARCH_SET_GS sets the base %gs addresses with, and a process keeps its own", r == 0 && v == 0x1234 && w == 0x1234 && ok);
        EXPECT("ARCH_GET_GS gives it back", syscall(SYS_arch_prctl, 0x1004, &got) == 0 && got == (unsigned long)mine);
        EXPECT("ARCH_SET_GS with a base that is not canonical is EPERM",
               syscall(SYS_arch_prctl, 0x1001, 0x8000000000000000UL) == -1 && errno == EPERM);
    }
#endif
    EXPECT("mlock", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED && mlock(q, 4096) == 0);
    {
        int ok = 1;
        for (int i = 0; i < 200 && ok; i++) {
            char *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            ok = m != MAP_FAILED && m[0] == 0 && m[(1 << 20) - 1] == 0;
            if (ok) { memset(m, 0xAA, 1 << 20); ok = munmap(m, 1 << 20) == 0; }
        }
        EXPECT("munmap gives address space back", ok);
        for (int i = 0; i < 200 && ok; i++) {
            char *m = malloc(600000);
            ok = m != NULL;
            if (ok) { m[0] = m[599999] = 1; free(m); }
        }
        EXPECT("large malloc and free, many times", ok);
        /* A buffer that doubles by mremap, as realloc grows a big one. If
         * each move took fresh address space, a round would use 63 MiB and
         * the rounds together far more than the window has. */
        for (int r = 0; r < 6 && ok; r++) {
            char *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            ok = m != MAP_FAILED;
            for (size_t n = 1 << 20; ok && n < (32 << 20); n *= 2) {
                m[n - 1] = 7;
                char *g = mremap(m, n, 2 * n, MREMAP_MAYMOVE);
                ok = g != MAP_FAILED && g[n - 1] == 7;
                if (ok) m = g;
            }
            if (ok) ok = munmap(m, 32 << 20) == 0;
        }
        EXPECT("mremap growth reuses address space", ok);
    }
    {
        void *a[64]; int ok = 1;
        for (int i = 0; i < 64; i++) { a[i] = malloc(1000 + i * 3000); ok &= a[i] != NULL; if (a[i]) memset(a[i], i, 1000 + i * 3000); }
        for (int i = 0; i < 64; i += 2) free(a[i]);
        for (int i = 1; i < 64; i += 2) { void *r = realloc(a[i], 50000); ok &= r != NULL; a[i] = r; }
        for (int i = 1; i < 64; i += 2) free(a[i]);
        EXPECT("malloc churn", ok);
    }
    EXPECT("64 MiB of address space", (q = mmap(0, 48 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           (q[0] = 1, q[(48 << 20) - 1] = 1, 1));
    EXPECT("more than the old 64 MiB window: a 96 MiB mmap, then a 100 MiB malloc", ({
           char *r = mmap(0, 96 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int ok = r != MAP_FAILED;
           if (ok) { r[0] = 1; r[(96 << 20) - 1] = 2; ok = r[0] == 1 && r[(96 << 20) - 1] == 2; munmap(r, 96 << 20); }
           char *b = malloc(100 << 20);
           if (b) { b[0] = 3; b[(100 << 20) - 1] = 4; ok = ok && b[(100 << 20) - 1] == 4; free(b); }
           ok && b != 0; }));
    EXPECT("an 8 MiB stack: 6 MiB of locals in a function", ({
           pid_t c = fork();
           if (c == 0) { volatile char big[6 << 20]; big[0] = 1; big[sizeof big - 1] = 2; _exit(big[0] + big[sizeof big - 1] == 3 ? 0 : 1); }
           int st; waitpid(c, &st, 0);
           WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("pages come when first touched: 640 MiB of mmap, system calls and fork on pages never touched", ({
           size_t n = (size_t)640 << 20;
           char *r = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int ok = r != MAP_FAILED, pf[2] = { -1, -1 };
           if (ok) {
               r[0] = 1; r[n - 1] = 2;
               char z[4] = { 9, 9, 9, 9 };
               ok = pipe(pf) == 0 && write(pf[1], "abc", 3) == 3 && read(pf[0], r + (300 << 20), 3) == 3 && r[300 << 20] == 'a' &&
                    write(pf[1], r + (400 << 20), 4) == 4 && read(pf[0], z, 4) == 4 && z[0] == 0 && z[3] == 0;
               ok = ok && munmap(r + (100 << 20), 4096) == 0 && r[(100 << 20) + 4096] == 0 && r[(100 << 20) - 1] == 0;
               pid_t c = fork();
               if (c == 0) { r[500 << 20] = 5; _exit(r[0] == 1 && r[n - 1] == 2 && r[500 << 20] == 5 ? 0 : 1); }
               int st2; waitpid(c, &st2, 0);
               ok = ok && WIFEXITED(st2) && WEXITSTATUS(st2) == 0 && r[500 << 20] == 0;
               munmap(r, n);
               close(pf[0]); close(pf[1]);
           }
           ok; }));
    EXPECT("fork: parent and child each see only their own writes (copy-on-write), read(2) into a shared page, a second thread, mprotect and MADV_DONTNEED", ({
           size_t n = 64 * 4096;
           char *r = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int ok = r != MAP_FAILED, pf[2] = { -1, -1 };
           if (ok) {
               for (size_t i = 0; i < n; i += 4096) r[i] = 1;
               ok = pipe(pf) == 0;
               pid_t c = fork();
               if (c == 0) {
                   int bad = 0;
                   r[0] = 2;                                      /* a write fault copies the page */
                   bad |= r[4096] != 1;
                   bad |= read(pf[0], r + 4096 * 2, 3) != 3 || r[4096 * 2] != 'a';   /* the kernel writes a shared page */
                   cow_buf = r; pthread_t t;                      /* a second thread copies the rest first */
                   bad |= pthread_create(&t, 0, cow_thread, 0) != 0 || pthread_join(t, 0) != 0 || r[4096 * 3] != 7;
                   bad |= mprotect(r + 4096 * 4, 4096, PROT_READ) != 0 || r[4096 * 4] != 1;
                   bad |= madvise(r + 4096 * 5, 4096, MADV_DONTNEED) != 0 || r[4096 * 5] != 0;
                   _exit(bad);
               }
               ok = ok && write(pf[1], "abc", 3) == 3;
               int st2 = -1; waitpid(c, &st2, 0);
               ok = ok && WIFEXITED(st2) && WEXITSTATUS(st2) == 0;
               /* None of the child's writes reached the parent; the parent's own still go in. */
               ok = ok && r[0] == 1 && r[4096 * 2] == 1 && r[4096 * 3] == 1 && r[4096 * 5] == 1;
               r[4096 * 6] = 9;
               ok = ok && r[4096 * 6] == 9 && r[4096 * 7] == 1;
               munmap(r, n);
               close(pf[0]); close(pf[1]);
           }
           ok; }));
    EXPECT("a big window: 1.2 GiB of PROT_NONE reservations as the Go runtime makes, then 4 GiB of mmap touched at both ends", ({
           size_t sz[6] = { 128 << 10, 1 << 20, 8 << 20, 64 << 20, 512 << 20, 512 << 20 };
           void *res[6]; int ok = 1;
           for (int i = 0; i < 6; i++) {
               res[i] = mmap(0, sz[i], PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
               ok = ok && res[i] != MAP_FAILED;
           }
           size_t n = (size_t)4 << 30;
           char *r = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           ok = ok && r != MAP_FAILED;
           if (r != MAP_FAILED) {
               r[0] = 1; r[n - 1] = 2;
               ok = ok && r[0] == 1 && r[n - 1] == 2 && r[n / 2] == 0;
               munmap(r, n);
           }
           for (int i = 0; i < 6; i++) if (res[i] != MAP_FAILED) munmap(res[i], sz[i]);
           ok; }));
    EXPECT("pages come when first touched even after 100 separate untouched mappings (a browser's thread stacks and heap chunks): then 4 GiB of mmap touched at both ends", ({
           enum { NMAP = 100 };
           char *small[NMAP], *sep[NMAP]; int ok = 1;
           for (int i = 0; i < NMAP; i++) {         /* a read-only page after each keeps any two of them from touching */
               small[i] = mmap(0, 64 << 10, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
               sep[i] = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
               ok = ok && small[i] != MAP_FAILED && sep[i] != MAP_FAILED;
           }
           size_t n = (size_t)4 << 30;
           char *r = ok ? mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : MAP_FAILED;
           ok = ok && r != MAP_FAILED;
           if (r != MAP_FAILED) {
               r[0] = 1; r[n - 1] = 2;
               ok = ok && r[0] == 1 && r[n - 1] == 2 && r[n / 2] == 0;
               munmap(r, n);
           }
           for (int i = 0; i < NMAP; i++) {
               if (small[i] != MAP_FAILED) {
                   ok = ok && small[i][0] == 0 && small[i][(64 << 10) - 1] == 0;
                   small[i][4096] = 7;
                   ok = ok && small[i][4096] == 7;
                   munmap(small[i], 64 << 10);
               }
               if (sep[i] != MAP_FAILED) munmap(sep[i], 4096);
           }
           ok; }));
    EXPECT("memory that may run code stays runnable when its record gives up its slot to a later mmap, while another thread runs on the other processor", ({
           pid_t c = fork();
           if (c == 0) {
               enum { NX = 100 };
               unsigned char *x[NX]; char *sep[NX];
               pthread_t t; spin_stop = 0;
               if (pthread_create(&t, 0, spin_thread, 0)) _exit(2);
               for (int i = 0; i < NX; i++) {     /* untouched, each kept apart by a read-only page */
                   x[i] = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                   sep[i] = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                   if (x[i] == MAP_FAILED || sep[i] == MAP_FAILED) _exit(3);
               }
               for (int i = 0; i < NX; i++) {     /* larger mappings take the slots of the one-page ones */
                   char *r = mmap(0, 64 << 10, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                   if (r == MAP_FAILED) _exit(4);
               }
               for (int i = 0; i < NX; i++) run_ret(x[i]);
               spin_stop = 1; pthread_join(t, 0);
               _exit(0);
           }
           int st = 0;
           c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("pages come when first touched even after 400 scattered pages were handed back with MADV_DONTNEED (jemalloc's purge): then 4 GiB of mmap touched at both ends, and the pages between keep what they hold", ({
           size_t m = 16 << 20;
           unsigned char *h = mmap(0, m, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int ok = h != MAP_FAILED;
           if (ok) {
               for (size_t i = 0; i < m; i += 4096) h[i] = (unsigned char)(i / 4096 % 251 + 1);
               for (size_t i = 0; ok && i < 400; i++) ok = madvise(h + i * 8 * 4096, 4096, MADV_DONTNEED) == 0;
               size_t n = (size_t)4 << 30;
               char *r = ok ? mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : MAP_FAILED;
               ok = ok && r != MAP_FAILED;
               if (r != MAP_FAILED) {
                   r[0] = 1; r[n - 1] = 2;
                   ok = ok && r[0] == 1 && r[n - 1] == 2 && r[n / 2] == 0;
                   munmap(r, n);
               }
               for (size_t i = 0; ok && i < m; i += 4096) {
                   size_t pg = i / 4096;
                   ok = h[i] == (pg % 8 == 0 && pg < 3200 ? 0 : (unsigned char)(pg % 251 + 1));
               }
               munmap(h, m);
           }
           ok; }));
    EXPECT("mmap takes a free address hint and grows from it, as the Go runtime's heap arenas do", ({
           char *want = (char *)0x4000000000ul;      /* 256 GiB: the user address space is 512 GiB from 0 */
           char *a = mmap(want, 64 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           char *b = mmap(want + (64 << 20), 64 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int ok = a == want && b == want + (64 << 20);
           /* made usable piece by piece, as Go does */
           ok = ok && mmap(a, 1 << 20, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == a;
           if (ok) { a[0] = 3; a[(1 << 20) - 1] = 4; ok = a[0] == 3 && a[(1 << 20) - 1] == 4; }
           /* a hint on memory in use is not taken */
           char *c = mmap(want, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           ok = ok && c != MAP_FAILED && c != want;
           if (c != MAP_FAILED) munmap(c, 4096);
           if (a != MAP_FAILED) munmap(a, 64 << 20);
           if (b != MAP_FAILED) munmap(b, 64 << 20);
           ok; }));
    /* Firefox places its read-only shared memory as follows: it reserves
       16 GiB, unmaps it, maps the memfd read-write in the middle of that
       range, unmaps it again and maps it read-only at the same address,
       which it checks. The hints must be heard while another thread of the
       process runs on the other processor. */
    EXPECT("a shared file mapping takes an address hint in a range just unmapped, and again read-only, while another thread runs on the other processor", ({
           int ok = 0;
           pid_t c = fork();
           if (c == 0) {
               int sf = open("mem_hint", O_RDWR | O_CREAT | O_TRUNC, 0600);
               if (sf < 0 || ftruncate(sf, 112 * 4096) != 0) _exit(2);
               pthread_t t; spin_stop = 0;
               if (pthread_create(&t, 0, spin_thread, 0)) _exit(3);
               int bad = 0;
               for (int i = 0; i < 20; i++) {
                   char *g = mmap(0, 1UL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                   if (g == MAP_FAILED) _exit(4);
                   munmap(g, 1UL << 30);
                   usleep(5000);                    /* the other thread runs again */
                   char *want = g + (512 << 20);
                   char *w = mmap(want, 112 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, sf, 0);
                   if (w == MAP_FAILED) _exit(5);
                   if (w != want) bad |= 1;
                   w[5] = (char)i;
                   munmap(w, 112 * 4096);
                   usleep(5000);
                   char *r = mmap(w, 112 * 4096, PROT_READ, MAP_SHARED, sf, 0);
                   if (r != w || r[5] != (char)i) bad |= 2;
                   if (r != MAP_FAILED) munmap(r, 112 * 4096);
               }
               spin_stop = 1; pthread_join(t, 0);
               close(sf); unlink("mem_hint");
               _exit(bad ? 8 + bad : 0);
           }
           int ws;
           if (c > 0 && waitpid(c, &ws, 0) == c) ok = WIFEXITED(ws) && WEXITSTATUS(ws) == 0;
           if (!ok && c > 0) printf("  (child status %d)\n", WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
           ok; }));
    /* Twelve MAP_SHARED mappings of one file at once: each is written back. */
    EXPECT("twelve shared file mappings are all written back", ({
           int sf = open("mem_shared", O_RDWR | O_CREAT | O_TRUNC, 0600);
           int ok = sf >= 0 && ftruncate(sf, 12 * 4096) == 0;
           char *mp[12];
           for (int i = 0; ok && i < 12; i++) {
               mp[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, sf, i * 4096);
               ok = mp[i] != MAP_FAILED;
               if (ok) mp[i][0] = 'a' + i;
           }
           for (int i = 0; ok && i < 12; i++) ok = msync(mp[i], 4096, MS_SYNC) == 0;
           for (int i = 0; i < 12; i++) if (ok) munmap(mp[i], 4096);
           for (int i = 0; ok && i < 12; i++) {
               char c = 0;
               ok = pread(sf, &c, 1, i * 4096) == 1 && c == 'a' + i;
           }
           if (sf >= 0) close(sf);
           unlink("mem_shared");
           ok; }));
    for (int x = 0; x < 2; x++)
    EXPECT(x ? "mprotect makes 300 MiB of a PROT_NONE reservation readable, writable and executable (a JIT's code range), more than the machine has: only touched pages count, and code written there runs"
             : "mprotect makes 300 MiB of a PROT_NONE reservation writable, more than the machine has: only touched pages count", ({
           int rwx = PROT_READ | PROT_WRITE | (x ? PROT_EXEC : 0);
           size_t sz = 300u << 20;
           char *r = mmap(0, 512u << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
           int ok = r != MAP_FAILED && mprotect(r, sz, rwx) == 0;
           if (ok) {
               r[0] = 1; r[sz / 2] = 2; r[sz - 1] = 3;
               ok = r[0] == 1 && r[sz / 2] == 2 && r[sz - 1] == 3 && r[4096] == 0;
#if defined(__x86_64__) || defined(__aarch64__)
               if (ok && x) {
#ifdef __x86_64__
                   static const unsigned char code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   /* mov eax, 42; ret */
#else
                   static const unsigned char code[] = {0x40, 0x05, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};   /* mov w0, #42; ret */
#endif
                   char *c = r + sz / 4;   /* a page not touched yet */
                   memcpy(c, code, sizeof code);
                   __builtin___clear_cache(c, c + sizeof code);
                   ok = ((int (*)(void))(void *)c)() == 42;
               }
#endif
               ok = ok && mprotect(r + 4096, 8192, PROT_NONE) == 0 && mprotect(r + 4096, 8192, PROT_READ | PROT_WRITE) == 0 && r[4096] == 0;
               ok = ok && munmap(r, 512u << 20) == 0;
           }
           ok; }));
    /* /proc/PID/status of a process another processor is running: the count
     * of its pages cannot be taken then, and VmRSS must not turn into the
     * size of the whole window (8 TiB), which top and ps would show. */
    EXPECT("VmRSS of a process running on another processor is not the size of the address space", ({
           volatile int *stop = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
           pid_t c = fork();
           if (c == 0) {
               while (!*stop) { }
               _exit(0);
           }
           int ok = c > 0;
           for (int i = 0; ok && i < 200; i++) {
               char path[64], buf[2048];
               snprintf(path, sizeof path, "/proc/%d/status", (int)c);
               int fd = open(path, O_RDONLY);
               ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
               if (fd >= 0) close(fd);
               if (n <= 0) { ok = 0; break; }
               buf[n] = 0;
               const char *r = strstr(buf, "VmRSS:"), *z = strstr(buf, "VmSize:");
               ok = r && z && atol(r + 6) < (1l << 20) && atol(z + 7) < (1l << 30);   /* kB: under 1 GiB, and 1 TiB */
               sched_yield();
           }
           *stop = 1;
           if (c > 0) waitpid(c, 0, 0);
           ok; }));
    /* MADV_DONTNEED gives private pages back: jemalloc (Firefox) and glibc's
     * malloc hand memory back this way, and a kernel that kept the pages made
     * Firefox grow to gigabytes. */
    EXPECT("MADV_DONTNEED gives the memory of private writable pages back, which read as zeros and work again", ({
           size_t sz = 64u << 20;
           long free0 = memfree_kb();
           char *p = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int ok = p != MAP_FAILED;
           if (ok) {
               for (size_t i = 0; i < sz; i += 4096) p[i] = 1;
               long used = free0 - memfree_kb();
               ok = used > 56 * 1024;                       /* the pages are in memory */
               ok = ok && madvise(p, sz, MADV_DONTNEED) == 0;
               long back = memfree_kb() - (free0 - used);
               ok = ok && back > 56 * 1024;                 /* and nearly all come back */
               ok = ok && p[0] == 0 && p[sz / 2] == 0 && p[sz - 4096] == 0;
               for (size_t i = 0; i < sz; i += 4096) p[i] = 2;   /* they work again */
               ok = ok && p[4096] == 2 && p[sz - 4096] == 2;
               pid_t c = fork();                            /* a copy-on-write page too */
               if (c == 0) {
                   madvise(p, sz, MADV_DONTNEED);
                   _exit(p[8192] == 0 && p[sz - 4096] == 0 ? 0 : 1);
               }
               int st = 0;
               ok = ok && c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0 && p[8192] == 2;
               ok = ok && munmap(p, sz) == 0;
           }
           ok; }));
    /* Xvfb -fbdir writes its screen file with write(2) and then maps it
     * MAP_SHARED; a compositor maps the file later to show the screen. The
     * written pages were cached as pages a read had brought in, which the
     * cache's clock takes back: the later mapping got a page of its own
     * and showed a blank screen. */
    EXPECT("a file written with write(2) and mapped MAP_SHARED is the same memory to a mapping made after the page cache has filled up", ({
           int sf = open("/disk/mem_coh", O_RDWR | O_CREAT | O_TRUNC, 0600);
           static char z[65536];
           int ok = sf >= 0 && write(sf, z, sizeof z) == sizeof z;
           char *a = ok ? mmap(NULL, sizeof z, PROT_READ | PROT_WRITE, MAP_SHARED, sf, 0) : MAP_FAILED;
           ok = a != MAP_FAILED;
           for (int r = 0; ok && r < 2; r++) {
               fill_left = 96l << 20;
               nftw("/disk", fill_one, 16, FTW_PHYS);
           }
           if (ok) memset(a, 'Q', sizeof z);
           char *b = ok ? mmap(NULL, sizeof z, PROT_READ, MAP_SHARED, sf, 0) : MAP_FAILED;
           int seen = 0;
           for (size_t i = 0; b != MAP_FAILED && i < sizeof z; i += 4096) seen += b[i] == 'Q';
           if (b != MAP_FAILED) munmap(b, sizeof z);
           if (a != MAP_FAILED) munmap(a, sizeof z);
           if (sf >= 0) close(sf);
           unlink("/disk/mem_coh");
           ok && seen == 16; }));
    return done();
}
