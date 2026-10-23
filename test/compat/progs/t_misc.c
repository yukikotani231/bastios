/* The rest of what a program's start-up and libc touch: randomness, the
 * terminal, process control knobs, file system information. */
#include "compat.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <net/if.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <termios.h>
#include <pwd.h>
#include <sched.h>
#include <errno.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* The classic BPF and seccomp definitions musl does not ship. */
struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };
#define BPF_STMT(c, k) { (c), 0, 0, (k) }
#define BPF_JUMP(c, k, jt, jf) { (c), (jt), (jf), (k) }
#define BPF_LD_W_ABS 0x20
#define BPF_JEQ_K 0x15
#define BPF_RET_K 0x06
#define SECCOMP_MODE_FILTER 2
#define SECCOMP_RET_ALLOW 0x7fff0000u
#define SECCOMP_RET_ERRNO 0x00050000u

/* The number after `key` in the file at path, or -1. */
static long file_num(const char *path, const char *key) {
    char b[4096];
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;
    ssize_t n = read(f, b, sizeof b - 1);
    close(f);
    if (n <= 0) return -1;
    b[n] = 0;
    char *q = strstr(b, key);
    return q ? atol(q + strlen(key)) : -1;
}

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
    EXPECT("/proc/stat has cpu lines", ({
        char t[2048]; int f = open("/proc/stat", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        strncmp(t, "cpu ", 4) == 0 && strstr(t, "\ncpu0 ") && strstr(t, "\nbtime ") && strstr(t, "\nprocesses "); }));
    EXPECT("/proc/net/dev lists lo and eth0", ({
        char t[2048]; int f = open("/proc/net/dev", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        strstr(t, "Receive") && strstr(t, "lo:") && strstr(t, "eth0:"); }));
    EXPECT("/proc/net is a directory", ({ struct stat st; stat("/proc/net", &st) == 0 && S_ISDIR(st.st_mode); }));
    EXPECT("/proc/modules opens", open("/proc/modules", O_RDONLY) >= 0);
    EXPECT("/proc/net/udp lists a bound socket", ({
        int us = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in ua = {.sin_family = AF_INET, .sin_port = htons(40124)};
        int ok = us >= 0 && bind(us, (struct sockaddr *)&ua, sizeof ua) == 0;
        char t[4096]; int f = open("/proc/net/udp", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        if (us >= 0) close(us);
        ok && strstr(t, "local_address") && strstr(t, "00000000:9CBC 00000000:0000 07 "); }));
    EXPECT("/proc/net/unix lists a bound socket", ({
        int xs = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un xa = {.sun_family = AF_UNIX, .sun_path = "/pnu_sock"};
        unlink(xa.sun_path);
        int ok = xs >= 0 && bind(xs, (struct sockaddr *)&xa, sizeof xa) == 0 && listen(xs, 1) == 0;
        char t[4096]; int f = open("/proc/net/unix", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        if (xs >= 0) close(xs);
        unlink(xa.sun_path);
        ok && strstr(t, "RefCount") && strstr(t, " 00010000 0001 01 ") && strstr(t, " /pnu_sock\n"); }));
    EXPECT("SIOCGIFHWADDR and SIOCGIFNETMASK on lo", ({
        int is = socket(AF_INET, SOCK_DGRAM, 0); struct ifreq hw, nm;
        memset(&hw, 0, sizeof hw); memset(&nm, 0, sizeof nm);
        strcpy(hw.ifr_name, "lo"); strcpy(nm.ifr_name, "lo");
        int ok = is >= 0 && ioctl(is, SIOCGIFHWADDR, &hw) == 0 && ioctl(is, SIOCGIFNETMASK, &nm) == 0;
        if (is >= 0) close(is);
        ok && hw.ifr_hwaddr.sa_family == 772 && ((unsigned char *)&nm.ifr_netmask)[4] == 255 && ((unsigned char *)&nm.ifr_netmask)[5] == 0; }));
    EXPECT("/proc/self/limits shows the open-files limit", ({
        struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl); rl.rlim_cur = rl.rlim_cur < 100 ? rl.rlim_cur : 100;
        setrlimit(RLIMIT_NOFILE, &rl);
        char t[2048]; int f = open("/proc/self/limits", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        char want[64]; snprintf(want, sizeof want, "Max open files            %lu ", (unsigned long)rl.rlim_cur);
        strstr(t, "Soft Limit") && strstr(t, "Max address space") && strstr(t, want); }));
    EXPECT("/proc/self/mountinfo lists the root and /proc", ({
        char t[2048]; int f = open("/proc/self/mountinfo", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        strstr(t, " / / rw") && strstr(t, " / /proc rw") && strstr(t, " - proc proc rw\n"); }));
    EXPECT("/proc/self/statm counts the pages and io has its counters", ({
        char t[256]; int f = open("/proc/self/statm", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        unsigned long sz = 0, res = 0, sh = 1; sscanf(t, "%lu %lu %lu", &sz, &res, &sh);
        char u[256]; f = open("/proc/self/io", O_RDONLY); n = f < 0 ? 0 : read(f, u, sizeof u - 1);
        if (f >= 0) close(f);
        u[n > 0 ? n : 0] = 0;
        sz > 0 && res > 0 && sh == 0 && strstr(u, "rchar: ") && strstr(u, "write_bytes: "); }));
    EXPECT("/proc/self/smaps_rollup sums the resident pages, as psutil's uss reads them", ({
        char t[1024]; int f = open("/proc/self/smaps_rollup", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        unsigned long rss = 0, priv = 0; char *p = strstr(t, "\nRss:"), *q = strstr(t, "\nPrivate_Dirty:");
        if (p) sscanf(p + 5, "%lu", &rss);
        if (q) sscanf(q + 15, "%lu", &priv);
        rss > 0 && priv == rss && strstr(t, "\nPss:") && strstr(t, "\nSwap:"); }));
    EXPECT("core_pattern and nr_open read, drop_caches takes 3 and refuses 9", ({
        char t[64], u[64]; int f = open("/proc/sys/kernel/core_pattern", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        f = open("/proc/sys/fs/nr_open", O_RDONLY); n = f < 0 ? 0 : read(f, u, sizeof u - 1);
        if (f >= 0) close(f);
        u[n > 0 ? n : 0] = 0;
        f = open("/proc/sys/vm/drop_caches", O_WRONLY); int ok = f >= 0 && write(f, "3", 1) == 1;
        errno = 0; int bad = f >= 0 && write(f, "9", 1) < 0 && errno == EINVAL;
        if (f >= 0) close(f);
        strcmp(t, "core\n") == 0 && atol(u) == 1048576 && ok && bad; }));
    EXPECT("/proc/net/arp has its header", ({
        char t[4096]; int f = open("/proc/net/arp", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        strstr(t, "IP address") && strstr(t, "HW address") && strstr(t, "Device"); }));
    EXPECT("/proc/net/raw lists a raw ICMP socket", ({
        int rs = socket(AF_INET, SOCK_RAW, 1);
        char t[4096]; int f = open("/proc/net/raw", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        if (rs >= 0) close(rs);
        rs >= 0 && strstr(t, "local_address") && strstr(t, "00000000:0001 00000000:0000 07 "); }));
    EXPECT("/proc/net/route lists the default route", ({
        char t[4096]; int f = open("/proc/net/route", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        strstr(t, "Iface\tDestination\tGateway") && strstr(t, "\t00000000\t") && strstr(t, "\t0003\t"); }));
    EXPECT("/proc/net/tcp lists a listening socket", ({
        int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(40123)};
        int ok = ls >= 0 && bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(ls, 1) == 0;
        char t[4096]; int f = open("/proc/net/tcp", O_RDONLY); int n = f < 0 ? 0 : read(f, t, sizeof t - 1);
        if (f >= 0) close(f);
        t[n > 0 ? n : 0] = 0;
        if (ls >= 0) close(ls);
        ok && strstr(t, "local_address") && strstr(t, "00000000:9CBB 00000000:0000 0A "); }));
    EXPECT("syslog (klogctl) reports a buffer size and reads nothing; an unknown type is EINVAL", ({ char kb[64];
        long sz = syscall(SYS_syslog, 10, 0, 0), rd = syscall(SYS_syslog, 3, kb, sizeof kb);
        sz > 0 && rd == 0 && syscall(SYS_syslog, 99, 0, 0) == -1 && errno == EINVAL
        && syscall(SYS_syslog, 2, 0, 8) == -1 && errno == EINVAL && syscall(SYS_syslog, 8, kb, 9) == -1 && errno == EINVAL; }));
    EXPECT("/proc/diskstats opens", open("/proc/diskstats", O_RDONLY) >= 0);
    EXPECT("/proc/diskstats has the disk's line with its counts", ({ char t[256]; int f = open("/proc/diskstats", O_RDONLY);
        int n = f >= 0 ? read(f, t, sizeof t - 1) : -1; if (f >= 0) close(f); t[n > 0 ? n : 0] = 0;
        n > 0 && strstr(t, " bastio0 ") && t[n - 1] == '\n'; }));
    EXPECT("/proc/partitions lists the disk that /proc/diskstats counts, with its size in 1 KiB blocks", ({ char t[256]; int f = open("/proc/partitions", O_RDONLY);
        int n = f >= 0 ? read(f, t, sizeof t - 1) : -1; if (f >= 0) close(f); t[n > 0 ? n : 0] = 0;
        unsigned long blk = 0; char *q = strstr(t, "\n   8        0 "); if (q) sscanf(q + 16, "%lu", &blk);
        n > 0 && strncmp(t, "major minor  #blocks  name\n", 27) == 0 && blk > 0 && strstr(t, " bastio0\n"); }));
    EXPECT("/etc/passwd", open("/etc/passwd", O_RDONLY) >= 0);
    EXPECT("getlogin or getpwuid", getpwuid(getuid()) != NULL);
    EXPECT("sched_getaffinity", ({ cpu_set_t c; sched_getaffinity(0, sizeof c, &c) == 0; }));
    EXPECT("sched_getaffinity names every processor /proc/stat lists, as sysconf counts them", ({ cpu_set_t c; int n = sched_getaffinity(0, sizeof c, &c) == 0 ? CPU_COUNT(&c) : -1;
        char t[4096]; int f = open("/proc/stat", O_RDONLY); int r = f >= 0 ? read(f, t, sizeof t - 1) : -1; if (f >= 0) close(f); t[r > 0 ? r : 0] = 0;
        int m = 0; for (char *q = t; (q = strstr(q, "\ncpu")) != 0; q += 4) m++;
        n == m && m >= 1 && sysconf(_SC_NPROCESSORS_ONLN) == m; }));
    EXPECT("sched_setaffinity to a processor that is not there is EINVAL", ({ cpu_set_t c; CPU_ZERO(&c); CPU_SET(100, &c); sched_setaffinity(0, sizeof c, &c) == -1 && errno == EINVAL; }));
    EXPECT("sched_getaffinity returns the mask sched_setaffinity set", ({ cpu_set_t all, one, got; sched_getaffinity(0, sizeof all, &all); CPU_ZERO(&one); CPU_SET(0, &one); int ok = sched_setaffinity(0, sizeof one, &one) == 0 && sched_getaffinity(0, sizeof got, &got) == 0 && CPU_COUNT(&got) == 1 && CPU_ISSET(0, &got); ok = ok && sched_setaffinity(0, sizeof all, &all) == 0 && sched_getaffinity(0, sizeof got, &got) == 0 && CPU_COUNT(&got) == CPU_COUNT(&all); ok; }));
    EXPECT("voluntary_ctxt_switches counts the sleeps, and /proc/stat ctxt and getrusage count them too", ({ long v0 = file_num("/proc/self/status", "voluntary_ctxt_switches:"), c0 = file_num("/proc/stat", "ctxt "); struct rusage r0, r1; getrusage(RUSAGE_SELF, &r0); struct timespec ts = {0, 5000000}; for (int i = 0; i < 4; i++) nanosleep(&ts, 0); getrusage(RUSAGE_SELF, &r1);
        v0 >= 0 && file_num("/proc/self/status", "voluntary_ctxt_switches:") >= v0 + 4 && file_num("/proc/stat", "ctxt ") >= c0 + 4 && r1.ru_nvcsw >= r0.ru_nvcsw + 4 && file_num("/proc/stat", "softirq ") >= 0; }));
    EXPECT("/proc/self/io counts the read and write calls and their characters", ({ long r0 = file_num("/proc/self/io", "syscr: "), c0 = file_num("/proc/self/io", "rchar: "), w0 = file_num("/proc/self/io", "syscw: "), x0 = file_num("/proc/self/io", "wchar: "); char b[100]; int f = open("/proc/version", O_RDONLY); ssize_t n = f >= 0 ? read(f, b, sizeof b) : -1; if (f >= 0) close(f); int g = open("/dev/null", O_WRONLY); ssize_t m = g >= 0 ? write(g, b, 10) : -1; if (g >= 0) close(g);
        long r1 = file_num("/proc/self/io", "syscr: "), c1 = file_num("/proc/self/io", "rchar: "), w1 = file_num("/proc/self/io", "syscw: "), x1 = file_num("/proc/self/io", "wchar: ");
        errno = r0 < 0 ? 1 : n <= 0 ? 2 : m != 10 ? 3 : r1 < r0 + 1 ? 4 : c1 < c0 + n ? 5 : w1 != w0 + 1 ? 6 : x1 < x0 + 10 ? 7 : 0; errno == 0; }));
    EXPECT("getcpu names a processor the affinity mask allows", ({ cpu_set_t all, one; sched_getaffinity(0, sizeof all, &all); int c = -1; for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &all)) c = i; CPU_ZERO(&one); CPU_SET(c, &one); int ok = c >= 0 && sched_setaffinity(0, sizeof one, &one) == 0 && sched_getcpu() == c; sched_setaffinity(0, sizeof all, &all); ok; }));
    EXPECT("SSE floating point", ({ volatile double x = 2.0; x * x / 3.0 > 1.33; }));
    EXPECT("printf %f", (snprintf(b, sizeof b, "%.3f", 3.14159), strcmp(b, "3.142") == 0));
    EXPECT("strtod", strtod("2.5e3", 0) == 2500.0);
    EXPECT("qsort", ({ int v[5] = {5, 1, 4, 2, 3}; int cmp(const void *x, const void *y) { return *(int *)x - *(int *)y; }
           qsort(v, 5, sizeof *v, cmp); v[0] == 1 && v[4] == 5; }));
    /* A seccomp filter sees getpid and gettid too, which the kernel
     * would otherwise answer without looking at them. */
    if (begin("seccomp filter denies getpid")) {
        pid_t c = fork();
        if (c == 0) {
            struct sock_filter f[] = {
                BPF_STMT(BPF_LD_W_ABS, 0),
                BPF_JUMP(BPF_JEQ_K, SYS_getpid, 0, 1),
                BPF_STMT(BPF_RET_K, SECCOMP_RET_ERRNO | EPERM),
                BPF_STMT(BPF_RET_K, SECCOMP_RET_ALLOW),
            };
            struct sock_fprog prog = { sizeof f / sizeof f[0], f };
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(10);
            if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) _exit(11);
            long r = syscall(SYS_getpid);
            _exit(r == -1 && errno == EPERM && syscall(SYS_gettid) > 0 ? 0 : 12);
        }
        int st = 0;
        if (c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0) pass();
        else fail("status %#x", st);
    }
    /* Forty filters stacked on one thread (eight was the system-wide
     * limit once), the newest denying getpid. */
    if (begin("forty stacked seccomp filters")) {
        pid_t c = fork();
        if (c == 0) {
            struct sock_filter ok[] = { BPF_STMT(BPF_RET_K, SECCOMP_RET_ALLOW) };
            struct sock_fprog pok = { 1, ok };
            struct sock_filter f[] = {
                BPF_STMT(BPF_LD_W_ABS, 0),
                BPF_JUMP(BPF_JEQ_K, SYS_getpid, 0, 1),
                BPF_STMT(BPF_RET_K, SECCOMP_RET_ERRNO | EPERM),
                BPF_STMT(BPF_RET_K, SECCOMP_RET_ALLOW),
            };
            struct sock_fprog prog = { sizeof f / sizeof f[0], f };
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(10);
            for (int i = 0; i < 39; i++)
                if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &pok) != 0) _exit(11);
            if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) _exit(12);
            long r = syscall(SYS_getpid);
            _exit(r == -1 && errno == EPERM && syscall(SYS_gettid) > 0 ? 0 : 13);
        }
        int st = 0;
        if (c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0) pass();
        else fail("status %#x", st);
    }
#ifdef __aarch64__
    /* The Go runtime reads ID_AA64ISAR0_EL1 when it starts; Linux emulates
     * the read (HWCAP_CPUID) instead of killing the program. */
    EXPECT("an EL0 read of ID_AA64ISAR0_EL1 is emulated", ({
        unsigned long v = 123;
        __asm__ volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(v));
        v == 0;
    }));
#endif
    return done();
}
