/* What a service manager (OpenRC, and the daemons it starts) asks of the
 * kernel: dropping root and keeping a capability (chronyd, dnsmasq),
 * crediting entropy at boot (seedrng), restarting or powering off a PID
 * namespace instead of the machine (a container's init, a test boot),
 * the sysctls Alpine's /etc/sysctl.d sets, and the file system types that
 * devfs looks for. */
#define _GNU_SOURCE
#include "compat.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

#define DAEMON 100

/* musl has no linux/capability.h or linux/reboot.h. */
#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#define CAP_NET_BIND_SERVICE 10
#define CAP_SYS_TIME 25
#define CAP_SYS_NICE 23
struct __user_cap_header_struct { unsigned version; int pid; };
struct __user_cap_data_struct { unsigned effective, permitted, inheritable; };
#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_CMD_RESTART 0x01234567
#define LINUX_REBOOT_CMD_HALT 0xCDEF0123
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC
#define LINUX_REBOOT_CMD_CAD_OFF 0

static int in_child(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) _exit(f() ? 0 : 1);
    int st;
    return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int capget_sets(unsigned *eff, unsigned *prm) {
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2];
    if (syscall(SYS_capget, &h, d) != 0) return -1;
    *eff = d[0].effective;
    *prm = d[0].permitted;
    return 0;
}

/* Keeps CAP_NET_BIND_SERVICE and CAP_SYS_TIME in the effective set, the
 * rest of the permitted set staying where it is. */
static int hold(unsigned eff) {
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2];
    if (syscall(SYS_capget, &h, d) != 0) return -1;
    d[0].effective = eff;
    d[1].effective = 0;
    return syscall(SYS_capset, &h, d);
}

static int try_bind(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int r = bind(s, (struct sockaddr *)&a, sizeof a);
    int e = errno;
    close(s);
    errno = e;
    return r;
}

/* chronyd's way: setuid with PR_SET_KEEPCAPS empties the effective set and
 * keeps the permitted one; capset then picks what to hold. */
static int keepcaps_drop(void) {
    unsigned eff, prm;
    if (prctl(PR_SET_KEEPCAPS, 1) != 0) return 0;
    if (setresuid(DAEMON, DAEMON, DAEMON) != 0) return 0;
    if (capget_sets(&eff, &prm) != 0 || eff != 0 || (prm & (1u << CAP_SYS_TIME)) == 0) return 0;
    if (try_bind(81) == 0 || errno != EACCES) return 0;
    unsigned want = (1u << CAP_NET_BIND_SERVICE) | (1u << CAP_SYS_TIME);
    if (hold(want) != 0) return 0;
    if (capget_sets(&eff, &prm) != 0 || eff != want) return 0;
    return try_bind(81) == 0;
}

static int keepcaps_clock(void) {
    if (prctl(PR_SET_KEEPCAPS, 1) != 0 || setresuid(DAEMON, DAEMON, DAEMON) != 0) return 0;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    if (clock_settime(CLOCK_REALTIME, &t) == 0 || errno != EPERM) return 0;
    if (hold(1u << CAP_SYS_TIME) != 0) return 0;
    return clock_settime(CLOCK_REALTIME, &t) == 0;
}

/* Nothing held beyond what the permitted set had. */
static int keepcaps_no_more(void) {
    if (prctl(PR_SET_KEEPCAPS, 1) != 0 || setresuid(DAEMON, DAEMON, DAEMON) != 0) return 0;
    if (hold(1u << CAP_SYS_TIME) != 0) return 0;
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2];
    if (syscall(SYS_capget, &h, d) != 0) return 0;
    d[0].permitted = d[0].effective = 1u << CAP_SYS_TIME;
    if (syscall(SYS_capset, &h, d) != 0) return 0;
    d[0].permitted = d[0].effective = (1u << CAP_SYS_TIME) | (1u << CAP_SYS_NICE);
    return syscall(SYS_capset, &h, d) == -1 && errno == EPERM;
}

static int plain_drop(void) {
    unsigned eff, prm;
    if (setresuid(DAEMON, DAEMON, DAEMON) != 0) return 0;
    if (capget_sets(&eff, &prm) != 0 || eff != 0 || prm != 0) return 0;
    return try_bind(82) == -1 && errno == EACCES;
}

/* The kept sets end at execve. */
static int keepcaps_exec(void) {
    if (prctl(PR_SET_KEEPCAPS, 1) != 0 || setresuid(DAEMON, DAEMON, DAEMON) != 0) return 0;
    if (hold(1u << CAP_SYS_TIME) != 0) return 0;
    execl("/proc/self/exe", "t_svc", "--caps-after-exec", (char *)0);
    return 0;
}

static int caps_after_exec(void) {
    unsigned eff, prm;
    return capget_sets(&eff, &prm) == 0 && eff == 0 && prm == 0 ? 0 : 1;
}

