/* The vDSO: clock_gettime in user mode, with no system call. The kernel
 * maps this image, read-only, on one page of every Linux process and
 * points AT_SYSINFO_EHDR at it; musl finds __vdso_clock_gettime there.
 * The page also holds the clock (src/tsc.bastio): the time-stamp counter
 * reading `base` stands for `at` ns since boot, and each tick adds
 * mult / 2^32 ns. The arithmetic below is tsc_scale and tsc_ns step for
 * step, so a program reads the same clock the kernel keeps.
 *
 * It runs in the process, like libc: a bug here can give that process a
 * wrong time, and nothing else. Clocks it does not keep (the CPU-time
 * ones, and any it does not know) go to the system call, made from here
 * as Linux's vDSO makes it: glibc takes what the vDSO returns as the
 * answer and never retries with the system call itself, so an -ENOSYS
 * from here left glibc's programs with no wall clock at all. The wall
 * clocks (REALTIME) go there too: clock_settime, settimeofday and
 * adjtimex move the kernel's epoch, and the copy on this page, written at
 * exec, would not follow. */
#include <stdint.h>

struct vdata { uint64_t mult, base, at, epoch; };
extern const volatile struct vdata vdso_data __attribute__((visibility("hidden")));

struct timespec { long tv_sec; long tv_nsec; };

#define NS_MAX 18446744073709551615ULL
#define FOREVER 18446744073709551613ULL  /* src/lxtime.bastio */
#define TWO32 4294967296ULL
#define MULT_MAX 1099511627776ULL
#define SYS_CLOCK_GETTIME 228

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
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(SYS_CLOCK_GETTIME), "D"(clk), "S"(ts)
                     : "rcx", "r11", "memory");
    return r;
}

int __vdso_clock_gettime(int clk, struct timespec *ts)
{
    uint64_t mult = vdso_data.mult, base = vdso_data.base, at = vdso_data.at;
    /* MONOTONIC(_RAW, _COARSE) 1, 4, 6; BOOTTIME 7 */
    if (mult == 0 || mult >= MULT_MAX || (clk != 1 && clk != 4 && clk != 6 && clk != 7))
        return (int)sys_clock_gettime(clk, ts);
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now = (uint64_t)hi << 32 | lo, t = at;
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
