// The user's FS base must come back from every trap as the program left it:
// after system calls, page faults and the switches between processes. Built
// with musl; runs with no libc call between changing FS and putting it back.
//
// It prints "fsbase: ok" or "fsbase: FAIL <what>". On a processor with
// RDFSBASE and WRFSBASE (CPUID.7:EBX bit 0, which the kernel then enables in
// CR4) it also checks the instructions; otherwise only arch_prctl.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static uint64_t blk[3][512] __attribute__((aligned(4096)));    // FS bases: %fs:0 holds a magic
static volatile uint64_t *volatile page;

static inline long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline uint64_t fs0(void) {
    uint64_t v;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(v));
    return v;
}

static inline uint64_t get_fs(void) {
    uint64_t v = 0;
    sys3(SYS_arch_prctl, ARCH_GET_FS, (long)&v, 0);
    return v;
}

static int has_fsgsbase(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (a < 7) return 0;
    a = 7;
    c = 0;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return b & 1;
}

// One round of traps with FS pointing at blk[i]: a system call, a page
// fault on a page not touched yet, and a yield. 0 if %fs:0 and the kernel's
// idea of FS agree with blk[i] after each.
static int traps(int i, int n, int fsgs) {
    uint64_t want = (uint64_t)blk[i];
    for (int k = 0; k < n; k++) {
        sys3(SYS_getpid, 0, 0, 0);
        if (fs0() != blk[i][0] || get_fs() != want) return 1;
        page[(k % 64) * 512] = k;                      // faults the first time round
        if (fs0() != blk[i][0]) return 2;
        sys3(SYS_sched_yield, 0, 0, 0);
        if (fs0() != blk[i][0] || get_fs() != want) return 3;
        if (fsgs) {
            uint64_t v;
            __asm__ volatile ("rdfsbase %0" : "=r"(v));
            if (v != want) return 4;
        }
    }
    return 0;
}

int main(void) {
    int fsgs = has_fsgsbase();
    for (int i = 0; i < 3; i++) blk[i][0] = 0xF5BA5E0000ull + i;
    page = mmap(0, 64 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { puts("fsbase: FAIL mmap"); return 1; }
    uint64_t old = get_fs();
    int bad = 0;
    const char *what = "";

    // arch_prctl sets it, the traps keep it.
    sys3(SYS_arch_prctl, ARCH_SET_FS, (long)blk[0], 0);
    int r = traps(0, 200, fsgs);
    if (r) { bad = 1; what = "arch_prctl, traps"; }

    // The instructions set it, arch_prctl sees it (and the traps keep it).
    if (!bad && fsgs) {
        __asm__ volatile ("wrfsbase %0" : : "r"((uint64_t)blk[1]));
        r = traps(1, 200, fsgs);
        if (r) { bad = 1; what = "wrfsbase, traps"; }
    }

    // Two processes with different bases, switched against each other.
    if (!bad) {
        sys3(SYS_arch_prctl, ARCH_SET_FS, (long)blk[0], 0);
        long pid = sys3(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            sys3(SYS_arch_prctl, ARCH_SET_FS, (long)blk[2], 0);
            int c = traps(2, 300, fsgs);
            sys3(SYS_exit, c, 0, 0);
        }
        r = traps(0, 300, fsgs);
        int st = 0;
        sys3(SYS_wait4, pid, (long)&st, 0);
        if (r) { bad = 1; what = "parent after fork"; }
        else if (((st >> 8) & 255) != 0) { bad = 1; what = "child"; }
    }

    sys3(SYS_arch_prctl, ARCH_SET_FS, (long)old, 0);
    if (bad) printf("fsbase: FAIL %s (rdfsbase %s)\n", what, fsgs ? "used" : "not available");
    else printf("fsbase: ok (rdfsbase %s)\n", fsgs ? "used" : "not available");
    return bad;
}
