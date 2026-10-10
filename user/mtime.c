/* Time and waiting through the Linux system calls, against musl: the
 * monotonic clock, nanosleep, poll timing out on an empty pipe, poll seeing
 * data and the end of a pipe, poll waking when a child writes, and select
 * and pselect over pipes, and alarm, setitimer and pause. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/select.h>
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

    printf("mtime: rang %d, selected, slept %lld, timed out %lld, woke %s\n", rang >= 3 ? 3 : rang, slept / 100 * 100, waited / 100 * 100,
           woke >= 100 && woke < 5000 ? "in time" : "late");
    return 0;
}
