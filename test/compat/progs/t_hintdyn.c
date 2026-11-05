/* A dynamically linked program (the only one among these checks): the
 * kernel maps its interpreter, musl's ld-musl (libc itself), below the
 * area mmap hands out. A hint far below that area (the JVM asks for its
 * compressed-oops heap at a few GiB) must not make the span above it,
 * libc's own pages included, free address space: a later mapping placed
 * there replaced libc's code and data, and musl's malloc found its state
 * broken (a SIGSEGV at a hlt in ld-musl). */
#include "compat.h"
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

/* The run of mappings in /proc/self/maps that holds address a: its first
 * address and its end, and where the next mapping above it starts. */
static int around(uintptr_t a, uintptr_t *lo, uintptr_t *hi, uintptr_t *next) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    unsigned long s, e;
    char line[512];
    int in = 0;
    *next = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx", &s, &e) != 2) continue;
        if (!in && s <= a && a < e) { in = 1; *lo = s; *hi = e; continue; }
        if (in && s == *hi) { *hi = e; continue; }
        if (in && s > *hi) { *next = s; break; }
    }
    fclose(f);
    return in;
}

int main(void) {
    setup();
    if (begin("an mmap hint far below the mmap area leaves libc's pages out of the space mmap hands out")) {
        uintptr_t lo, hi, next;
        /* 1 GiB above the break, far under the interpreter. */
        void *low = (void *)((((uintptr_t)sbrk(0) + 0x3fffffff) & ~(uintptr_t)0x3fffffff) + 0x40000000);
        if (!around((uintptr_t)getpid, &lo, &hi, &next)) fail("libc not in /proc/self/maps");
        else if ((uintptr_t)low + (1 << 20) >= lo) pass();      /* libc is not above the hint: nothing to check */
        else {
            void *p = mmap(low, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            /* Ask for the free space just above libc, so that what is
             * free below it is the highest space left. */
            void *g = next > hi ? mmap((void *)hi, next - hi, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) : NULL;
            size_t n = 64 << 20;
            char *r = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            (void)g;                          /* (it may find no room elsewhere: ENOMEM) */
            if (p == MAP_FAILED || r == MAP_FAILED) fail("mmap: errno %d", errno);
            else if ((uintptr_t)r < hi && lo < (uintptr_t)r + n) fail("%p..+%zu covers libc at %#lx-%#lx", r, n, (unsigned long)lo, (unsigned long)hi);
            else if (getpid() <= 0) fail("libc's code is gone");
            else pass();
        }
    }
    return done();
}
