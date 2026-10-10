/* Small Linux system calls, against musl: uname and gethostname, readv,
 * pread and pwrite, limits, sysinfo, statfs, getrandom, priorities, umask,
 * sessions and process groups, the terminal's name and group, sched_yield,
 * how much a pipe holds, and a private copy of a read-only page that
 * mprotect makes writable (the kernel shares those pages between the
 * processes running a built-in program). */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("mmisc: %s failed (errno %d)\n", what, errno);
    return 1;
}

static volatile int got_usr, got_chld, info_pid;

static void on_info(int s, siginfo_t *si, void *uc)
{
    (void)uc;
    info_pid = s == SIGUSR2 && si->si_signo == SIGUSR2 ? si->si_pid : -1;
}

static void on_usr(int s)
{
    (void)s;
    got_usr++;
}

static void on_chld(int s)
{
    (void)s;
    got_chld++;
}

/* Waiting for signals: sigpending, sigwaitinfo, sigtimedwait, sigsuspend,
 * SIGCHLD from a child that ends, and sigaltstack. */
static int signals(void)
{
    sigset_t usr1, cur;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &usr1, 0) != 0)
        return fail("sigprocmask");
    kill(getpid(), SIGUSR1);
    if (sigpending(&cur) != 0 || !sigismember(&cur, SIGUSR1))
        return fail("sigpending");
    siginfo_t si;
    if (sigwaitinfo(&usr1, &si) != SIGUSR1 || si.si_signo != SIGUSR1 || si.si_pid != getpid())
        return fail("sigwaitinfo");
    struct timespec brief = { 0, 50000000 };
    if (sigtimedwait(&usr1, &si, &brief) != -1 || errno != EAGAIN)
        return fail("sigtimedwait timing out");

    struct sigaction sa = { 0 };
    sa.sa_handler = on_usr;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, 0);
    pid_t c = fork();
    if (c < 0)
        return fail("fork");
    if (c == 0) {
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    sigset_t none;
    sigemptyset(&none);
    while (!got_usr)
        if (sigsuspend(&none) != -1 || errno != EINTR)
            return fail("sigsuspend");
    if (sigprocmask(SIG_BLOCK, 0, &cur) != 0 || !sigismember(&cur, SIGUSR1))
        return fail("sigsuspend giving the mask back");
    int st;
    waitpid(c, &st, 0);

    sa.sa_handler = on_chld;
    sa.sa_flags = 0;
    got_chld = 0;
    sigaction(SIGCHLD, &sa, 0);
    c = fork();
    if (c < 0)
        return fail("fork");
    if (c == 0)
        _exit(3);
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 3)
        return fail("waitpid with a SIGCHLD handler");
    if (got_chld != 1)
        return fail("SIGCHLD");
    signal(SIGCHLD, SIG_DFL);

    struct sigaction si_sa = { 0 };
    si_sa.sa_sigaction = on_info;
    si_sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR2, &si_sa, 0) != 0 || kill(getpid(), SIGUSR2) != 0 || info_pid != getpid())
        return fail("SA_SIGINFO's si_pid");

    stack_t old;
    if (sigaltstack(0, &old) != 0 || !(old.ss_flags & SS_DISABLE))
        return fail("sigaltstack");
    return 0;
}

/* kill(0) reaches the caller's group, and kill(-g) group g, and nobody
 * else: a child in a group of its own signals itself and its own child. */
static volatile int usr_seen;
static void on_usr1(int s)
{
    usr_seen |= s == SIGUSR1 ? 1 : 2;   /* signals of one kind do not queue */
}

static int groups(void)
{
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr1);
    usr_seen = 0;
    pid_t c = fork();
    if (c < 0)
        return -1;
    if (c == 0) {
        if (setpgid(0, 0) != 0 || getpgrp() != getpid())
            _exit(2);
        pid_t gc = fork();
        if (gc == 0) {
            for (int i = 0; usr_seen != 3 && i < 1000000; i++)
                sched_yield();
            _exit(usr_seen == 3 ? 0 : 3);  /* SIGUSR1 from kill(0), SIGUSR2 from kill(-g) */
        }
        if (getpgid(gc) != getpid() || kill(0, SIGUSR1) != 0 || kill(-getpid(), SIGUSR2) != 0)
            _exit(4);
        int st;
        if (waitpid(gc, &st, 0) != gc || !WIFEXITED(st) || WEXITSTATUS(st) != 0 || usr_seen != 3)
            _exit(5);
        _exit(0);
    }
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0 || usr_seen != 0)
        return -1;                      /* the parent, in another group, saw nothing */
    if (kill(-c, 0) != -1 || errno != ESRCH)
        return -1;                      /* that group is gone */
    return 0;
}

/* A pipe holds 64 KiB, as on Linux. Once nearly full, a write of PIPE_BUF
 * bytes or fewer goes in whole or not at all; a bigger one goes in part. */
