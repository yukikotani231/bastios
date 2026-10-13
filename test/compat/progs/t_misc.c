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
    EXPECT("/etc/passwd", open("/etc/passwd", O_RDONLY) >= 0);
    EXPECT("getlogin or getpwuid", getpwuid(getuid()) != NULL);
    EXPECT("sched_getaffinity", ({ cpu_set_t c; sched_getaffinity(0, sizeof c, &c) == 0; }));
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
    return done();
}
