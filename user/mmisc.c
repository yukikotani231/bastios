/* Small Linux system calls, against musl: uname and gethostname, readv,
 * pread and pwrite, limits, sysinfo, statfs, getrandom, priorities, umask,
 * sessions and sched_yield. */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
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
    unsigned char r[16];
    if (getrandom(r, sizeof r, 0) != sizeof r)
        return fail("getrandom");
    errno = 0;
    if (getpriority(PRIO_PROCESS, 0) != 0 || errno != 0 || setpriority(PRIO_PROCESS, 0, 5) != 0)
        return fail("priorities");
    umask(022);
    if (sched_yield() != 0)
        return fail("sched_yield");
    if (setsid() != getpid() || getsid(0) != getpid() || getpgrp() != getpid())
        return fail("sessions");
    if (signals() != 0)
        return 1;
    printf("mmisc: ok on %s %s, signals\n", u.sysname, u.machine);
    return 0;
}
