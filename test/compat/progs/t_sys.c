/* Settings root may change: the host and domain names, the wall clock and
 * the NTP clock discipline, and a cgroup namespace; a user who is not root gets EPERM for each. */
#include "compat.h"
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>

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

/* memory.max: memory.current counts the memory of the group's processes,
 * and a mapping or a fork that would pass the limit fails with ENOMEM
 * (a PROT_NONE reservation is not counted). */
static int cgroup_mem(void) {
    if (mkdir("/sys/fs/cgroup/cgm", 0755) != 0) return 0;
    pid_t p = fork();
    if (p == 0) {
        char me[32], buf[64];
        snprintf(me, sizeof me, "%d", (int)getpid());
        if (!put("/sys/fs/cgroup/cgm/cgroup.procs", me)) _exit(1);
        if (!put("/sys/fs/cgroup/cgm/memory.max", "16777216")) _exit(2);
        char *a = mmap(0, 4 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (a == MAP_FAILED) _exit(3);
        memset(a, 1, 4 << 20);
        if (!slurp("/sys/fs/cgroup/cgm/memory.current", buf, sizeof buf) || atol(buf) < (4 << 20)) _exit(4);
        void *big = mmap(0, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (big != MAP_FAILED || errno != ENOMEM) _exit(5);
        void *res = mmap(0, 64 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (res == MAP_FAILED) _exit(6);
        char *m = mmap(0, 4 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) _exit(7);
        memset(m, 1, 4 << 20);
        char *third = mmap(0, 4 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (third == MAP_FAILED) _exit(8);
        memset(third, 1, 4 << 20);
        /* 12 MiB in use of 16: another 6 MiB does not fit, nor does a copy of it all */
        if (mmap(0, 6 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED || errno != ENOMEM) _exit(9);
        if (fork() != -1 || errno != ENOMEM) _exit(10);
        if (!put("/sys/fs/cgroup/cgm/memory.max", "max")) _exit(11);
        pid_t c = fork();
        if (c == 0) _exit(0);
        if (c < 0) _exit(12);
        int cst;
        waitpid(c, &cst, 0);
        if (!put("/sys/fs/cgroup/cgroup.procs", me)) _exit(13);
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    ok = rmdir("/sys/fs/cgroup/cgm") == 0 && ok;
    return ok;
}

int main(void) {
    setup();
    EXPECT("sethostname, uname, /proc/sys/kernel/hostname", hostname());
    EXPECT("setdomainname", domainname());
    EXPECT("clock_settime moves CLOCK_REALTIME", set_clock());
    EXPECT("settimeofday", timeofday());
    EXPECT("adjtimex reads and sets", adjust());
    EXPECT("not root: EPERM", not_root());
    EXPECT("a cgroup namespace roots the tree", cgroup_ns());
    EXPECT("memory.max refuses a mapping that would pass it", cgroup_mem());
    return done();
}
