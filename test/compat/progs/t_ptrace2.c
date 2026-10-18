/* ptrace corners a debugger or strace meets after the first stop: the event
 * stops (fork, clone, vfork, exit), PTRACE_O_EXITKILL, PTRACE_O_TRACESYSGOOD,
 * what a signal stop's siginfo says, changing or dropping a signal on
 * resume, skipping a system call, group-stop with PTRACE_SEIZE and
 * PTRACE_LISTEN, PTRACE_DETACH with a signal, register sets, and a tracer
 * that dies. Each expectation was checked on a Linux host. */
#include "compat.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <elf.h>
#include <time.h>

#ifndef PTRACE_O_EXITKILL
#define PTRACE_O_EXITKILL (1 << 20)
#endif
#ifndef PTRACE_O_TRACEVFORKDONE
#define PTRACE_O_TRACEVFORKDONE 0x20
#endif
#ifndef PTRACE_EVENT_VFORK_DONE
#define PTRACE_EVENT_VFORK_DONE 5
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#define PTRACE_INTERRUPT 0x4207
#define PTRACE_LISTEN 0x4208
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 1
#endif

static volatile int handled;
static void on_usr1(int s) { (void)s; handled = SIGUSR1; }
static void on_usr2(int s) { (void)s; handled = SIGUSR2; }

/* Waits for c and returns its status, or -1. */
static int wst(pid_t c)
{
    int st = 0;
    return waitpid(c, &st, __WALL) == c ? st : -1;
}

static int stopped_with(pid_t c, int sig)
{
    int st = wst(c);
    return st != -1 && WIFSTOPPED(st) && WSTOPSIG(st) == sig;
}

/* Waits for an event stop: SIGTRAP with the event in bits 16 and up. */
static int event_of(pid_t c)
{
    int st = wst(c);
    if (st == -1 || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGTRAP) return -1;
    return st >> 16;
}

/* Resumes whatever stops (a signal-delivery stop gets its signal) until
 * process c ends, and returns c's status; the status of the other process
 * that ended on the way goes to *other. */
static int drain(pid_t c, int *other)
{
    for (int i = 0; i < 100; i++) {
        int st = 0;
        pid_t w = waitpid(-1, &st, __WALL);
        if (w == -1) return -1;
        if (WIFSTOPPED(st)) {
            int sig = WSTOPSIG(st);
            ptrace(PTRACE_CONT, w, 0, (void *)(long)((st >> 16) == 0 && sig != SIGTRAP && sig != SIGSTOP ? sig : 0));
            continue;
        }
        if (w == c) return st;
        if (other) *other = st;
    }
    return -1;
}

/* PC and SP of a stopped tracee through PTRACE_GETREGSET NT_PRSTATUS, which
 * both architectures have; 0 on failure. */
static int pc_sp(pid_t c, unsigned long *pc, unsigned long *sp)
{
#ifdef __x86_64__
    struct user_regs_struct r; struct iovec iov = {&r, sizeof r};
    if (ptrace(PTRACE_GETREGSET, c, (void *)NT_PRSTATUS, &iov) != 0) return 0;
    *pc = r.rip; *sp = r.rsp;
#else
    unsigned long long r[34]; struct iovec iov = {r, sizeof r};  /* x0-x30, sp, pc, pstate */
    if (ptrace(PTRACE_GETREGSET, c, (void *)NT_PRSTATUS, &iov) != 0) return 0;
    *pc = r[32]; *sp = r[31];
#endif
    return 1;
}

#ifdef __x86_64__
/* Runs the tracee to the entry of system call nr; its registers are left in *r. */
static int to_call(pid_t c, long nr, struct user_regs_struct *r)
{
    for (int i = 0; i < 64; i++) {
        if (ptrace(PTRACE_SYSCALL, c, 0, 0) != 0) return 0;
        int st = wst(c);
        if (st == -1 || !WIFSTOPPED(st)) return 0;
        ptrace(PTRACE_GETREGS, c, 0, r);
        if ((long)r->orig_rax == nr && (long)r->rax == -ENOSYS) return 1;
    }
    return 0;
}
#endif

static pid_t traced_child(void (*body)(void))
{
    pid_t c = fork();
    if (c == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) _exit(90);
        raise(SIGSTOP);
        body();
        _exit(0);
    }
    return c;
}

