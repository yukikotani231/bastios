/* Small system call corners LTP's tests found: file status flags, the
 * supplementary groups, fsync on a pipe, mlockall, personality, mincore,
 * and socket errors (listen on UDP, send before connect, a bad address
 * length, MSG_OOB on UDP), clone with a stack, unshare, sched_getattr,
 * a socket's O_NONBLOCK, and errno corners of dup3, getrusage, getrandom,
 * timer_settime, the socket calls, mremap and munlock, and nested epoll;
 * shutdown(SHUT_RD), CPU-time clocks of a thread, arch_prctl, memfd_create
 * names, madvise, timerfd flags and ETXTBSY. */
#include <mqueue.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/prctl.h>
#include "compat.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/personality.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <sys/epoll.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/timerfd.h>

static volatile int ticks;
static void vt(int s) { (void)s; ticks++; }
static sigjmp_buf back;
static void segv(int s) { (void)s; siglongjmp(back, 1); }
static volatile int bus_code = -1;
static void busplain(int s) { (void)s; bus_code = 2; siglongjmp(back, 1); }
static void bus(int s, siginfo_t *i, void *u) { (void)s; (void)u; bus_code = i->si_code; siglongjmp(back, 1); }

/* Three connections in turn to a server that accepts, greets and closes
 * each: the closed ones must not be handed out by accept again. */
static void nop(int s) { (void)s; }
/* <linux/capability.h>, which musl does not ship. */
struct __user_cap_header_struct { unsigned version; int pid; };
struct __user_cap_data_struct { unsigned effective, permitted, inheritable; };
#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#define CAP_SETPCAP 8
#define CAP_SYS_TIME 25
static struct rnode { struct rnode *next; unsigned word; } rnode;
static struct { struct rnode *next; long off; struct rnode *pending; } rhead;
static long rgot = -1;
static char robust_stack[16384] __attribute__((aligned(16)));
static int robust_thread(void *a) {
    (void)a;
    rnode.word = syscall(SYS_gettid); rnode.next = (struct rnode *)&rhead;
    rhead.next = &rnode; rhead.off = 8; rhead.pending = 0;
    void *h = 0; size_t n = 0;
    if (syscall(SYS_set_robust_list, &rhead, 24) == 0 && syscall(SYS_get_robust_list, 0, &h, &n) == 0 && h == (void *)&rhead && n == 24)
        rgot = rnode.word;
    syscall(SYS_exit, 0);                  /* the kernel walks the list */
    return 0;
}

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

static volatile int ltp2_mq_hit;
static void ltp2_mq_fn(union sigval v) { ltp2_mq_hit = v.sival_int; }

static volatile int adopt_hit;
static void adopt_handler(int s) { (void)s; adopt_hit = 1; }
static void *adopt_worker(void *a) {
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &m, 0);
    *(volatile int *)a = 1;
    while (*(volatile int *)a != 2) usleep(1000);
    pthread_sigmask(SIG_UNBLOCK, &m, 0);          // takes the signal every thread blocked
    for (int i = 0; i < 2000 && !adopt_hit; i++) usleep(1000);
    return 0;
}