static int pipes(void)
{
    static char blk[4096];
    int q[2];
    if (pipe2(q, O_NONBLOCK) != 0)
        return -1;
    int held = 0, n;
    while (held < (1 << 20) && (n = write(q[1], blk, sizeof blk)) > 0)
        held += n;
    if (n != -1 || errno != EAGAIN)
        return -1;
    if (read(q[0], blk, 100) != 100)
        return -1;
    if (write(q[1], blk, 200) != -1 || errno != EAGAIN)
        return -1;                          /* atomic: no room for all 200 */
    if (write(q[1], blk, 50) != 50 || write(q[1], blk, 8192) != 50)
        return -1;                          /* not atomic: what fits goes in */
    close(q[0]);
    close(q[1]);
    return held;
}

/* Big enough to hold a whole page of constants, which the kernel maps
 * from one shared copy in every process running this program. */
static const char motto[8192] = "read-only, and shared";

/* A child that makes a page of constants writable and writes it gets its
 * own copy: the parent, which runs on the same shared page, still reads
 * what the program says. */
static int private_copy(void)
{
    volatile char *p = (volatile char *)(((unsigned long)motto + 4095) & ~4095UL);
    pid_t c = fork();
    if (c == 0) {
        if (mprotect((void *)p, 4096, PROT_READ | PROT_WRITE) != 0)
            _exit(2);
        p[0] = 'R';
        _exit(p[0] == 'R' ? 0 : 3);
    }
    int st;
    if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return fail("mprotect a page of constants");
    if (p[0] != 0 || motto[0] != 'r')
        return fail("a written page of constants stays private");
    return 0;
}

int main(void)
{
    struct utsname u;
    char host[64];
    if (uname(&u) != 0 || strcmp(u.sysname, "Linux") != 0 || strcmp(u.machine, "x86_64") != 0)
        return fail("uname");
    if (gethostname(host, sizeof host) != 0 || strcmp(host, "bastios") != 0)
        return fail("gethostname");

    int fd = open("mm", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "abcdefgh", 8) != 8)
        return fail("write");
    char x[4] = { 0 }, y[4] = { 0 };
    if (pread(fd, x, 3, 2) != 3 || strcmp(x, "cde") != 0 || lseek(fd, 0, SEEK_CUR) != 8)
        return fail("pread");
    if (pwrite(fd, "XY", 2, 0) != 2 || lseek(fd, 0, SEEK_CUR) != 8)
        return fail("pwrite");
    lseek(fd, 0, SEEK_SET);
    char z[4] = { 0 };
    struct iovec iov[3] = { { x, 0 }, { y, 3 }, { z, 3 } };
    if (readv(fd, iov, 3) != 6 || strncmp(y, "XYc", 3) != 0 || strncmp(z, "def", 3) != 0)
        return fail("readv");
    if (fsync(fd) != 0)
        return fail("fsync");
    struct stat fs;
    if (ftruncate(fd, 4) != 0 || fstat(fd, &fs) != 0 || fs.st_size != 4)
        return fail("ftruncate shrinking");
    if (ftruncate(fd, 100) != 0 || fstat(fd, &fs) != 0 || fs.st_size != 100 || pread(fd, z, 2, 50) != 2 || z[0] || z[1])
        return fail("ftruncate growing");
    close(fd);
    if (truncate("mm", 3) != 0 || stat("mm", &fs) != 0 || fs.st_size != 3)
        return fail("truncate");
    if (chmod("mm", 0600) != 0 || unlink("mm") != 0)
        return fail("chmod");

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == 0)
        return fail("getrlimit");
    struct sysinfo si;
    if (sysinfo(&si) != 0 || si.totalram == 0 || si.procs == 0)
        return fail("sysinfo");
    struct statfs sf;
    if (statfs("/", &sf) != 0 || sf.f_bsize != 4096)
        return fail("statfs");
    unsigned char r[16], r2[16];
    if (getrandom(r, sizeof r, 0) != sizeof r || getrandom(r2, sizeof r2, 0) != sizeof r2
        || memcmp(r, r2, sizeof r) == 0)
        return fail("getrandom");                   /* back to back, still different */
    errno = 0;
    if (getpriority(PRIO_PROCESS, 0) != 0 || errno != 0 || setpriority(PRIO_PROCESS, 0, 5) != 0)
        return fail("priorities");
    umask(022);
    if (sched_yield() != 0)
        return fail("sched_yield");
    if (setsid() != getpid() || getsid(0) != getpid() || getpgrp() != getpid())
        return fail("sessions");
    if (setsid() != -1 || errno != EPERM)
        return fail("setsid of a group leader");
    const char *tty = ttyname(0);
    if (!tty || strcmp(tty, "/dev/tty") != 0 || tcgetpgrp(0) != getpgrp())
        return fail("ttyname and tcgetpgrp");
    if (groups() != 0)
        return fail("process groups");
    if (signals() != 0)
        return 1;
    int held = pipes();
    if (held < 0)
        return fail("pipe capacity");
    if (private_copy() != 0)
        return 1;
    printf("mmisc: ok on %s %s, signals, pipe %d\n", u.sysname, u.machine, held);
    return 0;
}