static void body_fork(void)
{
    pid_t g = fork();
    if (g == 0) _exit(11);
    waitpid(g, 0, 0);
}
static void body_exit5(void) { _exit(5); }
static void body_usr1(void) { raise(SIGUSR1); _exit(handled == SIGUSR2 ? 20 : handled == SIGUSR1 ? 21 : 22); }
static void body_term(void) { signal(SIGTERM, SIG_DFL); raise(SIGTERM); _exit(0); }
static void body_getpid(void) { syscall(SYS_getppid); syscall(SYS_getppid); }
static void body_vfork(void)
{
    pid_t g = vfork();
    if (g == 0) _exit(12);
    waitpid(g, 0, 0);
}
static void *thread_fn(void *a) { (void)a; return 0; }
static void body_thread(void)
{
    pthread_t t;
    pthread_create(&t, 0, thread_fn, 0);
    pthread_join(t, 0);
}
static void body_segv(void) { *(volatile char *)0x10 = 1; }
static void body_stopcont(void) { for (int i = 0; i < 2000; i++) usleep(1000); _exit(33); }

int main(void)
{
    setup();
    int st;
    pid_t c;
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);

    /* Event stops. */
    EXPECT("PTRACE_O_TRACEFORK: the event message is the new child's pid", ({
           c = traced_child(body_fork); int ok = stopped_with(c, SIGSTOP);
           ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACEFORK);
           ptrace(PTRACE_CONT, c, 0, 0);
           int ev = event_of(c); unsigned long msg = 0; ptrace(PTRACE_GETEVENTMSG, c, 0, &msg);
           ok = ok && ev == PTRACE_EVENT_FORK && msg > 0 && (pid_t)msg != c;
           pid_t g = (pid_t)msg; int gs = wst(g); ok = ok && WIFSTOPPED(gs);
           ptrace(PTRACE_CONT, g, 0, 0); ptrace(PTRACE_CONT, c, 0, 0);
           int gx = -1, cx = drain(c, &gx); ok = ok && WIFEXITED(gx) && WEXITSTATUS(gx) == 11 && WIFEXITED(cx) && WEXITSTATUS(cx) == 0; ok; }));
    EXPECT("PTRACE_O_TRACEVFORK and PTRACE_O_TRACEVFORKDONE report the vfork and its end", ({
           c = traced_child(body_vfork); int ok = stopped_with(c, SIGSTOP);
           ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)(PTRACE_O_TRACEVFORK | PTRACE_O_TRACEVFORKDONE));
           ptrace(PTRACE_CONT, c, 0, 0);
           int ev = event_of(c); unsigned long msg = 0; ptrace(PTRACE_GETEVENTMSG, c, 0, &msg); pid_t g = (pid_t)msg;
           ok = ok && ev == PTRACE_EVENT_VFORK && g > 0;
           int gs = wst(g); ok = ok && WIFSTOPPED(gs);
           ptrace(PTRACE_CONT, g, 0, 0); ptrace(PTRACE_CONT, c, 0, 0);
           ev = event_of(c); ok = ok && ev == PTRACE_EVENT_VFORK_DONE;
           ptrace(PTRACE_CONT, c, 0, 0);
           int cx = drain(c, 0); ok = ok && WIFEXITED(cx); ok; }));
    EXPECT("PTRACE_O_TRACECLONE reports a new thread, which is traced", ({
           c = traced_child(body_thread); int ok = stopped_with(c, SIGSTOP);
           ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACECLONE);
           ptrace(PTRACE_CONT, c, 0, 0);
           int ev = event_of(c); unsigned long msg = 0; ptrace(PTRACE_GETEVENTMSG, c, 0, &msg); pid_t t = (pid_t)msg;
           ok = ok && ev == PTRACE_EVENT_CLONE && t > 0 && t != c;
           int ts = wst(t); ok = ok && WIFSTOPPED(ts);
           ptrace(PTRACE_CONT, t, 0, 0); ptrace(PTRACE_CONT, c, 0, 0);
           int cx = drain(c, 0); ok = ok && WIFEXITED(cx) && WEXITSTATUS(cx) == 0; ok; }));
    EXPECT("PTRACE_O_TRACEEXIT stops the tracee at exit with the status as the event message", ({
           c = traced_child(body_exit5); int ok = stopped_with(c, SIGSTOP);
           ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACEEXIT);
           ptrace(PTRACE_CONT, c, 0, 0);
           int ev = event_of(c); unsigned long msg = 0; ptrace(PTRACE_GETEVENTMSG, c, 0, &msg);
           ok = ok && ev == PTRACE_EVENT_EXIT && (msg >> 8) == 5;
           ptrace(PTRACE_CONT, c, 0, 0); int cx = wst(c); ok = ok && WIFEXITED(cx) && WEXITSTATUS(cx) == 5; ok; }));
    EXPECT("PTRACE_O_TRACEEXIT also reports a death by signal, after the signal stop", ({
           c = traced_child(body_term); int ok = stopped_with(c, SIGSTOP);
           ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACEEXIT);
           ptrace(PTRACE_CONT, c, 0, 0);
           ok = ok && stopped_with(c, SIGTERM); ptrace(PTRACE_CONT, c, 0, (void *)(long)SIGTERM);
           int ev = event_of(c); unsigned long msg = 0; ptrace(PTRACE_GETEVENTMSG, c, 0, &msg);
           ok = ok && ev == PTRACE_EVENT_EXIT && msg == SIGTERM;
           ptrace(PTRACE_CONT, c, 0, 0); int cx = wst(c); ok = ok && WIFSIGNALED(cx) && WTERMSIG(cx) == SIGTERM; ok; }));
    EXPECT("PTRACE_SETOPTIONS with an unknown option is EINVAL", ({
           c = traced_child(body_exit5); int ok = stopped_with(c, SIGSTOP);
           int r = ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)0x40000000); int e = errno;
           ptrace(PTRACE_KILL, c, 0, 0); kill(c, SIGKILL); wst(c); ok && r == -1 && e == EINVAL; }));

    /* Signals: what the stop says, and what resume does with them. */
    EXPECT("PTRACE_GETSIGINFO at a signal stop gives the signal and its sender", ({
           c = traced_child(body_usr1); int ok = stopped_with(c, SIGSTOP); ptrace(PTRACE_CONT, c, 0, 0);
           ok = ok && stopped_with(c, SIGUSR1); siginfo_t si; memset(&si, 0, sizeof si);
           int r = ptrace(PTRACE_GETSIGINFO, c, 0, &si);
           ok = ok && r == 0 && si.si_signo == SIGUSR1 && si.si_code == SI_TKILL && si.si_pid == c;
           ptrace(PTRACE_KILL, c, 0, 0); kill(c, SIGKILL); wst(c); ok; }));
    EXPECT("PTRACE_GETSIGINFO at a fault: SIGSEGV, SEGV_MAPERR and the address", ({
           c = traced_child(body_segv); int ok = stopped_with(c, SIGSTOP); ptrace(PTRACE_CONT, c, 0, 0);
           ok = ok && stopped_with(c, SIGSEGV); siginfo_t si; memset(&si, 0, sizeof si);
           int r = ptrace(PTRACE_GETSIGINFO, c, 0, &si);
           ok = ok && r == 0 && si.si_signo == SIGSEGV && si.si_code == SEGV_MAPERR && si.si_addr == (void *)0x10;
           ptrace(PTRACE_CONT, c, 0, (void *)(long)SIGKILL); int cx = wst(c); ok && WIFSIGNALED(cx); }));
    EXPECT("resuming with another signal delivers that one", ({
           c = fork(); if (c == 0) { sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, 0); sa.sa_handler = on_usr2; sigaction(SIGUSR2, &sa, 0);
                                     ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); body_usr1(); }
           int ok = stopped_with(c, SIGSTOP); ptrace(PTRACE_CONT, c, 0, 0);
           ok = ok && stopped_with(c, SIGUSR1); ptrace(PTRACE_CONT, c, 0, (void *)(long)SIGUSR2);
           int cx = wst(c); ok && WIFEXITED(cx) && WEXITSTATUS(cx) == 20; }));
    EXPECT("resuming with no signal drops it", ({
           c = fork(); if (c == 0) { sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, 0); ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); body_usr1(); }
           int ok = stopped_with(c, SIGSTOP); ptrace(PTRACE_CONT, c, 0, 0);
           ok = ok && stopped_with(c, SIGUSR1); ptrace(PTRACE_CONT, c, 0, 0);
           int cx = wst(c); ok && WIFEXITED(cx) && WEXITSTATUS(cx) == 22; }));
    EXPECT("a signal the tracee blocks does not stop it", ({
           c = fork(); if (c == 0) { sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, 0);
                                     ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); raise(SIGUSR1); _exit(44); }
           int ok = stopped_with(c, SIGSTOP); ptrace(PTRACE_CONT, c, 0, 0);
           int cx = wst(c); ok && WIFEXITED(cx) && WEXITSTATUS(cx) == 44; }));
    EXPECT("SIGKILL ends a tracee in a signal stop", ({
           c = traced_child(body_usr1); int ok = stopped_with(c, SIGSTOP); ptrace(PTRACE_CONT, c, 0, 0);
           ok = ok && stopped_with(c, SIGUSR1); kill(c, SIGKILL); int cx = wst(c); ok && WIFSIGNALED(cx) && WTERMSIG(cx) == SIGKILL; }));
    EXPECT("PTRACE_DETACH with a signal delivers it", ({
           c = fork(); if (c == 0) { sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, 0); ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP);
                                     for (int i = 0; i < 100 && !handled; i++) usleep(10000); _exit(handled == SIGUSR1 ? 55 : 56); }
           int ok = stopped_with(c, SIGSTOP); ptrace(PTRACE_DETACH, c, 0, (void *)(long)SIGUSR1);
           int cx = wst(c); ok && WIFEXITED(cx) && WEXITSTATUS(cx) == 55; }));

    /* System call stops. */
    EXPECT("PTRACE_O_TRACESYSGOOD marks a system call stop with SIGTRAP | 0x80", ({
           c = traced_child(body_getpid); int ok = stopped_with(c, SIGSTOP);
           ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACESYSGOOD);
           ptrace(PTRACE_SYSCALL, c, 0, 0); int s1 = wst(c);
           ok = ok && WIFSTOPPED(s1) && WSTOPSIG(s1) == (SIGTRAP | 0x80);
           ptrace(PTRACE_KILL, c, 0, 0); kill(c, SIGKILL); wst(c); ok; }));
