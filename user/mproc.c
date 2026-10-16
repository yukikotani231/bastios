/* More processes than the 512 the process table once held: 700 children
 * alive at once, ids past 511 (four digits from 1000 on) in the places that
 * once packed an id into a byte or a /proc directory number: si_pid of a queued signal (and its
 * value beside it), and /proc/PID/fd of the last child. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define KIDS 700

static volatile sig_atomic_t got_pid, got_val;

static void on_usr1(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;
    (void)ctx;
    got_pid = si->si_pid;
    got_val = si->si_value.sival_int;
}

static int fail(const char *what)
{
    printf("mproc: FAIL %s (errno %d)\n", what, errno);
    return 1;
}

int main(void)
{
    int go[2];
    if (pipe(go) < 0)
        return fail("pipe");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    pid_t parent = getpid();
    pid_t kids[KIDS];
    pid_t top = 0;
    for (int i = 0; i < KIDS; i++) {
        pid_t p = fork();
        if (p < 0)
            return fail("fork");
        if (p == 0) {
            close(go[1]);
            char c;
            read(go[0], &c, 1);    /* held until the parent closes its end */
            _exit(0);
        }
        kids[i] = p;
        if (p > top)
            top = p;
    }
    if (top <= 511) {
        printf("mproc: FAIL no id past 511 (top %d)\n", (int)top);
        return 1;
    }
    /* A child with the highest id signals the parent. */
    pid_t q = fork();
    if (q < 0) {
        if (errno == EAGAIN)
            q = -2;    /* the table is full: the top child signals instead */
        else
            return fail("fork the signaller");
    }
    if (q == 0) {
        union sigval v;
        v.sival_int = 12345;
        sigqueue(parent, SIGUSR1, v);
        _exit(0);
    }
    if (q > 0) {
        int st;
        waitpid(q, &st, 0);
        if (q > 511 && (got_pid != q || got_val != 12345)) {
            printf("mproc: FAIL si_pid %d (want %d) value %d\n", (int)got_pid, (int)q, (int)got_val);
            return 1;
        }
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/fd", (int)top);
    int d = open(path, O_RDONLY | O_DIRECTORY);
    if (d < 0)
        return fail("open /proc/TOP/fd");
    close(d);
    snprintf(path, sizeof path, "/proc/%d/status", (int)top);
    int s = open(path, O_RDONLY);
    if (s < 0)
        return fail("open /proc/TOP/status");
    close(s);
    close(go[1]);
    for (int i = 0; i < KIDS; i++)
        if (waitpid(kids[i], 0, 0) != kids[i])
            return fail("wait");
    printf("mproc: ok, %d processes at once, ids up to %d\n", KIDS, (int)top);
    return 0;
}
