/* Settings root may change: the host and domain names, the wall clock and
 * the NTP clock discipline, and a cgroup namespace; a user who is not root gets EPERM for each. */
#include "compat.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <stdint.h>
#define ENOKEY_ 126
#define EKEYEXPIRED_ 127
#define EKEYREVOKED_ 128

static int slurp(const char *path, char *buf, int n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int got = (int)fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got < 0 ? 0 : got] = 0;
    return got > 0;
}

static int hostname(void) {
    struct utsname u, was;
    char buf[128];
    uname(&was);
    int ok = sethostname("compat-host", 11) == 0 && uname(&u) == 0 && strcmp(u.nodename, "compat-host") == 0;
    ok = ok && slurp("/proc/sys/kernel/hostname", buf, sizeof buf) && strcmp(buf, "compat-host\n") == 0;
    ok = ok && slurp("/proc/sys/kernel/osrelease", buf, sizeof buf) && strncmp(buf, u.release, strlen(u.release)) == 0;
    ok = ok && sethostname("x", 65) == -1 && errno == EINVAL;
    sethostname(was.nodename, strlen(was.nodename));
    return ok;
}

static int domainname(void) {
    struct utsname u, was;
    uname(&was);
    int ok = setdomainname("example.org", 11) == 0 && uname(&u) == 0 && strcmp(u.domainname, "example.org") == 0;
    setdomainname(was.domainname, strlen(was.domainname));
    return ok;
}

/* The same names through their /proc/sys/kernel files: a write ends at its newline. */
static int proc_names(void) {
    struct utsname u, was;
    uname(&was);
    int ok = 1;
    int fd = open("/proc/sys/kernel/domainname", O_WRONLY);
    ok = ok && fd >= 0 && write(fd, "proc.example\nignored", 20) == 20;
    if (fd >= 0) close(fd);
    ok = ok && uname(&u) == 0 && strcmp(u.domainname, "proc.example") == 0;
    fd = open("/proc/sys/kernel/hostname", O_WRONLY);
    ok = ok && fd >= 0 && write(fd, "procbox", 7) == 7;
    if (fd >= 0) close(fd);
    ok = ok && uname(&u) == 0 && strcmp(u.nodename, "procbox") == 0;
    char big[80];
    memset(big, 'a', sizeof big);
    fd = open("/proc/sys/kernel/hostname", O_WRONLY);
    ok = ok && fd >= 0 && write(fd, big, sizeof big) == -1 && errno == EINVAL;
    if (fd >= 0) close(fd);
    setdomainname(was.domainname, strlen(was.domainname));
    sethostname(was.nodename, strlen(was.nodename));
    return ok;
}

static int set_clock(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_REALTIME, &a);
    a.tv_sec += 1000;
    int ok = clock_settime(CLOCK_REALTIME, &a) == 0;
    clock_gettime(CLOCK_REALTIME, &b);
    ok = ok && b.tv_sec >= a.tv_sec && b.tv_sec < a.tv_sec + 5;
    b.tv_sec -= 1000;
    ok = ok && clock_settime(CLOCK_REALTIME, &b) == 0;
    ok = ok && clock_settime(CLOCK_MONOTONIC, &b) == -1 && errno == EINVAL;
    return ok;
}

static int timeofday(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    int ok = settimeofday(&tv, 0) == 0;
    tv.tv_usec = 1000000;
    ok = ok && settimeofday(&tv, 0) == -1 && errno == EINVAL;
    return ok;
}

static int adjust(void) {
    struct timex t = {0};
    int r = adjtimex(&t);
    int ok = r >= TIME_OK && r <= TIME_ERROR && t.tick == 10000;
    long freq = t.freq;
    t.modes = ADJ_FREQUENCY;
    t.freq = freq + 100;
    ok = ok && adjtimex(&t) >= 0;
    struct timex v = {0};
    ok = ok && adjtimex(&v) >= 0 && v.freq == freq + 100;
    v.modes = ADJ_FREQUENCY;
    v.freq = freq;
    adjtimex(&v);
    v.modes = ADJ_TICK;
    v.tick = 20000;
    ok = ok && adjtimex(&v) == -1 && errno == EINVAL;
    v.modes = 0x8000;                       /* ADJ_ADJTIME without ADJ_OFFSET */
    ok = ok && adjtimex(&v) == -1 && errno == EINVAL;
    return ok;
}

