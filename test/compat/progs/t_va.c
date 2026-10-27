/* The address space is 8 TiB, in 512 GiB segments (one level-3 table each):
 * mappings above the first segment, and system calls and copies that cross
 * the boundary between two. */
#define _GNU_SOURCE
#include "compat.h"
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define SEG (1UL << 39)   /* 512 GiB */
#define MAPF MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED

int main(void) {
    setup();
    int st;
    char *p = MAP_FAILED, *q = MAP_FAILED;

    EXPECT("a 1 TiB PROT_NONE reservation", (p = mmap(0, 1UL << 40, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)) != MAP_FAILED);
    EXPECT("pages in it, in two segments, work", p != MAP_FAILED && mprotect(p + 4096, 8192, PROT_READ | PROT_WRITE) == 0 &&
           mprotect(p + (1UL << 40) - 8192, 4096, PROT_READ | PROT_WRITE) == 0 &&
           (p[4096] = 1, p[(1UL << 40) - 8192] = 2, p[4096] + p[(1UL << 40) - 8192] == 3));
    EXPECT("a page between them is still PROT_NONE", p != MAP_FAILED && ({ pid_t c = fork(); c == 0 ? (p[1UL << 39] = 1, _exit(0), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));
    EXPECT("the reservation is given back", p != MAP_FAILED && munmap(p, 1UL << 40) == 0);

    /* Two pages that meet at a multiple of 512 GiB. */
    char *b = (char *)(3 * SEG);
    EXPECT("a fixed mapping across a segment boundary", (q = mmap(b - 4096, 8192, PROT_READ | PROT_WRITE, MAPF, -1, 0)) == b - 4096);
    EXPECT("an 8-byte word across it", q == b - 4096 && (*(volatile unsigned long *)(b - 4) = 0x1122334455667788UL, *(volatile unsigned long *)(b - 4) == 0x1122334455667788UL));
    int fd[2];
    EXPECT("pipe", pipe(fd) == 0);
    EXPECT("write() from a buffer across it", ({ for (int i = 0; i < 16; i++) b[i - 8] = 'a' + i; write(fd[1], b - 8, 16) == 16; }));
    EXPECT("read() into a buffer across it", ({ read(fd[0], b - 8, 16) == 16; }));
    EXPECT("what was written comes back across it", ({ int ok = 1; for (int i = 0; i < 16; i++) ok &= b[i - 8] == 'a' + i; ok; }));
    EXPECT("mprotect across it", mprotect(b - 4096, 8192, PROT_READ) == 0 && mprotect(b - 4096, 8192, PROT_READ | PROT_WRITE) == 0);
    EXPECT("madvise across it", madvise(b - 4096, 8192, MADV_DONTNEED) == 0 && b[-1] == 0 && b[0] == 0);
    EXPECT("a fork copies both sides of it", ({ b[-1] = 5; b[0] = 6; pid_t c = fork(); c == 0 ? (b[-1] = 9, b[0] = 9, _exit(b[-1] + b[0] == 18 ? 0 : 1), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0 && b[-1] == 5 && b[0] == 6; }));
    EXPECT("mremap moves it to another segment", ({ char *d = mremap(b - 4096, 8192, 8192, MREMAP_MAYMOVE | MREMAP_FIXED, (char *)(5 * SEG + 4096)); d == (char *)(5 * SEG + 4096) && d[4095] == 5 && d[4096] == 6; }));
    EXPECT("munmap across it", munmap((char *)(5 * SEG + 4096), 8192) == 0);

    /* Growing a mapping across a boundary. */
    EXPECT("two adjacent fixed mappings in different segments", mmap(b, 4096, PROT_READ | PROT_WRITE, MAPF, -1, 0) == b &&
           mmap(b - 4096, 4096, PROT_READ | PROT_WRITE, MAPF, -1, 0) == b - 4096);
    EXPECT("munmap over both", munmap(b - 4096, 8192) == 0);
    EXPECT("each is gone", ({ pid_t c = fork(); c == 0 ? (b[0] = 1, _exit(0), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }));

    /* Page tables of every segment are given back at exit: map in each, then
     * let the process end (the leak check of the shell test sees the rest). */
    EXPECT("a page in every segment", ({ int ok = 1; for (int s = 1; s < 16 && ok; s++) { char *a = (char *)((unsigned long)s * SEG + (1UL << 30)); ok = mmap(a, 4096, PROT_READ | PROT_WRITE, MAPF, -1, 0) == a && (a[0] = s, a[0] == s); } ok; }));
    EXPECT("a child sees them copied", ({ pid_t c = fork(); c == 0 ? ({ int ok = 1; for (int s = 1; s < 16; s++) ok &= ((char *)((unsigned long)s * SEG + (1UL << 30)))[0] == s; _exit(ok ? 0 : 1); 0; }) :
           c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    return done();
}
