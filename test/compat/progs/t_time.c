/* Time: clocks, sleeping, the date. */
#include "compat.h"
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

int main(void) {
    setup();
    struct timespec a, b;
    struct timeval tv;

    EXPECT("clock_gettime MONOTONIC", clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    if (begin("clock_gettime MONOTONIC agrees with the system call")) {
        struct timespec s1, v, s2;
        int ok = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &s1) == 0 && clock_gettime(CLOCK_MONOTONIC, &v) == 0 &&
                 syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &s2) == 0;
        long long n1 = s1.tv_sec * 1000000000LL + s1.tv_nsec, nv = v.tv_sec * 1000000000LL + v.tv_nsec,
                  n2 = s2.tv_sec * 1000000000LL + s2.tv_nsec;
        if (ok && n1 <= nv && nv <= n2) pass(); else fail("%lld <= %lld <= %lld", n1, nv, n2);
    }
#ifdef __aarch64__
    /* The vDSO reads the counter in user mode: 20000 reads cost less than
     * 20000 system calls. */
    if (begin("clock_gettime MONOTONIC is read without a system call")) {
        struct timespec x, y, z;
        clock_gettime(CLOCK_MONOTONIC, &x);
        for (int i = 0; i < 20000; i++) clock_gettime(CLOCK_MONOTONIC, &z);
        clock_gettime(CLOCK_MONOTONIC, &y);
        long long fast = (y.tv_sec - x.tv_sec) * 1000000000LL + (y.tv_nsec - x.tv_nsec);
        clock_gettime(CLOCK_MONOTONIC, &x);
        for (int i = 0; i < 20000; i++) syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &z);
        clock_gettime(CLOCK_MONOTONIC, &y);
        long long slow = (y.tv_sec - x.tv_sec) * 1000000000LL + (y.tv_nsec - x.tv_nsec);
        if (fast * 2 < slow) pass(); else fail("vdso %lld ns, syscall %lld ns", fast, slow);
    }
#endif
    EXPECT("clock_gettime REALTIME after 2020", clock_gettime(CLOCK_REALTIME, &b) == 0 && b.tv_sec > 1577836800);
    EXPECT("clock_gettime PROCESS_CPUTIME", clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &b) == 0);
    EXPECT("clock_gettime BOOTTIME", clock_gettime(CLOCK_BOOTTIME, &b) == 0);
    EXPECT("clock_getres", clock_getres(CLOCK_MONOTONIC, &b) == 0);
    EXPECT("gettimeofday", gettimeofday(&tv, 0) == 0 && tv.tv_sec > 1577836800);
    EXPECT("time", time(0) > 1577836800);
    if (begin("nanosleep 100 ms")) {
        struct timespec d = {0, 100000000};
        clock_gettime(CLOCK_MONOTONIC, &a);
        nanosleep(&d, 0);
        clock_gettime(CLOCK_MONOTONIC, &b);
        long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        if (ms >= 95 && ms < 2000) pass(); else fail("slept %ld ms", ms);
    }
    if (begin("a sleep uses no CPU time")) {
        struct timespec d = {0, 200000000};
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &a);
        nanosleep(&d, 0);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &b);
        long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        if (ms < 20) pass(); else fail("200 ms of sleep took %ld ms of CPU time", ms);
    }
    {
        struct timespec d;
        clock_gettime(CLOCK_MONOTONIC, &a);
        d = a; d.tv_nsec += 50000000;
        if (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }
        EXPECT("clock_nanosleep TIMER_ABSTIME", clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, 0) == 0 &&
               clock_gettime(CLOCK_MONOTONIC, &b) == 0 && (b.tv_sec > d.tv_sec || (b.tv_sec == d.tv_sec && b.tv_nsec >= d.tv_nsec)));
    }
    EXPECT("sleep(1)", sleep(1) == 0);
    {
        time_t t = 0; struct tm tm;
        EXPECT("gmtime_r", gmtime_r(&t, &tm) && tm.tm_year == 70);
        char s[64];
        EXPECT("strftime + localtime", (t = time(0), localtime_r(&t, &tm) != 0 && strftime(s, sizeof s, "%Y-%m-%d", &tm) == 10));
    }
    {
        struct timespec r;
        EXPECT("monotonic does not go back", clock_gettime(CLOCK_MONOTONIC, &a) == 0 && clock_gettime(CLOCK_MONOTONIC, &r) == 0 &&
               (r.tv_sec > a.tv_sec || (r.tv_sec == a.tv_sec && r.tv_nsec >= a.tv_nsec)));
    }
    return done();
}
