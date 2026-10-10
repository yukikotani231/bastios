/* Time and waiting through the Linux system calls, against musl: the
 * monotonic clock, nanosleep, poll timing out on an empty pipe, poll seeing
 * data and the end of a pipe, and poll waking when a child writes. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("mtime: %s failed (errno %d)\n", what, errno);
    return 1;
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
    printf("mtime: slept %lld, timed out %lld, woke %s\n", slept / 100 * 100, waited / 100 * 100,
           woke >= 100 && woke < 5000 ? "in time" : "late");
    return 0;
}
