/* Time and waiting through the Linux system calls, against musl: the
 * monotonic clock, nanosleep, poll timing out on an empty pipe, poll seeing
 * data and the end of a pipe, poll waking when a child writes, and select
 * and pselect over pipes, alarm, setitimer and pause, and eventfd, timerfd
 * and epoll. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("mtime: %s failed (errno %d)\n", what, errno);
    return 1;
}

static volatile int rang;

static void on_alarm(int s)
{
    (void)s;
    rang++;
}

static long long now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return -1;
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* eventfd, timerfd and epoll. Returns 0, or what failed. */
static const char *events(void)
{
    uint64_t v;
    int e = eventfd(3, EFD_NONBLOCK | EFD_CLOEXEC);
    v = 4;
    if (e < 0 || write(e, &v, 8) != 8 || read(e, &v, 8) != 8 || v != 7)
        return "eventfd counting";
    if (read(e, &v, 8) != -1 || errno != EAGAIN)
        return "an empty eventfd";
    int sem = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    if (sem < 0 || read(sem, &v, 8) != 8 || v != 1 || read(sem, &v, 8) != 8 || v != 1
        || read(sem, &v, 8) != -1 || errno != EAGAIN)
        return "a semaphore eventfd";
    close(sem);

    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 20000000 }, { 0, 50000000 } }, cur;
    long long t0 = now_ms();
    if (t < 0 || timerfd_settime(t, 0, &its, 0) != 0)
        return "timerfd_settime";
    if (read(t, &v, 8) != 8 || v < 1 || now_ms() - t0 < 40)
        return "a timerfd read that waits";
    if (timerfd_gettime(t, &cur) != 0 || cur.it_interval.tv_nsec != 20000000)
        return "timerfd_gettime";
    struct itimerspec off = { { 0, 0 }, { 0, 0 } };
    if (timerfd_settime(t, 0, &off, &cur) != 0 || cur.it_interval.tv_nsec != 20000000)
        return "disarming a timerfd";

    int p[2], ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 7 }, out[8];
    if (ep < 0 || pipe(p) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) != 0)
        return "epoll_ctl ADD";
    if (epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) != -1 || errno != EEXIST)
        return "adding twice";
    ev.data.u64 = 9;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return "watching a pipe";
    ev.data.u64 = 11;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, t, &ev) != 0)
        return "watching a timerfd";
    if (epoll_wait(ep, out, 8, 0) != 0)
        return "epoll_wait with nothing ready";
    t0 = now_ms();
    if (epoll_wait(ep, out, 8, 60) != 0 || now_ms() - t0 < 40)
        return "an epoll_wait that times out";
    v = 1;
    if (write(e, &v, 8) != 8 || epoll_wait(ep, out, 8, 0) != 1 || out[0].data.u64 != 7
        || !(out[0].events & EPOLLIN))
        return "epoll_wait seeing an eventfd";
    read(e, &v, 8);
    pid_t c = fork();
    if (c < 0)
        return "fork";
    if (c == 0) {
        struct timespec d = { 0, 100000000 };
        nanosleep(&d, 0);
        write(p[1], "x", 1);
        _exit(0);
    }
    int n = epoll_wait(ep, out, 8, 5000);
    if (n != 1 || out[0].data.u64 != 9)
        return "epoll_wait woken by a pipe";
    waitpid(c, 0, 0);
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) != 0 || epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) != -1
        || errno != ENOENT)
        return "epoll_ctl DEL";
    its.it_value.tv_nsec = 10000000;
    if (timerfd_settime(t, 0, &its, 0) != 0 || epoll_wait(ep, out, 8, 2000) != 1
        || out[0].data.u64 != 11)
        return "epoll_wait woken by a timerfd";
    int f = open("/mtime_f", O_CREAT | O_RDWR, 0644);
    if (f >= 0 && (epoll_ctl(ep, EPOLL_CTL_ADD, f, &ev) != -1 || errno != EPERM))
        return "watching a file";
    if (f >= 0) {
        close(f);
        unlink("/mtime_f");
    }
    close(t);
    close(ep);
    close(e);
    close(p[0]);
    close(p[1]);
    return 0;
}

