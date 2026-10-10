/* Time: clocks, sleeping, the date. */
#include "compat.h"
#include <sys/time.h>
#include <time.h>

int main(void) {
    setup();
    struct timespec a, b;
    struct timeval tv;

    EXPECT("clock_gettime MONOTONIC", clock_gettime(CLOCK_MONOTONIC, &a) == 0);
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
