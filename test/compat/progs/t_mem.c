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
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/wait.h>

static char *cow_buf;
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
    EXPECT("mprotect read-only faults on write", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           mprotect(q, 4096, PROT_READ) == 0 && ({ pid_t c = fork(); c == 0 ? (q[0] = 1, _exit(0), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));
    EXPECT("munmap faults on touch", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           munmap(q, 4096) == 0 && ({ pid_t c = fork(); c == 0 ? (q[0] = 1, _exit(0), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));
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
    return done();
}
