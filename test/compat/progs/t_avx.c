/* AVX: the upper halves of the YMM registers are part of a process's state
 * (the kernel saves them with XSAVE): they survive system calls, other
 * processes and threads, fork and signal handlers, and a handler sees and
 * can change them in its frame. The checks run only where the processor
 * offers AVX and the kernel has enabled it (CPUID OSXSAVE, XCR0). */
#include "compat.h"
#ifdef __x86_64__
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>

#define NB 96          /* ymm0, ymm9 and ymm15 */

static void fill(unsigned char *b, int seed) {
    for (int i = 0; i < NB; i++) b[i] = (unsigned char)(seed * 37 + i * 11 + 5);
}

/* Loads ymm0, ymm9 and ymm15 from `in`, makes the system call `no` (a1, a2) n
 * times, and stores the three registers to `out`. All in one asm block, so no
 * compiled code (which may use the registers itself) runs in between. The
 * result of the last call is returned. */
static long ymm_run(const unsigned char *in, unsigned char *out, long n, long no, long a1, long a2) {
    long ret;
    __asm__ volatile(
        "vmovdqu (%[in]), %%ymm0\n\t"
        "vmovdqu 32(%[in]), %%ymm9\n\t"
        "vmovdqu 64(%[in]), %%ymm15\n\t"
        "1:\n\t"
        "mov %[no], %%rax\n\t"
        "mov %[a1], %%rdi\n\t"
        "mov %[a2], %%rsi\n\t"
        "syscall\n\t"
        "mov %%rax, %[ret]\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n\t"
        "vmovdqu %%ymm0, (%[out])\n\t"
        "vmovdqu %%ymm9, 32(%[out])\n\t"
        "vmovdqu %%ymm15, 64(%[out])\n\t"
        "vzeroupper\n\t"
        : [n] "+r"(n), [ret] "=&r"(ret)
        : [in] "r"(in), [out] "r"(out), [no] "r"(no), [a1] "r"(a1), [a2] "r"(a2)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory", "ymm0", "ymm9", "ymm15");
    return ret;
}

static int has_avx(void) {
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (!(c >> 27 & 1) || !(c >> 28 & 1)) return 0;
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}

static int run_ok(int seed, long n) {
    unsigned char in[NB], out[NB];
    fill(in, seed);
    ymm_run(in, out, n, SYS_sched_yield, 0, 0);
    return memcmp(in, out, NB) == 0;
}

static void *thread_main(void *arg) {
    return (void *)(long)run_ok((int)(long)arg, 3000);
}

static unsigned char sig_in[NB];
static volatile unsigned char sig_frame_hi[16];
static volatile int sig_mode, sig_seen;

static void on_usr1(int s, siginfo_t *si, void *ctx) {
    (void)s; (void)si;
    ucontext_t *uc = ctx;
    unsigned char *fp = (unsigned char *)uc->uc_mcontext.fpregs;
    if (sig_mode == 1) {
        /* ymm0's upper half in the frame: after the 512 bytes of FXSAVE and the 64 of the header. */
        for (int i = 0; i < 16; i++) sig_frame_hi[i] = fp[576 + i];
        sig_seen = 1;
    } else if (sig_mode == 2) {
        for (int i = 0; i < 16; i++) fp[576 + i] = 0xAB;
        sig_seen = 1;
    }
    /* Whatever the handler does to the registers must not reach the interrupted code. */
    __asm__ volatile("vxorps %%ymm0, %%ymm0, %%ymm0\n\tvxorps %%ymm9, %%ymm9, %%ymm9\n\tvxorps %%ymm15, %%ymm15, %%ymm15"
                     : : : "ymm0", "ymm9", "ymm15");
}
#endif

int main(void) {
    setup();
#ifdef __x86_64__
    if (!has_avx()) return 0;

    if (begin("AVX state survives system calls")) {
        if (run_ok(1, 300)) pass(); else fail("ymm0, ymm9 or ymm15 changed over sched_yield");
    }

    if (begin("AVX state is per process")) {
        int ok = 1;
        pid_t kids[3];
        for (int k = 0; k < 3; k++) {
            kids[k] = fork();
            if (kids[k] == 0) _exit(run_ok(10 + k, 3000) ? 0 : 1);
        }
        if (!run_ok(20, 3000)) ok = 0;
        for (int k = 0; k < 3; k++) {
            int st = 0;
            waitpid(kids[k], &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) ok = 0;
        }
        if (ok) pass(); else fail("a process saw another's YMM registers");
    }

    if (begin("AVX state is inherited by fork")) {
        unsigned char in[NB], out[NB];
        fill(in, 30);
        long r = ymm_run(in, out, 1, SYS_fork, 0, 0);
        if (r == 0) _exit(memcmp(in, out, NB) == 0 ? 0 : 1);
        int st = 0;
        waitpid((pid_t)r, &st, 0);
        if (r > 0 && memcmp(in, out, NB) == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) pass();
        else fail("fork %ld, child status %d", r, st);
    }

    if (begin("AVX state is per thread")) {
        pthread_t t[3];
        int ok = 1;
        for (int k = 0; k < 3; k++) pthread_create(&t[k], NULL, thread_main, (void *)(long)(40 + k));
        if (!run_ok(50, 3000)) ok = 0;
        for (int k = 0; k < 3; k++) {
            void *r = NULL;
            pthread_join(t[k], &r);
            if (!r) ok = 0;
        }
        if (ok) pass(); else fail("a thread saw another's YMM registers");
    }

    struct sigaction sa = {0};
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    pid_t me = getpid();

    if (begin("AVX state survives a signal handler")) {
        unsigned char out[NB];
        fill(sig_in, 60);
        sig_mode = 0;
        ymm_run(sig_in, out, 1, SYS_kill, me, SIGUSR1);
        if (memcmp(sig_in, out, NB) == 0) pass(); else fail("the handler's changes reached the interrupted code");
    }

    if (begin("AVX state is in the signal frame")) {
        unsigned char out[NB];
        fill(sig_in, 61);
        sig_mode = 1;
        sig_seen = 0;
        ymm_run(sig_in, out, 1, SYS_kill, me, SIGUSR1);
        if (sig_seen && memcmp((const void *)sig_frame_hi, sig_in + 16, 16) == 0) pass();
        else fail("seen %d, the frame's ymm0 upper half differs", sig_seen);
    }

    if (begin("AVX state can be changed in the signal frame")) {
        unsigned char out[NB];
        fill(sig_in, 62);
        sig_mode = 2;
        sig_seen = 0;
        ymm_run(sig_in, out, 1, SYS_kill, me, SIGUSR1);
        int ok = sig_seen && memcmp(out, sig_in, 16) == 0 && memcmp(out + 32, sig_in + 32, 64) == 0;
        for (int i = 16; i < 32; i++) if (out[i] != 0xAB) ok = 0;
        if (ok) pass(); else fail("seen %d, ymm0 upper half %02x", sig_seen, out[16]);
    }
#endif
    return done();
}
