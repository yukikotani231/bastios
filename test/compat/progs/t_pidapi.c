/* The process API recent runtimes use (Go's os/exec, Rust's std::process,
 * systemd-style tools): clone3 with CLONE_PIDFD, pidfd_open and its flags,
 * waitid on a pidfd, pidfd_send_signal and its flags, pidfd_getfd, polling a
 * pidfd. */
#include "compat.h"
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x1000
#endif
#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#ifndef PIDFD_NONBLOCK
#define PIDFD_NONBLOCK O_NONBLOCK
#endif
#ifndef PIDFD_THREAD
#define PIDFD_THREAD O_EXCL
#endif
#ifndef PIDFD_SIGNAL_THREAD
#define PIDFD_SIGNAL_THREAD (1UL << 0)
#define PIDFD_SIGNAL_THREAD_GROUP (1UL << 1)
#define PIDFD_SIGNAL_PROCESS_GROUP (1UL << 2)
#endif

static int pidfd_open_(pid_t p, unsigned f) { return syscall(SYS_pidfd_open, p, f); }
static int pidfd_signal(int fd, int s, siginfo_t *i, unsigned f) { return syscall(SYS_pidfd_send_signal, fd, s, i, f); }

struct c3 {
    unsigned long long flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls;
};

/* A child that exits with `code` once it reads a byte from `go`, or at once if go < 0. */
static pid_t child_c3(int *pidfd, unsigned long long flags, int go, int code)
{
    struct c3 a = { 0 };
    a.flags = flags | CLONE_PIDFD;
    a.pidfd = (unsigned long long)(unsigned long)pidfd;
    a.exit_signal = SIGCHLD;
    pid_t p = syscall(SYS_clone3, &a, sizeof a);
    if (p == 0) {
        char c;
        if (go >= 0) read(go, &c, 1);
        _exit(code);
    }
    return p;
}

