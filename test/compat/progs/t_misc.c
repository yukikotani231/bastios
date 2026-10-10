/* The rest of what a program's start-up and libc touch: randomness, the
 * terminal, process control knobs, file system information. */
#include "compat.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <termios.h>
#include <pwd.h>
#include <sched.h>

int main(void) {
    setup();
    char b[32];
    struct termios t;
    struct winsize w;

    EXPECT("getrandom", getrandom(b, sizeof b, 0) == sizeof b);
    EXPECT("tcgetattr", tcgetattr(0, &t) == 0);
    EXPECT("tcsetattr", tcsetattr(0, TCSANOW, &t) == 0);
    EXPECT("TIOCGWINSZ", ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0);
    EXPECT("tcgetpgrp", tcgetpgrp(0) > 0);
    EXPECT("prctl PR_SET_NAME", prctl(PR_SET_NAME, "compat") == 0);
    {
        struct statfs s;
        EXPECT("statfs", statfs("/", &s) == 0);
    }
    EXPECT("sync", (sync(), 1));
    EXPECT("/proc/self/exe", readlink("/proc/self/exe", b, sizeof b) > 0);
    EXPECT("/proc/meminfo", open("/proc/meminfo", O_RDONLY) >= 0);
    EXPECT("/etc/passwd", open("/etc/passwd", O_RDONLY) >= 0);
    EXPECT("getlogin or getpwuid", getpwuid(getuid()) != NULL);
    EXPECT("sched_getaffinity", ({ cpu_set_t c; sched_getaffinity(0, sizeof c, &c) == 0; }));
    EXPECT("SSE floating point", ({ volatile double x = 2.0; x * x / 3.0 > 1.33; }));
    EXPECT("printf %f", (snprintf(b, sizeof b, "%.3f", 3.14159), strcmp(b, "3.142") == 0));
    EXPECT("strtod", strtod("2.5e3", 0) == 2500.0);
    EXPECT("qsort", ({ int v[5] = {5, 1, 4, 2, 3}; int cmp(const void *x, const void *y) { return *(int *)x - *(int *)y; }
           qsort(v, 5, sizeof *v, cmp); v[0] == 1 && v[4] == 5; }));
    return done();
}
