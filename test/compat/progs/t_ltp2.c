/* Small system call corners LTP's tests found: file status flags, the
 * supplementary groups, fsync on a pipe, mlockall, personality, mincore,
 * and socket errors (listen on UDP, send before connect, a bad address
 * length, MSG_OOB on UDP), clone with a stack, unshare, sched_getattr,
 * a socket's O_NONBLOCK, and errno corners of dup3, getrusage, getrandom,
 * timer_settime, the socket calls, mremap and munlock, and nested epoll. */
#include "compat.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <sys/epoll.h>
#include <time.h>

static volatile int ticks;
static void vt(int s) { (void)s; ticks++; }
static sigjmp_buf back;
static void segv(int s) { (void)s; siglongjmp(back, 1); }

/* Three connections in turn to a server that accepts, greets and closes
 * each: the closed ones must not be handed out by accept again. */
static int seven(void *a) { (void)a; return 7; }
static char kid_stack[16384] __attribute__((aligned(16)));

static int serve_three(void) {
    struct sockaddr_in a = {.sin_family = AF_INET}; socklen_t l = sizeof a;
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(sfd, (void *)&a, sizeof a) || listen(sfd, 4) || getsockname(sfd, (void *)&a, &l)) return 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pid_t c = fork();
    if (c == 0) {
        for (;;) { int u = accept(sfd, 0, 0); if (u < 0) _exit(1); write(u, "hi", 2); char b[4]; read(u, b, 4); close(u); }
    }
    close(sfd);
    int good = 0;
    for (int i = 0; i < 3; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0); char b[2];
        fd_set r; FD_ZERO(&r); FD_SET(s, &r); struct timeval t = {2, 0};
        if (connect(s, (void *)&a, sizeof a) == 0 && select(s + 1, &r, 0, 0, &t) == 1 && read(s, b, 2) == 2) good++;
        close(s);
    }
    kill(c, SIGKILL); waitpid(c, 0, 0);
    return good == 3;
}

