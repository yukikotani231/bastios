/*
 * A musl C program that uses more of Linux: malloc (brk and mmap), memory
 * well past 2 MiB (a 16 MiB mmap block and 4 MiB more brk heap), more
 * address space than the machine has memory (pages come when touched),
 * fork's copy-on-write pages, a file
 * written with stdio and read back, and a line read from the console.
 * Prints one "mtest: ..." line per check and exits with the number that
 * failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned long long tsc(void) {
    unsigned lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* Spins for about a second with the direction flag set, across many timer
 * interrupts. The kernel's own copies and fills assume DF clear, so it must
 * clear it on entry rather than run with the user's (musl's memmove sets it
 * for a backward copy). Returns whether the system survived to say so. */
static int direction_flag(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    unsigned long long t0 = tsc();
    do clock_gettime(CLOCK_MONOTONIC, &b);
    while ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) < 250000000L);
    unsigned long long quarter = tsc() - t0;
    for (int i = 0; i < 4; i++) {
        unsigned long long s = tsc();
        __asm__ volatile ("std" ::: "cc");
        while (tsc() - s < quarter) {}
        __asm__ volatile ("cld" ::: "cc");
    }
    return 1;
}

int main(int argc, char **argv) {
    int bad = 0;

    /* `mtest crash` touches a page above the heap that is not the process's,
     * to show the kernel's report of a killed process (test/shell_test.sh). */
    if (argc > 1 && strcmp(argv[1], "crash") == 0)
        return *(volatile char *)((char *)sbrk(0) + (8 << 20));

    /* Small blocks come from brk, a big one from mmap. */
    char *small = malloc(100);
    char *big = malloc(300 * 1024);
    if (!small || !big) { puts("mtest: malloc failed"); bad++; }
    else {
        memset(big, 0x5a, 300 * 1024);
        strcpy(small, "heap");
        int sum = 0;
        for (int i = 0; i < 300 * 1024; i += 4096) sum += big[i] == 0x5a;
        printf("mtest: malloc %s %d\n", small, sum);
        free(big);
        free(small);
    }

    /* More than the old 2 MiB window: every page of a 16 MiB block from
     * mmap and of 4 MiB of brk heap, each written and read back. */
    char *huge = malloc(16 << 20);
    /* musl's sbrk refuses to grow the heap, so call brk itself. */
    char *heap = (char *)syscall(SYS_brk, 0);
    char *end = (char *)syscall(SYS_brk, heap + (4 << 20));
    if (!huge || end != heap + (4 << 20)) { puts("mtest: big memory failed"); bad++; }
    else {
        int hs = 0, bs = 0;
        for (int i = 0; i < (16 << 20); i += 4096) huge[i] = (char)(i >> 12);
        for (int i = 0; i < (4 << 20); i += 4096) heap[i] = (char)(i >> 12);
        for (int i = 0; i < (16 << 20); i += 4096) hs += huge[i] == (char)(i >> 12);
        for (int i = 0; i < (4 << 20); i += 4096) bs += heap[i] == (char)(i >> 12);
        printf("mtest: big memory %d %d\n", hs, bs);
        free(huge);
    }

    /* More than the machine has: 8 GiB of mmap on a 64 MiB machine,
     * whose pages come only when first touched (and past the first GiB
     * of the window). */
    char *wide = mmap(0, (size_t)8 << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (wide == MAP_FAILED) { puts("mtest: wide mmap failed"); bad++; }
    else {
        int ws = 0;
        for (int i = 0; i < 4; i++) wide[(size_t)i << 31] = (char)(i + 1);
        for (int i = 0; i < 4; i++) ws += wide[(size_t)i << 31] == (char)(i + 1) && wide[((size_t)i << 31) + 4096] == 0;
        printf("mtest: wide %d\n", ws);
        munmap(wide, (size_t)8 << 30);
    }

    /* fork shares the pages until one side writes (copy-on-write): the
     * child's writes stay in the child. */
    char *cw = malloc(1 << 20);
    if (!cw) { puts("mtest: cow malloc failed"); bad++; }
    else {
        memset(cw, 1, 1 << 20);
        pid_t c = fork();
        if (c == 0) { memset(cw, 2, 1 << 20); _exit(cw[0] == 2 && cw[(1 << 20) - 1] == 2 ? 0 : 1); }
        int st = -1;
        waitpid(c, &st, 0);
        printf("mtest: cow %s\n", c > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 && cw[0] == 1 && cw[(1 << 20) - 1] == 1 ? "kept" : "broken");
        free(cw);
    }

    if (direction_flag()) puts("mtest: direction flag spun");

    FILE *f = fopen("/mtest.txt", "w");
    if (!f) { puts("mtest: fopen w failed"); bad++; }
    else {
        fprintf(f, "line %d\n", 42);
        fclose(f);
        char buf[32] = {0};
        f = fopen("mtest.txt", "r");
        if (!f || !fgets(buf, sizeof buf, f)) { puts("mtest: read back failed"); bad++; }
        else printf("mtest: file %s", buf);
        if (f) fclose(f);
        if (remove("mtest.txt") != 0) { puts("mtest: remove failed"); bad++; }
    }

    char line[64];
    printf("mtest: type a line\n");
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) { puts("mtest: no input"); bad++; }
    else printf("mtest: got %s", line);
    return bad;
}
