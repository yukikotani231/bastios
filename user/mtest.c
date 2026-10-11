/*
 * A musl C program that uses more of Linux: malloc (brk and mmap), memory
 * well past 2 MiB (a 16 MiB mmap block and 4 MiB more brk heap), a file
 * written with stdio and read back, and a line read from the console.
 * Prints one "mtest: ..." line per check and exits with the number that
 * failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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

int main(void) {
    int bad = 0;

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
