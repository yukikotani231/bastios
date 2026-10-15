/* Address space layout randomization, and the bytes AT_RANDOM points at.
 *
 * The program starts itself a few times (fork and execve) and compares what
 * each run reports about its own layout: where its stack, its break, its
 * mmap area and its code lie, and the 16 bytes the kernel gave it for a
 * stack protector. With randomization on, no two runs agree; with
 * personality(ADDR_NO_RANDOMIZE) or kernel.randomize_va_space = 0, all do. */
#include "compat.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>

#define RUNS 8

struct layout {
    unsigned long stack, mmap, brk, code;
    unsigned char rnd[16];
};

static int report(void) {
    int local = 0;
    void *m = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *b = sbrk(0);
    const unsigned char *r = (const unsigned char *)getauxval(AT_RANDOM);
    if (m == MAP_FAILED || b == (void *)-1 || !r) return 1;
    printf("L %lx %lx %lx %lx ", (unsigned long)&local, (unsigned long)m, (unsigned long)b, (unsigned long)report);
    for (int i = 0; i < 16; i++) printf("%02x", r[i]);
    printf("\n");
    return 0;
}

/* Runs the program again as a child that prints its layout. */
static int spawn(const char *self, struct layout *out) {
    int fds[2];
    if (pipe(fds) < 0) return -1;
    pid_t c = fork();
    if (c < 0) return -1;
    if (c == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execl(self, self, "child", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char buf[256];
    size_t n = 0;
    ssize_t g;
    while (n < sizeof buf - 1 && (g = read(fds[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)g;
    buf[n] = 0;
    close(fds[0]);
    int st = 0;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
    char hex[40];
    if (sscanf(buf, "L %lx %lx %lx %lx %32s", &out->stack, &out->mmap, &out->brk, &out->code, hex) != 5 || strlen(hex) != 32) return -1;
    for (int i = 0; i < 16; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out->rnd[i] = (unsigned char)v;
    }
    return 0;
}

static int run_all(const char *self, struct layout *l) {
    for (int i = 0; i < RUNS; i++)
        if (spawn(self, &l[i]) < 0) return -1;
    return 0;
}

/* How many different values `field` takes over the runs. */
#define DISTINCT(l, field) ({ int d = 0; for (int i = 0; i < RUNS; i++) { int seen = 0; for (int j = 0; j < i; j++) seen |= (l)[j].field == (l)[i].field; d += !seen; } d; })

/* The same for the break's distance from the code, which only the second level of randomization varies. */
#define DISTINCT_GAP(l) ({ int d = 0; for (int i = 0; i < RUNS; i++) { int seen = 0; for (int j = 0; j < i; j++) seen |= (l)[j].brk - (l)[j].code == (l)[i].brk - (l)[i].code; d += !seen; } d; })

static int all_same(struct layout *l) {
    return DISTINCT(l, stack) == 1 && DISTINCT(l, mmap) == 1 && DISTINCT(l, brk) == 1 && DISTINCT(l, code) == 1;
}

static int set_aslr(const char *v) {
    int fd = open("/proc/sys/kernel/randomize_va_space", O_WRONLY);
    if (fd < 0) return -1;
    int ok = write(fd, v, strlen(v)) == (ssize_t)strlen(v);
    close(fd);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) return report();
    setup();
    char self[256];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    if (sl > 0) self[sl] = 0; else snprintf(self, sizeof self, "%s", argv[0]);

    static struct layout on[RUNS], off[RUNS];
    int have = run_all(self, on) == 0;
    EXPECT("a program starts itself and reports its layout", have);
    if (!have) return 1;

    EXPECT("the stack moves between runs", DISTINCT(on, stack) >= RUNS - 1);
    EXPECT("the mmap area moves between runs", DISTINCT(on, mmap) >= RUNS - 1);
    EXPECT("the program break moves between runs", DISTINCT(on, brk) >= RUNS - 1);
    EXPECT("a position-independent program moves between runs", DISTINCT(on, code) >= RUNS - 1);
    EXPECT("the break starts at a random distance above the image", DISTINCT_GAP(on) >= RUNS - 1);
    EXPECT("every address is page-plausible and in user space", ({
        int ok = 1;
        for (int i = 0; i < RUNS; i++)
            ok &= on[i].stack < (1ul << 47) && on[i].mmap < (1ul << 47) && on[i].brk < (1ul << 47) && on[i].code < (1ul << 47) && on[i].mmap % 4096 == 0;
        ok;
    }));
    EXPECT("the stack is above the mmap area and the break below it", ({
        int ok = 1;
        for (int i = 0; i < RUNS; i++) ok &= on[i].stack > on[i].mmap && on[i].mmap > on[i].brk;
        ok;
    }));
    EXPECT("the AT_RANDOM bytes differ between runs", ({
        int distinct = 0;
        for (int i = 0; i < RUNS; i++) {
            int seen = 0;
            for (int j = 0; j < i; j++) seen |= memcmp(on[j].rnd, on[i].rnd, 16) == 0;
            distinct += !seen;
        }
        distinct == RUNS;
    }));
    EXPECT("the AT_RANDOM bytes use all of their 16 bytes", ({
        /* A 31-bit generator seeded by a tick count leaves structure: check
         * that no two runs share even their first or last 8 bytes. */
        int ok = 1;
        for (int i = 0; i < RUNS; i++)
            for (int j = 0; j < i; j++) ok &= memcmp(on[j].rnd, on[i].rnd, 8) != 0 && memcmp(on[j].rnd + 8, on[i].rnd + 8, 8) != 0;
        ok;
    }));

    if (begin("a fork child keeps its parent's layout")) {
        int local = 0;
        unsigned long here = (unsigned long)&local;
        pid_t c = fork();
        if (c == 0) _exit((unsigned long)&local == here ? 0 : 1);
        int st = 0;
        if (c < 0 || waitpid(c, &st, 0) != c) fail("fork or wait: errno %d", errno);
        else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) fail("status %#x", st);
        else pass();
    }

    if (begin("personality ADDR_NO_RANDOMIZE fixes the layout across execve")) {
        int old = personality(0xffffffff);
        if (old < 0 || personality((unsigned long)old | ADDR_NO_RANDOMIZE) < 0) {
            fail("personality: errno %d", errno);
        } else {
            int r = run_all(self, off);
            personality((unsigned long)old);
            if (r < 0) fail("a run failed");
            else if (!all_same(off)) fail("layouts differ: stack %d mmap %d brk %d code %d", DISTINCT(off, stack), DISTINCT(off, mmap), DISTINCT(off, brk), DISTINCT(off, code));
            else pass();
        }
    }

    if (begin("randomize_va_space 0 fixes the layout, 2 brings the movement back")) {
        if (set_aslr("0\n") < 0) {
            fail("cannot write the sysctl: errno %d", errno);
        } else {
            int r = run_all(self, off);
            int same = r == 0 && all_same(off);
            int back = set_aslr("2\n") == 0 && run_all(self, on) == 0 && DISTINCT(on, mmap) >= RUNS - 1;
            if (!same) fail("layouts differ with randomization off");
            else if (!back) fail("randomization did not come back");
            else pass();
        }
    }

    if (begin("randomize_va_space 1 moves everything but the break's distance")) {
        if (set_aslr("1\n") < 0) {
            fail("cannot write the sysctl: errno %d", errno);
        } else {
            int r = run_all(self, on);
            set_aslr("2\n");
            if (r < 0) fail("a run failed");
            else if (DISTINCT(on, mmap) < RUNS - 1 || DISTINCT(on, stack) < RUNS - 1) fail("mmap or stack did not move");
            else if (DISTINCT_GAP(on) != 1) fail("the break's distance from the code varied (%d values)", DISTINCT_GAP(on));
            else pass();
        }
    }

    EXPECT("a randomized program still runs: heap, big mmap, threads of data", ({
        char *p = malloc(8 << 20);
        int ok = p != NULL;
        if (p) { memset(p, 7, 8 << 20); ok = p[(8 << 20) - 1] == 7; free(p); }
        void *q = mmap(NULL, 1ul << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        ok && q != MAP_FAILED;
    }));
    return done();
}
