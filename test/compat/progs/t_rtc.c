/* The real-time clock as /dev/rtc0: RTC_RD_TIME reads the time the clock
 * says (the same as CLOCK_REALTIME at boot, to within a few seconds),
 * RTC_SET_TIME writes it (CAP_SYS_TIME), a bad time is EINVAL, and the node
 * is the character device 10:135. */
#define _GNU_SOURCE
#include "compat.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>

struct rtc_time { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
#define RTC_RD_TIME _IOR('p', 0x09, struct rtc_time)
#define RTC_SET_TIME _IOW('p', 0x0a, struct rtc_time)

static time_t as_time(const struct rtc_time *r) {
    struct tm t = {.tm_sec = r->sec, .tm_min = r->min, .tm_hour = r->hour, .tm_mday = r->mday, .tm_mon = r->mon, .tm_year = r->year};
    return timegm(&t);
}

int main(void) {
    setup();
    int fd = open("/dev/rtc0", O_RDONLY);
    EXPECT("/dev/rtc0 opens", fd >= 0);
    if (fd < 0) return done();
    struct stat st;
    EXPECT("it is the character device 10:135", fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 10 && minor(st.st_rdev) == 135);
    int other = open("/dev/rtc", O_RDONLY);
    EXPECT("/dev/rtc is the same clock", other >= 0);
    if (other >= 0) close(other);

    struct rtc_time r;
    memset(&r, 0, sizeof r);
    EXPECT("RTC_RD_TIME reads the clock", ioctl(fd, RTC_RD_TIME, &r) == 0);
    long off = (long)as_time(&r) - (long)time(NULL);
    EXPECT("it agrees with CLOCK_REALTIME to within 20 seconds", off > -20 && off < 20);
    struct tm now;
    time_t t = as_time(&r);
    gmtime_r(&t, &now);
    EXPECT("the weekday and the day of the year are right", r.wday == now.tm_wday && r.yday == now.tm_yday && r.isdst == 0);

    struct rtc_time set = {.sec = 26, .min = 9, .hour = 15, .mday = 14, .mon = 2, .year = 131};   /* 2031-03-14 15:09:26 */
    EXPECT("RTC_SET_TIME sets it", ioctl(fd, RTC_SET_TIME, &set) == 0);
    memset(&r, 0, sizeof r);
    EXPECT("it reads back the same, a moment later", ioctl(fd, RTC_RD_TIME, &r) == 0 && as_time(&r) - as_time(&set) >= 0 && as_time(&r) - as_time(&set) <= 3);
    EXPECT("the weekday is a Friday", r.wday == 5 && r.yday == 72);

    struct rtc_time bad = set;
    bad.mon = 12;
    EXPECT("a month of 12 is EINVAL", ioctl(fd, RTC_SET_TIME, &bad) == -1 && errno == EINVAL);
    bad = set;
    bad.year = 99;
    EXPECT("a year before 2000 is EINVAL", ioctl(fd, RTC_SET_TIME, &bad) == -1 && errno == EINVAL);
    EXPECT("an unknown ioctl is ENOTTY", ioctl(fd, _IO('p', 0x7f), 0) == -1 && errno == ENOTTY);

    /* put the clock back where it was */
    time_t back = time(NULL);
    struct tm bt;
    gmtime_r(&back, &bt);
    struct rtc_time restore = {.sec = bt.tm_sec, .min = bt.tm_min, .hour = bt.tm_hour, .mday = bt.tm_mday, .mon = bt.tm_mon, .year = bt.tm_year};
    EXPECT("the clock is put back", ioctl(fd, RTC_SET_TIME, &restore) == 0);

    EXPECT("an unprivileged program cannot set it", ({
        int ok = 0;
        pid_t c = fork();
        if (c == 0) {
            if (setgid(100) || setuid(65534)) _exit(2);
            int f = open("/dev/rtc0", O_RDONLY);
            struct rtc_time x = restore;
            _exit(f >= 0 && ioctl(f, RTC_SET_TIME, &x) == -1 && errno == EPERM && ioctl(f, RTC_RD_TIME, &x) == 0 ? 0 : 1);
        }
        int stt = 0;
        waitpid(c, &stt, 0);
        ok = WIFEXITED(stt) && WEXITSTATUS(stt) == 0;
        ok;
    }));
    close(fd);
    return done();
}