int main(void)
{
    long long t0 = now_ms();
    if (t0 < 0)
        return fail("clock_gettime");
    struct timespec nap = { 0, 200 * 1000000 };
    if (nanosleep(&nap, 0) != 0)
        return fail("nanosleep");
    long long slept = now_ms() - t0;
    if (slept < 200 || slept > 2000)
        return fail("sleeping 200 ms");
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0 || tv.tv_usec >= 1000000)
        return fail("gettimeofday");

    int p[2];
    if (pipe(p) != 0)
        return fail("pipe");
    struct pollfd pf = { p[0], POLLIN, 0 };
    t0 = now_ms();
    if (poll(&pf, 1, 100) != 0)
        return fail("poll on an empty pipe");
    long long waited = now_ms() - t0;
    if (waited < 100 || waited > 2000)
        return fail("poll timing out after 100 ms");
    if (write(p[1], "x", 1) != 1)
        return fail("write");
    if (poll(&pf, 1, -1) != 1 || pf.revents != POLLIN)
        return fail("poll seeing data");
    char c;
    close(p[1]);
    if (read(p[0], &c, 1) != 1)
        return fail("read");
    if (poll(&pf, 1, 0) != 1 || !(pf.revents & POLLHUP))
        return fail("poll seeing the end");
    close(p[0]);

    if (pipe(p) != 0)
        return fail("pipe");
    pid_t child = fork();
    if (child < 0)
        return fail("fork");
    if (child == 0) {
        struct timespec d = { 0, 150 * 1000000 };
        nanosleep(&d, 0);
        write(p[1], "y", 1);
        _exit(0);
    }
    pf.fd = p[0];
    pf.revents = 0;
    t0 = now_ms();
    if (poll(&pf, 1, 5000) != 1 || pf.revents != POLLIN)
        return fail("poll waking on a write");
    long long woke = now_ms() - t0;
    int status;
    waitpid(child, &status, 0);

    if (pipe(p) != 0)
        return fail("pipe");
    fd_set r, w;
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timeval to = { 0, 100000 };
    t0 = now_ms();
    if (select(p[0] + 1, &r, 0, 0, &to) != 0 || FD_ISSET(p[0], &r))
        return fail("select on an empty pipe");
    long long sel = now_ms() - t0;
    if (sel < 100 || sel > 2000)
        return fail("select timing out after 100 ms");
    if (write(p[1], "z", 1) != 1)
        return fail("write");
    int top = p[0] > p[1] ? p[0] : p[1];
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    FD_ZERO(&w);
    FD_SET(p[1], &w);
    if (select(top + 1, &r, &w, 0, 0) != 2 || !FD_ISSET(p[0], &r) || !FD_ISSET(p[1], &w))
        return fail("select seeing data and room");
    close(p[1]);
    FD_ZERO(&w);
    FD_SET(p[1], &w);
    struct timeval zero = { 0, 0 };
    if (select(top + 1, 0, &w, 0, &zero) != -1 || errno != EBADF)
        return fail("select on a closed descriptor");
    struct timespec none = { 0, 0 };
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    if (pselect(p[0] + 1, &r, 0, 0, &none, 0) != 1 || !FD_ISSET(p[0], &r))
        return fail("pselect");
    close(p[0]);

    /* SA_RESTART, which pause must not honour. */
    struct sigaction sa = { 0 };
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGALRM, &sa, 0) != 0)
        return fail("sigaction");
    if (alarm(5) != 0)
        return fail("alarm");
    if (alarm(0) != 5)
        return fail("alarm giving the time left");
    struct itimerval it = { { 0, 50000 }, { 0, 100000 } }, cur;
    t0 = now_ms();
    if (setitimer(ITIMER_REAL, &it, 0) != 0)
        return fail("setitimer");
    if (getitimer(ITIMER_REAL, &cur) != 0 || cur.it_interval.tv_usec != 50000 || cur.it_value.tv_sec != 0
        || cur.it_value.tv_usec == 0 || cur.it_value.tv_usec > 100000)
        return fail("getitimer");
    while (rang < 3)
        if (pause() != -1 || errno != EINTR)
            return fail("pause");
    long long ticked = now_ms() - t0;
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    if (setitimer(ITIMER_REAL, &off, &cur) != 0 || cur.it_interval.tv_usec != 50000)
        return fail("stopping the timer");
    if (ticked < 200 || ticked > 3000)
        return fail("a 50 ms interval timer");
    child = fork();
    if (child < 0)
        return fail("fork");
    if (child == 0) {
        signal(SIGALRM, SIG_DFL);
        struct itimerval once = { { 0, 0 }, { 0, 50000 } };
        setitimer(ITIMER_REAL, &once, 0);
        pause();
        _exit(0);
    }
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGALRM)
        return fail("SIGALRM ending a process");

    const char *bad = events();
    if (bad)
        return fail(bad);
    printf("mtime: rang %d, selected, slept %lld, timed out %lld, woke %s, events ok\n", rang >= 3 ? 3 : rang, slept / 100 * 100, waited / 100 * 100,
           woke >= 100 && woke < 5000 ? "in time" : "late");
    return 0;
}