/* A root user id coming back ends them. */
static int keepcaps_root_again(void) {
    unsigned eff, prm;
    if (prctl(PR_SET_KEEPCAPS, 1) != 0 || setresuid(DAEMON, DAEMON, 0) != 0) return 0;
    if (setresuid(0, 0, 0) != 0) return 0;
    if (capget_sets(&eff, &prm) != 0 || eff == 0) return 0;
    return prctl(PR_SET_KEEPCAPS, 0) == 0 && setresuid(DAEMON, DAEMON, DAEMON) == 0
        && capget_sets(&eff, &prm) == 0 && eff == 0 && prm == 0;
}

static int status_caps(void) {
    char t[2048];
    int f = open("/proc/self/status", O_RDONLY);
    int n = f < 0 ? -1 : read(f, t, sizeof t - 1);
    if (f >= 0) close(f);
    if (n <= 0) return 0;
    t[n] = 0;
    return strstr(t, "\nCapInh:\t0000000000000000\n") && strstr(t, "\nCapPrm:\t000001ffffffffff\n")
        && strstr(t, "\nCapEff:\t000001ffffffffff\n") && strstr(t, "\nCapBnd:\t000001ffffffffff\n")
        && strstr(t, "\nCapAmb:\t0000000000000000\n");
}

static int status_caps_kept(void) {
    char t[2048];
    if (prctl(PR_SET_KEEPCAPS, 1) != 0 || setresuid(DAEMON, DAEMON, DAEMON) != 0) return 0;
    if (hold(1u << CAP_SYS_TIME) != 0) return 0;
    int f = open("/proc/self/status", O_RDONLY);
    int n = f < 0 ? -1 : read(f, t, sizeof t - 1);
    if (f >= 0) close(f);
    if (n <= 0) return 0;
    t[n] = 0;
    return strstr(t, "\nCapPrm:\t000001ffffffffff\n") && strstr(t, "\nCapEff:\t0000000002000000\n");
}

static int rnd_ioctls(void) {
    int f = open("/dev/urandom", O_RDWR);
    if (f < 0) return 0;
    struct { int entropy_count, buf_size; char buf[32]; } req = { 256, 32, "0123456789abcdef0123456789abcdef" };
    int a = ioctl(f, 0x40085203 /* RNDADDENTROPY */, &req);
    int b = ioctl(f, 0x5207 /* RNDRESEEDCRNG */);
    close(f);
    return a == 0 && b == 0;
}

static int rnd_ioctl_user(void) {
    int f = open("/dev/urandom", O_RDONLY);
    if (f < 0) return 0;
    if (setresuid(DAEMON, DAEMON, DAEMON) != 0) return 0;
    struct { int entropy_count, buf_size; char buf[32]; } req = { 256, 32, "0123456789abcdef0123456789abcdef" };
    int a = ioctl(f, 0x40085203, &req);
    return a == -1 && errno == EPERM;
}

static int read_file(const char *path, char *t, int size) {
    int f = open(path, O_RDONLY);
    int n = f < 0 ? -1 : read(f, t, size - 1);
    if (f >= 0) close(f);
    if (n < 0) return -1;
    t[n] = 0;
    return n;
}

static int write_file(const char *path, const char *s) {
    int f = open(path, O_WRONLY);
    int n = f < 0 ? -1 : write(f, s, strlen(s));
    if (f >= 0) close(f);
    return n;
}

/* One sysctl: it reads as `def`, takes `val` and gives it back. */
static int sysctl(const char *path, const char *def, const char *val) {
    char t[64], was[64];
    if (read_file(path, t, sizeof t) < 0 || strcmp(t, def) != 0) return 0;
    strcpy(was, t);
    if (write_file(path, val) < 0) return 0;
    char v[64];
    strcpy(v, val);
    if (read_file(path, t, sizeof t) < 0) return 0;
    int ok = strncmp(t, v, strlen(v) - (v[strlen(v) - 1] == '\n')) == 0;
    write_file(path, was);
    return ok;
}

static int net_sysctls(void) {
    return sysctl("/proc/sys/net/ipv4/conf/all/rp_filter", "0\n", "1\n")
        && sysctl("/proc/sys/net/ipv4/conf/default/rp_filter", "0\n", "2\n")
        && sysctl("/proc/sys/net/ipv4/conf/all/accept_redirects", "1\n", "0\n")
        && sysctl("/proc/sys/net/ipv4/conf/all/secure_redirects", "1\n", "0\n")
        && sysctl("/proc/sys/net/ipv4/conf/all/accept_source_route", "0\n", "1\n")
        && sysctl("/proc/sys/net/ipv4/tcp_rfc1337", "0\n", "1\n")
        && sysctl("/proc/sys/net/ipv6/conf/all/accept_redirects", "1\n", "0\n")
        && sysctl("/proc/sys/net/ipv6/conf/all/accept_source_route", "0\n", "1\n")
        && sysctl("/proc/sys/net/ipv6/conf/all/use_tempaddr", "0\n", "2\n")
        && sysctl("/proc/sys/net/ipv6/conf/default/use_tempaddr", "0\n", "2\n")
        && sysctl("/proc/sys/kernel/unprivileged_bpf_disabled", "2\n", "1\n");
}

