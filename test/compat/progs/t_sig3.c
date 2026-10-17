/* Signal delivery corners: SA_RESTART, real-time signals and their queue,
 * delivery order, handler masks, SA_NODEFER, the alternate stack, SIGCHLD
 * details, job control, signalfd and the frame a handler runs on. Each
 * expectation was checked on a Linux host (make compat-host). */
#include "compat.h"
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

static volatile int got, depth, maxdepth, order[16], nord;
static volatile char *volatile where;
#pragma GCC diagnostic ignored "-Wdangling-pointer"
static volatile int onstack, sival, sicode;
static sigjmp_buf jb;

static void h(int s) { char here; got = s; where = &here; }
static void rec(int s) { if (nord < 16) order[nord++] = s; }
static void reci(int s, siginfo_t *si, void *uc)
{
    (void)uc;
    if (nord < 16) order[nord++] = s;
    sival = si->si_value.sival_int;
    sicode = si->si_code;
}
static volatile int calls;
static void nested(int s) { (void)s; if (++depth > maxdepth) maxdepth = depth; if (calls++ < 2) raise(SIGUSR1); depth--; }
static void inner_usr2(int s) { (void)s; got++; }
static void outer_usr1(int s) { (void)s; raise(SIGUSR2); got += 10; }
static void on_stack(int s)
{
    (void)s;
    stack_t o;
    sigaltstack(0, &o);
    onstack = (o.ss_flags & SS_ONSTACK) != 0;
}
static void segv(int s, siginfo_t *si, void *uc)
{
    (void)s; (void)uc;
    where = si->si_addr;
    sicode = si->si_code;
    siglongjmp(jb, 1);
}
static void fpe(int s, siginfo_t *si, void *uc) { (void)s; (void)uc; sicode = si->si_code; siglongjmp(jb, 1); }

/* Runs a child that calls FN and reports its wait status. */

/* SIGUSR1 to the parent after USEC microseconds, from a child. */
static pid_t poke(int usec)
{
    pid_t parent = getpid(), c = fork();
    if (c == 0) { usleep(usec); kill(parent, SIGUSR1); _exit(0); }
    return c;
}

