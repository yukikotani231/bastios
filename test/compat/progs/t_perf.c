/* perf_event_open for software counters: TASK_CLOCK and CPU_CLOCK count the
 * nanoseconds a process has run, CONTEXT_SWITCHES its switches; a counter
 * can be disabled, enabled and reset, reads in the read_format asked for,
 * and keeps its count when the process ends. Hardware events, sampling,
 * groups and counters for every process are refused. */
#define _GNU_SOURCE
#include "compat.h"
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>

struct attr {
    unsigned type, size;
    unsigned long long config, sample_period, sample_type, read_format, flags;
    char pad[64 - 48];
};
#define PERF_TYPE_HARDWARE 0
#define PERF_TYPE_SOFTWARE 1
#define SW_CPU_CLOCK 0
#define SW_TASK_CLOCK 1
#define SW_CSW 3
#define SW_PAGE_FAULTS 2
#define IOC_ENABLE _IO('$', 0)
#define IOC_DISABLE _IO('$', 1)
#define IOC_RESET _IO('$', 3)
#define IOC_ID _IOR('$', 7, unsigned long long)

static int perf_open(unsigned type, unsigned long long config, unsigned long long flags, unsigned long long rf, int pid, int cpu) {
    struct attr a;
    memset(&a, 0, sizeof a);
    a.type = type;
    a.size = sizeof a;
    a.config = config;
    a.flags = flags;
    a.read_format = rf;
    return (int)syscall(SYS_perf_event_open, &a, pid, cpu, -1, 0UL);
}

static unsigned long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void burn(unsigned long long ms) {
    unsigned long long end = now_ns() + ms * 1000000ull;
    volatile unsigned long x = 0;
    while (now_ns() < end) x++;
}

static long long rd(int fd) {
    unsigned long long v = 0;
    return read(fd, &v, 8) == 8 ? (long long)v : -1;
}

