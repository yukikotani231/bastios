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
#include <poll.h>
#include <stdint.h>

struct rtc_time { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
#define RTC_RD_TIME _IOR('p', 0x09, struct rtc_time)
#define RTC_SET_TIME _IOW('p', 0x0a, struct rtc_time)
#define RTC_ALM_SET _IOW('p', 0x07, struct rtc_time)
#define RTC_ALM_READ _IOR('p', 0x08, struct rtc_time)
#define RTC_AIE_ON _IO('p', 0x01)
#define RTC_AIE_OFF _IO('p', 0x02)
struct rtc_wkalrm { unsigned char enabled, pending; struct rtc_time time; };
#define RTC_WKALM_SET _IOW('p', 0x0f, struct rtc_wkalrm)
#define RTC_WKALM_RD _IOR('p', 0x10, struct rtc_wkalrm)

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

    {
        /* /sys/class/rtc/rtc0 says the same */
        char nm[64] = "", dt[64] = "", tm_[64] = "", ep[64] = "", fq[64] = "";
        const char *names[] = {"name", "date", "time", "since_epoch", "max_user_freq"};
        char *dst[] = {nm, dt, tm_, ep, fq};
        for (int i = 0; i < 5; i++) {
            char p[96];
            snprintf(p, sizeof p, "/sys/class/rtc/rtc0/%s", names[i]);
            int f = open(p, O_RDONLY);
            if (f >= 0) {
                int r = read(f, dst[i], 63);
                dst[i][r > 0 ? r : 0] = 0;
                close(f);
            }
        }
        time_t now = time(NULL);
        long e = atol(ep);
        EXPECT("sysfs: the name is rtc_cmos", !strncmp(nm, "rtc_cmos", 8));
        EXPECT("sysfs: since_epoch is the time the clock says", e > 1000000000 && e - now >= -5 && e - now <= 5);
        struct tm gt;
        time_t et = e;
        gmtime_r(&et, &gt);
        char want[32];
        strftime(want, sizeof want, "%Y-%m-%d\n", &gt);
        EXPECT("sysfs: date is the UTC date of since_epoch", !strcmp(dt, want));
        strftime(want, sizeof want, "%H:%M:%S\n", &gt);
        EXPECT("sysfs: time is the UTC time of since_epoch", !strcmp(tm_, want));
        EXPECT("sysfs: max_user_freq is a number", atoi(fq) > 0);
    }

    {
        /* The alarm: due 3 seconds from now. */
        struct rtc_wkalrm w;
        memset(&w, 0, sizeof w);
        ioctl(fd, RTC_RD_TIME, &w.time);
        time_t due = as_time(&w.time) + 3;
        struct tm dt;
        gmtime_r(&due, &dt);
        w.enabled = 1;
        w.time = (struct rtc_time){.sec = dt.tm_sec, .min = dt.tm_min, .hour = dt.tm_hour, .mday = dt.tm_mday, .mon = dt.tm_mon, .year = dt.tm_year};
        EXPECT("RTC_WKALM_SET arms an alarm", ioctl(fd, RTC_WKALM_SET, &w) == 0);
        struct rtc_wkalrm g;
        memset(&g, 0, sizeof g);
        EXPECT("RTC_WKALM_RD says armed, not yet pending, and the same time",
               ioctl(fd, RTC_WKALM_RD, &g) == 0 && g.enabled == 1 && g.pending == 0 && as_time(&g.time) == due);
        int fl = fcntl(fd, F_GETFL);
        unsigned long data = 0;
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        EXPECT("a nonblocking read before the alarm is EAGAIN", read(fd, &data, sizeof data) == -1 && errno == EAGAIN);
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        EXPECT("poll does not report it ready", poll(&pf, 1, 0) == 0);
        fcntl(fd, F_SETFL, fl);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ssize_t n = read(fd, &data, sizeof data);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        EXPECT("a blocking read waits for the alarm and returns an unsigned long", n == (ssize_t)sizeof data && (data & 0xff) == 0xa0 && (data >> 8) == 1);
        EXPECT("it waited about 3 seconds", el > 1.5 && el < 8.0);
        EXPECT("the alarm is disarmed after the read", ioctl(fd, RTC_WKALM_RD, &g) == 0 && g.enabled == 0);

        /* RTC_ALM_SET takes a time of day: the next time the clock shows it. */
        struct rtc_time t;
        ioctl(fd, RTC_RD_TIME, &t);
        time_t soon = as_time(&t) + 2;
        gmtime_r(&soon, &dt);
        struct rtc_time al = {.sec = dt.tm_sec, .min = dt.tm_min, .hour = dt.tm_hour, .mday = -1, .mon = -1, .year = -1};
        EXPECT("RTC_ALM_SET takes a time of day", ioctl(fd, RTC_ALM_SET, &al) == 0);
        struct rtc_time back_tm;
        memset(&back_tm, 0, sizeof back_tm);
        EXPECT("RTC_ALM_READ gives it back", ioctl(fd, RTC_ALM_READ, &back_tm) == 0 && back_tm.sec == al.sec && back_tm.min == al.min && back_tm.hour == al.hour);
        EXPECT("RTC_AIE_ON arms it", ioctl(fd, RTC_AIE_ON, 0) == 0);
        pf.revents = 0;
        EXPECT("poll reports it readable once it is due", poll(&pf, 1, 8000) == 1 && (pf.revents & POLLIN));
        EXPECT("the read then returns at once", read(fd, &data, sizeof data) == (ssize_t)sizeof data);
        ioctl(fd, RTC_AIE_ON, 0);
        EXPECT("RTC_AIE_OFF disarms it", ioctl(fd, RTC_AIE_OFF, 0) == 0 && ioctl(fd, RTC_WKALM_RD, &g) == 0 && g.enabled == 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        EXPECT("a disarmed nonblocking read is EAGAIN", read(fd, &data, sizeof data) == -1 && errno == EAGAIN);
        fcntl(fd, F_SETFL, fl);
        EXPECT("a blocking read with no alarm armed is EIO, not a hang", read(fd, &data, sizeof data) == -1 && errno == EIO);
        w.time = back_tm;
        ioctl(fd, RTC_RD_TIME, &w.time);
        w.time.year -= 1;
        EXPECT("an alarm in the past is refused", ioctl(fd, RTC_WKALM_SET, &w) == -1 && errno == EINVAL);
        EXPECT("a short read is EINVAL", read(fd, &data, 4) == -1 && errno == EINVAL);
    }

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