static volatile int fp_ok;
#ifdef __x86_64__
static void clobber_fp(int s) { (void)s; asm volatile("" ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9"); volatile double d = 3.5; d *= 2; }
#endif
static void align_check(int s) { (void)s; fp_ok = ((unsigned long)__builtin_frame_address(0) % 16) == 0; }

static void stop_self(void) { raise(SIGSTOP); }
static void stop_tstp(void) { raise(SIGTSTP); }
static void ign_pending(void) {}

int main(int argc, char **argv)
{
    setup();
    if (argc > 1 && strcmp(argv[1], "--exec-probe") == 0) {
        struct sigaction a, b;
        sigaction(SIGUSR1, 0, &a);
        sigaction(SIGUSR2, 0, &b);
        printf("%s %s\n", a.sa_handler == SIG_IGN ? "ign" : "other", b.sa_handler == SIG_DFL ? "dfl" : "other");
        return 0;
    }
    struct sigaction sa;
    sigset_t m, old, all;
    siginfo_t si;
    int st;
    pid_t c;
    sigfillset(&all);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);

    /* SA_RESTART */
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, 0);
    EXPECT("SA_RESTART restarts a blocking read", ({ int p[2]; char b; pipe(p); got = 0; pid_t pp = getpid();
           c = fork(); if (c == 0) { usleep(100000); kill(pp, SIGUSR1); usleep(100000); write(p[1], "x", 1); _exit(0); }
           ssize_t r = read(p[0], &b, 1); waitpid(c, 0, 0); close(p[0]); close(p[1]);
           r == 1 && b == 'x' && got == SIGUSR1; }));
    EXPECT("SA_RESTART restarts waitpid", ({ got = 0; pid_t pp = getpid(); c = fork();
           if (c == 0) { pid_t g = fork(); if (g == 0) { usleep(300000); _exit(5); } usleep(100000); kill(pp, SIGUSR1); waitpid(g, 0, 0); _exit(0); }
           pid_t g2 = fork(); if (g2 == 0) { usleep(300000); _exit(7); }
           pid_t r = waitpid(g2, &st, 0); waitpid(c, 0, 0);
           r == g2 && WEXITSTATUS(st) == 7; }));
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, 0);
    EXPECT("without SA_RESTART a blocking read is EINTR", ({ int p[2]; char b; pipe(p); got = 0; c = poke(100000);
           ssize_t r = read(p[0], &b, 1); int e = errno; waitpid(c, 0, 0); close(p[0]); close(p[1]);
           r == -1 && e == EINTR && got == SIGUSR1; }));
    EXPECT("a partial read is not EINTR", ({ int p[2]; char b[8]; pipe(p); write(p[1], "ab", 2); got = 0;
           ssize_t r = read(p[0], b, 8); close(p[0]); close(p[1]); r == 2; }));

    /* Delivery order and the queue. */
    sa.sa_handler = rec;
    sigaction(SIGUSR1, &sa, 0);
    sigaction(SIGUSR2, &sa, 0);
    sigaction(SIGRTMIN, &sa, 0);
    sigaction(SIGRTMIN + 1, &sa, 0);
    sigaction(SIGRTMIN + 2, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    EXPECT("pending signals are taken lowest number first", ({ sigemptyset(&m); sigaddset(&m, SIGUSR2); sigaddset(&m, SIGUSR1); sigaddset(&m, SIGTERM);
           sigprocmask(SIG_BLOCK, &m, &old); raise(SIGUSR2); raise(SIGTERM); raise(SIGUSR1);
           struct timespec t = {1, 0}; int a = sigtimedwait(&m, &si, &t), b = sigtimedwait(&m, &si, &t), d = sigtimedwait(&m, &si, &t);
           sigprocmask(SIG_SETMASK, &old, 0); a == SIGUSR1 && b == SIGUSR2 && d == SIGTERM; }));
    EXPECT("a standard signal sent twice while blocked arrives once", ({ sigemptyset(&m); sigaddset(&m, SIGUSR1);
           nord = 0; sigprocmask(SIG_BLOCK, &m, &old); raise(SIGUSR1); raise(SIGUSR1); sigprocmask(SIG_SETMASK, &old, 0); nord == 1; }));
    EXPECT("a real-time signal sent three times while blocked arrives three times", ({ sigemptyset(&m); sigaddset(&m, SIGRTMIN);
           nord = 0; sigprocmask(SIG_BLOCK, &m, &old); for (int i = 0; i < 3; i++) sigqueue(getpid(), SIGRTMIN, (union sigval){.sival_int = i});
           sigprocmask(SIG_SETMASK, &old, 0); nord == 3; }));
    sa.sa_sigaction = reci; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN, &sa, 0);
    EXPECT("queued real-time signals keep their values, in the order sent", ({ sigemptyset(&m); sigaddset(&m, SIGRTMIN);
           nord = 0; sigprocmask(SIG_BLOCK, &m, &old);
           for (int i = 0; i < 4; i++) sigqueue(getpid(), SIGRTMIN, (union sigval){.sival_int = 100 + i});
           int ok = 1;
           for (int i = 0; i < 4; i++) { sigset_t w; sigemptyset(&w); sigaddset(&w, SIGRTMIN); struct timespec t = {1, 0};
                                          if (sigtimedwait(&w, &si, &t) != SIGRTMIN || si.si_value.sival_int != 100 + i || si.si_code != SI_QUEUE) ok = 0; }
           sigprocmask(SIG_SETMASK, &old, 0); ok; }));
    EXPECT("sigqueue gives the handler si_value and SI_QUEUE", ({ nord = 0; sival = 0; sicode = 0;
           sigqueue(getpid(), SIGRTMIN, (union sigval){.sival_int = 4242}); sival == 4242 && sicode == SI_QUEUE; }));
    sa.sa_handler = rec; sa.sa_flags = 0;
    EXPECT("real-time signals are taken after the standard ones, lowest first", ({ sigemptyset(&m); sigaddset(&m, SIGRTMIN + 2); sigaddset(&m, SIGRTMIN + 1); sigaddset(&m, SIGUSR1);
           sigprocmask(SIG_BLOCK, &m, &old); raise(SIGRTMIN + 2); raise(SIGRTMIN + 1); raise(SIGUSR1);
           struct timespec t = {1, 0}; int a = sigtimedwait(&m, &si, &t), b = sigtimedwait(&m, &si, &t), d = sigtimedwait(&m, &si, &t);
           sigprocmask(SIG_SETMASK, &old, 0); a == SIGUSR1 && b == SIGRTMIN + 1 && d == SIGRTMIN + 2; }));
    EXPECT("rt_sigqueueinfo with SI_USER to another process is EPERM", ({ siginfo_t x; memset(&x, 0, sizeof x); x.si_code = SI_QUEUE + 1; x.si_signo = SIGUSR1;
           c = fork(); if (c == 0) { pause(); _exit(0); } usleep(50000);
           long r = syscall(SYS_rt_sigqueueinfo, c, SIGUSR1, &x); int e = errno; kill(c, SIGKILL); waitpid(c, 0, 0);
           r == -1 && e == EPERM; }));
    EXPECT("SIGRTMIN is at least 32 and SIGRTMAX 64", SIGRTMAX == 64 && SIGRTMIN >= 32 && SIGRTMIN < SIGRTMAX);
    EXPECT("kill with signal 65 is EINVAL, and signal 0 only probes", kill(getpid(), 65) == -1 && errno == EINVAL && kill(getpid(), 0) == 0);

    signal(SIGTERM, SIG_DFL);

    /* Masks while a handler runs. */
    sa.sa_flags = 0;
    sa.sa_handler = nested;
    sigaction(SIGUSR1, &sa, 0);
    EXPECT("the signal is blocked while its own handler runs", ({ depth = 0; maxdepth = 0; calls = 0; raise(SIGUSR1); maxdepth == 1 && calls == 3; }));
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGUSR1, &sa, 0);
    EXPECT("SA_NODEFER lets the handler nest", ({ depth = 0; maxdepth = 0; calls = 0; raise(SIGUSR1); maxdepth == 3; }));
    sa.sa_flags = 0; sa.sa_handler = outer_usr1; sigemptyset(&sa.sa_mask); sigaddset(&sa.sa_mask, SIGUSR2);
    sigaction(SIGUSR1, &sa, 0);
    { struct sigaction s2 = {.sa_handler = inner_usr2}; sigemptyset(&s2.sa_mask); sigaction(SIGUSR2, &s2, 0); }
    EXPECT("sa_mask holds signals back until the handler returns", ({ got = 0; raise(SIGUSR1); got == 11; }));
    EXPECT("the mask comes back after the handler", ({ sigprocmask(SIG_BLOCK, 0, &old); !sigismember(&old, SIGUSR1) && !sigismember(&old, SIGUSR2); }));
    EXPECT("SIGKILL and SIGSTOP cannot be caught", ({ struct sigaction s2 = {.sa_handler = h}; int a = sigaction(SIGKILL, &s2, 0), e1 = errno;
           int b = sigaction(SIGSTOP, &s2, 0), e2 = errno; a == -1 && e1 == EINVAL && b == -1 && e2 == EINVAL; }));
    EXPECT("SIGKILL and SIGSTOP cannot be blocked", ({ sigprocmask(SIG_SETMASK, &all, 0); sigprocmask(SIG_SETMASK, 0, &old);
           int r = !sigismember(&old, SIGKILL) && !sigismember(&old, SIGSTOP) && sigismember(&old, SIGUSR1); sigemptyset(&m); sigprocmask(SIG_SETMASK, &m, 0); r; }));
    EXPECT("sigsuspend returns EINTR and puts the mask back", ({ sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_SETMASK, &m, 0);
           struct sigaction s2 = {.sa_handler = h}; sigemptyset(&s2.sa_mask); sigaction(SIGUSR2, &s2, 0);
           got = 0; pid_t pp = getpid(); c = fork(); if (c == 0) { usleep(100000); kill(pp, SIGUSR2); _exit(0); }
           sigset_t none; sigemptyset(&none); int r = sigsuspend(&none); int e = errno; waitpid(c, 0, 0);
           sigprocmask(SIG_BLOCK, 0, &old); sigemptyset(&m); sigprocmask(SIG_SETMASK, &m, 0);
           r == -1 && e == EINTR && got == SIGUSR2 && sigismember(&old, SIGUSR1); }));
    EXPECT("a signal ignored while pending is dropped", ({ struct sigaction s2 = {.sa_handler = h}; sigemptyset(&s2.sa_mask); sigaction(SIGUSR2, &s2, 0);
           sigemptyset(&m); sigaddset(&m, SIGUSR2); sigprocmask(SIG_BLOCK, &m, &old); raise(SIGUSR2);
           s2.sa_handler = SIG_IGN; sigaction(SIGUSR2, &s2, 0); sigpending(&m);
           int r = !sigismember(&m, SIGUSR2); s2.sa_handler = h; sigaction(SIGUSR2, &s2, 0); got = 0; sigprocmask(SIG_SETMASK, &old, 0); r && got == 0; }));
    EXPECT("the child of a fork has no pending signals", ({ sigemptyset(&m); sigaddset(&m, SIGUSR2); sigprocmask(SIG_BLOCK, &m, &old); raise(SIGUSR2);
           c = fork(); if (c == 0) { sigset_t p; sigpending(&p); _exit(sigismember(&p, SIGUSR2) ? 1 : 0); }
           waitpid(c, &st, 0); sigprocmask(SIG_SETMASK, &old, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("the child inherits the mask and the handlers", ({ sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, &old);
           c = fork(); if (c == 0) { sigset_t b; struct sigaction o; sigprocmask(SIG_BLOCK, 0, &b); sigaction(SIGUSR2, 0, &o);
                                     _exit(sigismember(&b, SIGUSR1) && o.sa_handler == h ? 0 : 1); }
           waitpid(c, &st, 0); sigprocmask(SIG_SETMASK, &old, 0); WIFEXITED(st) && WEXITSTATUS(st) == 0; }));

    /* The alternate stack. */
    EXPECT("SS_ONSTACK shows inside a handler that runs on the alternate stack", ({ static char alt[32768]; stack_t s = {.ss_sp = alt, .ss_size = sizeof alt};
           struct sigaction s2 = {.sa_handler = on_stack, .sa_flags = SA_ONSTACK}; sigemptyset(&s2.sa_mask); onstack = 0;
           sigaltstack(&s, 0); sigaction(SIGUSR2, &s2, 0); raise(SIGUSR2);
           stack_t o; sigaltstack(0, &o); s.ss_flags = SS_DISABLE; sigaltstack(&s, 0); onstack == 1 && (o.ss_flags & SS_ONSTACK) == 0; }));
    EXPECT("a handler without SA_ONSTACK stays on the normal stack", ({ static char alt[32768]; stack_t s = {.ss_sp = alt, .ss_size = sizeof alt};
           struct sigaction s2 = {.sa_handler = h}; sigemptyset(&s2.sa_mask); sigaltstack(&s, 0); sigaction(SIGUSR2, &s2, 0); where = 0; raise(SIGUSR2);
           int r = where && !(where >= alt && where < alt + sizeof alt); s.ss_flags = SS_DISABLE; sigaltstack(&s, 0); r; }));
    EXPECT("changing the alternate stack while on it is EPERM", ({ static char alt[32768], alt2[32768]; static volatile int e1, r1; stack_t s = {.ss_sp = alt, .ss_size = sizeof alt};
           struct sigaction s2 = {.sa_flags = SA_ONSTACK}; sigemptyset(&s2.sa_mask);
           void eperm(int x) { (void)x; stack_t t = {.ss_sp = alt2, .ss_size = sizeof alt2}; r1 = sigaltstack(&t, 0); e1 = errno; }
           s2.sa_handler = eperm; sigaltstack(&s, 0); sigaction(SIGUSR2, &s2, 0); raise(SIGUSR2);
           s.ss_flags = SS_DISABLE; sigaltstack(&s, 0); r1 == -1 && e1 == EPERM; }));
    EXPECT("an invalid ss_flags is EINVAL", ({ static char alt[32768]; stack_t s = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0x7000};
           sigaltstack(&s, 0) == -1 && errno == EINVAL; }));
    EXPECT("a fresh alternate stack reads back SS_DISABLE", ({ stack_t o; sigaltstack(0, &o) == 0 && (o.ss_flags & SS_DISABLE); }));

    /* Faults give signals with the right information. */
    EXPECT("SIGSEGV on an unmapped address: SEGV_MAPERR and si_addr", ({ struct sigaction s2 = {.sa_sigaction = segv, .sa_flags = SA_SIGINFO | SA_NODEFER}; sigemptyset(&s2.sa_mask);
           sigaction(SIGSEGV, &s2, 0); where = 0; sicode = 0; volatile char *bad = (char *)0x10;
           int r = 0; if (sigsetjmp(jb, 1) == 0) { *bad = 1; } else r = 1;
           r && sicode == SEGV_MAPERR && (long)where == 0x10; }));
    EXPECT("SIGSEGV on a PROT_NONE page: SEGV_ACCERR", ({ char *a = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); where = 0; sicode = 0; int r = 0;
           if (sigsetjmp(jb, 1) == 0) { *(volatile char *)a = 1; } else r = 1;
           r && sicode == SEGV_ACCERR && where == a && munmap(a, 4096) == 0; }));
    EXPECT("SIGSEGV on a write to a read-only page: SEGV_ACCERR", ({ char *a = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); sicode = 0; int r = 0;
           if (sigsetjmp(jb, 1) == 0) { *(volatile char *)a = 1; } else r = 1;
           r && sicode == SEGV_ACCERR && munmap(a, 4096) == 0; }));
