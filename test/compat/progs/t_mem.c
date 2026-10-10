/* Memory: anonymous and file mappings, protection, remapping, advice. */
#include "compat.h"
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

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
    EXPECT("file for mapping", (fd = open("cm_f", O_RDWR | O_CREAT | O_TRUNC, 0644)) >= 0 && write(fd, "mapped!", 7) == 7);
    EXPECT("mmap a file MAP_PRIVATE", (q = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0)) != MAP_FAILED && memcmp(q, "mapped!", 7) == 0);
    EXPECT("mmap a file MAP_SHARED writes back", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED &&
           (q[0] = 'M', msync(q, 4096, MS_SYNC) == 0) && ({ char c = 0; pread(fd, &c, 1, 0) == 1 && c == 'M'; }));
    EXPECT("MAP_SHARED write-back at munmap", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED &&
           (q[1] = 'U', munmap(q, 4096) == 0) && ({ char c = 0; pread(fd, &c, 1, 1) == 1 && c == 'U'; }));
    EXPECT("MAP_SHARED write-back at exit, after close", ({ pid_t c = fork(); c == 0 ? ({ char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
           close(fd); m != MAP_FAILED ? (m[2] = 'X', _exit(0), 0) : (_exit(1), 0); }) :
           c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0; }) && ({ char c = 0; pread(fd, &c, 1, 2) == 1 && c == 'X'; }));
    close(fd);
    unlink("cm_f");
    EXPECT("MAP_SHARED anonymous across fork", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED &&
           ({ pid_t c = fork(); c == 0 ? (q[0] = 9, _exit(0), 0) : c > 0 && waitpid(c, &st, 0) == c && q[0] == 9; }));
    EXPECT("mlock", (q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED && mlock(q, 4096) == 0);
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
    return done();
}