#ifdef __x86_64__
    EXPECT("changing orig_rax to -1 at the entry skips the call; rax at the exit is the result", ({
           c = fork(); if (c == 0) { ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); long r = syscall(SYS_getppid); _exit(r == 777 ? 66 : 67); }
           int ok = stopped_with(c, SIGSTOP); struct user_regs_struct r;
           ok = ok && to_call(c, SYS_getppid, &r);
           r.orig_rax = -1; ptrace(PTRACE_SETREGS, c, 0, &r);
           ptrace(PTRACE_SYSCALL, c, 0, 0); ok = ok && stopped_with(c, SIGTRAP);
           ptrace(PTRACE_GETREGS, c, 0, &r); ok = ok && (long)r.rax == -ENOSYS;
           r.rax = 777; ptrace(PTRACE_SETREGS, c, 0, &r);
           ptrace(PTRACE_CONT, c, 0, 0); int cx = wst(c); ok && WIFEXITED(cx) && WEXITSTATUS(cx) == 66; }));
    EXPECT("changing the arguments at the entry changes the call", ({
           c = fork(); if (c == 0) { ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); long r = syscall(SYS_getpgid, 0x7fffff00); _exit(r == -1 ? 68 : 69); }
           int ok = stopped_with(c, SIGSTOP); struct user_regs_struct r;
           ok = ok && to_call(c, SYS_getpgid, &r); r.rdi = 0; ptrace(PTRACE_SETREGS, c, 0, &r);
           ptrace(PTRACE_CONT, c, 0, 0); int cx = wst(c); ok && WIFEXITED(cx) && WEXITSTATUS(cx) == 69; }));
    EXPECT("PTRACE_GETFPREGS gives the SSE state", ({
           c = traced_child(body_exit5); int ok = stopped_with(c, SIGSTOP); struct user_fpregs_struct f;
           ok = ok && ptrace(PTRACE_GETFPREGS, c, 0, &f) == 0 && f.mxcsr == 0x1f80;
           kill(c, SIGKILL); wst(c); ok; }));