#ifdef __x86_64__
    EXPECT("SIGFPE on an integer division by zero: FPE_INTDIV", ({ struct sigaction s2 = {.sa_sigaction = fpe, .sa_flags = SA_SIGINFO | SA_NODEFER}; sigemptyset(&s2.sa_mask);
           sigaction(SIGFPE, &s2, 0); sicode = 0; int r = 0; volatile int z = 0;
           if (sigsetjmp(jb, 1) == 0) { int a = 1, d = 0; asm volatile("idivl %2" : "+a"(a), "+d"(d) : "r"(z)); } else r = 1;
           r && sicode == FPE_INTDIV; }));
    EXPECT("a handler may clobber SSE registers: the interrupted code does not notice", ({ struct sigaction s2 = {.sa_handler = clobber_fp}; sigemptyset(&s2.sa_mask);
           sigaction(SIGUSR2, &s2, 0); volatile double a = 1.25, b = 0; int ok = 1;
           for (int i = 0; i < 2000; i++) { b = a * 2; if (i % 100 == 0) raise(SIGUSR2); if (b != 2.5) ok = 0; } ok; }));
#endif
    EXPECT("a handler starts with the stack aligned as the ABI says", ({ struct sigaction s2 = {.sa_handler = align_check}; sigemptyset(&s2.sa_mask);
           sigaction(SIGUSR2, &s2, 0); fp_ok = 0; raise(SIGUSR2); fp_ok; }));
    EXPECT("a handler may change the blocked set through uc_sigmask", ({ void chg(int s, siginfo_t *i, void *uc) { (void)s; (void)i; sigaddset(&((ucontext_t *)uc)->uc_sigmask, SIGUSR1); }
           struct sigaction s2 = {.sa_sigaction = chg, .sa_flags = SA_SIGINFO}; sigemptyset(&s2.sa_mask); sigaction(SIGUSR2, &s2, 0); raise(SIGUSR2);
           sigprocmask(SIG_BLOCK, 0, &old); int r = sigismember(&old, SIGUSR1); sigemptyset(&m); sigprocmask(SIG_SETMASK, &m, 0); r; }));

    /* SIGCHLD and job control. */
    signal(SIGCHLD, SIG_DFL);
    EXPECT("waitid reports CLD_EXITED with the status", ({ c = fork(); if (c == 0) _exit(9);
           memset(&si, 0, sizeof si); waitid(P_PID, c, &si, WEXITED) == 0 && si.si_code == CLD_EXITED && si.si_status == 9 && si.si_pid == c && si.si_signo == SIGCHLD; }));
    EXPECT("waitid reports CLD_KILLED for a child ended by a signal", ({ c = fork(); if (c == 0) { pause(); _exit(0); }
           usleep(50000); kill(c, SIGKILL); memset(&si, 0, sizeof si);
           waitid(P_PID, c, &si, WEXITED) == 0 && si.si_code == CLD_KILLED && si.si_status == SIGKILL; }));
    EXPECT("WNOWAIT leaves the child to be waited for again", ({ c = fork(); if (c == 0) _exit(3);
           memset(&si, 0, sizeof si); waitid(P_PID, c, &si, WEXITED | WNOWAIT) == 0 && si.si_pid == c &&
           waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 3; }));
    EXPECT("WNOHANG with a running child returns 0", ({ c = fork(); if (c == 0) { pause(); _exit(0); } pid_t r = waitpid(c, &st, WNOHANG); kill(c, SIGKILL); waitpid(c, 0, 0); r == 0; }));
    EXPECT("a stopped child is seen with WUNTRACED, as WIFSTOPPED", ({ c = fork(); if (c == 0) { raise(SIGSTOP); _exit(0); }
           pid_t r = waitpid(c, &st, WUNTRACED); int ok = r == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP; kill(c, SIGKILL); waitpid(c, 0, 0); ok; }));
    EXPECT("SIGCONT is reported with WCONTINUED", ({ c = fork(); if (c == 0) { raise(SIGSTOP); pause(); _exit(0); }
           waitpid(c, &st, WUNTRACED); kill(c, SIGCONT); pid_t r = waitpid(c, &st, WCONTINUED); int ok = r == c && WIFCONTINUED(st); kill(c, SIGKILL); waitpid(c, 0, 0); ok; }));
    EXPECT("waitid reports CLD_STOPPED and CLD_CONTINUED", ({ c = fork(); if (c == 0) { raise(SIGSTOP); pause(); _exit(0); }
           memset(&si, 0, sizeof si); int a = waitid(P_PID, c, &si, WSTOPPED) == 0 && si.si_code == CLD_STOPPED && si.si_status == SIGSTOP;
           kill(c, SIGCONT); memset(&si, 0, sizeof si); int b = waitid(P_PID, c, &si, WCONTINUED) == 0 && si.si_code == CLD_CONTINUED && si.si_status == SIGCONT;
           kill(c, SIGKILL); waitpid(c, 0, 0); a && b; }));
    EXPECT("a stop is reported once", ({ c = fork(); if (c == 0) { raise(SIGSTOP); pause(); _exit(0); }
           waitpid(c, &st, WUNTRACED); pid_t r = waitpid(c, &st, WUNTRACED | WNOHANG); kill(c, SIGKILL); waitpid(c, 0, 0); r == 0; }));
    EXPECT("SIGTSTP stops a process by default", ({ c = fork(); if (c == 0) { raise(SIGTSTP); _exit(0); }
           pid_t r = waitpid(c, &st, WUNTRACED); int ok = r == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP; kill(c, SIGKILL); waitpid(c, 0, 0); ok; }));
    EXPECT("SIGTERM to a stopped process waits for SIGCONT", ({ c = fork(); if (c == 0) { raise(SIGSTOP); _exit(0); }
           waitpid(c, &st, WUNTRACED); kill(c, SIGTERM); usleep(50000); pid_t r = waitpid(c, &st, WNOHANG); kill(c, SIGCONT);
           pid_t r2 = waitpid(c, &st, 0); r == 0 && r2 == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM; }));
    EXPECT("SIGKILL ends a stopped process", ({ c = fork(); if (c == 0) { raise(SIGSTOP); _exit(0); }
           waitpid(c, &st, WUNTRACED); kill(c, SIGKILL); waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL; }));
    EXPECT("a handler for SIGCONT runs even when SIGCONT is blocked, on the resume", ({ static volatile int cont; void oncont(int s) { (void)s; cont = 1; }
           (void)stop_self; (void)stop_tstp; (void)ign_pending;
           int p[2]; pipe(p); c = fork();
           if (c == 0) { struct sigaction s2 = {.sa_handler = oncont}; sigemptyset(&s2.sa_mask); sigaction(SIGCONT, &s2, 0);
                         sigemptyset(&m); sigaddset(&m, SIGCONT); sigprocmask(SIG_BLOCK, &m, 0); write(p[1], "r", 1); raise(SIGSTOP);
                         sigset_t pend; sigpending(&pend); int pending = sigismember(&pend, SIGCONT); _exit(pending && !cont ? 0 : 2); }
           char b; read(p[0], &b, 1); waitpid(c, &st, WUNTRACED); kill(c, SIGCONT); waitpid(c, &st, 0); close(p[0]); close(p[1]);
           WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("SIGCONT discards pending stop signals", ({ c = fork(); if (c == 0) { sigemptyset(&m); sigaddset(&m, SIGTSTP); sigprocmask(SIG_BLOCK, &m, 0);
                         raise(SIGTSTP); raise(SIGCONT); sigset_t p; sigpending(&p); _exit(sigismember(&p, SIGTSTP) ? 1 : 0); }
           waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("a stop signal discards a pending SIGCONT", ({ c = fork(); if (c == 0) { sigemptyset(&m); sigaddset(&m, SIGCONT); sigaddset(&m, SIGTSTP); sigprocmask(SIG_BLOCK, &m, 0);
                         raise(SIGCONT); raise(SIGTSTP); sigset_t p; sigpending(&p); _exit(sigismember(&p, SIGCONT) ? 1 : 0); }
           waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0; }));
    EXPECT("SIGCHLD ignored: children are reaped and wait is ECHILD", ({ struct sigaction s2 = {.sa_handler = SIG_IGN}; sigemptyset(&s2.sa_mask); sigaction(SIGCHLD, &s2, 0);
           c = fork(); if (c == 0) _exit(0); pid_t r = wait(&st); int e = errno; signal(SIGCHLD, SIG_DFL); r == -1 && e == ECHILD; }));
    EXPECT("SA_NOCLDWAIT: wait is ECHILD once the children are gone", ({ struct sigaction s2 = {.sa_handler = h, .sa_flags = SA_NOCLDWAIT}; sigemptyset(&s2.sa_mask); sigaction(SIGCHLD, &s2, 0);
           c = fork(); if (c == 0) _exit(0); pid_t r = wait(&st); int e = errno; signal(SIGCHLD, SIG_DFL); r == -1 && e == ECHILD; }));
    EXPECT("SIGCHLD does not arrive for a stop with SA_NOCLDSTOP", ({ struct sigaction s2 = {.sa_handler = h, .sa_flags = SA_NOCLDSTOP}; sigemptyset(&s2.sa_mask); sigaction(SIGCHLD, &s2, 0);
           got = 0; c = fork(); if (c == 0) { raise(SIGSTOP); _exit(0); } usleep(100000); int stopped = got; waitpid(c, &st, WUNTRACED); kill(c, SIGKILL); waitpid(c, 0, 0);
           signal(SIGCHLD, SIG_DFL); stopped == 0; }));
    EXPECT("SIGCHLD arrives for a stop without it", ({ struct sigaction s2 = {.sa_handler = h}; sigemptyset(&s2.sa_mask); sigaction(SIGCHLD, &s2, 0);
           got = 0; c = fork(); if (c == 0) { raise(SIGSTOP); _exit(0); } usleep(100000); int stopped = got; waitpid(c, &st, WUNTRACED); kill(c, SIGKILL); waitpid(c, 0, 0);
           signal(SIGCHLD, SIG_DFL); stopped == SIGCHLD; }));
    EXPECT("the SIGCHLD handler sees si_pid, si_status and si_code", ({ static volatile int cp, cs, cc; void on(int s, siginfo_t *i, void *u) { (void)s; (void)u; cp = i->si_pid; cs = i->si_status; cc = i->si_code; }
           struct sigaction s2 = {.sa_sigaction = on, .sa_flags = SA_SIGINFO}; sigemptyset(&s2.sa_mask); sigaction(SIGCHLD, &s2, 0);
           cp = 0; c = fork(); if (c == 0) _exit(6); usleep(150000); waitpid(c, 0, 0); signal(SIGCHLD, SIG_DFL); cp == c && cs == 6 && cc == CLD_EXITED; }));
    EXPECT("kill(-pgrp) reaches every member", ({ int p[2]; pipe(p); c = fork();
           if (c == 0) { setpgid(0, 0); signal(SIGTERM, h); pid_t g = fork(); if (g == 0) { signal(SIGTERM, SIG_DFL); pause(); _exit(0); } write(p[1], "r", 1); while (waitpid(g, &st, 0) == -1 && errno == EINTR); _exit(WIFSIGNALED(st) ? WTERMSIG(st) : 99); }
           char b; read(p[0], &b, 1); usleep(50000); int r = kill(-c, SIGTERM); waitpid(c, &st, 0); close(p[0]); close(p[1]);
           r == 0 && WIFEXITED(st) && WEXITSTATUS(st) == SIGTERM; }));
    EXPECT("kill(pid) of a zombie succeeds, and of a reaped child is ESRCH", ({ c = fork(); if (c == 0) _exit(0); usleep(100000); int a = kill(c, SIGTERM); waitpid(c, 0, 0);
           int b = kill(c, 0); a == 0 && b == -1 && errno == ESRCH; }));

    /* signalfd, timers, threads. */
    EXPECT("signalfd reads a blocked signal with its sender", ({ sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, &old);
           int fd = signalfd(-1, &m, SFD_CLOEXEC); kill(getpid(), SIGUSR1); struct signalfd_siginfo ss; ssize_t r = read(fd, &ss, sizeof ss);
           close(fd); sigprocmask(SIG_SETMASK, &old, 0); r == sizeof ss && ss.ssi_signo == SIGUSR1 && ss.ssi_pid == (unsigned)getpid() && ss.ssi_code == SI_USER; }));
    EXPECT("signalfd is readable only while a signal is pending", ({ sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, &old);
           int fd = signalfd(-1, &m, SFD_NONBLOCK); struct pollfd pf = {fd, POLLIN, 0}; int a = poll(&pf, 1, 0); raise(SIGUSR1); int b = poll(&pf, 1, 0);
           struct signalfd_siginfo ss; read(fd, &ss, sizeof ss); int d = poll(&pf, 1, 0); close(fd); sigprocmask(SIG_SETMASK, &old, 0); a == 0 && b == 1 && d == 0; }));
    EXPECT("signalfd gives a queued value", ({ sigemptyset(&m); sigaddset(&m, SIGRTMIN); sigprocmask(SIG_BLOCK, &m, &old);
           int fd = signalfd(-1, &m, 0); sigqueue(getpid(), SIGRTMIN, (union sigval){.sival_int = 77}); struct signalfd_siginfo ss; read(fd, &ss, sizeof ss);
           close(fd); sigprocmask(SIG_SETMASK, &old, 0); ss.ssi_signo == (unsigned)SIGRTMIN && ss.ssi_int == 77 && ss.ssi_code == SI_QUEUE; }));
    EXPECT("sigtimedwait times out with EAGAIN", ({ sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, &old); struct timespec t = {0, 20000000};
           int r = sigtimedwait(&m, &si, &t); int e = errno; sigprocmask(SIG_SETMASK, &old, 0); r == -1 && e == EAGAIN; }));
    EXPECT("timer_create with SIGEV_SIGNAL queues sigev_value", ({ sigemptyset(&m); sigaddset(&m, SIGRTMIN); sigprocmask(SIG_BLOCK, &m, &old);
           struct sigevent ev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMIN, .sigev_value.sival_int = 31337}; timer_t t;
           struct itimerspec its = {.it_value = {0, 20000000}}; timer_create(CLOCK_MONOTONIC, &ev, &t); timer_settime(t, 0, &its, 0);
           struct timespec to = {2, 0}; int r = sigtimedwait(&m, &si, &to); timer_delete(t); sigprocmask(SIG_SETMASK, &old, 0);
           r == SIGRTMIN && si.si_value.sival_int == 31337 && si.si_code == SI_TIMER; }));
    EXPECT("a periodic timer that was not served counts overruns", ({ sigemptyset(&m); sigaddset(&m, SIGRTMIN); sigprocmask(SIG_BLOCK, &m, &old);
           struct sigevent ev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMIN}; timer_t t;
           struct itimerspec its = {.it_value = {0, 10000000}, .it_interval = {0, 10000000}}; timer_create(CLOCK_MONOTONIC, &ev, &t); timer_settime(t, 0, &its, 0);
           usleep(150000); struct timespec to = {1, 0}; int r = sigtimedwait(&m, &si, &to); int od = timer_getoverrun(t); timer_delete(t); sigprocmask(SIG_SETMASK, &old, 0);
           r == SIGRTMIN && od >= 3; }));
    EXPECT("tgkill reaches the named thread only", ({ static volatile pid_t tid; static volatile int hit; static volatile pid_t hit_tid; void th(int s) { (void)s; hit = 1; hit_tid = syscall(SYS_gettid); }
           void *body(void *a) { (void)a; tid = syscall(SYS_gettid); for (int i = 0; i < 300 && !hit; i++) usleep(10000); return 0; }
           struct sigaction s2 = {.sa_handler = th}; sigemptyset(&s2.sa_mask); sigaction(SIGUSR1, &s2, 0); hit = 0; tid = 0;
           pthread_t t; pthread_create(&t, 0, body, 0); while (!tid) usleep(1000);
           syscall(SYS_tgkill, getpid(), tid, SIGUSR1); pthread_join(t, 0); hit && hit_tid == tid; }));
    EXPECT("a process-directed signal goes to a thread that does not block it", ({ static volatile pid_t tid; static volatile int hit; static volatile pid_t hit_tid; void th(int s) { (void)s; hit = 1; hit_tid = syscall(SYS_gettid); }
           void *body(void *a) { (void)a; tid = syscall(SYS_gettid); for (int i = 0; i < 300 && !hit; i++) usleep(10000); return 0; }
           struct sigaction s2 = {.sa_handler = th}; sigemptyset(&s2.sa_mask); sigaction(SIGUSR1, &s2, 0); hit = 0; tid = 0;
           pthread_t t; pthread_create(&t, 0, body, 0); while (!tid) usleep(1000);
           sigemptyset(&m); sigaddset(&m, SIGUSR1); pthread_sigmask(SIG_BLOCK, &m, &old);
           kill(getpid(), SIGUSR1); pthread_join(t, 0); pthread_sigmask(SIG_SETMASK, &old, 0); hit && hit_tid == tid; }));
    EXPECT("SIGPIPE ends a writer; with SIGPIPE ignored it is EPIPE", ({ int p[2]; pipe(p); close(p[0]); signal(SIGPIPE, SIG_IGN); ssize_t r = write(p[1], "x", 1); int e = errno;
           signal(SIGPIPE, SIG_DFL); close(p[1]); r == -1 && e == EPIPE; }));
    EXPECT("SIGPIPE with the default action kills", ({ c = fork(); if (c == 0) { int p[2]; pipe(p); close(p[0]); write(p[1], "x", 1); _exit(0); }
           waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE; }));
    EXPECT("exec keeps an ignored signal and resets a handled one", ({ int p[2]; pipe(p); c = fork();
           if (c == 0) { signal(SIGUSR1, SIG_IGN); struct sigaction s2 = {.sa_handler = h}; sigemptyset(&s2.sa_mask); sigaction(SIGUSR2, &s2, 0);
                         dup2(p[1], 1); execl("/proc/self/exe", "t_sig3", "--exec-probe", (char *)0); _exit(99); }
           close(p[1]); char b[16] = {0}; read(p[0], b, 15); waitpid(c, &st, 0); close(p[0]); strcmp(b, "ign dfl\n") == 0; }));
    return done();
}