int main(void)
{
    setup();
    int pfd = -1, go[2];
    pipe(go);
    pid_t p = child_c3(&pfd, 0, go[0], 7);
    EXPECT("clone3 with CLONE_PIDFD returns a pidfd", p > 0 && pfd >= 0);
    EXPECT("the pidfd is close-on-exec", p > 0 && (fcntl(pfd, F_GETFD) & FD_CLOEXEC));
    {
        struct pollfd q = { pfd, POLLIN, 0 };
        EXPECT("poll on a running child's pidfd says nothing", poll(&q, 1, 0) == 0);
    }
    {
        siginfo_t si = { 0 };
        EXPECT("waitid(P_PIDFD, WNOHANG) of a running child returns 0 with si_pid 0",
               waitid(P_PIDFD, pfd, &si, WEXITED | WNOHANG) == 0 && si.si_pid == 0);
    }
    write(go[1], "x", 1);
    {
        struct pollfd q = { pfd, POLLIN, 0 };
        EXPECT("poll on the pidfd of a child that exits says POLLIN", poll(&q, 1, 5000) == 1 && (q.revents & POLLIN));
    }
    {
        struct epoll_event ev = { EPOLLIN, { 0 } }, out;
        int ep = epoll_create1(0);
        epoll_ctl(ep, EPOLL_CTL_ADD, pfd, &ev);
        EXPECT("epoll on the pidfd of an ended child", epoll_wait(ep, &out, 1, 1000) == 1 && (out.events & EPOLLIN));
        close(ep);
    }
    {
        siginfo_t si = { 0 };
        EXPECT("waitid(P_PIDFD, WNOWAIT) reads the status and leaves the zombie",
               waitid(P_PIDFD, pfd, &si, WEXITED | WNOWAIT) == 0 && si.si_pid == p && si.si_code == CLD_EXITED && si.si_status == 7);
        EXPECT("waitid(P_PIDFD) reaps it", waitid(P_PIDFD, pfd, &si, WEXITED) == 0 && si.si_pid == p && si.si_status == 7);
        EXPECT("a second waitid(P_PIDFD) is ECHILD", waitid(P_PIDFD, pfd, &si, WEXITED) == -1 && errno == ECHILD);
        EXPECT("pidfd_send_signal to the reaped child is ESRCH", pidfd_signal(pfd, 0, NULL, 0) == -1 && errno == ESRCH);
    }
    close(pfd);

    /* pidfd_open and its flags. */
    {
        int pp[2];
        pipe(pp);
        pid_t c = fork();
        if (c == 0) { char b; read(pp[0], &b, 1); _exit(3); }
        int fd = pidfd_open_(c, PIDFD_NONBLOCK);
        EXPECT("pidfd_open with PIDFD_NONBLOCK sets O_NONBLOCK", fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
        siginfo_t si = { 0 };
        EXPECT("waitid on a non-blocking pidfd with nothing to report is EAGAIN",
               fd >= 0 && waitid(P_PIDFD, fd, &si, WEXITED) == -1 && errno == EAGAIN);
        siginfo_t qi = { 0 };
        qi.si_signo = SIGUSR1;
        qi.si_code = SI_QUEUE;
        EXPECT("pidfd_send_signal with a siginfo of the right signal", pidfd_signal(fd, SIGUSR1, &qi, 0) == 0);
        siginfo_t s2 = { 0 };
        struct pollfd q = { fd, POLLIN, 0 };
        poll(&q, 1, 5000);
        int r = waitid(P_PIDFD, fd, &s2, WEXITED);
        EXPECT("a child killed by the signal is reported as CLD_KILLED with its signal",
               r == 0 && s2.si_code == CLD_KILLED && s2.si_status == SIGUSR1);
        write(pp[1], "x", 1);
        close(fd);
        EXPECT("pidfd_open of a pid with no process is ESRCH", pidfd_open_(c, 0) == -1 && errno == ESRCH);
        EXPECT("pidfd_open of pid 0 is EINVAL", pidfd_open_(0, 0) == -1 && errno == EINVAL);
    }

    /* A zombie can still be opened and waited for. */
    {
        pid_t c = fork();
        if (c == 0) _exit(9);
        usleep(100000);
        int fd = pidfd_open_(c, 0);
        siginfo_t si = { 0 };
        EXPECT("pidfd_open of a zombie works and waitid reaps it", fd >= 0 && waitid(P_PIDFD, fd, &si, WEXITED) == 0 && si.si_status == 9);
        close(fd);
    }

    /* pidfd_getfd. */
    {
        int pp[2], hold[2];
        pipe(pp);
        pipe(hold);
        pid_t c = fork();
        if (c == 0) { char b; read(hold[0], &b, 1); _exit(0); }
        int fd = pidfd_open_(c, 0);
        int got = syscall(SYS_pidfd_getfd, fd, hold[1], 0);
        EXPECT("pidfd_getfd copies the child's descriptor, close-on-exec", got >= 0 && (fcntl(got, F_GETFD) & FD_CLOEXEC));
        EXPECT("pidfd_getfd with flags is EINVAL", syscall(SYS_pidfd_getfd, fd, hold[1], 1) == -1 && errno == EINVAL);
        EXPECT("pidfd_getfd of a closed descriptor is EBADF", syscall(SYS_pidfd_getfd, fd, 99, 0) == -1 && errno == EBADF);
        write(hold[1], "x", 1);
        waitid(P_PIDFD, fd, &(siginfo_t){ 0 }, WEXITED);
        close(fd);
    }

    /* clone3 flag combinations. */
    {
        int fd = -1;
        struct c3 a = { 0 };
        a.flags = CLONE_PIDFD | CLONE_PARENT_SETTID;
        a.pidfd = (unsigned long)&fd;
        a.parent_tid = (unsigned long)&fd;
        a.exit_signal = SIGCHLD;
        EXPECT("clone3 CLONE_PIDFD and CLONE_PARENT_SETTID at one address is EINVAL", syscall(SYS_clone3, &a, sizeof a) == -1 && errno == EINVAL);
        int tid = 0;
        fd = -1;
        a.parent_tid = (unsigned long)&tid;
        long r = syscall(SYS_clone3, &a, sizeof a);
        if (r == 0)
            _exit(0);
        EXPECT("clone3 CLONE_PIDFD and CLONE_PARENT_SETTID at two addresses fill both", r > 0 && fd >= 0 && tid == r);
        if (r > 0) {
            waitid(P_PIDFD, fd, &(siginfo_t){ 0 }, WEXITED);
            close(fd);
        }
        struct c3 b = { 0 };
        b.exit_signal = 300;
        EXPECT("clone3 with an exit signal above 255 is EINVAL", syscall(SYS_clone3, &b, sizeof b) == -1 && errno == EINVAL);
        EXPECT("clone3 with a size below 64 is EINVAL", syscall(SYS_clone3, &b, 63) == -1 && errno == EINVAL);
    }

    /* A thread's pidfd (6.9). */
    {
        int fd = pidfd_open_(getpid(), PIDFD_THREAD);
        EXPECT("pidfd_open with PIDFD_THREAD on the caller works", fd >= 0);
        EXPECT("pidfd_send_signal with PIDFD_SIGNAL_THREAD_GROUP to itself",
               fd >= 0 && pidfd_signal(fd, 0, NULL, PIDFD_SIGNAL_THREAD_GROUP) == 0);
        EXPECT("pidfd_send_signal with PIDFD_SIGNAL_THREAD and PIDFD_SIGNAL_PROCESS_GROUP together is EINVAL",
               fd >= 0 && pidfd_signal(fd, 0, NULL, PIDFD_SIGNAL_THREAD | PIDFD_SIGNAL_PROCESS_GROUP) == -1 && errno == EINVAL);
        close(fd);
    }
    return done();
}