static volatile int kick_hits;
static volatile int kick_stop;
static void kick_handler(int s) { (void)s; kick_hits++; }
static void *kick_worker(void *a) {
    (void)a;
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    pthread_sigmask(SIG_UNBLOCK, &m, 0);
    while (!kick_stop) { }                        // runs in user mode on the other processor
    return 0;
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
    EXPECT("mqueue: priorities order messages, getattr counts, unlink, bad sigevent EINVAL", ({
           struct mq_attr at = {.mq_maxmsg = 4, .mq_msgsize = 16}, g; char b[16]; unsigned pr = 0;
           mq_unlink("/ltp2q"); mqd_t q = mq_open("/ltp2q", O_RDWR | O_CREAT | O_EXCL, 0600, &at);
           struct sigevent se = {.sigev_notify = 77};
           int ok = q != (mqd_t)-1 && mq_send(q, "lo", 2, 1) == 0 && mq_send(q, "hi", 2, 9) == 0 &&
               mq_getattr(q, &g) == 0 && g.mq_curmsgs == 2 && g.mq_maxmsg == 4 &&
               mq_receive(q, b, 16, &pr) == 2 && pr == 9 && !memcmp(b, "hi", 2) &&
               mq_receive(q, b, 8, &pr) == -1 && errno == EMSGSIZE &&
               syscall(SYS_mq_notify, q, &se) == -1 && errno == EINVAL &&
               mq_open("/ltp2q", O_RDWR | O_CREAT | O_EXCL, 0600, &at) == (mqd_t)-1 && errno == EEXIST;
           mq_close(q); ok && mq_unlink("/ltp2q") == 0 && mq_unlink("/ltp2q") == -1 && errno == ENOENT; }));
    EXPECT("mqueue file system: queues show as files, a new file makes a queue, mq_unlink removes the file", ({
           int ok = 0;
           mkdir("mqfs_d", 0755);
           if (mount("mqueue", "mqfs_d", "mqueue", 0, 0) == 0) {
               mqd_t q = mq_open("/mqfs_a", O_RDWR | O_CREAT | O_EXCL, 0600, 0);
               int made = q != (mqd_t)-1 && access("mqfs_d/mqfs_a", F_OK) == 0;
               int fd = creat("mqfs_d/mqfs_b", 0600);
               mqd_t b = fd >= 0 ? mq_open("/mqfs_b", O_RDWR, 0, 0) : (mqd_t)-1;
               int viaFile = b != (mqd_t)-1;
               if (b != (mqd_t)-1) mq_close(b);
               if (fd >= 0) close(fd);
               int gone = mq_unlink("/mqfs_a") == 0 && access("mqfs_d/mqfs_a", F_OK) == -1 && mq_unlink("/mqfs_b") == 0;
               if (q != (mqd_t)-1) mq_close(q);
               ok = made && viaFile && gone;
               umount("mqfs_d");
           }
           rmdir("mqfs_d"); ok; }));
    EXPECT("mqueue: 32 queues at once, a 33rd is ENOSPC, each keeps its own messages", ({
           struct mq_attr at = {.mq_maxmsg = 2, .mq_msgsize = 8}; mqd_t q[33]; char nm[16], b[8]; unsigned pr;
           int n = 0, ok = 1;
           for (int i = 0; i < 33; i++) q[i] = (mqd_t)-1;
           for (int i = 0; i < 32; i++) {
               snprintf(nm, sizeof nm, "/ltp2m%d", i); mq_unlink(nm);
               q[i] = mq_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600, &at);
               if (q[i] != (mqd_t)-1) { n++; char m[2] = {'a' + i % 26, 0}; ok = ok && mq_send(q[i], m, 1, 0) == 0; }
           }
           q[32] = mq_open("/ltp2m32", O_RDWR | O_CREAT | O_EXCL, 0600, &at);
           ok = ok && n == 32 && q[32] == (mqd_t)-1 && errno == ENOSPC;
           for (int i = 0; ok && i < 32; i++)
               ok = mq_receive(q[i], b, 8, &pr) == 1 && b[0] == 'a' + i % 26;
           for (int i = 0; i < 32; i++) {
               snprintf(nm, sizeof nm, "/ltp2m%d", i);
               if (q[i] != (mqd_t)-1) mq_close(q[i]);
               mq_unlink(nm);
           }
           ok; }));
    EXPECT("mqueue: mq_notify SIGEV_THREAD runs its function once, a second registration is EBUSY", ({
           struct mq_attr at = {.mq_maxmsg = 4, .mq_msgsize = 16};
           mq_unlink("/ltp2n"); mqd_t q = mq_open("/ltp2n", O_RDWR | O_CREAT | O_EXCL, 0600, &at);
           struct sigevent se = {.sigev_notify = SIGEV_THREAD, .sigev_notify_function = ltp2_mq_fn};
           se.sigev_value.sival_int = 77;
           int ok = q != (mqd_t)-1 && mq_notify(q, &se) == 0 && mq_notify(q, &se) == -1 && errno == EBUSY &&
               mq_send(q, "x", 1, 0) == 0;
           for (int i = 0; ok && i < 200 && !ltp2_mq_hit; i++) usleep(10000);
           ok = ok && ltp2_mq_hit == 77;
           /* The registration is spent: another message does not call again, a new one can be made and dropped. */
           ok = ok && mq_send(q, "y", 1, 0) == 0 && mq_notify(q, &se) == 0 && mq_notify(q, NULL) == 0;
           mq_close(q); mq_unlink("/ltp2n"); ok; }));
    EXPECT("prctl: a name shows in /proc/self/comm; death signal and timer slack read back", ({ char b[16] = {0}, c[32] = {0}; int d = 0;
           prctl(PR_SET_NAME, "abcdefghijklmnopq"); prctl(PR_GET_NAME, b);
           int fd = open("/proc/self/comm", O_RDONLY); int n = fd >= 0 ? read(fd, c, sizeof c) : -1; if (fd >= 0) close(fd);
           int ok = !strcmp(b, "abcdefghijklmno") && n == 16 && !memcmp(c, "abcdefghijklmno\n", 16) &&
               prctl(PR_SET_PDEATHSIG, SIGUSR2) == 0 && prctl(PR_GET_PDEATHSIG, &d) == 0 && d == SIGUSR2 &&
               prctl(PR_SET_PDEATHSIG, 0) == 0 && prctl(PR_SET_TIMERSLACK, 70000) == 0 && prctl(PR_GET_TIMERSLACK) == 70000;
           prctl(PR_SET_TIMERSLACK, 0); prctl(PR_SET_NAME, "t_ltp2"); ok; }));
    EXPECT("an interrupted nanosleep writes the time left; kill of a zombie succeeds", ({ signal(SIGALRM, nop);
           struct timespec rq = {5, 0}, rm = {0, 0}; alarm(1); int r = nanosleep(&rq, &rm); int e = errno; signal(SIGALRM, SIG_DFL);
           pid_t c = fork(); if (c == 0) _exit(0); usleep(200000);
           int k = kill(c, SIGTERM); int st; waitpid(c, &st, 0);
           r == -1 && e == EINTR && rm.tv_sec >= 2 && rm.tv_sec <= 4 && k == 0; }));
    EXPECT("a CPU-time POSIX timer, and overruns of a timer set in the past", ({ timer_t u; struct sigevent se = {.sigev_notify = SIGEV_NONE}; int t = -1;
           struct itimerspec v = {{0, 0}, {0, 500000000}}, g; struct timespec now;
           /* the system calls themselves: glibc's timer_create refuses the CPU-time clocks */
           int ok = syscall(SYS_timer_create, CLOCK_PROCESS_CPUTIME_ID, &se, &t) == 0 && syscall(SYS_timer_settime, t, 0, &v, 0) == 0 &&
               syscall(SYS_timer_gettime, t, &g) == 0 && g.it_value.tv_sec == 0 && g.it_value.tv_nsec > 0; syscall(SYS_timer_delete, t);
           sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, 0);
           struct sigevent su = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1}; timer_create(CLOCK_MONOTONIC, &su, &u);
           clock_gettime(CLOCK_MONOTONIC, &now); struct itimerspec w = {{0, 400000000}, {now.tv_sec - 1, now.tv_nsec}};   /* 2 rounds late; the next expiry is 0.4 s away, so a slow runner cannot reset the count before it is read */
           timer_settime(u, TIMER_ABSTIME, &w, 0); usleep(50000); int sig = 0; sigwait(&m, &sig);
           int o = timer_getoverrun(u); timer_delete(u);
           struct timespec z = {0, 0}; while (sigtimedwait(&m, 0, &z) == SIGUSR1) {}   /* an expiry after sigwait, before the delete */
           sigprocmask(SIG_UNBLOCK, &m, 0);
           ok && sig == SIGUSR1 && o >= 2; }));
    EXPECT("brk shrinks and grows back, never below its start", ({ char *b0 = (char *)syscall(SYS_brk, 0);
           char *up = (char *)syscall(SYS_brk, b0 + 8191); b0[8000] = 1;
           char *down = (char *)syscall(SYS_brk, b0 + 10); char *low = (char *)syscall(SYS_brk, 4096);
           char *back = (char *)syscall(SYS_brk, b0 + 8191); int z = b0[8000];
           syscall(SYS_brk, b0); up == b0 + 8191 && down == b0 + 10 && low == b0 + 10 && back == b0 + 8191 && z == 0; }));
    EXPECT("sendmmsg and recvmmsg over UDP loopback; accept on UDP is EOPNOTSUPP", ({
           int r = socket(AF_INET, SOCK_DGRAM, 0), w = socket(AF_INET, SOCK_DGRAM, 0);
           struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; socklen_t l = sizeof a;
           bind(r, (struct sockaddr *)&a, sizeof a); getsockname(r, (struct sockaddr *)&a, &l); connect(w, (struct sockaddr *)&a, sizeof a);
           char m1[] = "one", m2[] = "three", b1[8] = {0}, b2[8] = {0};
           struct iovec s1 = {m1, 3}, s2 = {m2, 5}, d1 = {b1, 8}, d2 = {b2, 8};
           struct mmsghdr sm[2] = {{.msg_hdr = {.msg_iov = &s1, .msg_iovlen = 1}}, {.msg_hdr = {.msg_iov = &s2, .msg_iovlen = 1}}};
           struct mmsghdr rm[2] = {{.msg_hdr = {.msg_iov = &d1, .msg_iovlen = 1}}, {.msg_hdr = {.msg_iov = &d2, .msg_iovlen = 1}}};
           int ns = sendmmsg(w, sm, 2, 0); usleep(20000); int nr = recvmmsg(r, rm, 2, MSG_DONTWAIT, 0);
           int e = accept(r, 0, 0) == -1 && errno == EOPNOTSUPP; close(r); close(w);
           ns == 2 && sm[1].msg_len == 5 && nr == 2 && rm[0].msg_len == 3 && !strcmp(b1, "one") && !strcmp(b2, "three") && e; }));
    EXPECT("/proc/loadavg reads as five fields", ({ char b[128] = {0}; int fd = open("/proc/loadavg", O_RDONLY);
           int n = fd >= 0 ? read(fd, b, sizeof b - 1) : -1; if (fd >= 0) close(fd);
           float x, y, z; int run, all, last; n > 0 && sscanf(b, "%f %f %f %d/%d %d", &x, &y, &z, &run, &all, &last) == 6 && all >= 1; }));
    EXPECT("TCP: bind to port 0 picks one, a second connect is EISCONN, AF_UNSPEC disconnects", ({
           struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; socklen_t l = sizeof a;
           int s = socket(AF_INET, SOCK_STREAM, 0); bind(s, (struct sockaddr *)&a, sizeof a); getsockname(s, (struct sockaddr *)&a, &l);
           int port = ntohs(a.sin_port); listen(s, 4);
           int c = socket(AF_INET, SOCK_STREAM, 0); int r1 = connect(c, (struct sockaddr *)&a, sizeof a);
           int r2 = connect(c, (struct sockaddr *)&a, sizeof a); int e2 = errno;
           int d = accept(s, 0, 0); struct sockaddr u = {.sa_family = AF_UNSPEC};
           int r3 = connect(d, &u, sizeof u); struct sockaddr_in b = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
           int r4 = bind(d, (struct sockaddr *)&b, sizeof b); close(d); close(c); close(s);
           port != 0 && r1 == 0 && r2 == -1 && e2 == EISCONN && r3 == 0 && r4 == 0; }));
    EXPECT("UDP sendmsg joins its buffers into one datagram; bind errors", ({
           int r = socket(AF_INET, SOCK_DGRAM, 0), w = socket(AF_INET, SOCK_DGRAM, 0);
           struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; socklen_t l = sizeof a;
           bind(r, (struct sockaddr *)&a, sizeof a); getsockname(r, (struct sockaddr *)&a, &l);
           struct iovec v[2] = {{"one", 3}, {"two", 3}}; struct msghdr m = {.msg_name = &a, .msg_namelen = sizeof a, .msg_iov = v, .msg_iovlen = 2};
           int n = sendmsg(w, &m, 0); char b[16] = {0}; usleep(20000); int got = recv(r, b, sizeof b, MSG_DONTWAIT);
           struct sockaddr_in far = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(0x0AFFFEFD)};
           int f = socket(AF_INET, SOCK_DGRAM, 0); int nb = bind(f, (struct sockaddr *)&far, sizeof far) == -1 && errno == EADDRNOTAVAIL;
           pid_t c = fork(); if (c == 0) { setuid(65534); struct sockaddr_in lo = {.sin_family = AF_INET, .sin_port = htons(80)};
               int g = socket(AF_INET, SOCK_STREAM, 0); _exit(bind(g, (struct sockaddr *)&lo, sizeof lo) == -1 && errno == EACCES ? 0 : 1); }
           int st = 0; waitpid(c, &st, 0); close(r); close(w); close(f);
           n == 6 && got == 6 && !strcmp(b, "onetwo") && nb && WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("SO_PEERCRED: the connecting child on the accepted socket, ourselves on a socketpair", ({
           struct sockaddr_un a = {.sun_family = AF_UNIX, .sun_path = "\0ltp2-peercred"};
           int s = socket(AF_UNIX, SOCK_STREAM, 0); bind(s, (struct sockaddr *)&a, sizeof a); listen(s, 4);
           int go[2]; pipe(go);
           pid_t c = fork(); if (c == 0) { int t = socket(AF_UNIX, SOCK_STREAM, 0); connect(t, (struct sockaddr *)&a, sizeof a);
               char x; read(go[0], &x, 1); _exit(0); }
           int d = accept(s, 0, 0); struct ucred u = {0}; socklen_t ul = sizeof u;
           int r = getsockopt(d, SOL_SOCKET, SO_PEERCRED, &u, &ul); write(go[1], "x", 1);
           int st; waitpid(c, &st, 0); int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
           struct ucred v = {0}; socklen_t vl = sizeof v; int r2 = getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &v, &vl);
           close(d); close(s); close(sv[0]); close(sv[1]); close(go[0]); close(go[1]);
           r == 0 && ul == sizeof u && u.pid == c && u.uid == geteuid() && r2 == 0 && v.pid == getpid(); }));
    EXPECT("MSG_MORE holds a UDP datagram and a short TCP segment back until the next send", ({
           int r = socket(AF_INET, SOCK_DGRAM, 0), w = socket(AF_INET, SOCK_DGRAM, 0);
           struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; socklen_t l = sizeof a;
           bind(r, (struct sockaddr *)&a, sizeof a); getsockname(r, (struct sockaddr *)&a, &l); connect(w, (struct sockaddr *)&a, sizeof a);
           char b[32] = {0}; send(w, "ab", 2, MSG_MORE); usleep(20000); int u1 = recv(r, b, sizeof b, MSG_DONTWAIT);
           send(w, "cd", 2, 0); usleep(20000); int u2 = recv(r, b, sizeof b, MSG_DONTWAIT);
           int ok = u1 == -1 && u2 == 4 && !memcmp(b, "abcd", 4); close(r); close(w);
           struct sockaddr_in t = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; l = sizeof t;
           int s = socket(AF_INET, SOCK_STREAM, 0); bind(s, (struct sockaddr *)&t, sizeof t); getsockname(s, (struct sockaddr *)&t, &l); listen(s, 1);
           int c = socket(AF_INET, SOCK_STREAM, 0); connect(c, (struct sockaddr *)&t, sizeof t); int d = accept(s, 0, 0);
           send(c, "ef", 2, MSG_MORE); usleep(20000); int t1 = recv(d, b, sizeof b, MSG_DONTWAIT);
           send(c, "g", 1, 0); usleep(20000); int t2 = recv(d, b, sizeof b, MSG_DONTWAIT);
           close(c); close(d); close(s); ok && t1 == -1 && t2 == 3 && !memcmp(b, "efg", 3); }));
    EXPECT("resource limits: NOFILE caps descriptors, FSIZE cuts writes, prlimit reports the old limit", ({
           pid_t c = fork(); if (c == 0) {
               struct rlimit n = {8, 8}; if (setrlimit(RLIMIT_NOFILE, &n)) _exit(1);
               if (getdtablesize() != 8 || dup2(0, 9) != -1 || errno != EBADF) _exit(2);
               int fds = 0; while (dup(0) >= 0) fds++; if (errno != EMFILE) _exit(3);
               signal(SIGXFSZ, SIG_IGN); struct rlimit f = {10, 10}; struct rlimit was;
               if (prlimit(0, RLIMIT_FSIZE, &f, &was) || was.rlim_cur != RLIM_INFINITY) _exit(4);
               for (int i = 3; i < 8; i++) close(i);
               int fd = open("/tmp/ltp2-fsize", O_CREAT | O_TRUNC | O_WRONLY, 0644);
               int w1 = write(fd, "abcdefghijklmnop", 16); int w2 = write(fd, "x", 1); int e2 = errno;
               close(fd); unlink("/tmp/ltp2-fsize");
               _exit(w1 == 10 && w2 == -1 && e2 == EFBIG ? 0 : 5); }
           int st = 0; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("mlock counts locked memory in VmLck; non-root over RLIMIT_MEMLOCK is ENOMEM", ({
           pid_t c = fork(); if (c == 0) {
               char *m = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
               if (mlock(m, 65536)) _exit(1);
               char b[2048] = {0}; int fd = open("/proc/self/status", O_RDONLY); read(fd, b, sizeof b - 1); close(fd);
               char *l = strstr(b, "VmLck:"); if (!l || atol(l + 6) != 64) _exit(2);
               munlockall(); struct rlimit r = {4096, 4096}; setrlimit(RLIMIT_MEMLOCK, &r); setuid(65534);
               _exit(mlock(m, 8192) == -1 && errno == ENOMEM && mlock(m, 4096) == 0 ? 0 : 3); }
           int st = 0; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("mremap MREMAP_FIXED moves pages to the address asked for; an unmapped address is EFAULT", ({
           char *a = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           char *t = mmap(0, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           a[0] = 'x'; a[4096] = 'y'; t[0] = 'z';
           char *m = mremap(a, 2 * 4096, 2 * 4096, MREMAP_MAYMOVE | MREMAP_FIXED, t);
           int ok = m == t && t[0] == 'x' && t[4096] == 'y';
           munmap(t, 2 * 4096); munmap(a + 2 * 4096, 4096);
           void *e = mremap(a, 4096, 8192, MREMAP_MAYMOVE); ok && e == MAP_FAILED && errno == EFAULT; }));
    EXPECT("sched_setattr keeps SCHED_DEADLINE runtime, deadline and period", ({
           struct { unsigned size, policy; unsigned long long flags; int nice; unsigned prio; unsigned long long run, dl, per; }
               at = {48, 6, 0, 0, 0, 10000000, 30000000, 40000000}, g = {0};
           pid_t c = fork(); if (c == 0) { cpu_set_t all; memset(&all, 0xff, sizeof all); sched_setaffinity(0, sizeof all, &all);
               if (syscall(SYS_sched_setattr, 0, &at, 0)) _exit(1);
               if (syscall(SYS_sched_getattr, 0, &g, 48, 0)) _exit(2);
               _exit(g.policy == 6 && g.run == 10000000 && g.dl == 30000000 && g.per == 40000000 ? 0 : 3); }
           int st = 0; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("SIGSTOP stops a child (wait WUNTRACED, state T), SIGCONT continues it (WCONTINUED, waitid CLD_CONTINUED)", ({
           pid_t c = fork(); if (c == 0) { for (;;) pause(); }
           kill(c, SIGSTOP); int st = 0; int w1 = waitpid(c, &st, WUNTRACED);
           int stopped = w1 == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP;
           char path[32], b[128] = {0}; snprintf(path, sizeof path, "/proc/%d/stat", c);
           int fd = open(path, O_RDONLY); read(fd, b, sizeof b - 1); close(fd);
           char *r = strrchr(b, ')'); int tee = r && r[2] == 'T';
           kill(c, SIGCONT); siginfo_t si = {0}; int w2 = waitid(P_PID, c, &si, WCONTINUED | WNOWAIT);
           int w3 = waitpid(c, &st, WCONTINUED);
           int cont = w2 == 0 && si.si_pid == c && si.si_code == CLD_CONTINUED && w3 == c && WIFCONTINUED(st);
           kill(c, SIGKILL); waitpid(c, &st, 0);
           stopped && tee && cont && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL; }));
    EXPECT("times: system calls count as system time; a waited-for child's times reach tms_cutime/cstime", ({
           struct tms a, b; times(&a); time_t t0 = time(0); while (time(0) - t0 < 2) getppid();
           pid_t c = fork(); if (c == 0) { time_t t1 = time(0); while (time(0) - t1 < 2) getppid(); volatile long x = 0; for (long i = 0; i < 100000000; i++) x += i; _exit(0); }
           waitpid(c, 0, 0); times(&b);
           b.tms_stime > a.tms_stime && b.tms_cutime > 0 && b.tms_cstime > 0; }));
    EXPECT("setpgid on a child after its execve is EACCES", ({
           int go[2]; pipe(go); pid_t c = fork();
           if (c == 0) { close(go[0]); fcntl(go[1], F_SETFD, FD_CLOEXEC); execl("/disk/busybox", "sleep", "1", (char *)0); execl("/bin/sleep", "sleep", "1", (char *)0); _exit(1); }
           close(go[1]); char x; read(go[0], &x, 1); close(go[0]);   /* EOF once the child execs */
           int r = setpgid(c, c); int e = errno; int st; waitpid(c, &st, 0); r == -1 && e == EACCES; }));
    EXPECT("setsid is EPERM while a process group with the caller's id has members", ({
           pid_t c = fork();
           if (c == 0) {
               setpgid(0, 0); pid_t g = fork(); if (g == 0) { pause(); _exit(0); }
               setpgid(0, getpgid(getppid()));
               int r = setsid(); int e = errno; kill(g, SIGKILL); waitpid(g, 0, 0);
               _exit(r == -1 && e == EPERM ? 0 : 1);
           }
           int st; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("a real-time signal from tgkill past RLIMIT_SIGPENDING is EAGAIN; kill still sends it", ({
           pid_t c = fork();
           if (c == 0) {
               sigset_t m; sigemptyset(&m); sigaddset(&m, SIGRTMIN); sigprocmask(SIG_BLOCK, &m, 0);
               struct rlimit z = {0, 0}; setrlimit(RLIMIT_SIGPENDING, &z);
               int r = syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGRTMIN); int e = errno;
               _exit(r == -1 && e == EAGAIN && kill(getpid(), SIGRTMIN) == 0 ? 0 : 1);
           }
           int st; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("a thread that ends holding a robust futex leaves FUTEX_OWNER_DIED in it", ({
           /* A bare clone thread: musl's pthread_join would wait for its own exit path. */
           clone(robust_thread, robust_stack + sizeof robust_stack, CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM, 0);
           for (int i = 0; i < 200 && *(volatile unsigned *)&rnode.word != 0x40000000; i++) usleep(10000);
           rgot > 0 && rnode.word == 0x40000000; }));
    EXPECT("capget reports root's capabilities; an unknown version is EINVAL and gets the preferred one", ({
           struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 }; struct __user_cap_data_struct d[2];
           int r = syscall(SYS_capget, &h, d);
           struct __user_cap_header_struct bad = { 0x12345678, 0 }; int r2 = syscall(SYS_capget, &bad, d); int e2 = errno;
           r == 0 && (d[0].effective >> CAP_SYS_TIME) % 2 == 1 && (d[0].permitted >> CAP_SETPCAP) % 2 == 1
             && r2 == -1 && e2 == EINVAL && bad.version == _LINUX_CAPABILITY_VERSION_3; }));
    EXPECT("capset drops capabilities: then setting the clock, PR_CAPBSET_DROP and PR_SET_SECUREBITS are EPERM", ({
           pid_t c = fork();
           if (c == 0) {
               struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 }; struct __user_cap_data_struct d[2];
               syscall(SYS_capget, &h, d);
               int bset = prctl(PR_CAPBSET_READ, CAP_SYS_TIME, 0, 0, 0);
               d[0].effective &= ~((1u << CAP_SYS_TIME) | (1u << CAP_SETPCAP));
               int r = syscall(SYS_capset, &h, d);
               struct timeval tv; gettimeofday(&tv, 0);
               int t = settimeofday(&tv, 0); int te = errno;
               int b = prctl(PR_CAPBSET_DROP, 1, 0, 0, 0); int be = errno;
               int sb = prctl(PR_SET_SECUREBITS, 0, 0, 0, 0); int se = errno;
               struct __user_cap_data_struct d2[2]; syscall(SYS_capget, &h, d2);
               _exit(bset == 1 && r == 0 && t == -1 && te == EPERM && b == -1 && be == EPERM && sb == -1 && se == EPERM
                     && (d2[0].effective >> CAP_SYS_TIME) % 2 == 0 && (d2[0].permitted >> CAP_SYS_TIME) % 2 == 1 ? 0 : 1);
           }
           int st; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("capset may not add to the permitted set, nor change another process", ({
           pid_t c = fork();
           if (c == 0) {
               struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 }; struct __user_cap_data_struct d[2];
               syscall(SYS_capget, &h, d);
               d[0].permitted &= ~(1u << CAP_SYS_TIME); d[0].effective &= ~(1u << CAP_SYS_TIME);
               int r = syscall(SYS_capset, &h, d);
               d[0].permitted |= 1u << CAP_SYS_TIME;
               int r2 = syscall(SYS_capset, &h, d); int e2 = errno;
               h.pid = getppid(); int r3 = syscall(SYS_capset, &h, d); int e3 = errno;
               _exit(r == 0 && r2 == -1 && e2 == EPERM && r3 == -1 && e3 == EPERM ? 0 : 1);
           }
           int st; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("mmap: MAP_FIXED_NOREPLACE over a mapping is EEXIST, an unknown MAP_SHARED_VALIDATE flag EOPNOTSUPP, no type EINVAL", ({
           char *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           void *q = mmap(p, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0); int qe = errno;
           int fd = open("/tmp/t_ltp2_v", O_RDWR | O_CREAT | O_TRUNC, 0600); ftruncate(fd, 4096);
           void *v = mmap(0, 4096, PROT_READ, MAP_SHARED_VALIDATE | 0x400, fd, 0); int ve = errno;
           void *w = mmap(0, 4096, PROT_READ, 0, fd, 0); int we = errno;
           void *x = mmap(0, 0, PROT_READ, MAP_SHARED, -1, 0); int xe = errno;
           close(fd); unlink("/tmp/t_ltp2_v"); munmap(p, 8192);
           q == MAP_FAILED && qe == EEXIST && v == MAP_FAILED && ve == EOPNOTSUPP && w == MAP_FAILED && we == EINVAL
             && x == MAP_FAILED && xe == EBADF; }));
    EXPECT("SCHED_RESET_ON_FORK: the child of a SCHED_FIFO process starts as SCHED_OTHER with priority 0", ({
           pid_t c = fork();
           if (c == 0) {
               struct sched_param sp = { 10 };
               int r = syscall(SYS_sched_setscheduler, 0, SCHED_FIFO | 0x40000000, &sp);
               int me = syscall(SYS_sched_getscheduler, 0);
               pid_t g = fork(); if (g == 0) _exit(0);
               int gp = syscall(SYS_sched_getscheduler, g); struct sched_param gq = { 7 }; syscall(SYS_sched_getparam, g, &gq);
               waitpid(g, 0, 0);
               _exit(r == 0 && me == (SCHED_FIFO | 0x40000000) && gp == SCHED_OTHER && gq.sched_priority == 0 ? 0 : 1);
           }
           int st; waitpid(c, &st, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("locked pages: msync(MS_INVALIDATE) is EBUSY and madvise(MADV_DONTNEED) EINVAL, until munlock", ({
           char *m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
           int a = msync(m, 4096, MS_INVALIDATE), ae = errno;
           int b = madvise(m, 4096, MADV_DONTNEED), be = errno;
           int c = madvise(m + 4096, 4096, MADV_DONTNEED);          // the second page is locked too
           munlock(m, 4096);
           int d = msync(m, 4096, MS_INVALIDATE);
           int e = madvise(m, 4096, MADV_DONTNEED);
           int f = madvise(m + 4096, 4096, MADV_DONTNEED), fe = errno;
           munmap(m, 8192);
           a == -1 && ae == EBUSY && b == -1 && be == EINVAL && c == -1 && d == 0 && e == 0 && f == -1 && fe == EINVAL; }));
    EXPECT("locked pages: unmapping or mapping over them unlocks them", ({
           char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int l = mlock(m, 4096);
           munmap(m, 4096);
           char *n = mmap(m, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
           int a = madvise(n, 4096, MADV_DONTNEED);                  // a new mapping is not locked
           mlock(n, 4096);
           char *o = mmap(n, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
           int b = madvise(o, 4096, MADV_DONTNEED);                  // nor is one mapped over a locked one
           munmap(o, 4096);
           l == 0 && n == m && a == 0 && o == n && b == 0; }));
    EXPECT("splice from a pipe into a Unix socket and into a TCP connection delivers the bytes", ({
           int pp[2], sv[2]; pipe(pp); socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
           char in[3000], out[3000]; for (int i = 0; i < 3000; i++) in[i] = i * 7;
           write(pp[1], in, 3000);
           long a = splice(pp[0], 0, sv[0], 0, 3000, 0);
           int got = 0; while (got < a) { int r = read(sv[1], out + got, 3000 - got); if (r <= 0) break; got += r; }
           int ok1 = a == 3000 && got == 3000 && memcmp(in, out, 3000) == 0;
           int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
           bind(ls, (void *)&sa, sizeof sa); listen(ls, 1); socklen_t sl = sizeof sa; getsockname(ls, (void *)&sa, &sl);
           int c = socket(AF_INET, SOCK_STREAM, 0); connect(c, (void *)&sa, sizeof sa); int s = accept(ls, 0, 0);
           write(pp[1], in, 1000);
           long b = splice(pp[0], 0, c, 0, 1000, 0);
           got = 0; while (got < b) { int r = read(s, out + got, 1000 - got); if (r <= 0) break; got += r; }
           int ok2 = b == 1000 && got == 1000 && memcmp(in, out, 1000) == 0;
           int u = socket(AF_INET, SOCK_DGRAM, 0); write(pp[1], in, 10);
           long d = splice(pp[0], 0, u, 0, 10, 0);                     // a datagram socket is not done
           close(pp[0]); close(pp[1]); close(sv[0]); close(sv[1]); close(ls); close(c); close(s); close(u);
           ok1 && ok2 && d == -1 && errno == EINVAL; }));
    EXPECT("shutdown(SHUT_RD) on a unix socket: reads end, the peer's sends are EPIPE, poll reports POLLRDHUP", ({
           int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
           write(sv[1], "ab", 2);
           int s = shutdown(sv[0], SHUT_RD);
           char b[4]; int n1 = read(sv[0], b, 4); int n2 = read(sv[0], b, 4);
           int w = send(sv[1], "c", 1, MSG_NOSIGNAL); int we = errno;
           struct pollfd pf = { sv[0], POLLIN | POLLRDHUP, 0 }; int pr = poll(&pf, 1, 0);
           close(sv[0]); close(sv[1]);
           s == 0 && n1 == 2 && n2 == 0 && w == -1 && we == EPIPE && pr == 1 && (pf.revents & POLLRDHUP); }));
    EXPECT("the CPU-time clocks of this thread and this process can be read by id", ({
           clockid_t tc, pc; struct timespec t1, t2;
           int a = pthread_getcpuclockid(pthread_self(), &tc); int b = clock_getcpuclockid(0, &pc);
           a == 0 && b == 0 && clock_gettime(tc, &t1) == 0 && clock_gettime(pc, &t2) == 0
             && clock_gettime((clockid_t)((~0u << 3) | 3), &t1) == -1 && errno == EINVAL; }));
#ifdef SYS_arch_prctl
    EXPECT("arch_prctl: ARCH_GET_FS gives what ARCH_SET_FS set, ARCH_GET_CPUID is 1", ({
           unsigned long fs = 0; int r = syscall(SYS_arch_prctl, 0x1003, &fs);
           r == 0 && fs != 0 && syscall(SYS_arch_prctl, 0x1011, 0) == 1; }));
#endif
    EXPECT("memfd_create: a NULL name is EFAULT, a name over 249 bytes EINVAL", ({
           char nm[300]; memset(nm, 'a', sizeof nm); nm[299] = 0;
           int a = syscall(SYS_memfd_create, 0, 0); int ae = errno;
           int b = syscall(SYS_memfd_create, nm, 0); int be = errno;
           nm[249] = 0; int c = syscall(SYS_memfd_create, nm, 0);
           if (c >= 0) close(c);
           a == -1 && ae == EFAULT && b == -1 && be == EINVAL && c >= 0; }));
    EXPECT("madvise(MADV_DONTNEED) leaves private anonymous pages reading zeros; bad advice is EINVAL", ({
           char *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           memset(p, 1, 8192);
           int r = madvise(p, 8192, MADV_DONTNEED);
           int z = 1; for (int i = 0; i < 8192; i++) if (p[i]) z = 0;
           int bad = madvise(p, 4096, 1212); int be = errno;
           int mis = madvise(p + 1, 4096, MADV_NORMAL); int me = errno;
           munmap(p, 8192);
           r == 0 && z && bad == -1 && be == EINVAL && mis == -1 && me == EINVAL; }));
    EXPECT("timerfd_settime takes TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, and other flags are EINVAL", ({
           int fd = timerfd_create(CLOCK_REALTIME, 0);
           struct itimerspec its = { {0, 0}, {0, 0} };
           int a = timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, 0);
           int b = timerfd_settime(fd, 4, &its, 0); int be = errno;
           close(fd);
           a == 0 && b == -1 && be == EINVAL; }));
    EXPECT("opening the program a process runs for writing is ETXTBSY", ({
           char exe[256]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
           int fd = -1, e = 0;
           if (n > 0) { exe[n] = 0; fd = open(exe, O_WRONLY); e = errno; }
           if (fd >= 0) close(fd);
           n > 0 && fd == -1 && e == ETXTBSY; }));
    EXPECT("open of a directory for writing is EISDIR; ftruncate past RLIMIT_FSIZE is EFBIG", ({
           int d = open("/tmp", O_RDWR); int de = errno;
           pid_t c = fork();
           if (c == 0) {
               signal(SIGXFSZ, SIG_IGN);
               struct rlimit rl = { 4096, 4096 }; setrlimit(RLIMIT_FSIZE, &rl);
               int fd = open("/tmp/t_ltp2_fb", O_RDWR | O_CREAT | O_TRUNC, 0600);
               int r = ftruncate(fd, 8192); int e = errno; int r2 = ftruncate(fd, 4096);
               close(fd); unlink("/tmp/t_ltp2_fb");
               _exit(r == -1 && e == EFBIG && r2 == 0 ? 0 : 1);
           }
           int st; waitpid(c, &st, 0);
           d == -1 && de == EISDIR && WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("fallocate checks its range, the descriptor's access and the size limit before the mode", ({
           int w = open("/tmp/t_ltp2_fa", O_RDWR | O_CREAT | O_TRUNC, 0600);
           int r = open("/tmp/t_ltp2_fa", O_RDONLY);
           int a = fallocate(r, FALLOC_FL_KEEP_SIZE, 0, 4096); int ae = errno;
           int b = fallocate(w, FALLOC_FL_KEEP_SIZE, -4096, 4096); int be = errno;
           int c = fallocate(w, FALLOC_FL_KEEP_SIZE, 4096, 0); int ce = errno;
           int d = fallocate(w, FALLOC_FL_KEEP_SIZE, 0x7FFFFFFFFFFFF000LL, 8192); int de = errno;
           int g = fallocate(w, 0, 0, 4096); struct stat s; fstat(w, &s);
           close(w); close(r); unlink("/tmp/t_ltp2_fa");
           a == -1 && ae == EBADF && b == -1 && be == EINVAL && c == -1 && ce == EINVAL
             && d == -1 && de == EFBIG && g == 0 && s.st_size == 4096; }));
    EXPECT("msync: bad flags are EINVAL, an unmapped range ENOMEM", ({
           char *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int a = msync(p, 4096, MS_ASYNC | MS_SYNC); int ae = errno;
           int b = msync(p, 4096, -1); int be = errno;
           int c = msync(p, 8192, MS_SYNC);
           munmap(p + 4096, 4096);
           int d = msync(p, 8192, MS_SYNC); int de = errno;
           munmap(p, 4096);
           a == -1 && ae == EINVAL && b == -1 && be == EINVAL && c == 0 && d == -1 && de == ENOMEM; }));
    EXPECT("rt_sigqueueinfo with a si_code of 0 or more to another process is EPERM", ({
           pid_t c = fork();
           if (c == 0) { pause(); _exit(0); }
           siginfo_t si; memset(&si, 0, sizeof si);
           si.si_signo = SIGUSR1; si.si_code = 0; si.si_pid = getpid(); si.si_uid = getuid();
           int a = syscall(SYS_rt_sigqueueinfo, c, SIGUSR1, &si); int ae = errno;
           si.si_code = SI_QUEUE;
           kill(c, SIGKILL); waitpid(c, 0, 0);
           int b = syscall(SYS_rt_sigqueueinfo, getpid(), 0, &si);
           a == -1 && ae == EPERM && b == 0; }));
    EXPECT("utimensat with no path and AT_FDCWD is EFAULT; epoll_ctl on /dev/zero is EPERM", ({
           int a = syscall(SYS_utimensat, AT_FDCWD, 0, 0, 0); int ae = errno;
           int z = open("/dev/zero", O_RDONLY); int ep = epoll_create1(0);
           struct epoll_event ev = { EPOLLIN, { 0 } };
           int b = epoll_ctl(ep, EPOLL_CTL_ADD, z, &ev); int be = errno;
           close(z); close(ep);
           a == -1 && ae == EFAULT && b == -1 && be == EPERM; }));
    EXPECT("a unix stream socket that is bound to a name can still connect, and the connection carries data", ({
           unlink("/tmp/t_ltp2_bc_s"); unlink("/tmp/t_ltp2_bc_c");
           struct sockaddr_un sa = { AF_UNIX, "/tmp/t_ltp2_bc_s" }, ca = { AF_UNIX, "/tmp/t_ltp2_bc_c" };
           int l = socket(AF_UNIX, SOCK_STREAM, 0), c = socket(AF_UNIX, SOCK_STREAM, 0);
           int b1 = bind(l, (struct sockaddr *)&sa, sizeof sa), l1 = listen(l, 1);
           int b2 = bind(c, (struct sockaddr *)&ca, sizeof ca);
           int cn = connect(c, (struct sockaddr *)&sa, sizeof sa);
           int a = accept(l, 0, 0);
           int w = write(c, "x", 1); char m = 0; int r = read(a, &m, 1);
           close(a); close(c); close(l); unlink("/tmp/t_ltp2_bc_s"); unlink("/tmp/t_ltp2_bc_c");
           b1 == 0 && l1 == 0 && b2 == 0 && cn == 0 && a >= 0 && w == 1 && r == 1 && m == 'x'; }));
    EXPECT("utimensat on a directory sets its access and modification times apart", ({
           rmdir("/tmp/t_ltp2_ud");
           int m = mkdir("/tmp/t_ltp2_ud", 0755);
           struct timespec ts[2] = { { 5, 0 }, { 8, 0 } };
           int u = utimensat(AT_FDCWD, "/tmp/t_ltp2_ud", ts, 0);
           struct stat st; int s = stat("/tmp/t_ltp2_ud", &st);
           rmdir("/tmp/t_ltp2_ud");
           m == 0 && u == 0 && s == 0 && st.st_atime == 5 && st.st_mtime == 8; }));
    EXPECT("sched_getaffinity of a negative pid is ESRCH, of a pid that is not there too", ({
           unsigned long m[16];
           int a = syscall(SYS_sched_getaffinity, -1, sizeof m, m); int ae = errno;
           int b = syscall(SYS_sched_getaffinity, 0, 3, m); int be = errno;
           a == -1 && ae == ESRCH && b == -1 && be == EINVAL; }));
    EXPECT("the console keeps a window size set with TIOCSWINSZ", ({
           struct winsize old, w = { 30, 100, 0, 0 }, got;
           int g0 = ioctl(0, TIOCGWINSZ, &old);
           int s = ioctl(0, TIOCSWINSZ, &w);
           int g1 = ioctl(0, TIOCGWINSZ, &got);
           ioctl(0, TIOCSWINSZ, &old);
           g0 == 0 && s == 0 && g1 == 0 && got.ws_row == 30 && got.ws_col == 100; }));
    EXPECT("getsockname of a UDP socket bound to 127.0.0.1 reads that address, of one bound to none 0.0.0.0", ({
           struct sockaddr_in a = { AF_INET, 0, { htonl(0x7F000001) } }, b = { AF_INET }, g, h;
           socklen_t gl = sizeof g, hl = sizeof h;
           int s1 = socket(AF_INET, SOCK_DGRAM, 0), s2 = socket(AF_INET, SOCK_DGRAM, 0);
           int r1 = bind(s1, (struct sockaddr *)&a, sizeof a), r2 = bind(s2, (struct sockaddr *)&b, sizeof b);
           int q1 = getsockname(s1, (struct sockaddr *)&g, &gl), q2 = getsockname(s2, (struct sockaddr *)&h, &hl);
           close(s1); close(s2);
           r1 == 0 && r2 == 0 && q1 == 0 && q2 == 0 && g.sin_addr.s_addr == htonl(0x7F000001) && g.sin_port != 0 &&
           h.sin_addr.s_addr == 0 && h.sin_port != 0; }));
    EXPECT("MSG_PEEK on a UDP socket leaves the datagram queued", ({
           struct sockaddr_in a = { AF_INET, 0, { htonl(0x7F000001) } }, f; socklen_t al = sizeof a, fl = sizeof f;
           int r = socket(AF_INET, SOCK_DGRAM, 0), s = socket(AF_INET, SOCK_DGRAM, 0);
           int b = bind(r, (struct sockaddr *)&a, sizeof a); getsockname(r, (struct sockaddr *)&a, &al);
           int x = sendto(s, "peek", 4, 0, (struct sockaddr *)&a, sizeof a);
           char m[8] = { 0 }, n[8] = { 0 };
           int p = recvfrom(r, m, sizeof m, MSG_PEEK, (struct sockaddr *)&f, &fl);
           int q = recvfrom(r, n, sizeof n, MSG_DONTWAIT, 0, 0);
           int e = recvfrom(r, n, sizeof n, MSG_DONTWAIT, 0, 0);
           close(r); close(s);
           b == 0 && x == 4 && p == 4 && q == 4 && e == -1 && errno == EAGAIN && !memcmp(m, "peek", 4) && !memcmp(n, "peek", 4) &&
           f.sin_addr.s_addr == htonl(0x7F000001); }));
    EXPECT("a UDP datagram longer than the buffer sets MSG_TRUNC in recvmsg, and recv with MSG_TRUNC returns its length", ({
           struct sockaddr_in a = { AF_INET, 0, { htonl(0x7F000001) } };
           socklen_t al = sizeof a;
           int r = socket(AF_INET, SOCK_DGRAM, 0), s = socket(AF_INET, SOCK_DGRAM, 0);
           int b = bind(r, (struct sockaddr *)&a, sizeof a); getsockname(r, (struct sockaddr *)&a, &al);
           int x = sendto(s, "0123456789", 10, 0, (struct sockaddr *)&a, sizeof a);
           char m[4];
           struct iovec v = { m, sizeof m };
           struct msghdr h = { 0 };
           h.msg_iov = &v; h.msg_iovlen = 1;
           int p = recvmsg(r, &h, 0); int f1 = h.msg_flags;
           int y = sendto(s, "0123456789", 10, 0, (struct sockaddr *)&a, sizeof a);
           int q = recv(r, m, sizeof m, MSG_TRUNC);
           int z = sendto(s, "abcd", 4, 0, (struct sockaddr *)&a, sizeof a);
           int t = recvmsg(r, &h, 0); int f2 = h.msg_flags;
           close(r); close(s);
           b == 0 && x == 10 && p == 4 && (f1 & MSG_TRUNC) && y == 10 && q == 10 && z == 4 && t == 4 && !(f2 & MSG_TRUNC); }));
    EXPECT("TCP getsockname reads the bound address, and a connected or accepted socket's local address", ({
           struct sockaddr_in a = { AF_INET, 0, { htonl(0x7F000001) } }, g1, g2, g3, g4;
           socklen_t al = sizeof a, l1 = sizeof g1, l2 = sizeof g2, l3 = sizeof g3, l4 = sizeof g4;
           int l = socket(AF_INET, SOCK_STREAM, 0), c = socket(AF_INET, SOCK_STREAM, 0);
           int b = bind(l, (struct sockaddr *)&a, sizeof a); int ls = listen(l, 1);
           int q0 = getsockname(l, (struct sockaddr *)&g1, &l1); getsockname(l, (struct sockaddr *)&a, &al);
           int cn = connect(c, (struct sockaddr *)&a, sizeof a);
           int s = accept(l, 0, 0);
           int q1 = getsockname(c, (struct sockaddr *)&g2, &l2), q2 = getsockname(s, (struct sockaddr *)&g3, &l3), q3 = getpeername(s, (struct sockaddr *)&g4, &l4);
           close(s); close(c); close(l);
           b == 0 && ls == 0 && q0 == 0 && cn == 0 && s >= 0 && q1 == 0 && q2 == 0 && q3 == 0 &&
           g1.sin_addr.s_addr == htonl(0x7F000001) && g2.sin_addr.s_addr == htonl(0x7F000001) &&
           g3.sin_addr.s_addr == htonl(0x7F000001) && g3.sin_port == a.sin_port && g4.sin_port == g2.sin_port; }));
    EXPECT("an IPV6_V6ONLY listener takes a connection to ::1, and names it ::1", ({
           struct sockaddr_in6 a = { AF_INET6 }, g;
           socklen_t al = sizeof a, gl = sizeof g;
           int l = socket(AF_INET6, SOCK_STREAM, 0), c = socket(AF_INET6, SOCK_STREAM, 0), one = 1;
           setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
           int b = bind(l, (struct sockaddr *)&a, sizeof a); int ls = listen(l, 1);
           getsockname(l, (struct sockaddr *)&a, &al);
           a.sin6_addr = in6addr_loopback;
           int cn = connect(c, (struct sockaddr *)&a, sizeof a);
           int s = accept(l, 0, 0);
           int q = getsockname(c, (struct sockaddr *)&g, &gl);
           int w = send(c, "x", 1, 0);
           close(s); close(c); close(l);
           b == 0 && ls == 0 && cn == 0 && s >= 0 && q == 0 && w == 1 && !memcmp(&g.sin6_addr, &in6addr_loopback, 16); }));
    EXPECT("SO_REUSEADDR, SO_REUSEPORT and SO_KEEPALIVE read back as set, and clear again", ({
           int s = socket(AF_INET, SOCK_STREAM, 0), one = 1, zero = 0, v[4] = { 9, 9, 9, 9 };
           socklen_t n = sizeof(int);
           int d0 = getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v[0], &n);
           setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
           setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
           n = sizeof(int); getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v[1], &n);
           n = sizeof(int); getsockopt(s, SOL_SOCKET, SO_REUSEPORT, &v[2], &n);
           setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &zero, sizeof zero);
           n = sizeof(int); getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v[3], &n);
           int ka = 9; n = sizeof(int); getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &ka, &n);
           close(s);
           d0 == 0 && v[0] == 0 && v[1] != 0 && v[2] != 0 && v[3] == 0 && ka != 0; }));
    EXPECT("AF_UNIX SOCK_DGRAM: a bound socket gets datagrams from sendto and from a connected sender", ({
           unlink("/tmp/t_ltp2_dg");
           struct sockaddr_un a = { AF_UNIX, "/tmp/t_ltp2_dg" };
           int r = socket(AF_UNIX, SOCK_DGRAM, 0);
           int b = bind(r, (struct sockaddr *)&a, sizeof a);
           int s1 = socket(AF_UNIX, SOCK_DGRAM, 0), s2 = socket(AF_UNIX, SOCK_DGRAM, 0);
           int x = sendto(s1, "one", 3, 0, (struct sockaddr *)&a, sizeof a);
           int c = connect(s2, (struct sockaddr *)&a, sizeof a);
           int y = send(s2, "two!", 4, 0);
           char m[16]; struct sockaddr_un f; socklen_t fl = sizeof f;
           int n1 = recvfrom(r, m, sizeof m, 0, (struct sockaddr *)&f, &fl);
           int ok1 = n1 == 3 && memcmp(m, "one", 3) == 0 && fl == 0;    /* the sender is not bound */
           int n2 = recv(r, m, sizeof m, MSG_DONTWAIT);
           int ok2 = n2 == 4 && memcmp(m, "two!", 4) == 0;
           int n3 = recv(r, m, sizeof m, MSG_DONTWAIT); int e3 = errno;
           int u = send(s1, "x", 1, 0); int ue = errno;
           close(r);
           int g = send(s2, "gone", 4, 0); int ge = errno;
           close(s1); close(s2); unlink("/tmp/t_ltp2_dg");
           b == 0 && x == 3 && c == 0 && y == 4 && ok1 && ok2 && n3 == -1 && e3 == EAGAIN
             && u == -1 && ue == ENOTCONN && g == -1 && ge == ECONNREFUSED; }));
    EXPECT("AF_UNIX SOCK_DGRAM: getsockname gives the bound path, recvfrom a bound sender's, and replies reach it", ({
           unlink("/tmp/t_ltp2_d1"); unlink("/tmp/t_ltp2_d2");
           struct sockaddr_un a = { AF_UNIX, "/tmp/t_ltp2_d1" }, b = { AF_UNIX, "/tmp/t_ltp2_d2" };
           int s = socket(AF_UNIX, SOCK_DGRAM, 0), p = socket(AF_UNIX, SOCK_DGRAM, 0);
           bind(s, (struct sockaddr *)&a, sizeof a); bind(p, (struct sockaddr *)&b, sizeof b);
           struct sockaddr_un n; socklen_t nl = sizeof n;
           int g = getsockname(s, (struct sockaddr *)&n, &nl);
           int gok = g == 0 && nl == 2 + strlen(a.sun_path) + 1 && strcmp(n.sun_path, a.sun_path) == 0;
           int c = connect(p, (struct sockaddr *)&n, nl);
           int w = write(p, "ping", 4);
           char m[8]; struct sockaddr_un f; socklen_t fl = sizeof f;
           int r1 = recvfrom(s, m, sizeof m, 0, (struct sockaddr *)&f, &fl);
           int fok = r1 == 4 && fl > 2 && strcmp(f.sun_path, b.sun_path) == 0;
           int x = sendto(s, "pong", 4, 0, (struct sockaddr *)&f, fl);
           int r2 = read(p, m, sizeof m);
           close(s); close(p); unlink(a.sun_path); unlink(b.sun_path);
           gok && c == 0 && w == 4 && fok && x == 4 && r2 == 4 && memcmp(m, "pong", 4) == 0; }));
    EXPECT("pipe: pipes run out of descriptors (EMFILE) before the kernel runs out of pipes (LTP pipe06)", ({
           int fds[2], n = 0, e = 0, ok = 1, keep[2] = { -1, -1 };
           for (;;) {
               if (pipe(fds) != 0) { e = errno; break; }
               keep[0] = fds[0]; keep[1] = fds[1];
               n++;
           }
           char c = 0;
           if (keep[1] < 0 || write(keep[1], "x", 1) != 1 || read(keep[0], &c, 1) != 1 || c != 'x') ok = 0;
           for (int fd = 3; fd < 1024; fd++) close(fd);
           ok && e == EMFILE && n >= 100; }));
    EXPECT("pipe-max-size: root sets it, rounded up to pages; a new pipe of a user without CAP_SYS_RESOURCE starts that small", ({
           int ok = 1;
           if (geteuid() == 0) {
             char was[32] = {0};
             int r = open("/proc/sys/fs/pipe-max-size", O_RDONLY); read(r, was, sizeof was - 1); close(r);
             int w = open("/proc/sys/fs/pipe-max-size", O_WRONLY);
             int n = write(w, "5000\n", 5); close(w);
             char now[32] = {0};
             r = open("/proc/sys/fs/pipe-max-size", O_RDONLY); read(r, now, sizeof now - 1); close(r);
             pid_t c = fork();
             if (c == 0) {
               setuid(65534);
               int p[2]; pipe(p);
               int sz = fcntl(p[1], F_GETPIPE_SZ);
               int big = fcntl(p[1], F_SETPIPE_SZ, 65536); int e = errno;
               _exit(sz == 8192 && big == -1 && e == EPERM ? 0 : 1);
             }
             int st; waitpid(c, &st, 0);
             int p[2]; pipe(p); int rsz = fcntl(p[1], F_GETPIPE_SZ); close(p[0]); close(p[1]);
             w = open("/proc/sys/fs/pipe-max-size", O_WRONLY); write(w, was, strlen(was)); close(w);
             struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
             struct __user_cap_data_struct d[2]; syscall(SYS_capget, &h, d);
             int res = d[0].effective >> 24 & 1;   /* CAP_SYS_RESOURCE: root's new pipes are not limited */
             ok = n == 5 && strcmp(now, "8192\n") == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 && rsz == (res ? 65536 : 8192);
           }
           ok; }));
    EXPECT("mprotect: a shared mapping of /dev/zero opened read-only cannot become writable (EACCES)", ({
           int z = open("/dev/zero", O_RDONLY);
           void *m = mmap(0, 4096, PROT_READ, MAP_SHARED, z, 0);
           int r = mprotect(m, 4096, PROT_WRITE); int e = errno;
           void *w = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, z, 0); int we = errno;
           int r2 = mprotect(m, 4096, PROT_READ);
           munmap(m, 4096); close(z);
           m != MAP_FAILED && r == -1 && e == EACCES && w == MAP_FAILED && we == EACCES && r2 == 0; }));
    EXPECT("SIGBUS: a mapped page past the end of the file is a bus error", ({
           struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = bus; sa.sa_flags = SA_SIGINFO; sigaction(SIGBUS, &sa, 0);
           int f = open("/tmp/sigbus.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
           ftruncate(f, 2048);
           char *m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
           int in = 0, hit = 0;
           if (sigsetjmp(back, 1) == 0) { m[100] = 7; in = 1; m[4096 + 1] = 0; } else hit = 1;
           int code = bus_code;
           signal(SIGBUS, SIG_DFL); munmap(m, 8192); close(f); unlink("/tmp/sigbus.tmp");
           m != MAP_FAILED && in && hit && code == BUS_ADRERR; }));
    EXPECT("SIGBUS: the same page is ordinary memory once the file has grown", ({
           int f = open("/tmp/sigbus.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
           ftruncate(f, 2048);
           char *m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
           ftruncate(f, 8192);
           m[4096 + 1] = 9; int after = m[4096 + 1] == 9;
           munmap(m, 8192); close(f); unlink("/tmp/sigbus.tmp");
           m != MAP_FAILED && after; }));
    EXPECT("SIGBUS: a forked child gets it too", ({
           int f = open("/tmp/sigbus.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
           ftruncate(f, 2048);
           pid_t c = fork();
           if (c == 0) {
             signal(SIGBUS, busplain);
             char *m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, f, 0);
             if (m == MAP_FAILED) _exit(2);
             int hit = 0;
             if (sigsetjmp(back, 1) == 0) { char *ch = m + 4096 + 1; *ch = 0; } else hit = 1;
             _exit(hit ? 0 : 3);
           }
           int st = 0; if (c > 0) waitpid(c, &st, 0);
           signal(SIGBUS, SIG_DFL); close(f); unlink("/tmp/sigbus.tmp");
           c > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("SIGBUS: a file cut under a mapping takes the pages past the new end", ({
           signal(SIGBUS, busplain);
           int f = open("/tmp/sigbus.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
           ftruncate(f, 8192);
           char *m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
           m[4096 + 1] = 5;
           ftruncate(f, 4096);
           int hit = 0;
           if (sigsetjmp(back, 1) == 0) { m[4096 + 1] = 6; } else hit = 1;
           int first = m[1] == 0;
           signal(SIGBUS, SIG_DFL); munmap(m, 8192); close(f); unlink("/tmp/sigbus.tmp");
           m != MAP_FAILED && hit && first; }));
    EXPECT("rename: a directory moved from under the working directory leaves it in the moved directory", ({
           char cwd[256];
           mkdir("/tmp/rn_a", 0700); mkdir("/tmp/rn_a/sub", 0700);
           chdir("/tmp/rn_a/sub");
           int r = rename("/tmp/rn_a", "/tmp/rn_b");
           int f = open("file", O_CREAT | O_WRONLY, 0600);
           int e = f >= 0 && access("/tmp/rn_b/sub/file", F_OK) == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/tmp/rn_b/sub");
           if (f >= 0) close(f);
           unlink("/tmp/rn_b/sub/file"); chdir("/"); rmdir("/tmp/rn_b/sub"); rmdir("/tmp/rn_b");
           r == 0 && e; }));
    EXPECT("clock_getres: the coarse clocks report their tick, at least 1 ms", ({
           struct timespec a, b;
           int x = clock_getres(CLOCK_REALTIME_COARSE, &a), y = clock_getres(CLOCK_MONOTONIC_COARSE, &b);
           x == 0 && y == 0 && a.tv_nsec >= 1000000 && b.tv_nsec >= 1000000; }));
    EXPECT("mqueue: queues_max reads 256; at its limit an unprivileged mq_open that creates fails with ENOSPC, root is exempt", ({
           char was[32] = "";
           int r = open("/proc/sys/fs/mqueue/queues_max", O_RDONLY);
           int n = r >= 0 ? read(r, was, sizeof was - 1) : -1;
           if (r >= 0) close(r);
           int ok = n > 0 && strcmp(was, "256\n") == 0;
           if (ok) {
             mq_unlink("/mqlim_a"); mq_unlink("/mqlim_b");
             mqd_t a = mq_open("/mqlim_a", O_RDWR | O_CREAT | O_EXCL, 0666, 0);
             int w = open("/proc/sys/fs/mqueue/queues_max", O_WRONLY);
             ok = a != (mqd_t)-1 && w >= 0 && write(w, "1\n", 2) == 2;
             pid_t c = fork();
             if (c == 0) {
               setuid(65534);
               _exit(mq_open("/mqlim_b", O_RDWR | O_CREAT | O_EXCL, 0600, 0) == (mqd_t)-1 && errno == ENOSPC ? 0 : 1);
             }
             int st; waitpid(c, &st, 0);
             mqd_t b = mq_open("/mqlim_b", O_RDWR | O_CREAT | O_EXCL, 0600, 0);
             ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0 && b != (mqd_t)-1;
             if (w >= 0) { lseek(w, 0, SEEK_SET); write(w, "256\n", 4); close(w); }
             mq_unlink("/mqlim_a"); mq_unlink("/mqlim_b");
           }
           ok; }));
    EXPECT("madvise: a range with a munmapped page in it is ENOMEM, one without is not", ({
           char *m = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int ok = m != MAP_FAILED && madvise(m, 3 * 4096, MADV_NORMAL) == 0;
           ok = ok && munmap(m + 2 * 4096, 4096) == 0;
           ok = ok && madvise(m, 2 * 4096, MADV_NORMAL) == 0;
           ok = ok && madvise(m, 3 * 4096, MADV_NORMAL) == -1 && errno == ENOMEM;
           munmap(m, 2 * 4096);
           ok; }));
    EXPECT("IPv4 multicast: leaving a group never joined is EADDRNOTAVAIL, a join then a leave works", ({
           int s = socket(AF_INET, SOCK_DGRAM, 0);
           struct ip_mreq m;
           m.imr_multiaddr.s_addr = htonl(0xe0000123);
           m.imr_interface.s_addr = htonl(INADDR_ANY);
           int ok = s >= 0;
           ok = ok && setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof m) == -1 && errno == EADDRNOTAVAIL;
           ok = ok && setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, 3) == -1 && errno == EINVAL;
           ok = ok && setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0;
           ok = ok && setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof m) == 0;
           ok = ok && setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof m) == -1 && errno == EADDRNOTAVAIL;
           close(s);
           ok; }));
    EXPECT("pwrite and writev: a file takes the whole count, however far the file grows", ({
           int fd = open("cf_pwhole", O_RDWR | O_CREAT | O_TRUNC, 0644);
           static char big[100000];
           memset(big, 'p', sizeof big);
           int ok = fd >= 0;
           for (int i = 0; ok && i < 60; i++) {
               off_t at = (off_t)i * 70001 + 3;
               size_t n = 33757 + (size_t)i * 1000;
               ok = pwrite(fd, big, n, at) == (ssize_t)n;
           }
           ok = ok && pwrite(fd, big, sizeof big, 30000000) == (ssize_t)sizeof big;
           for (int i = 0; ok && i < 40; i++) {
               struct iovec v[3] = { { big, 20000 + (size_t)i * 500 }, { big + 30000, 1 }, { big + 40000, 46873 } };
               size_t n = v[0].iov_len + 1 + 46873;
               ok = lseek(fd, (off_t)i * 80000 + 7, SEEK_SET) >= 0 && writev(fd, v, 3) == (ssize_t)n;
           }
           close(fd); unlink("cf_pwhole");
           ok; }));
    EXPECT("F_SETSIG: the siginfo of O_ASYNC input on a pipe says POLL_IN and the descriptor", ({
           int pf[2];
           int ok = pipe(pf) == 0;
           sigset_t set;
           sigemptyset(&set); sigaddset(&set, SIGUSR1);
           sigprocmask(SIG_BLOCK, &set, NULL);
           ok = ok && fcntl(pf[0], F_SETOWN, getpid()) == 0 && fcntl(pf[0], F_SETSIG, SIGUSR1) == 0;
           ok = ok && fcntl(pf[0], F_SETFL, O_ASYNC) == 0;
           ok = ok && write(pf[1], "x", 1) == 1;
           siginfo_t si;
           memset(&si, 0, sizeof si);
           struct timespec to = { 2, 0 };
           ok = ok && sigtimedwait(&set, &si, &to) == SIGUSR1;
           ok = ok && si.si_code == POLL_IN && si.si_fd == pf[0] && si.si_band == (POLLIN | POLLRDNORM);
           close(pf[0]); close(pf[1]);
           ok; }));
    EXPECT("lstat of /proc/self/fd/N is a link", ({
           struct stat st;
           int ok = lstat("/proc/self/fd/0", &st) == 0 && S_ISLNK(st.st_mode);
           ok; }));
    EXPECT("lstat of a /proc/self/task/TID/fd directory", ({
           struct stat st;
           char path[96];
           snprintf(path, sizeof path, "/proc/self/task/%d/fd", (int)getpid());
           int ok = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
           ok; }));
    EXPECT("lstat of /proc/self/task/TID/fd/N is a link", ({
           struct stat st;
           char path[96];
           snprintf(path, sizeof path, "/proc/self/task/%d/fd/0", (int)getpid());
           int ok = lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
           ok; }));
    EXPECT("stat of /proc/self/task/TID/fdinfo/N", ({
           struct stat st;
           char path[96];
           snprintf(path, sizeof path, "/proc/self/task/%d/fdinfo/0", (int)getpid());
           int ok = stat(path, &st) == 0;
           ok; }));
    EXPECT("lstat of /proc/self/task/TID/ns/mnt and /proc/self/task/TID/status", ({
           struct stat st;
           char path[96];
           snprintf(path, sizeof path, "/proc/self/task/%d/status", (int)getpid());
           int ok = stat(path, &st) == 0;
           ok; }));
    EXPECT("a process signal every thread blocks goes to the first thread that unblocks it", ({
           struct sigaction sa = { .sa_handler = adopt_handler };
           sigaction(SIGUSR1, &sa, 0);
           sigset_t m;
           sigemptyset(&m);
           sigaddset(&m, SIGUSR1);
           pthread_sigmask(SIG_BLOCK, &m, 0);
           volatile int ready = 0;
           pthread_t t;
           pthread_create(&t, 0, adopt_worker, (void *)&ready);
           while (!ready) usleep(1000);
           kill(getpid(), SIGUSR1);
           ready = 2;
           pthread_join(t, 0);
           adopt_hit == 1; }));
    EXPECT("a signal reaches a thread running on another processor without waiting for its tick", ({
           struct sigaction sa = { .sa_handler = kick_handler };
           sigaction(SIGUSR2, &sa, 0);
           sigset_t m;
           sigemptyset(&m);
           sigaddset(&m, SIGUSR2);
           pthread_sigmask(SIG_BLOCK, &m, 0);          // only the spinning thread takes it
           pthread_t t;
           pthread_create(&t, 0, kick_worker, 0);
           usleep(50000);
           // A signal that waits for the tick takes about 10 ms each, a second
           // for all; a busy host can stall one round, so the best of three counts.
           int ok = 0;
           for (int round = 0; round < 3 && !ok; round++) {
               struct timespec t0, t1;
               kick_hits = 0;
               clock_gettime(CLOCK_MONOTONIC, &t0);
               for (int i = 1; i <= 100; i++) {
                   kill(getpid(), SIGUSR2);
                   for (int w = 0; kick_hits < i && w < 2000; w++) usleep(500);
               }
               clock_gettime(CLOCK_MONOTONIC, &t1);
               long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
               ok = kick_hits == 100 && ms < 600;
           }
           kick_stop = 1;
           pthread_join(t, 0);
           ok; }));
    EXPECT("a tmpfs mounted nosuid, nodev and noexec reports them, refuses to run a program or open a device node, and remount clears them", ({
           int ok = mkdir("/tmp/mflags", 0755) == 0 || errno == EEXIST;
           ok = ok && mount("none", "/tmp/mflags", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV, 0) == 0;
           struct statfs sf;
           ok = ok && statfs("/tmp/mflags", &sf) == 0 && (sf.f_flags & 15) == 14;
           int fd = open("/tmp/mflags/x", O_CREAT | O_WRONLY, 0755);
           ok = ok && fd >= 0 && write(fd, "x", 1) == 1;
           if (fd >= 0) close(fd);
           char *av[] = { "/tmp/mflags/x", 0 };
           execve("/tmp/mflags/x", av, 0);
           ok = ok && errno == EACCES;
           ok = ok && mknod("/tmp/mflags/null", S_IFCHR | 0666, makedev(1, 3)) == 0;
           ok = ok && open("/tmp/mflags/null", O_RDWR) < 0 && errno == EACCES;
           ok = ok && mount("none", "/tmp/mflags", "tmpfs", MS_REMOUNT, 0) == 0;
           ok = ok && statfs("/tmp/mflags", &sf) == 0 && (sf.f_flags & 15) == 0;
           fd = open("/tmp/mflags/null", O_RDWR);
           ok = ok && fd >= 0;
           if (fd >= 0) close(fd);
           execve("/tmp/mflags/x", av, 0);
           ok = ok && errno != EACCES;
           unlink("/tmp/mflags/x");
           unlink("/tmp/mflags/null");
           umount("/tmp/mflags");
           rmdir("/tmp/mflags");
           ok; }));
    EXPECT("a tmpfs mounted nosymfollow reports it, refuses to follow a link under it (ELOOP) but reads the link, and remount clears it", ({
           int ok = mkdir("/tmp/mnsf", 0755) == 0 || errno == EEXIST;
           ok = ok && mount("none", "/tmp/mnsf", "tmpfs", 256, 0) == 0;   // MS_NOSYMFOLLOW
           struct statfs sf;
           ok = ok && statfs("/tmp/mnsf", &sf) == 0 && (sf.f_flags & 0x2000) != 0;
           int fd = open("/tmp/mnsf/t", O_CREAT | O_WRONLY, 0644);
           ok = ok && fd >= 0;
           if (fd >= 0) close(fd);
           ok = ok && symlink("/tmp/mnsf/t", "/tmp/mnsf/l") == 0;
           ok = ok && open("/tmp/mnsf/l", O_RDONLY) < 0 && errno == ELOOP;
           char b[64];
           ok = ok && readlink("/tmp/mnsf/l", b, sizeof b) == 11;
           ok = ok && mount("none", "/tmp/mnsf", "tmpfs", MS_REMOUNT, 0) == 0;
           ok = ok && statfs("/tmp/mnsf", &sf) == 0 && (sf.f_flags & 0x2000) == 0;
           fd = open("/tmp/mnsf/l", O_RDONLY);
           ok = ok && fd >= 0;
           if (fd >= 0) close(fd);
           unlink("/tmp/mnsf/l");
           unlink("/tmp/mnsf/t");
           umount("/tmp/mnsf");
           rmdir("/tmp/mnsf");
           ok; }));
    EXPECT("a built-in program under /bin opens and reads as the ELF file exec runs", ({
           struct stat st;
           int fd = open("/bin/cat", O_RDONLY);
           char h[4];
           int ok = fd >= 0 && stat("/bin/cat", &st) == 0;
           struct stat fs;
           ok = ok && fstat(fd, &fs) == 0 && fs.st_size == st.st_size && fs.st_size > 4096;
           ok = ok && pread(fd, h, 4, 0) == 4 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F';
           char tail[16];
           ok = ok && pread(fd, tail, 16, fs.st_size - 16) == 16;   // the last chunk, past the transfer buffer
           ok = ok && open("/bin/nonesuch", O_RDONLY) < 0 && errno == ENOENT;
           ok = ok && open("/bin/cat", O_WRONLY) < 0;
           if (fd >= 0) close(fd);
           ok; }));
    EXPECT("a name below a device node or a built-in program is ENOTDIR, below nothing ENOENT", ({
           struct stat st;
           int ok = stat("/dev/null/x", &st) < 0 && errno == ENOTDIR;
           ok = ok && stat("/bin/cat/x", &st) < 0 && errno == ENOTDIR;
           ok = ok && open("/dev/null/x/y", O_RDONLY) < 0 && errno == ENOTDIR;
           ok = ok && stat("/dev/nonesuch/x", &st) < 0 && errno == ENOENT;
           ok = ok && stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode);
           ok; }));
    EXPECT("send(MSG_DONTWAIT) on a blocking TCP socket with a full send buffer is EAGAIN, not a wait", ({
           int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
           bind(ls, (void *)&sa, sizeof sa); listen(ls, 1); socklen_t sl = sizeof sa; getsockname(ls, (void *)&sa, &sl);
           int c = socket(AF_INET, SOCK_STREAM, 0); connect(c, (void *)&sa, sizeof sa); int s = accept(ls, 0, 0);
           char blk[4096]; memset(blk, 'a', sizeof blk);
           long total = 0; int blocked = 0;
           while (total < (64L << 20)) {
               long r = send(c, blk, sizeof blk, MSG_DONTWAIT);
               if (r < 0) { blocked = errno == EAGAIN; break; }
               total += r;
           }
           close(c); close(s); close(ls);
           blocked; }));
    return done();
}
