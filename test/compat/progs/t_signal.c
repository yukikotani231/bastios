/* Signals: handlers, masks, raise and kill, alarms and timers, waiting. */
#include "compat.h"
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

static volatile sig_atomic_t got, info_ok;
static void h(int s) { got = s; }
static void hi(int s, siginfo_t *si, void *uc) { (void)uc; info_ok = s == SIGUSR2 && si->si_signo == SIGUSR2 && si->si_pid == getpid(); }

int main(void) {
    setup();
    struct sigaction sa = {0};
    sigset_t m, old;
    int st;
    pid_t p;

    sa.sa_handler = h;
    EXPECT("sigaction", sigaction(SIGUSR1, &sa, 0) == 0);
    EXPECT("raise runs the handler", (got = 0, raise(SIGUSR1) == 0 && got == SIGUSR1));
    EXPECT("kill self", (got = 0, kill(getpid(), SIGUSR1) == 0 && got == SIGUSR1));
    sa.sa_sigaction = hi; sa.sa_flags = SA_SIGINFO;
    EXPECT("SA_SIGINFO", sigaction(SIGUSR2, &sa, 0) == 0 && kill(getpid(), SIGUSR2) == 0 && info_ok);
    sigemptyset(&m); sigaddset(&m, SIGUSR1);
    EXPECT("sigprocmask blocks", (got = 0, sigprocmask(SIG_BLOCK, &m, &old) == 0 && raise(SIGUSR1) == 0 && got == 0));
    {
        sigset_t pend;
        EXPECT("sigpending", sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1));
    }
    EXPECT("unblock delivers", sigprocmask(SIG_SETMASK, &old, 0) == 0 && got == SIGUSR1);
    EXPECT("SIG_IGN", signal(SIGUSR1, SIG_IGN) != SIG_ERR && raise(SIGUSR1) == 0);
    signal(SIGUSR1, h);
    EXPECT("signal to a child", (p = fork()) >= 0 && (p == 0 ? (pause(), _exit(0), 0) :
           (usleep(100000), kill(p, SIGTERM) == 0) && waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM));
    EXPECT("SIGCHLD on child exit", ({ signal(SIGCHLD, h); got = 0; (p = fork()) == 0 ? (_exit(0), 0) :
           p > 0 && waitpid(p, &st, 0) == p && (got == SIGCHLD || (usleep(10000), got == SIGCHLD)); }));
    signal(SIGCHLD, SIG_DFL);
    signal(SIGALRM, h);
    EXPECT("alarm", (got = 0, alarm(1) == 0 && (pause(), got == SIGALRM)));
    {
        struct itimerval it = {{0, 0}, {0, 50000}};
        EXPECT("setitimer", (got = 0, setitimer(ITIMER_REAL, &it, 0) == 0 && (pause(), got == SIGALRM)));
        EXPECT("getitimer", getitimer(ITIMER_REAL, &it) == 0);
    }
    {
        sigset_t w; int sig = 0;
        sigemptyset(&w); sigaddset(&w, SIGUSR1);
        sigprocmask(SIG_BLOCK, &w, &old);
        EXPECT("sigwait", raise(SIGUSR1) == 0 && sigwait(&w, &sig) == 0 && sig == SIGUSR1);
        sigprocmask(SIG_SETMASK, &old, 0);
    }
    {
        sigset_t w;
        sigemptyset(&w);
        if (begin("sigsuspend")) {
            got = 0;
            p = fork();
            if (p == 0) { usleep(100000); kill(getppid(), SIGUSR1); _exit(0); }
            int r = p > 0 ? sigsuspend(&w) : 0, e = errno;
            /* Always reap the child, so it cannot signal anyone later. */
            int reaped = p > 0 && waitpid(p, &st, 0) == p;
            if (r == -1 && e == EINTR && got == SIGUSR1 && reaped) pass();
            else fail("sigsuspend returned %d (errno %d %s), handler %s", r, e, strerror(e), got ? "ran" : "did not run");
        }
    }
    {
        static char stk[16384];
        stack_t ss = {.ss_sp = stk, .ss_size = sizeof stk};
        EXPECT("sigaltstack", sigaltstack(&ss, 0) == 0);
    }
    {
        timer_t t;
        struct sigevent ev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGALRM};
        struct itimerspec its = {{0, 0}, {0, 50000000}};
        EXPECT("timer_create", (got = 0, timer_create(CLOCK_MONOTONIC, &ev, &t) == 0 && timer_settime(t, 0, &its, 0) == 0 &&
               (pause(), got == SIGALRM)));
    }
    /* In a child that leads a process group of its own, so the signal
     * reaches nobody else (without setpgid it would reach the shell). */
    EXPECT("kill(0) to own group", (p = fork()) >= 0 && (p == 0 ? (setpgid(0, 0) != 0 || getpgrp() != getpid() ? _exit(2) :
           (signal(SIGUSR1, h), got = 0, _exit(kill(0, SIGUSR1) == 0 && got == SIGUSR1 ? 0 : 1)), 0) : waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0));
    EXPECT("kill missing pid is ESRCH", kill(30000, 0) == -1 && errno == ESRCH);
    return done();
}
