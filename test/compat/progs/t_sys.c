/* Settings root may change: the host and domain names, the wall clock and
 * the NTP clock discipline; a user who is not root gets EPERM for each. */
#include "compat.h"
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

int main(void) {
    setup();
    EXPECT("sethostname, uname, /proc/sys/kernel/hostname", hostname());
    EXPECT("setdomainname", domainname());
    EXPECT("clock_settime moves CLOCK_REALTIME", set_clock());
    EXPECT("settimeofday", timeofday());
    EXPECT("adjtimex reads and sets", adjust());
    EXPECT("not root: EPERM", not_root());
    return done();
}