int main(void) {
    setup();
    signal(SIGPIPE, SIG_IGN);

    EXPECT("F_GETFL reports the access mode of a file", ({ int fd = open("cf_ltp2", O_WRONLY | O_CREAT | O_TRUNC, 0644);
           int f = fcntl(fd, F_GETFL); close(fd); unlink("cf_ltp2"); (f & O_ACCMODE) == O_WRONLY; }));
    EXPECT("F_SETFL O_APPEND appends to a file", ({ int fd = open("cf_ltp2", O_RDWR | O_CREAT | O_TRUNC, 0644);
           write(fd, "ab", 2); lseek(fd, 0, SEEK_SET); int s = fcntl(fd, F_SETFL, O_APPEND | O_NONBLOCK);
           write(fd, "c", 1); int f = fcntl(fd, F_GETFL); char b[4] = {0}; pread(fd, b, 3, 0); close(fd); unlink("cf_ltp2");
           s == 0 && (f & (O_ACCMODE | O_APPEND | O_NONBLOCK)) == (O_RDWR | O_APPEND | O_NONBLOCK) && memcmp(b, "abc", 3) == 0; }));
    EXPECT("getgroups counts and lists the groups", ({ gid_t g[64]; int n = getgroups(0, 0);
           n >= 0 && n <= 64 && getgroups(64, g) == n && (n < 1 || getgroups(n - 1, g) == -1 || n == 1); }));
    EXPECT("getgroups with a negative size is EINVAL", getgroups(-1, 0) == -1 && errno == EINVAL);
    EXPECT("fsync of a pipe is EINVAL", ({ int p[2]; pipe(p); int r = fsync(p[0]); int e = errno; close(p[0]); close(p[1]);
           r == -1 && e == EINVAL; }));
    EXPECT("mlockall and munlockall succeed", mlockall(MCL_CURRENT | MCL_FUTURE) == 0 && munlockall() == 0);
    EXPECT("personality is kept", ({ int o = personality(0xffffffff); personality(PER_LINUX32);
           int n = personality(0xffffffff); personality(o); n == PER_LINUX32 && personality(0xffffffff) == o; }));
    EXPECT("mincore marks mapped pages", ({ char *a = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           a[0] = a[8192] = 1; unsigned char v[3] = {0}; int r = mincore(a, 3 * 4096, v); munmap(a, 3 * 4096); r == 0 && (v[0] & 1) && (v[2] & 1); }));
    EXPECT("mincore errors", ({ char *a = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); unsigned char v;
           int e1 = mincore(a + 1, 1, &v) == -1 && errno == EINVAL;
           int e2 = mincore(a, 4096, (void *)8) == -1 && errno == EFAULT;
           munmap(a, 4096);
           int e3 = mincore(a, 4096, &v) == -1 && errno == ENOMEM; e1 && e2 && e3; }));
    EXPECT("listen on a UDP socket is EOPNOTSUPP", ({ int s = socket(AF_INET, SOCK_DGRAM, 0); int r = listen(s, 1); int e = errno;
           close(s); r == -1 && e == EOPNOTSUPP; }));
    EXPECT("send on an unconnected TCP socket is EPIPE", ({ int s = socket(AF_INET, SOCK_STREAM, 0); int r = send(s, "x", 1, MSG_NOSIGNAL);
           int e = errno; close(s); r == -1 && e == EPIPE; }));
    EXPECT("sendto with a huge address length is EINVAL", ({ int s = socket(AF_INET, SOCK_DGRAM, 0);
           struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
           int r = sendto(s, "x", 1, 0, (struct sockaddr *)&to, (socklen_t)-1); int e = errno; close(s); r == -1 && e == EINVAL; }));
    EXPECT("MSG_OOB on UDP is EOPNOTSUPP", ({ int s = socket(AF_INET, SOCK_DGRAM, 0);
           struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
           int r = sendto(s, "x", 1, MSG_OOB, (struct sockaddr *)&to, sizeof to); int e = errno; close(s); r == -1 && e == EOPNOTSUPP; }));
    EXPECT("accept never hands out a closed connection", serve_three());
    EXPECT("a page fault runs the SIGSEGV handler", ({ signal(SIGSEGV, segv); char *p = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int caught = 0; if (sigsetjmp(back, 1) == 0) p[0] = 1; else caught = 1; signal(SIGSEGV, SIG_DFL); munmap(p, 4096); caught; }));
    EXPECT("a file reads 8 KiB in one read", ({ int fd = open("cf_ltp2", O_RDWR | O_CREAT | O_TRUNC, 0644); static char b[8192];
           memset(b, 'x', sizeof b); write(fd, b, sizeof b); lseek(fd, 0, SEEK_SET); int r = read(fd, b, sizeof b); close(fd); unlink("cf_ltp2");
           r == 8192; }));
    EXPECT("a FIFO opened O_RDWR reads what it writes", ({ unlink("cf_fifo"); int ok = mkfifo("cf_fifo", 0600) == 0;
           int fd = open("cf_fifo", O_RDWR | O_NONBLOCK); char b[3] = {0};
           ok = ok && fd >= 0 && write(fd, "ab", 2) == 2 && read(fd, b, 2) == 2 && b[0] == 'a' && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR;
           close(fd); unlink("cf_fifo"); ok; }));
    EXPECT("ITIMER_VIRTUAL sends SIGVTALRM", ({ signal(SIGVTALRM, vt); struct itimerval t = {{0, 10000}, {0, 10000}}, o;
           ticks = 0; setitimer(ITIMER_VIRTUAL, &t, 0); while (ticks < 3) ;
           struct itimerval z = {{0, 0}, {0, 0}}; setitimer(ITIMER_VIRTUAL, &z, &o); signal(SIGVTALRM, SIG_DFL);
           o.it_interval.tv_usec == 10000; }));
    EXPECT("SIOCGIFCONF lists lo", ({ int s = socket(AF_INET, SOCK_DGRAM, 0); struct ifreq r[8]; struct ifconf c = {.ifc_len = sizeof r, .ifc_req = r};
           int ok = ioctl(s, SIOCGIFCONF, &c) == 0 && c.ifc_len >= (int)sizeof r[0];
           int lo = 0; for (int i = 0; ok && i < c.ifc_len / (int)sizeof r[0]; i++)
               if (strcmp(r[i].ifr_name, "lo") == 0 && ioctl(s, SIOCGIFFLAGS, &r[i]) == 0 && (r[i].ifr_flags & IFF_LOOPBACK)) lo = 1;
           close(s); ok && lo; }));
    EXPECT("pread/pwrite: negative offset EINVAL, a directory EISDIR, 0 bytes 0", ({ int fd = open("cf_pio", O_RDWR | O_CREAT | O_TRUNC, 0644);
           char b[4]; int d = open(".", O_RDONLY | O_DIRECTORY);
           int ok = pread(fd, b, 4, -1) == -1 && errno == EINVAL && pwrite(fd, NULL, 0, 0) == 0 &&
               pread(d, b, 4, 0) == -1 && errno == EISDIR;
           close(d); close(fd); unlink("cf_pio"); ok; }));
    EXPECT("waitpid(0) waits only for the caller's group", ({ int ok = 1; pid_t c = fork();
           if (c == 0) { pid_t a = fork(); if (a == 0) _exit(1); setpgid(0, 0); pid_t b = fork(); if (b == 0) _exit(2);
               int st; pid_t g = waitpid(0, &st, 0); pid_t h = waitpid(0, &st, WNOHANG);
               pid_t r = waitpid(a, &st, 0); _exit(g == b && h == -1 && errno == ECHILD && r == a ? 0 : 3); }
           int st = 0; waitpid(c, &st, 0); ok && WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("waitpid with bad options is EINVAL", waitpid(-1, NULL, 0x7fff) == -1 && errno == EINVAL);
    EXPECT("rlimits: bad resource EINVAL; soft over hard EINVAL", ({ struct rlimit r, w = { 2, 1 };
           getrlimit(99, &r) == -1 && errno == EINVAL && setrlimit(RLIMIT_CORE, &w) == -1 && errno == EINVAL; }));
    EXPECT("nice: root lowers and raises it, a child inherits it, nobody may not lower it", ({ pid_t c = fork();
           if (c == 0) { errno = 0; int a = nice(-5); int g = getpriority(PRIO_PROCESS, 0); int b = nice(10);
               pid_t d = fork(); if (d == 0) _exit(getpriority(PRIO_PROCESS, 0) == 5 ? 0 : 1);
               int st; waitpid(d, &st, 0); setuid(65534); int e = nice(-1);
               _exit(a == -5 && g == -5 && b == 5 && WEXITSTATUS(st) == 0 && e == -1 && errno == EPERM ? 0 : 2); }
           int st = 0; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
           getpriority(PRIO_PROCESS, 0) == 0 && getpriority(9, 0) == -1 && errno == EINVAL &&
           getpriority(PRIO_PROCESS, 0x7ffffff0) == -1 && errno == ESRCH; }));
    EXPECT("sched_setscheduler: SCHED_FIFO 10, read back, bad priority EINVAL", ({ pid_t c = fork();
           if (c == 0) { struct sched_param p = {10}, q = {0}, bad = {100};
               int ok = sched_get_priority_max(SCHED_FIFO) == 99 && sched_get_priority_min(SCHED_RR) == 1 &&
                   syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &p) == 0 && syscall(SYS_sched_getscheduler, 0) == SCHED_FIFO &&
                   syscall(SYS_sched_getparam, 0, &q) == 0 && q.sched_priority == 10 &&
                   syscall(SYS_sched_setparam, 0, &bad) == -1 && errno == EINVAL &&
                   syscall(SYS_sched_setscheduler, 0, 7, &p) == -1 && errno == EINVAL;
               _exit(ok ? 0 : 1); }
           int st = 0; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("sched_setaffinity to processor 0", ({ cpu_set_t s; CPU_ZERO(&s); CPU_SET(0, &s); sched_setaffinity(0, sizeof s, &s) == 0; }));
    EXPECT("an empty path is ENOENT, an unmapped one EFAULT", ({ struct stat st; void *bad = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           stat("", &st) == -1 && errno == ENOENT && unlink("") == -1 && errno == ENOENT &&
           stat(bad, &st) == -1 && errno == EFAULT &&
           ({ int d = open(".", O_RDONLY); int r = fstatat(d, "", &st, AT_EMPTY_PATH); close(d); r == 0 && S_ISDIR(st.st_mode); }); }));
    EXPECT("tkill and tgkill reject a zero or negative id", syscall(SYS_tkill, 0, 0) == -1 && errno == EINVAL &&
           syscall(SYS_tgkill, -1, getpid(), 0) == -1 && errno == EINVAL && syscall(SYS_tgkill, getpid(), getpid(), 0) == 0);
    EXPECT("/proc/cmdline", ({ int fd = open("/proc/cmdline", O_RDONLY); char b[1024]; int n = fd >= 0 ? read(fd, b, sizeof b) : -1;
           if (fd >= 0) close(fd); n > 0 && b[n - 1] == '\n'; }));
    EXPECT("clone with a child stack makes a process", ({ pid_t t = 0;
           pid_t c = clone(seven, kid_stack + sizeof kid_stack, SIGCHLD | CLONE_PARENT_SETTID, 0, &t);
           int st = 0; c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 7 && t == c; }));
    EXPECT("unshare: files and cwd succeed, bad flags EINVAL", unshare(CLONE_FILES | CLONE_FS) == 0 &&
           unshare(-1) == -1 && errno == EINVAL);
    EXPECT("sched_getattr reads the policy; a short size is EINVAL", ({ unsigned a[14] = {0};
           syscall(SYS_sched_getattr, 0, a, 56, 0) == 0 && a[0] == 56 && a[1] == SCHED_OTHER &&
           syscall(SYS_sched_getattr, 0, a, 47, 0) == -1 && errno == EINVAL; }));
    EXPECT("F_GETFL and F_SETFL see a socket's O_NONBLOCK", ({ int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
           int a = fcntl(s, F_GETFL); fcntl(s, F_SETFL, 0); int b = fcntl(s, F_GETFL); close(s);
           (a & O_NONBLOCK) && !(b & O_NONBLOCK) && (a & O_ACCMODE) == O_RDWR; }));
    EXPECT("errors: dup3 flags, getrusage who, getrandom flags, a null timer setting", ({ struct rusage ru; char b[4]; timer_t t;
           int ok = dup3(0, 9, 1) == -1 && errno == EINVAL && getrusage(-2, &ru) == -1 && errno == EINVAL &&
               getrandom(b, 4, -1) == -1 && errno == EINVAL && timer_create(CLOCK_MONOTONIC, 0, &t) == 0;
           ok = ok && syscall(SYS_timer_settime, t, 0, 0, 0) == -1 && errno == EINVAL; timer_delete(t); ok; }));
    EXPECT("errors: socket types, socketpair of inet, option levels, a negative name length", ({ int s = socket(AF_INET, SOCK_STREAM, 0);
           int v = 1, sv[2]; socklen_t l = -1; struct sockaddr_in a;
           int ok = socket(AF_INET, 75, 0) == -1 && errno == EINVAL &&
               socketpair(AF_INET, SOCK_STREAM, 0, sv) == -1 && errno == EOPNOTSUPP &&
               setsockopt(s, 500, 1, &v, sizeof v) == -1 && errno == ENOPROTOOPT &&
               setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, 0) == -1 && errno == EINVAL &&
               getsockopt(s, 500, 1, &v, &(socklen_t){4}) == -1 && errno == EOPNOTSUPP &&
               getsockname(s, (struct sockaddr *)&a, &l) == -1 && errno == EINVAL; close(s); ok; }));
    EXPECT("errors: mremap MREMAP_FIXED alone, munlock of unmapped pages", ({ char *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int ok = mremap(p, 4096, 4096, MREMAP_FIXED, p + 4096) == MAP_FAILED && errno == EINVAL;
           munmap(p, 8192); ok && munlock(p, 4096) == -1 && errno == ENOMEM; }));
    EXPECT("nested epoll: readiness passes up, a loop is ELOOP", ({ int p[2]; pipe(p); int a = epoll_create1(0), b = epoll_create1(0);
           struct epoll_event ev = {.events = EPOLLIN}, out; ev.data.fd = p[0]; epoll_ctl(a, EPOLL_CTL_ADD, p[0], &ev);
           ev.data.fd = a; int ok = epoll_ctl(b, EPOLL_CTL_ADD, a, &ev) == 0 && epoll_wait(b, &out, 1, 0) == 0;
           write(p[1], "x", 1); ok = ok && epoll_wait(b, &out, 1, 0) == 1 && out.data.fd == a && (out.events & EPOLLIN);
           ev.data.fd = b; ok = ok && epoll_ctl(a, EPOLL_CTL_ADD, b, &ev) == -1 && errno == ELOOP;
           close(a); close(b); close(p[0]); close(p[1]); ok; }));
    return done();
}