static int ping_range(void) {
    char t[64];
    if (read_file("/proc/sys/net/ipv4/ping_group_range", t, sizeof t) < 0 || strcmp(t, "1\t0\n") != 0) return 0;
    if (write_file("/proc/sys/net/ipv4/ping_group_range", "0 2147483647\n") < 0) return 0;
    if (read_file("/proc/sys/net/ipv4/ping_group_range", t, sizeof t) < 0 || strcmp(t, "0\t2147483647\n") != 0) return 0;
    errno = 0;
    if (write_file("/proc/sys/net/ipv4/ping_group_range", "5\n") != -1 || errno != EINVAL) return 0;
    return write_file("/proc/sys/net/ipv4/ping_group_range", "1 0\n") > 0;
}

static int dir_lists(const char *dir, const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "%s/%s", dir, name);
    return access(cmd, F_OK) == 0;
}

static int filesystem_types(void) {
    char t[512];
    if (read_file("/proc/filesystems", t, sizeof t) < 0) return 0;
    return strstr(t, "nodev\tdevtmpfs\n") && strstr(t, "nodev\tdevpts\n") && strstr(t, "nodev\tsysfs\n")
        && strstr(t, "nodev\ttmpfs\n") && dir_lists("/proc/sys/net/ipv4/conf", "all");
}

/* reboot(2) in a PID namespace ends the namespace, not the machine. The
 * child here is the init of a new one: its reboot is seen by the parent as
 * the child's end by SIGHUP (restart) or SIGINT (halt, power off). */
static int ns_reboot(int cmd, int sig) {
    pid_t c = fork();
    if (c == 0) {
        if (unshare(CLONE_NEWPID) != 0) _exit(10);
        pid_t i = fork();
        if (i == 0) {
            syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, cmd, 0);
            _exit(11);
        }
        int st;
        waitpid(i, &st, 0);
        _exit(WIFSIGNALED(st) ? 100 + WTERMSIG(st) : 12);
    }
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st)) return 0;
    return WEXITSTATUS(st) == 100 + sig;
}

static int ns_reboot_restart(void) { return ns_reboot(LINUX_REBOOT_CMD_RESTART, SIGHUP); }
static int ns_reboot_poweroff(void) { return ns_reboot(LINUX_REBOOT_CMD_POWER_OFF, SIGINT); }
static int ns_reboot_halt(void) { return ns_reboot(LINUX_REBOOT_CMD_HALT, SIGINT); }

static int ns_reboot_cad(void) {
    pid_t c = fork();
    if (c == 0) {
        if (unshare(CLONE_NEWPID) != 0) _exit(10);
        pid_t i = fork();
        if (i == 0) {
            long r = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_CAD_OFF, 0);
            _exit(r == -1 && errno == EINVAL ? 0 : 1);
        }
        int st;
        waitpid(i, &st, 0);
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 2);
    }
    int st;
    return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* Not root: no reboot at all. */
static int reboot_not_root(void) {
    if (setresuid(DAEMON, DAEMON, DAEMON) != 0) return 0;
    return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, 0) == -1 && errno == EPERM;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--caps-after-exec") == 0) return caps_after_exec();
    setup();
    int root = geteuid() == 0;
    if (root) {
        EXPECT("setuid with PR_SET_KEEPCAPS keeps the permitted set, and capset holds part of it", in_child(keepcaps_drop));
        EXPECT("a process holding CAP_SYS_TIME after dropping root sets the clock, and not before", in_child(keepcaps_clock));
        EXPECT("the permitted set kept can only shrink", in_child(keepcaps_no_more));
        EXPECT("setuid without PR_SET_KEEPCAPS loses every capability", in_child(plain_drop));
        EXPECT("kept capabilities end at execve", in_child(keepcaps_exec));
        EXPECT("a root user id coming back ends the kept sets", in_child(keepcaps_root_again));
        EXPECT("/proc/PID/status shows the capability sets", status_caps());
        EXPECT("and the sets a process kept", in_child(status_caps_kept));
        EXPECT("RNDADDENTROPY and RNDRESEEDCRNG on /dev/urandom take root's request", rnd_ioctls());
        EXPECT("and refuse anyone else", in_child(rnd_ioctl_user));
        EXPECT("reboot(RESTART) in a PID namespace ends its init with SIGHUP", in_child(ns_reboot_restart));
        EXPECT("reboot(POWER_OFF) ends it with SIGINT", in_child(ns_reboot_poweroff));
        EXPECT("reboot(HALT) ends it with SIGINT", in_child(ns_reboot_halt));
        EXPECT("reboot(CAD_OFF) in a PID namespace is EINVAL", in_child(ns_reboot_cad));
        EXPECT("reboot is root's", in_child(reboot_not_root));
        EXPECT("the sysctls of Alpine's sysctl.d keep what is written", net_sysctls());
        EXPECT("ping_group_range takes two numbers", ping_range());
    }
    EXPECT("/proc/filesystems lists the special file systems devfs looks for", filesystem_types());
    return done();
}
