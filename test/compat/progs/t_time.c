/* Time: clocks, sleeping, the date. */
#include "compat.h"
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        /* Time zones: a POSIX TZ string, and a zone file (Asia/Tokyo) that TZ names, as /usr/share/zoneinfo's are read. */
        time_t t = 86400 * 365; struct tm tm; char s[64];
        setenv("TZ", "JST-9", 1); tzset();
        EXPECT("TZ=JST-9 is nine hours ahead", localtime_r(&t, &tm) && tm.tm_hour == 9 && tm.tm_mday == 1 && strcmp(tm.tm_zone, "JST") == 0);
        setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1); tzset();
        t = 1721000000;                               /* 2024-07-14 23:33:20 UTC */
        EXPECT("TZ with a DST rule", localtime_r(&t, &tm) && tm.tm_isdst == 1 && tm.tm_hour == 19);
        t = 1704067200;                               /* 2024-01-01 00:00:00 UTC */
        EXPECT("and outside the DST period", localtime_r(&t, &tm) && tm.tm_isdst == 0 && tm.tm_hour == 19 && tm.tm_mday == 31);
        /* Asia/Tokyo, 309 bytes, as tzdata makes it (public domain). */
        static const unsigned char z[] = {
                             0x54,0x5a,0x69,0x66,0x32,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                             0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x00,
                             0x00,0x00,0x00,0x09,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x0c,0x80,0x00,0x00,0x00,
                             0xd7,0x3e,0x02,0x70,0xd7,0xed,0x59,0xf0,0xd8,0xf8,0xfa,0x70,0xd9,0xcd,0x3b,0xf0,
                             0xdb,0x07,0x00,0xf0,0xdb,0xad,0x1d,0xf0,0xdc,0xe6,0xe2,0xf0,0xdd,0x8c,0xff,0xf0,
                             0x03,0x01,0x02,0x01,0x02,0x01,0x02,0x01,0x02,0x00,0x00,0x83,0x03,0x00,0x00,0x00,
                             0x00,0x8c,0xa0,0x01,0x04,0x00,0x00,0x7e,0x90,0x00,0x08,0x00,0x00,0x7e,0x90,0x00,
                             0x08,0x4c,0x4d,0x54,0x00,0x4a,0x44,0x54,0x00,0x4a,0x53,0x54,0x00,0x00,0x00,0x00,
                             0x01,0x00,0x00,0x00,0x01,0x54,0x5a,0x69,0x66,0x32,0x00,0x00,0x00,0x00,0x00,0x00,
                             0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
                             0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
                             0x0c,0xff,0xff,0xff,0xff,0x65,0xc2,0xa4,0x70,0xff,0xff,0xff,0xff,0xd7,0x3e,0x02,
                             0x70,0xff,0xff,0xff,0xff,0xd7,0xed,0x59,0xf0,0xff,0xff,0xff,0xff,0xd8,0xf8,0xfa,
                             0x70,0xff,0xff,0xff,0xff,0xd9,0xcd,0x3b,0xf0,0xff,0xff,0xff,0xff,0xdb,0x07,0x00,
                             0xf0,0xff,0xff,0xff,0xff,0xdb,0xad,0x1d,0xf0,0xff,0xff,0xff,0xff,0xdc,0xe6,0xe2,
                             0xf0,0xff,0xff,0xff,0xff,0xdd,0x8c,0xff,0xf0,0x03,0x01,0x02,0x01,0x02,0x01,0x02,
                             0x01,0x02,0x00,0x00,0x83,0x03,0x00,0x00,0x00,0x00,0x8c,0xa0,0x01,0x04,0x00,0x00,
                             0x7e,0x90,0x00,0x08,0x00,0x00,0x7e,0x90,0x00,0x08,0x4c,0x4d,0x54,0x00,0x4a,0x44,
                             0x54,0x00,0x4a,0x53,0x54,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x0a,0x4a,
                             0x53,0x54,0x2d,0x39,0x0a,
        };
        FILE *f = fopen("/tmp/ct_tzif", "wb");
        EXPECT("a zone file is written", f && fwrite(z, 1, sizeof z, f) == sizeof z && fclose(f) == 0);
        setenv("TZ", ":/tmp/ct_tzif", 1); tzset();
        t = 1721000000;
        EXPECT("TZ naming a zone file reads it", localtime_r(&t, &tm) && tm.tm_hour == 8 && strcmp(tm.tm_zone, "JST") == 0 &&
               strftime(s, sizeof s, "%H:%M %Z %z", &tm) > 0 && strcmp(s, "08:33 JST +0900") == 0);
        unlink("/tmp/ct_tzif");
        unsetenv("TZ"); tzset();
    }
    {
        struct timespec r;
        EXPECT("monotonic does not go back", clock_gettime(CLOCK_MONOTONIC, &a) == 0 && clock_gettime(CLOCK_MONOTONIC, &r) == 0 &&
               (r.tv_sec > a.tv_sec || (r.tv_sec == a.tv_sec && r.tv_nsec >= a.tv_nsec)));
    }
    return done();
}