/* The hardware clock, /dev/rtc0: hwclock's RTC_RD_TIME and RTC_SET_TIME. */
struct rtc_tm { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
#define RTC_RD_TIME_ 0x80247009u
#define RTC_SET_TIME_ 0x4024700Au

static int hw_clock(void) {
    int fd = open("/dev/rtc0", O_RDWR);
    if (fd < 0) return 0;
    struct rtc_tm was, t = {0};
    int ok = ioctl(fd, RTC_RD_TIME_, &was) == 0 && was.year >= 120;
    t.sec = 6; t.min = 5; t.hour = 4; t.mday = 3; t.mon = 1; t.year = 131;   /* 2031-02-03 04:05:06, a Monday */
    ok = ok && ioctl(fd, RTC_SET_TIME_, &t) == 0;
    struct rtc_tm r = {0};
    ok = ok && ioctl(fd, RTC_RD_TIME_, &r) == 0;
    ok = ok && r.year == 131 && r.mon == 1 && r.mday == 3 && r.hour == 4 && r.min == 5 && r.sec >= 6 && r.sec < 12;
    ok = ok && r.wday == 1 && r.yday == 33;
    struct rtc_tm bad = t;
    bad.mon = 1; bad.mday = 30;                          /* 30 February */
    ok = ok && ioctl(fd, RTC_SET_TIME_, &bad) == -1 && errno == EINVAL;
    bad = t; bad.mon = 12;
    ok = ok && ioctl(fd, RTC_SET_TIME_, &bad) == -1 && errno == EINVAL;
    bad = t; bad.year = 99;                              /* 1999: the clock holds 2000 to 2099 */
    ok = ok && ioctl(fd, RTC_SET_TIME_, &bad) == -1 && errno == EINVAL;
    bad = t; bad.hour = 24;
    ok = ok && ioctl(fd, RTC_SET_TIME_, &bad) == -1 && errno == EINVAL;
    t = (struct rtc_tm){ 10, 0, 0, 31, 11, 199, 0, 0, 0 };   /* 2099-12-31 00:00:10 */
    ok = ok && ioctl(fd, RTC_SET_TIME_, &t) == 0 && ioctl(fd, RTC_RD_TIME_, &r) == 0 && r.year == 199 && r.mon == 11 && r.mday == 31;
    t = (struct rtc_tm){ 0, 0, 12, 29, 1, 124, 0, 0, 0 };         /* 2024-02-29 12:00:00 (a leap day), a Thursday */
    ok = ok && ioctl(fd, RTC_SET_TIME_, &t) == 0 && ioctl(fd, RTC_RD_TIME_, &r) == 0
        && r.year == 124 && r.mon == 1 && r.mday == 29 && r.hour == 12 && r.wday == 4 && r.yday == 59;
    ok = ok && ioctl(fd, RTC_SET_TIME_, &was) == 0;      /* put it back (a second or so late) */
    close(fd);
    int fd2 = open("/dev/rtc", O_RDONLY);                /* the same clock by its other name */
    ok = ok && fd2 >= 0 && ioctl(fd2, RTC_RD_TIME_, &r) == 0 && r.year >= 120;
    if (fd2 >= 0) close(fd2);
    struct stat st;
    ok = ok && stat("/dev/rtc0", &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 10 && minor(st.st_rdev) == 135;
    pid_t p = fork();
    if (p == 0) {
        if (setuid(65534) != 0) _exit(1);
        int f = open("/dev/rtc0", O_RDONLY);
        _exit(f == -1 && errno == EACCES ? 0 : 2);       /* crw-------: root's */
    }
    int w = 0;
    waitpid(p, &w, 0);
    return ok && WIFEXITED(w) && WEXITSTATUS(w) == 0;
}

/* How far CLOCK_REALTIME has gone ahead of CLOCK_MONOTONIC since the call
 * that was made with a = the two readings, in usec. */
static long drift_since(const struct timespec *real0, const struct timespec *mono0) {
    struct timespec r, m;
    clock_gettime(CLOCK_REALTIME, &r);
    clock_gettime(CLOCK_MONOTONIC, &m);
    long dr = (r.tv_sec - real0->tv_sec) * 1000000L + (r.tv_nsec - real0->tv_nsec) / 1000;
    long dm = (m.tv_sec - mono0->tv_sec) * 1000000L + (m.tv_nsec - mono0->tv_nsec) / 1000;
    return dr - dm;
}

/* What slewing() found wrong, for the failure line. */
static char slew_why[160];
#define STEP(cond, ...) do { if (ok && !(cond)) { ok = 0; snprintf(slew_why, sizeof slew_why, __VA_ARGS__); } } while (0)

/* adjtimex moves the clock: frequency, a PLL offset and adjtime. */
static int slewing(void) {
    struct timespec r0, m0;
    struct timex t = {0};
    int ok = 1;
    long d;
    /* +500 ppm for two seconds: the wall clock gains 1 ms on the monotonic one. */
    t.modes = ADJ_FREQUENCY;
    t.freq = 500L * 65536;
    STEP(adjtimex(&t) >= 0, "set +500 ppm: errno %d", errno);
    clock_gettime(CLOCK_REALTIME, &r0);
    clock_gettime(CLOCK_MONOTONIC, &m0);
    sleep(2);
    d = drift_since(&r0, &m0);
    STEP(d > 700 && d < 1300, "+500 ppm for 2 s drifted %ld us", d);
    t.modes = ADJ_FREQUENCY;
    t.freq = -500L * 65536;                              /* and the same slow */
    STEP(adjtimex(&t) >= 0, "set -500 ppm: errno %d", errno);
    clock_gettime(CLOCK_REALTIME, &r0);
    clock_gettime(CLOCK_MONOTONIC, &m0);
    sleep(2);
    d = drift_since(&r0, &m0);
    STEP(d < -700 && d > -1300, "-500 ppm for 2 s drifted %ld us", d);
    t.modes = ADJ_FREQUENCY;
    t.freq = 0;
    STEP(adjtimex(&t) >= 0, "set 0 ppm: errno %d", errno);
    clock_gettime(CLOCK_REALTIME, &r0);
    clock_gettime(CLOCK_MONOTONIC, &m0);
    sleep(1);
    d = drift_since(&r0, &m0);
    STEP(d > -100 && d < 100, "0 ppm drifted %ld us", d);   /* back to the rate it had */

    /* A PLL offset of 20 ms, time constant 0 (1/16 of it a second): 2 s move it about 2.4 ms. */
    t = (struct timex){0};
    t.modes = ADJ_OFFSET | ADJ_STATUS | ADJ_TIMECONST;
    t.status = STA_PLL;
    t.constant = 0;
    t.offset = 20000;
    STEP(adjtimex(&t) >= 0, "set a PLL offset: errno %d", errno);
    clock_gettime(CLOCK_REALTIME, &r0);
    clock_gettime(CLOCK_MONOTONIC, &m0);
    sleep(2);
    d = drift_since(&r0, &m0);
    STEP(d > 1500 && d < 4000, "a 20 ms PLL offset moved the clock %ld us in 2 s", d);
    struct timex v = {0};
    STEP(adjtimex(&v) >= 0 && v.offset > 15000 && v.offset < 18500, "the PLL offset left reads %ld", (long)v.offset);
    t = (struct timex){0};
    t.modes = ADJ_OFFSET | ADJ_STATUS;                   /* cancel it */
    t.status = STA_PLL;
    t.offset = 0;
    STEP(adjtimex(&t) >= 0, "cancel the offset: errno %d", errno);
    t = (struct timex){0};
    t.modes = ADJ_STATUS;                                /* and leave the clock unsynchronised */
    t.status = STA_UNSYNC;
    adjtimex(&t);

    /* adjtime(): -10 ms at 500 ppm; 2 s of it is 1 ms (back). */
    struct timeval delta = { 0, -10000 }, old;
    clock_gettime(CLOCK_REALTIME, &r0);
    clock_gettime(CLOCK_MONOTONIC, &m0);
    STEP(adjtime(&delta, &old) == 0, "adjtime: errno %d", errno);
    sleep(2);
    d = drift_since(&r0, &m0);
    STEP(d < -700 && d > -1300, "adjtime of -10 ms moved the clock %ld us in 2 s", d);
    struct timeval left, none = { 0, 0 };
    long rest = adjtime(&none, &left) == 0 ? left.tv_sec * 1000000L + left.tv_usec : 0;
    STEP(rest < -7000 && rest > -9300, "adjtime left %ld us", rest);   /* what was left, and cancelled */
    return ok;
}

static int not_root(void) {
    pid_t p = fork();
    if (p == 0) {
        if (setuid(65534) != 0) _exit(1);
        struct timespec ts;
        struct timex t = {0};
        clock_gettime(CLOCK_REALTIME, &ts);
        int ok = sethostname("x", 1) == -1 && errno == EPERM;
        ok = ok && clock_settime(CLOCK_REALTIME, &ts) == -1 && errno == EPERM;
        ok = ok && adjtimex(&t) >= 0;                  /* reading is allowed */
        t.modes = ADJ_FREQUENCY;
        ok = ok && adjtimex(&t) == -1 && errno == EPERM;
        _exit(ok ? 0 : 2);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return 0;
    int ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}

/* A process that unshares CLONE_NEWCGROUP sees its group as the root of
 * /sys/fs/cgroup; /proc/PID/cgroup is relative to it, "/.." for a process
 * above it; and the group cannot be removed while the process is in it. */
static int cgroup_ns(void) {
    if (mkdir("/sys/fs/cgroup/cgn", 0755) != 0 || mkdir("/sys/fs/cgroup/cgn/in", 0755) != 0) return 0;
    pid_t parent = getpid();
    pid_t p = fork();
    if (p == 0) {
        char buf[128], path[64], me[32];
        snprintf(me, sizeof me, "%d", (int)getpid());
        if (!put("/sys/fs/cgroup/cgn/cgroup.procs", me)) _exit(1);
        char before[64], after[64];
        ssize_t a = readlink("/proc/self/ns/cgroup", before, sizeof before - 1);
        if (unshare(CLONE_NEWCGROUP) != 0) _exit(2);
        ssize_t b = readlink("/proc/self/ns/cgroup", after, sizeof after - 1);
        if (a <= 0 || b <= 0 || (a == b && memcmp(before, after, a) == 0)) _exit(3);
        if (!slurp("/proc/self/cgroup", buf, sizeof buf) || strcmp(buf, "0::/\n") != 0) _exit(4);
        snprintf(path, sizeof path, "/proc/%d/cgroup", (int)parent);
        if (!slurp(path, buf, sizeof buf) || strcmp(buf, "0::/..\n") != 0) _exit(5);
        if (!put("/sys/fs/cgroup/in/cgroup.procs", me)) _exit(6);
        if (!slurp("/proc/self/cgroup", buf, sizeof buf) || strcmp(buf, "0::/in\n") != 0) _exit(7);
        if (rmdir("/sys/fs/cgroup/in") != -1 || errno != EBUSY) _exit(8);
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    ok = rmdir("/sys/fs/cgroup/cgn/in") == 0 && ok;
    ok = rmdir("/sys/fs/cgroup/cgn") == 0 && ok;
    return ok;
}

/* memory.max: memory.current counts the pages the group's processes have
 * touched, a mapping nobody uses costs nothing, and the process with the most
 * pages in a group that passes the limit is killed (memory.events counts
 * it). */
static int cgroup_mem(void) {
    if (mkdir("/sys/fs/cgroup/cgm", 0755) != 0) return 0;
    pid_t p = fork();
    if (p == 0) {
        char me[32], buf[256];
        snprintf(me, sizeof me, "%d", (int)getpid());
        if (!put("/sys/fs/cgroup/cgm/cgroup.procs", me)) _exit(1);
        if (!put("/sys/fs/cgroup/cgm/memory.max", "16777216")) _exit(2);
        char *a = mmap(0, 4 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (a == MAP_FAILED) _exit(3);
        memset(a, 1, 4 << 20);
        if (!slurp("/sys/fs/cgroup/cgm/memory.current", buf, sizeof buf) || atol(buf) < (4 << 20)) _exit(4);
        char *big = mmap(0, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (big == MAP_FAILED) _exit(5);         /* mapping is not what the limit is about */
        if (!slurp("/sys/fs/cgroup/cgm/memory.current", buf, sizeof buf) || atol(buf) >= (16 << 20)) _exit(6);
        pid_t hog = fork();
        if (hog == 0) {
            memset(big, 1, 64 << 20);              /* passes 16 MiB: killed */
            _exit(0);
        }
        if (hog < 0) _exit(7);
        int hst = 0;
        if (waitpid(hog, &hst, 0) != hog || !WIFSIGNALED(hst) || WTERMSIG(hst) != SIGKILL) _exit(8);
        if (!slurp("/sys/fs/cgroup/cgm/memory.events", buf, sizeof buf) || !strstr(buf, "oom_kill 1")) _exit(9);
        a[0] = 2;                                  /* this process, with fewer pages, was left alone */
        if (!put("/sys/fs/cgroup/cgm/memory.max", "max")) _exit(10);
        if (!put("/sys/fs/cgroup/cgroup.procs", me)) _exit(11);
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok) errno = WIFEXITED(st) ? 1000 + WEXITSTATUS(st) : 2000 + WTERMSIG(st);   /* the report shows which step */
    ok = rmdir("/sys/fs/cgroup/cgm") == 0 && ok;
    return ok;
}

/* musl has no linux/seccomp.h: the parts of it the check uses. */
struct bpf_insn { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct bpf_prog { unsigned short len; struct bpf_insn *filter; };
#define BPF(c, t, f, kk) ((struct bpf_insn){ c, t, f, kk })
struct sc_data { int nr; unsigned int arch; unsigned long long ip; unsigned long long args[6]; };
struct sc_notif { unsigned long long id; unsigned int pid, flags; struct sc_data data; };
struct sc_resp { unsigned long long id; long long val; int error; unsigned int flags; };
#define SC_RECV 0xC0502100UL
#define SC_SEND 0xC0182101UL
#define SC_VALID 0x40082102UL
#define SC_CONTINUE 1
#define SC_ADDFD 0x40182103UL
#define SC_SETFLAGS 0x40082104UL
struct sc_addfd { unsigned long long id; unsigned int flags, srcfd, newfd, newfd_flags; };

/* A seccomp filter whose action for getppid is USER_NOTIF, with a notifier
 * the parent keeps while its children make the call. */
static int notifier(void) {
    struct bpf_insn prog[] = {
        BPF(0x20, 0, 0, 0),                   /* ld [nr] */
        BPF(0x15, 0, 1, SYS_getppid),         /* jeq getppid */
        BPF(0x06, 0, 0, 0x7fc00000),          /* ret USER_NOTIF */
        BPF(0x06, 0, 0, 0x7fff0000),          /* ret ALLOW */
    };
    struct bpf_prog fp = { 4, prog };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return 0;
    int lfd = (int)syscall(SYS_seccomp, 1, 8, &fp);
    if (lfd < 0) return 0;
    /* Each child makes the call and reports what it got. The supervisor
     * answers a value, an error, and CONTINUE (the real parent). */
    for (int round = 0; round < 3; round++) {
        int pp[2];
        if (pipe(pp)) return 0;
        pid_t c = fork();
        if (c == 0) {
            errno = 0;
            long r = syscall(SYS_getppid);
            long out[2] = { r, errno };
            write(pp[1], out, sizeof out);
            _exit(0);
        }
        struct sc_notif req;
        memset(&req, 0, sizeof req);
        if (ioctl(lfd, SC_RECV, &req) != 0) return 0;
        if (req.data.nr != SYS_getppid || req.data.arch == 0 || (pid_t)req.pid != c) return 0;
        if (ioctl(lfd, SC_VALID, &req.id) != 0) return 0;
        struct sc_resp rsp;
        memset(&rsp, 0, sizeof rsp);
        rsp.id = req.id;
        if (round == 0) rsp.val = 4242;
        if (round == 1) rsp.error = -EPERM;
        if (round == 2) rsp.flags = SC_CONTINUE;
        if (ioctl(lfd, SC_SEND, &rsp) != 0) return 0;
        if (ioctl(lfd, SC_SEND, &rsp) != -1 || errno != ENOENT) return 0;
        long out[2];
        if (read(pp[0], out, sizeof out) != (ssize_t)sizeof out) return 0;
        waitpid(c, 0, 0);
        close(pp[0]);
        close(pp[1]);
        if (round == 0 && out[0] != 4242) return 0;
        if (round == 1 && (out[0] != -1 || out[1] != EPERM)) return 0;
        if (round == 2 && out[0] != (long)getpid()) return 0;
    }
    /* A notifier that is closed gives its waiters ENOSYS. */
    int pp[2];
    if (pipe(pp)) return 0;
    pid_t c = fork();
    if (c == 0) {
        close(lfd);                           /* or the notifier would outlive the parent's close */
        errno = 0;
        long r = syscall(SYS_getppid);
        long out[2] = { r, errno };
        write(pp[1], out, sizeof out);
        _exit(0);
    }
    struct sc_notif req;
    memset(&req, 0, sizeof req);
    if (ioctl(lfd, SC_RECV, &req) != 0) return 0;
    close(lfd);
    long out[2];
    if (read(pp[0], out, sizeof out) != (ssize_t)sizeof out) return 0;
    waitpid(c, 0, 0);
    return out[0] == -1 && out[1] == ENOSYS;
}

/* NOTIF_ADDFD: the supervisor hands the waiting process one of its own
 * descriptors, at the lowest number and at a chosen one. */
static int notif_addfd(void) {
    struct bpf_insn prog[] = {
        BPF(0x20, 0, 0, 0),
        BPF(0x15, 0, 1, SYS_getppid),
        BPF(0x06, 0, 0, 0x7fc00000),
        BPF(0x06, 0, 0, 0x7fff0000),
    };
    struct bpf_prog fp = { 4, prog };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return 0;
    int lfd = (int)syscall(SYS_seccomp, 1, 8, &fp);
    if (lfd < 0) return 0;
    unsigned long long flags = 1;
    if (ioctl(lfd, SC_SETFLAGS, &flags) != 0) return 0;
    flags = 2;
    if (ioctl(lfd, SC_SETFLAGS, &flags) != -1 || errno != EINVAL) return 0;
    for (int round = 0; round < 2; round++) {
        int data[2], res[2];
        if (pipe(data) || pipe(res)) return 0;
        pid_t c = fork();
        if (c == 0) {
            close(lfd);
            long fd = syscall(SYS_getppid);
            if (fd >= 0) write((int)fd, "x", 1);
            write(res[1], &fd, sizeof fd);
            _exit(0);
        }
        struct sc_notif req;
        memset(&req, 0, sizeof req);
        if (ioctl(lfd, SC_RECV, &req) != 0) return 0;
        struct sc_addfd af;
        memset(&af, 0, sizeof af);
        af.id = req.id;
        af.srcfd = data[1];
        af.flags = 2 | (round == 1 ? 1 : 0);   /* SEND, then SETFD too */
        af.newfd = round == 1 ? 40 : 0;
        int got = ioctl(lfd, SC_ADDFD, &af);
        if (got < 0 || (round == 1 && got != 40)) return 0;
        long fd;
        if (read(res[0], &fd, sizeof fd) != (ssize_t)sizeof fd || fd != got) return 0;
        waitpid(c, 0, 0);
        char b = 0;
        close(data[1]);
        if (read(data[0], &b, 1) != 1 || b != 'x') return 0;
        /* A second add for an answered call finds nothing waiting. */
        if (ioctl(lfd, SC_ADDFD, &af) != -1 || errno != ENOENT) return 0;
        close(data[0]);
        close(res[0]);
        close(res[1]);
    }
    return 1;
}

/* splice of a /proc file into a pipe: the bytes, and the offset moves on. */
static int splice_proc(void) {
    int fd = open("/proc/sys/kernel/hostname", O_RDONLY), p[2];
    char want[80], buf[80];
    struct utsname u;
    if (fd < 0 || pipe(p) < 0) return 0;
    uname(&u);
    int n = snprintf(want, sizeof want, "%s\n", u.nodename);
    long a = splice(fd, NULL, p[1], NULL, 64, 0);
    long b = splice(fd, NULL, p[1], NULL, 64, 0);
    long c = read(p[0], buf, sizeof buf);
    int ok = a == n && b == 0 && c == n && memcmp(buf, want, n) == 0;
    off_t off = 1;
    ok = ok && splice(fd, &off, p[1], NULL, 8, 0) < 0;
    close(fd); close(p[0]); close(p[1]);
    return ok;
}

/* Kernel keys: add_key, request_key and keyctl. */
#define K_ADD SYS_add_key
#define K_REQ SYS_request_key
#define K_CTL SYS_keyctl
#define KS_THREAD (-1)
#define KS_PROCESS (-2)
#define KS_SESSION (-3)
static long kadd(const char *type, const char *desc, const void *pl, size_t n, long ring) { return syscall(K_ADD, type, desc, pl, n, ring); }
static long kctl(long cmd, long a, long b, long c, long d) { return syscall(K_CTL, cmd, a, b, c, d); }

static int keys_basic(void) {
    char buf[64], want[64];
    long key, again, ring, look;
    if (kctl(1, 0, 0, 0, 0) < 0) return 0;                          /* JOIN_SESSION_KEYRING: a new one */
    key = kadd("user", "compatkey", "hello", 5, KS_SESSION);
    if (key <= 0) return 0;
    if (kctl(11, key, (long)buf, sizeof buf, 0) != 5 || memcmp(buf, "hello", 5)) return 0;      /* READ */
    snprintf(want, sizeof want, "user;%u;%u;3f000000;compatkey", (unsigned)getuid(), (unsigned)getgid());
    memset(buf, 0, sizeof buf);
    if (kctl(6, key, (long)buf, sizeof buf, 0) != (long)strlen(want) + 1 || strcmp(buf, want)) return 0;   /* DESCRIBE */
    again = kadd("user", "compatkey", "bye", 3, KS_SESSION);        /* the same type and description: an update */
    if (again != key || kctl(11, key, (long)buf, sizeof buf, 0) != 3 || memcmp(buf, "bye", 3)) return 0;
    look = syscall(K_REQ, "user", "compatkey", NULL, 0);            /* request_key finds it */
    if (look != key) return 0;
    errno = 0;
    if (syscall(K_REQ, "user", "nosuchkey", NULL, 0) != -1 || errno != ENOKEY_) return 0;
    ring = kadd("keyring", "compatring", NULL, 0, KS_SESSION);
    if (ring <= 0 || kctl(8, key, ring, 0, 0) != 0) return 0;       /* LINK */
    if (kctl(11, ring, (long)buf, sizeof buf, 0) != 4) return 0;    /* a keyring reads as serial numbers */
    if (kctl(10, ring, (long)"user", (long)"compatkey", 0) != key) return 0;                    /* SEARCH */
    if (kctl(9, key, ring, 0, 0) != 0 || kctl(11, ring, (long)buf, sizeof buf, 0) != 0) return 0;   /* UNLINK */
    return 1;
}

static int keys_errors(void) {
    char big[40000];
    long key;
    memset(big, 'x', sizeof big);
    errno = 0;
    if (kadd("bogus", "d", "x", 1, KS_SESSION) != -1 || errno != ENODEV) return 0;
    errno = 0;
    if (kadd("user", "d", big, 32768, KS_SESSION) != -1 || errno != EINVAL) return 0;
    errno = 0;
    if (kadd("user", "d", NULL, 4, KS_SESSION) != -1 || errno != EFAULT) return 0;
    if (kadd("user", "big", big, 32767, KS_SESSION) <= 0) return 0;
    key = kadd("user", "tmo", "x", 1, KS_SESSION);
    if (key <= 0 || kctl(15, key, 1, 0, 0) != 0) return 0;          /* SET_TIMEOUT */
    sleep(2);
    errno = 0;
    if (kctl(11, key, (long)big, 8, 0) != -1 || errno != EKEYEXPIRED_) return 0;
    key = kadd("user", "rev", "x", 1, KS_SESSION);
    if (key <= 0 || kctl(3, key, 0, 0, 0) != 0) return 0;           /* REVOKE */
    errno = 0;
    if (kctl(11, key, (long)big, 8, 0) != -1 || errno != EKEYREVOKED_) return 0;
    errno = 0;
    if (kctl(11, 0x7ffffff0, (long)big, 8, 0) != -1 || errno != ENOKEY_) return 0;
    return 1;
}

static int keys_inherit(void) {
    long key;
    int st;
    pid_t p;
    if (kctl(1, 0, 0, 0, 0) < 0) return 0;
    key = kadd("user", "inherited", "v", 1, KS_SESSION);
    if (key <= 0) return 0;
    p = fork();
    if (p == 0) {                                                    /* the session keyring is the parent's */
        char b[8];
        long mine = kadd("user", "fromchild", "c", 1, KS_PROCESS);
        _exit(syscall(K_REQ, "user", "inherited", NULL, 0) == key && kctl(11, key, (long)b, sizeof b, 0) == 1
              && mine > 0 && kctl(11, mine, (long)b, sizeof b, 0) == 1 ? 0 : 1);
    }
    if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return 0;
    return syscall(K_REQ, "user", "fromchild", NULL, 0) == -1;       /* a process keyring is not shared */
}

static int keys_negative(void) {
    long ring, id;
    char b[8];
    if (kctl(1, 0, 0, 0, 0) < 0) return 0;
    ring = kadd("keyring", "negring", NULL, 0, KS_SESSION);
    errno = 0;
    if (ring <= 0 || syscall(K_REQ, "user", "nokey", "callout", ring) != -1 || errno != ENOKEY_) return 0;
    if (kctl(11, ring, (long)&id, sizeof id, 0) != 4) return 0;      /* a negative key stands for the failure */
    errno = 0;
    return kctl(11, id, (long)b, sizeof b, 0) == -1 && errno == ENOKEY_;
}

/* clone's exit_signal: the parent gets it, not SIGCHLD, when the child ends. */
static volatile int exit_sig_seen;
static void on_exit_sig(int sig) { exit_sig_seen = sig; }

static int exit_signal(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_exit_sig;
    sigaction(SIGUSR2, &sa, 0);
    sigaction(SIGCHLD, &sa, 0);
    long c = syscall(SYS_clone, SIGUSR2, 0, 0, 0, 0);
    if (c < 0) return 0;
    if (c == 0) _exit(0);
    int st;
    pid_t w;
    while ((w = waitpid((pid_t)c, &st, __WALL)) == -1 && errno == EINTR) {}   /* the exit signal interrupts the wait */
    if (w != (pid_t)c) return 0;
    for (int i = 0; i < 100 && exit_sig_seen == 0; i++) usleep(10000);   /* the signal may land just after the wait returns */
    return exit_sig_seen == SIGUSR2;
}

/* syncfs takes any open descriptor and fails EBADF on a bad one;
 * rt_tgsigqueueinfo delivers a queued signal to a thread of this process. */
static volatile int got_val;
static void on_rt(int sig, siginfo_t *si, void *ctx) { (void)sig; (void)ctx; got_val = si->si_value.sival_int; }

static int small_calls(void) {
    int fd = open("/", O_RDONLY);
    if (fd < 0) return 0;
    if (syscall(SYS_syncfs, fd) != 0) return 0;
    close(fd);
    if (syscall(SYS_syncfs, -1) != -1 || errno != EBADF) return 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_rt;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = SIGUSR1;
    si.si_code = SI_QUEUE;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value.sival_int = 77;
    if (syscall(SYS_rt_tgsigqueueinfo, getpid(), (pid_t)syscall(SYS_gettid), SIGUSR1, &si) != 0) return 0;
    return got_val == 77;
}

/* Linux AIO: requests are done at submit; events come back from io_getevents. */
struct aio_cb { uint64_t data; uint32_t key, rwf; uint16_t op, prio; uint32_t fd; uint64_t buf, nbytes; int64_t off; uint64_t res2; uint32_t flags, resfd; };
struct aio_ev { uint64_t data, obj; int64_t res, res2; };
#define AIO_SETUP SYS_io_setup
#define AIO_DESTROY SYS_io_destroy
#define AIO_SUBMIT SYS_io_submit
#define AIO_GET SYS_io_getevents
#define AIO_CANCEL SYS_io_cancel

static void aio_prep(struct aio_cb *cb, int op, int fd, void *buf, size_t n, long off, uint64_t data) {
    memset(cb, 0, sizeof *cb);
    cb->op = op; cb->fd = fd; cb->buf = (uint64_t)buf; cb->nbytes = n; cb->off = off; cb->data = data;
}

static int aio_basic(void) {
    unsigned long ctx = 0;
    struct aio_cb w, r, *list[2];
    struct aio_ev ev[2];
    char out[16] = "kernel-aio", in[16];
    int fd, ok = 0;
    fd = open("/tmp/aio.dat", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    if (syscall(AIO_SETUP, 4, &ctx) != 0 || ctx == 0) goto done;
    aio_prep(&w, 1, fd, out, 10, 0, 0x11);
    aio_prep(&r, 0, fd, in, 10, 0, 0x22);
    memset(in, 0, sizeof in);
    list[0] = &w; list[1] = &r;
    if (syscall(AIO_SUBMIT, ctx, 2, list) != 2) goto done;                      /* both, in order */
    if (syscall(AIO_GET, ctx, 2, 2, ev, NULL) != 2) goto done;
    if (ev[0].data != 0x11 || ev[0].obj != (uint64_t)&w || ev[0].res != 10) goto done;
    if (ev[1].data != 0x22 || ev[1].obj != (uint64_t)&r || ev[1].res != 10) goto done;
    if (memcmp(in, "kernel-aio", 10)) goto done;
    if (syscall(AIO_GET, ctx, 0, 2, ev, NULL) != 0) goto done;                  /* nothing left */
    ok = syscall(AIO_DESTROY, ctx) == 0;
done:
    close(fd);
    unlink("/tmp/aio.dat");
    return ok;
}

static int aio_line;
static int aio_errors(void) {
    unsigned long ctx = 0, bad = 0xffffffffffffffffUL;
    struct aio_cb cb, *list[1];
    struct aio_ev ev[1];
    char buf[16];
    int p[2], fd, ok = 0;
    if (syscall(AIO_SETUP, 1, NULL) != -1 || errno != EFAULT) return 0;
    if (syscall(AIO_SETUP, 0, &ctx) != -1 || errno != EINVAL) return 0;
    ctx = 5;
    if (syscall(AIO_SETUP, 1, &ctx) != -1 || errno != EINVAL) return 0;         /* *ctxp not zero */
    ctx = 0;
    if (syscall(AIO_SETUP, 1, &ctx) != 0) return 0;
    if (syscall(AIO_DESTROY, bad) != -1 || errno != EINVAL) { aio_line = __LINE__; goto done; }
    if (syscall(AIO_GET, bad, 0, 0, ev, NULL) != -1 || errno != EINVAL) { aio_line = __LINE__; goto done; }
    if (syscall(AIO_CANCEL, ctx, NULL, NULL) != -1 || errno != EFAULT) { aio_line = __LINE__; goto done; }
    list[0] = &cb;
    aio_prep(&cb, 0, -1, buf, 16, 0, 1);
    if (syscall(AIO_SUBMIT, ctx, 1, list) != -1 || errno != EBADF) { aio_line = __LINE__; goto done; }
    if (syscall(AIO_SUBMIT, bad, 1, list) != -1 || errno != EINVAL) { aio_line = __LINE__; goto done; }
    if (syscall(AIO_SUBMIT, ctx, 0, NULL) != 0) { aio_line = __LINE__; goto done; }
    if (syscall(AIO_SUBMIT, ctx, 1, (void *)-1) != -1 || errno != EFAULT) { aio_line = __LINE__; goto done; }
    fd = open("/tmp/aio.ro", O_RDONLY | O_CREAT, 0600);
    if (fd < 0) { aio_line = __LINE__; goto done; }
    aio_prep(&cb, 1, fd, buf, 16, 0, 1);                                        /* a write to a read-only file */
    if (syscall(AIO_SUBMIT, ctx, 1, list) != -1 || errno != EBADF) { close(fd); { aio_line = __LINE__; goto done; } }
    close(fd);
    unlink("/tmp/aio.ro");
    if (pipe(p) != 0) { aio_line = __LINE__; goto done; }
    aio_prep(&cb, 0, p[0], buf, 16, 0, 0x33);                                   /* an empty pipe is EAGAIN in the event */
    cb.rwf = 8;                                                                 /* RWF_NOWAIT */
    if (syscall(AIO_SUBMIT, ctx, 1, list) != 1) { aio_line = __LINE__; goto pipes; }
    if (syscall(AIO_GET, ctx, 1, 1, ev, NULL) != 1 || ev[0].data != 0x33 || ev[0].res != -EAGAIN) { aio_line = __LINE__; goto pipes; }
    ok = 1;
pipes:
    close(p[0]); close(p[1]);
done:
    syscall(AIO_DESTROY, ctx);
    if (!ok) errno = aio_line;
    return ok;
}

int main(void) {
    setup();
    EXPECT("sethostname, uname, /proc/sys/kernel/hostname", hostname());
    EXPECT("setdomainname", domainname());
    EXPECT("the names are written through /proc/sys/kernel/hostname and domainname", proc_names());
    EXPECT("splice reads a /proc file into a pipe", splice_proc());
    EXPECT("clock_settime moves CLOCK_REALTIME", set_clock());
    EXPECT("settimeofday", timeofday());
    EXPECT("adjtimex reads and sets", adjust());
    EXPECT("/dev/rtc0 reads and sets the hardware clock", hw_clock());
    if (begin("adjtimex slews the wall clock: frequency, PLL offset, adjtime")) {
        if (slewing()) pass(); else fail("%s", slew_why);
    }
    EXPECT("not root: EPERM", not_root());
    EXPECT("a cgroup namespace roots the tree", cgroup_ns());
    EXPECT("memory.max kills the biggest process of a group that passes it", cgroup_mem());
    EXPECT("seccomp USER_NOTIF: a supervisor answers a call", notifier());
    EXPECT("seccomp NOTIF_ADDFD installs a descriptor in the caller", notif_addfd());
    EXPECT("keys: add_key, read, describe, update, request_key, link, search, unlink", keys_basic());
    EXPECT("keys: unknown type, bad payload, expiry, revocation, no such key", keys_errors());
    EXPECT("keys: a child shares the session keyring but not the process keyring", keys_inherit());
    EXPECT("keys: request_key with a callout leaves a negative key", keys_negative());
    EXPECT("clone: the exit signal is what the parent gets", exit_signal());
    EXPECT("syncfs and rt_tgsigqueueinfo", small_calls());
    EXPECT("aio: io_submit does the requests, io_getevents returns them in order", aio_basic());
    EXPECT("aio: bad context, bad descriptor, bad pointer, and an empty pipe finishes with EAGAIN", aio_errors());
    return done();
}
