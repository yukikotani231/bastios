/* Time namespaces (time_namespaces(7)): unshare(CLONE_NEWTIME) moves the
 * caller's children into a namespace whose CLOCK_MONOTONIC and
 * CLOCK_BOOTTIME run offset by what was written to
 * /proc/self/timens_offsets; setns joins one again. */
#define _GNU_SOURCE
#include "compat.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>

#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME 0x80
#endif
#define NS_GET_NSTYPE _IO(0xb7, 0x3)

static long long now_ms(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* Runs f in a child; f returns 0 for success, else the step that failed. */
static int run_child(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) _exit(f());
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st)) return 255;
    return WEXITSTATUS(st);
}

#define CHILD(name, f) do { \
    if (begin(name)) { int r = run_child(f); if (r == 0) pass(); else fail("step %d failed", r); } \
} while (0)

static int put(const char *s) {
    int fd = open("/proc/self/timens_offsets", O_WRONLY);
    if (fd < 0) return -errno;
    int r = write(fd, s, strlen(s)) < 0 ? -errno : 0;
    close(fd);
    return r;
}

static long long kernel_mono, kernel_boot;

static int near(long long got, long long want) {
    return got >= want - 400 && got <= want + 400;
}

static ino_t ino_of(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? st.st_ino : 0;
}

static int wait_ok(pid_t c) {
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st)) return 255;
    return WEXITSTATUS(st);
}

/* The caller made a namespace (offsets 100 s and 200 s); its child sees them. */
static int parent_unshifted(void) {
    kernel_mono = now_ms(CLOCK_MONOTONIC);
    kernel_boot = now_ms(CLOCK_BOOTTIME);
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    if (put("1 100 0\n7 200 0\n") != 0) return 2;
    /* unshare moves the children only: the caller still reads the old time */
    if (!near(now_ms(CLOCK_MONOTONIC), kernel_mono)) return 3;
    pid_t c = fork();
    if (c == 0) {
        long long m = now_ms(CLOCK_MONOTONIC), b = now_ms(CLOCK_BOOTTIME);
        if (!near(m, kernel_mono + 100000)) _exit(10);
        if (!near(b, kernel_boot + 200000)) _exit(11);
        _exit(0);
    }
    return wait_ok(c);
}

static int frozen_in_child(void) {
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    if (put("1 5 0") != 0) return 2;
    pid_t c = fork();
    if (c == 0) _exit(put("1 6 0") == -EACCES ? 0 : 10);
    return wait_ok(c);
}

static int first_is_frozen(void) { return put("1 5 0") == -EACCES; }

static int bad_writes(void) {
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    if (put("3 1 0") != -EINVAL) return 2;
    if (put("1 1 1000000000") != -EINVAL) return 3;
    if (put("1 x 0") != -EINVAL) return 4;
    if (put("1 5 0") != 0) return 5;
    return 0;
}

/* An offset that takes the clock past what 64 bits of nanoseconds hold, or
 * below zero, is ERANGE; a large one that fits is not. */
static int range_writes(void) {
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    if (put("1 9223372036 0") != -ERANGE) return 2;
    if (put("1 -9223372036 0") != -ERANGE) return 3;
    if (put("7 99999999999 0") != -ERANGE) return 4;
    if (put("1 4000000000 0") != 0) return 5;
    return 0;
}

static int negative_ok(void) {
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    return put("1 -2 500000000") == 0 ? 0 : 2;
}

/* An absolute sleep on the shifted clock lasts as long as it asks for. */
static int abs_sleep(void) {
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    if (put("1 1000 0\n") != 0) return 2;
    pid_t c = fork();
    if (c == 0) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        long long was = now_ms(CLOCK_REALTIME);
        t.tv_nsec += 300000000;
        if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) != 0) _exit(10);
        long long d = now_ms(CLOCK_REALTIME) - was;
        _exit(d >= 250 && d <= 3000 ? 0 : 11);
    }
    return wait_ok(c);
}

static int timerfd_abs(void) {
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    if (put("7 1000 0\n") != 0) return 2;
    pid_t c = fork();
    if (c == 0) {
        int fd = timerfd_create(CLOCK_BOOTTIME, 0);
        struct itimerspec it = {{0, 0}, {0, 0}};
        clock_gettime(CLOCK_BOOTTIME, &it.it_value);
        long long was = now_ms(CLOCK_REALTIME);
        it.it_value.tv_nsec += 300000000;
        if (it.it_value.tv_nsec >= 1000000000) { it.it_value.tv_nsec -= 1000000000; it.it_value.tv_sec++; }
        if (fd < 0) _exit(10);
        if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &it, NULL) != 0) _exit(11);
        unsigned long long n;
        if (read(fd, &n, 8) != 8) _exit(12);
        long long d = now_ms(CLOCK_REALTIME) - was;
        _exit(d >= 250 && d <= 3000 ? 0 : 13);
    }
    return wait_ok(c);
}