int main(void) {
    setup();
    int fd = perf_open(PERF_TYPE_SOFTWARE, SW_TASK_CLOCK, 0, 0, 0, -1);
    EXPECT("a task-clock counter on this process opens", fd >= 0);
    if (fd < 0) return done();
    long long a = rd(fd);
    burn(150);
    long long b = rd(fd);
    EXPECT("it counts the time the process runs (150 ms of work)", a >= 0 && b - a >= 100000000 && b - a <= 400000000);

    int cpu = perf_open(PERF_TYPE_SOFTWARE, SW_CPU_CLOCK, 0, 0, 0, -1);
    EXPECT("a cpu-clock counter opens", cpu >= 0);
    long long c0 = rd(cpu);
    burn(60);
    EXPECT("it counts too", c0 >= 0 && rd(cpu) - c0 >= 30000000);

    int off = perf_open(PERF_TYPE_SOFTWARE, SW_TASK_CLOCK, 1 /* disabled */, 0, 0, -1);
    burn(40);
    EXPECT("a disabled counter reads 0", off >= 0 && rd(off) == 0);
    EXPECT("IOC_ENABLE starts it", ioctl(off, IOC_ENABLE, 0) == 0);
    burn(60);
    long long e1 = rd(off);
    EXPECT("it counts from then", e1 >= 30000000 && e1 <= 300000000);
    EXPECT("IOC_DISABLE stops it", ioctl(off, IOC_DISABLE, 0) == 0);
    burn(60);
    EXPECT("it keeps its count", rd(off) == e1);
    EXPECT("IOC_RESET zeroes it", ioctl(off, IOC_RESET, 0) == 0 && rd(off) == 0);

    int sw = perf_open(PERF_TYPE_SOFTWARE, SW_CSW, 0, 0, 0, -1);
    long long s0 = rd(sw);
    for (int i = 0; i < 20; i++) usleep(2000);
    EXPECT("a context-switches counter sees the process sleep", s0 >= 0 && rd(sw) - s0 >= 10);

    int pf = perf_open(PERF_TYPE_SOFTWARE, SW_PAGE_FAULTS, 0, 0, 0, -1);
    EXPECT("page-faults opens (it reads 0: not counted)", pf >= 0 && rd(pf) == 0);

    /* read_format: TOTAL_TIME_ENABLED | TOTAL_TIME_RUNNING | ID */
    int rf = perf_open(PERF_TYPE_SOFTWARE, SW_TASK_CLOCK, 0, 1 | 2 | 4, 0, -1);
    unsigned long long buf[4] = {0};
    burn(30);
    EXPECT("a read with the time and id formats gives 32 bytes", rf >= 0 && read(rf, buf, sizeof buf) == 32);
    EXPECT("the time enabled is the time running, and about 30 ms", buf[1] == buf[2] && buf[1] >= 25000000 && buf[1] <= 500000000);
    unsigned long long id = 0;
    EXPECT("IOC_ID gives the id the read had", ioctl(rf, IOC_ID, &id) == 0 && id == buf[3] && id != 0);
    EXPECT("a buffer too small is ENOSPC", read(rf, buf, 8) == -1 && errno == ENOSPC);

    /* a counter on a child keeps its count once the child has been waited for */
    int go[2];
    pipe(go);
    pid_t ch = fork();
    if (ch == 0) {
        char c;
        read(go[0], &c, 1);
        burn(120);
        _exit(0);
    }
    int cc = perf_open(PERF_TYPE_SOFTWARE, SW_TASK_CLOCK, 0, 0, ch, -1);
    EXPECT("a counter on a child opens", cc >= 0);
    write(go[1], "x", 1);
    int stt;
    waitpid(ch, &stt, 0);
    long long cv = rd(cc);
    EXPECT("the child's count is kept after it is waited for", cv >= 80000000 && cv <= 600000000);
    EXPECT("and does not change", rd(cc) == cv);

    EXPECT("a hardware event is ENOENT", perf_open(PERF_TYPE_HARDWARE, 0, 0, 0, 0, -1) == -1 && errno == ENOENT);
    EXPECT("an unknown software event is ENOENT", perf_open(PERF_TYPE_SOFTWARE, 99, 0, 0, 0, -1) == -1 && errno == ENOENT);
    struct attr samp;
    memset(&samp, 0, sizeof samp);
    samp.type = PERF_TYPE_SOFTWARE;
    samp.size = sizeof samp;
    samp.config = SW_TASK_CLOCK;
    samp.sample_period = 100000;
    EXPECT("sampling is EOPNOTSUPP", syscall(SYS_perf_event_open, &samp, 0, -1, -1, 0UL) == -1 && errno == EOPNOTSUPP);
    EXPECT("all processes on a cpu is EOPNOTSUPP", perf_open(PERF_TYPE_SOFTWARE, SW_TASK_CLOCK, 0, 0, -1, 0) == -1 && errno == EOPNOTSUPP);
    EXPECT("pid and cpu both -1 is EINVAL", perf_open(PERF_TYPE_SOFTWARE, SW_TASK_CLOCK, 0, 0, -1, -1) == -1 && errno == EINVAL);
    EXPECT("a process that does not exist is ESRCH", perf_open(PERF_TYPE_SOFTWARE, SW_TASK_CLOCK, 0, 0, 900, -1) == -1 && errno == ESRCH);
    samp.sample_period = 0;
    samp.size = 8;
    EXPECT("an attr size under 64 is EINVAL", syscall(SYS_perf_event_open, &samp, 0, -1, -1, 0UL) == -1 && errno == EINVAL);
    samp.size = sizeof samp;
    EXPECT("an unknown flag is EINVAL", syscall(SYS_perf_event_open, &samp, 0, -1, -1, 0x100UL) == -1 && errno == EINVAL);
    EXPECT("an unknown ioctl is ENOTTY", ioctl(fd, _IO('$', 0x7f), 0) == -1 && errno == ENOTTY);

    EXPECT("an unprivileged program cannot count init", ({
        int ok = 0;
        pid_t c = fork();
        if (c == 0) {
            if (setgid(100) || setuid(65534)) _exit(2);
            _exit(perf_open(PERF_TYPE_SOFTWARE, SW_TASK_CLOCK, 0, 0, 1, -1) == -1 && errno == EACCES ? 0 : 1);
        }
        int s2 = 0;
        waitpid(c, &s2, 0);
        ok = WIFEXITED(s2) && WEXITSTATUS(s2) == 0;
        ok;
    }));
    return done();
}
