/* The AArch64 vDSO: clock_gettime in user mode, with no system call, and the
 * signal return trampoline, exported as glibc's unwinder expects
 * (__kernel_rt_sigreturn). The kernel maps this image, read-only, on one
 * page of every Linux process and points AT_SYSINFO_EHDR at it, so that the
 * dynamic linker lists linux-vdso.so.1 and libc finds the symbols.
 *
 * The page also holds the clock (src/tsc.bastio): the virtual counter
 * reading `base` stands for `at` ns since boot, and each tick adds
 * mult / 2^32 ns. The arithmetic below is tsc_scale and tsc_ns step for
 * step (as user/vdso.c does on x86-64), so a program reads the same clock
 * the kernel keeps. The kernel lets user mode read the counter
 * (CNTKCTL_EL1.EL0VCTEN, boot/aarch64/boot.S).
 *
 * It runs in the process, like libc: a bug here can give that process a
 * wrong time, and nothing else. Clocks it does not keep (the CPU-time
 * ones, the wall clock, and any it does not know) go to the system call,
 * made from here as Linux's vDSO makes it: glibc takes what the vDSO
 * returns as the answer and never retries with the system call itself. The
 * wall clocks go there because clock_settime, settimeofday and adjtimex
 * move the kernel's epoch, and the copy on this page, written at exec,
 * would not follow.
 *
 * The kernel's own signal frames return through the copy of the trampoline
 * it writes at byte 0xF00 of the page (SIGTRAMP_AT); the one here is for
 * code that looks the symbol up. */
#include <stdint.h>

struct vdata { uint64_t mult, base, at, epoch, tmns; };
extern const volatile struct vdata vdso_data __attribute__((visibility("hidden")));

struct timespec { long tv_sec; long tv_nsec; };

#define NS_MAX 18446744073709551615ULL
#define FOREVER 18446744073709551613ULL  /* src/lxtime.bastio */
#define TWO32 4294967296ULL
#define MULT_MAX 1099511627776ULL
#define SYS_CLOCK_GETTIME 113

__asm__(".text\n"
        ".globl __kernel_rt_sigreturn\n"
        ".type __kernel_rt_sigreturn, %function\n"
        "__kernel_rt_sigreturn:\n"
        "  mov x8, #139\n"
        "  svc #0\n");

static uint64_t scale(uint64_t d, uint64_t mult)
{
    uint64_t hi = d / TWO32, lo = d % TWO32, mh = mult / TWO32, ml = mult % TWO32;
    if (mh != 0 && hi > (TWO32 - 1) / mh)
        return NS_MAX;
    uint64_t top = hi * mh * TWO32;
    uint64_t a = hi * ml, b = lo * mh, c = lo * ml / TWO32;
    if (a > NS_MAX - b || a + b > NS_MAX - c || a + b + c > NS_MAX - top)
        return NS_MAX;
    return top + a + b + c;
}

static long sys_clock_gettime(long clk, struct timespec *ts)
{
    register long x0 __asm__("x0") = clk;
    register long x1 __asm__("x1") = (long)ts;
    register long x8 __asm__("x8") = SYS_CLOCK_GETTIME;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

int __kernel_clock_gettime(int clk, struct timespec *ts)
{
    uint64_t mult = vdso_data.mult, base = vdso_data.base, at = vdso_data.at;
    /* MONOTONIC(_RAW) 1, 4; BOOTTIME 7. MONOTONIC_COARSE (6) is the system
     * call's, which counts in ticks: reading it here by the counter would
     * run ahead of a later system call's answer (LTP's clock_gettime04). */
    /* tmns: the process is in a time namespace, which shifts the monotonic
     * clocks by an offset only the kernel knows (src/lxnsk.bastio). */
    if (mult == 0 || mult >= MULT_MAX || vdso_data.tmns != 0 || (clk != 1 && clk != 4 && clk != 7))
        return (int)sys_clock_gettime(clk, ts);
    uint64_t now, t = at;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
    if (now >= base) {
        uint64_t d = scale(now - base, mult);
        t = d > NS_MAX - at ? NS_MAX : at + d;
    }
    if (t > FOREVER - 1)
        t = FOREVER - 1;
    if (ts) {
        ts->tv_sec = (long)(t / 1000000000);
        ts->tv_nsec = (long)(t % 1000000000);
    }
    return 0;
}