#endif
    EXPECT("PTRACE_GETREGSET NT_PRSTATUS gives the program counter and the stack pointer", ({
           c = traced_child(body_exit5); int ok = stopped_with(c, SIGSTOP); unsigned long pc = 0, sp = 0;
           ok = ok && pc_sp(c, &pc, &sp) && pc > 0x1000 && sp > 0x1000;
#ifdef __x86_64__
           struct user_regs_struct a; struct iovec iov = {&a, sizeof a};
           ok = ok && ptrace(PTRACE_GETREGS, c, 0, &a) == 0 && a.rip == pc && a.rsp == sp && iov.iov_len == sizeof a;
#endif
           kill(c, SIGKILL); wst(c); ok; }));
    EXPECT("PTRACE_SINGLESTEP from a signal stop inside raise() stops with SIGTRAP one instruction on, though SIGTRAP is blocked", ({
           c = fork(); if (c == 0) { ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); _exit(0); }
           int ok = stopped_with(c, SIGSTOP); unsigned long pc0 = 0, sp0 = 0, pc1 = 0, sp1 = 0;
           ok = ok && pc_sp(c, &pc0, &sp0) && ptrace(PTRACE_SINGLESTEP, c, 0, 0) == 0;
           int s1 = wst(c); ok = ok && WIFSTOPPED(s1) && WSTOPSIG(s1) == SIGTRAP;
           ok = ok && pc_sp(c, &pc1, &sp1) && sp1 == sp0 && pc1 > pc0 && pc1 - pc0 < 16;
           ptrace(PTRACE_CONT, c, 0, 0); int cx = wst(c); ok && WIFEXITED(cx); }));

    /* Group-stop and PTRACE_SEIZE. */
    EXPECT("PTRACE_SEIZE reports a group-stop as PTRACE_EVENT_STOP", ({
           c = fork(); if (c == 0) { body_stopcont(); }
           usleep(20000); int ok = ptrace(PTRACE_SEIZE, c, 0, 0) == 0; kill(c, SIGSTOP);
           int s0 = wst(c); ok = ok && WIFSTOPPED(s0) && WSTOPSIG(s0) == SIGSTOP && (s0 >> 16) == 0;   /* the signal-delivery stop */
           ptrace(PTRACE_CONT, c, 0, (void *)(long)SIGSTOP);
           int s1 = wst(c); ok = ok && WIFSTOPPED(s1) && WSTOPSIG(s1) == SIGSTOP && (s1 >> 16) == PTRACE_EVENT_STOP;
           kill(c, SIGKILL); wst(c); ok; }));
    EXPECT("PTRACE_LISTEN keeps a group-stopped tracee stopped; SIGCONT then wakes it", ({
           c = fork(); if (c == 0) { body_stopcont(); }
           usleep(20000); int ok = ptrace(PTRACE_SEIZE, c, 0, 0) == 0; kill(c, SIGSTOP);
           int s0 = wst(c); ok = ok && WIFSTOPPED(s0); ptrace(PTRACE_CONT, c, 0, (void *)(long)SIGSTOP);
           int s1 = wst(c); ok = ok && (s1 >> 16) == PTRACE_EVENT_STOP;
           ok = ok && ptrace(PTRACE_LISTEN, c, 0, 0) == 0;
           pid_t r = waitpid(c, &st, WNOHANG | __WALL); ok = ok && r == 0;
           kill(c, SIGCONT); int s2 = wst(c); ok = ok && WIFSTOPPED(s2);
           kill(c, SIGKILL); wst(c); ok; }));

    /* A tracer that goes away. */
    EXPECT("PTRACE_O_EXITKILL kills the tracee when the tracer dies", ({
           int p[2], q[2]; pipe(p); pipe(q); pid_t tr = fork();
           if (tr == 0) { pid_t g = fork(); if (g == 0) { close(p[0]); close(p[1]); close(q[0]); ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); for (;;) pause(); }
                          close(q[1]); waitpid(g, 0, 0); ptrace(PTRACE_SETOPTIONS, g, 0, (void *)(long)PTRACE_O_EXITKILL); ptrace(PTRACE_CONT, g, 0, 0); write(p[1], "r", 1); _exit(0); }
           close(q[1]); char b; read(p[0], &b, 1); waitpid(tr, 0, 0);
           struct pollfd pf = {q[0], POLLIN, 0}; int r = poll(&pf, 1, 3000); int ok = r == 1 && read(q[0], &b, 1) == 0;
           close(p[0]); close(p[1]); close(q[0]); ok; }));
    EXPECT("a tracee runs on when its tracer dies", ({
           int p[2], q[2]; pipe(p); pipe(q); pid_t tr = fork();
           if (tr == 0) { pid_t g = fork(); if (g == 0) { close(p[0]); close(p[1]); close(q[0]); ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); usleep(100000); write(q[1], "x", 1); _exit(77); }
                          close(q[1]); waitpid(g, 0, 0); ptrace(PTRACE_CONT, g, 0, 0); write(p[1], "r", 1); _exit(0); }
           close(q[1]); char b; read(p[0], &b, 1); waitpid(tr, 0, 0);
           struct pollfd pf = {q[0], POLLIN, 0}; int ok = poll(&pf, 1, 3000) == 1 && read(q[0], &b, 1) == 1 && b == 'x';
           close(p[0]); close(p[1]); close(q[0]); ok; }));
    EXPECT("a tracee stopped when its tracer dies runs again", ({
           int p[2], q[2]; pipe(p); pipe(q); pid_t tr = fork();
           if (tr == 0) { pid_t g = fork(); if (g == 0) { close(p[0]); close(p[1]); close(q[0]); ptrace(PTRACE_TRACEME, 0, 0, 0); raise(SIGSTOP); write(q[1], "x", 1); _exit(77); }
                          close(q[1]); waitpid(g, 0, 0); write(p[1], "r", 1); _exit(0); }
           close(q[1]); char b; read(p[0], &b, 1); waitpid(tr, 0, 0);
           struct pollfd pf = {q[0], POLLIN, 0}; int ok = poll(&pf, 1, 3000) == 1 && read(q[0], &b, 1) == 1 && b == 'x';
           close(p[0]); close(p[1]); close(q[0]); ok; }));

    /* Errors. */
    EXPECT("PTRACE_PEEKDATA of an unmapped address is EIO", ({
           c = traced_child(body_exit5); int ok = stopped_with(c, SIGSTOP); errno = 0;
           long v = ptrace(PTRACE_PEEKDATA, c, (void *)0x10, 0); int e = errno; (void)v;
           kill(c, SIGKILL); wst(c); ok && e == EIO; }));
    EXPECT("PTRACE_POKEDATA of an unmapped address is EIO", ({
           c = traced_child(body_exit5); int ok = stopped_with(c, SIGSTOP);
           int r = ptrace(PTRACE_POKEDATA, c, (void *)0x10, (void *)1); int e = errno;
           kill(c, SIGKILL); wst(c); ok && r == -1 && e == EIO; }));
    EXPECT("PTRACE_CONT of a tracee that is not stopped is ESRCH", ({
           c = traced_child(body_stopcont); int ok = stopped_with(c, SIGSTOP); ptrace(PTRACE_CONT, c, 0, 0);
           int r = ptrace(PTRACE_CONT, c, 0, 0); int e = errno; kill(c, SIGKILL); wst(c); ok && r == -1 && e == ESRCH; }));
    EXPECT("a request on a process that is not a tracee of the caller is ESRCH", ({
           c = fork(); if (c == 0) { for (;;) pause(); }
           siginfo_t si; int r = ptrace(PTRACE_GETSIGINFO, c, 0, &si); int e = errno; kill(c, SIGKILL); wst(c); r == -1 && e == ESRCH; }));
    EXPECT("an unknown request is EIO", ({ c = traced_child(body_exit5); int ok = stopped_with(c, SIGSTOP);
           int r = ptrace(0x7777, c, 0, 0); int e = errno; kill(c, SIGKILL); wst(c); ok && r == -1 && e == EIO; }));
    EXPECT("a tracer attaching to a stopped process sees it stop", ({
           c = fork(); if (c == 0) { raise(SIGSTOP); body_stopcont(); }
           waitpid(c, &st, WUNTRACED); int ok = ptrace(PTRACE_ATTACH, c, 0, 0) == 0;
           int s1 = wst(c); ok = ok && WIFSTOPPED(s1); ptrace(PTRACE_DETACH, c, 0, 0); kill(c, SIGCONT); kill(c, SIGKILL); wst(c); ok; }));
    EXPECT("a tracer can wait for a tracee that is not its child", ({
           int p[2]; pipe(p); pid_t mid = fork();
           if (mid == 0) { pid_t g = fork(); if (g == 0) { for (;;) pause(); } write(p[1], &g, sizeof g); waitpid(g, 0, 0); _exit(0); }
           pid_t g = 0; read(p[0], &g, sizeof g);
           int ok = ptrace(PTRACE_ATTACH, g, 0, 0) == 0; int s1 = wst(g); ok = ok && WIFSTOPPED(s1) && WSTOPSIG(s1) == SIGSTOP;
           ptrace(PTRACE_CONT, g, 0, 0); kill(g, SIGKILL); int s2 = wst(g); ok = ok && WIFSIGNALED(s2);
           waitpid(mid, 0, 0); close(p[0]); close(p[1]); ok; }));
    (void)on_usr2;
    return done();
}