static int uptime_shifted(void) {
    struct sysinfo a;
    sysinfo(&a);
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    if (put("7 300 0\n") != 0) return 2;
    pid_t c = fork();
    if (c == 0) {
        struct sysinfo b;
        sysinfo(&b);
        FILE *f = fopen("/proc/uptime", "r");
        double up = 0;
        if (!f || fscanf(f, "%lf", &up) != 1) _exit(10);
        long d = b.uptime - a.uptime;
        if (d < 300 || d > 302) _exit(11);
        if (up < 300 || up - b.uptime > 2 || b.uptime - up > 2) _exit(12);
        _exit(0);
    }
    return wait_ok(c);
}

/* setns to the namespace the children were in before, undoing the shift. */
static int setns_back(void) {
    int orig = open("/proc/self/ns/time_for_children", O_RDONLY);
    if (orig < 0) return 1;
    kernel_mono = now_ms(CLOCK_MONOTONIC);
    if (unshare(CLONE_NEWTIME) != 0) return 2;
    if (put("1 100 0\n") != 0) return 3;
    if (ino_of("/proc/self/ns/time") == ino_of("/proc/self/ns/time_for_children")) return 4;
    pid_t c = fork();
    if (c == 0) {
        if (!near(now_ms(CLOCK_MONOTONIC), kernel_mono + 100000)) _exit(10);
        if (setns(orig, CLONE_NEWTIME) != 0) _exit(11);
        if (!near(now_ms(CLOCK_MONOTONIC), kernel_mono)) _exit(12);
        if (ino_of("/proc/self/ns/time") != ino_of("/proc/self/ns/time_for_children")) _exit(13);
        _exit(0);
    }
    return wait_ok(c);
}

static int setns_wrong_type(void) {
    int t = open("/proc/self/ns/time", O_RDONLY);
    return t >= 0 && setns(t, CLONE_NEWUTS) == -1 && errno == EINVAL;
}

static int shown_back(void) {
    if (unshare(CLONE_NEWTIME) != 0) return 1;
    if (put("1 7 500000000\n7 -3 0\n") != 0) return 2;
    pid_t c = fork();
    if (c == 0) {                       /* a process reads its own namespace's */
        char b[200];
        int fd = open("/proc/self/timens_offsets", O_RDONLY);
        int n = fd < 0 ? -1 : read(fd, b, sizeof b - 1);
        if (n <= 0) _exit(10);
        b[n] = 0;
        if (!strstr(b, "monotonic") || !strstr(b, " 7 500000000")) _exit(11);
        if (!strstr(b, "boottime") || !strstr(b, " -3 0")) _exit(12);
        _exit(0);
    }
    return wait_ok(c);
}

static int links(void) {
    int t = open("/proc/self/ns/time", O_RDONLY);
    int tc = open("/proc/self/ns/time_for_children", O_RDONLY);
    if (t < 0 || tc < 0) return 1;
    if (ino_of("/proc/self/ns/time") != 4026531834UL) return 2;
    if (ino_of("/proc/self/ns/time_for_children") != 4026531834UL) return 3;
    if (ioctl(t, NS_GET_NSTYPE) != CLONE_NEWTIME) return 4;
    return 0;
}

int main(void) {
    setup();
    /* A negative offset must leave the clock above zero (as on Linux, which
     * answers ERANGE), so a guest that has only just booted waits a little. */
    while (now_ms(CLOCK_MONOTONIC) < 4000 || now_ms(CLOCK_BOOTTIME) < 4000)
        usleep(100000);
    kernel_mono = now_ms(CLOCK_MONOTONIC);
    kernel_boot = now_ms(CLOCK_BOOTTIME);
    CHILD("the time links name the first namespace, and NS_GET_NSTYPE says CLONE_NEWTIME", links);
    EXPECT("the first namespace's offsets are frozen", first_is_frozen());
    CHILD("unshare moves the children only: they read the written offsets", parent_unshifted);
    CHILD("the offsets are frozen once a process is in the namespace", frozen_in_child);
    CHILD("a clock other than monotonic or boottime, or a bad time, is EINVAL", bad_writes);
    CHILD("an offset may be negative", negative_ok);
    CHILD("an offset that overflows the clock is ERANGE", range_writes);
    CHILD("an absolute clock_nanosleep waits on the shifted clock", abs_sleep);
    CHILD("an absolute timerfd waits on the shifted clock", timerfd_abs);
    CHILD("sysinfo and /proc/uptime show the shifted boot time", uptime_shifted);
    CHILD("setns to the first time namespace undoes the shift", setns_back);
    EXPECT("setns with a different type is EINVAL", setns_wrong_type());
    CHILD("/proc/PID/timens_offsets reads back what was written", shown_back);
    return done();
}
