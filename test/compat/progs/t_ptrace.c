/* ptrace, as a debugger or strace uses it: a child that asks to be traced
 * (PTRACE_TRACEME) or a process attached to (PTRACE_ATTACH) stops for each
 * signal and after execve; the tracer reads and writes its memory and
 * registers, resumes it with PTRACE_CONT passing a signal or none, and
 * detaches. The checks stay on what Linux gives every tracer on x86-64. */
#include "compat.h"
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <stddef.h>

static volatile long cell = 0x1111;
static volatile int handled;
static void on_usr1(int s) { (void)s; handled = 1; }

static int wait_stop(pid_t c, int *sig) {
    int st = 0;
    if (waitpid(c, &st, 0) != c || !WIFSTOPPED(st)) return 0;
    *sig = WSTOPSIG(st);
    return 1;
}

int main(int argc, char **argv) {
    setup();
    if (argc > 1 && !strcmp(argv[1], "exec-child")) return 7;

    /* TRACEME, a signal stop, memory and registers. */
    {
        pid_t c = fork();
        if (c == 0) {
            if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) _exit(90);
            raise(SIGUSR2);
            _exit(cell == 0x2222 ? 3 : 4);
        }
        int sig = 0, st = 0;
        int stopped = wait_stop(c, &sig);
        EXPECT("a traced child stops for a signal and the tracer hears of it", stopped && sig == SIGUSR2);
        errno = 0;
        long v = ptrace(PTRACE_PEEKDATA, c, (void *)&cell, 0);
        int e1 = errno;
        EXPECT("PTRACE_PEEKDATA reads the tracee's memory", v == 0x1111 && e1 == 0);
        int r2 = (int)ptrace(PTRACE_POKEDATA, c, (void *)&cell, (void *)0x2222);
        errno = 0;
        long v2 = ptrace(PTRACE_PEEKDATA, c, (void *)&cell, 0);
        EXPECT("PTRACE_POKEDATA writes it", r2 == 0 && v2 == 0x2222 && cell == 0x1111);
        struct user_regs_struct regs;
        int r3 = (int)ptrace(PTRACE_GETREGS, c, 0, &regs);
        EXPECT("PTRACE_GETREGS gives a user rip and rsp", r3 == 0 && regs.rip != 0 && regs.rsp != 0 && regs.cs != 0);
        errno = 0;
        long ru = ptrace(PTRACE_PEEKUSER, c, (void *)(offsetof(struct user, regs.rip)), 0);
        int e4 = errno;
        EXPECT("PTRACE_PEEKUSER reads a register", ru == (long)regs.rip && e4 == 0);
        int r4 = (int)ptrace(PTRACE_SETREGS, c, 0, &regs);
        int r5 = (int)ptrace(PTRACE_CONT, c, 0, 0);       /* SIGUSR2 is dropped */
        int w = waitpid(c, &st, 0);
        EXPECT("PTRACE_CONT without a signal drops the one that stopped it, and the child saw the poke",
               r4 == 0 && r5 == 0 && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 3);
    }

    /* CONT with a signal delivers it, to its handler. */
    {
        pid_t c = fork();
        if (c == 0) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = on_usr1;
            sigaction(SIGUSR1, &sa, 0);
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            _exit(handled ? 5 : 6);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        int r = (int)ptrace(PTRACE_CONT, c, 0, (void *)(long)SIGUSR1);
        int w = waitpid(c, &st, 0);
        EXPECT("PTRACE_CONT with a signal runs the handler for it",
               ok && r == 0 && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 5);
    }

    /* A traced child stops after execve with SIGTRAP. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            execl("/proc/self/exe", "t_ptrace", "exec-child", (char *)0);
            execl(argv[0], "t_ptrace", "exec-child", (char *)0);
            _exit(99);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGTRAP;
        ptrace(PTRACE_CONT, c, 0, 0);
        int w = waitpid(c, &st, 0);
        EXPECT("a traced child stops with SIGTRAP after execve",
               ok && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 7);
    }

    /* PTRACE_ATTACH stops a running process; DETACH lets it go. */
    {
        int p[2];
        pipe(p);
        pid_t c = fork();
        if (c == 0) {
            close(p[1]);
            char b;
            read(p[0], &b, 1);                  /* until the tracer is done */
            _exit(8);
        }
        close(p[0]);
        int sig = 0, st = 0;
        int r1 = (int)ptrace(PTRACE_ATTACH, c, 0, 0);
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        EXPECT("PTRACE_ATTACH stops the target with SIGSTOP", r1 == 0 && ok);
        errno = 0;
        int r2 = (int)ptrace(PTRACE_ATTACH, c, 0, 0), e2 = errno;
        EXPECT("attaching to what is traced already is EPERM", r2 == -1 && e2 == EPERM);
        int r3 = (int)ptrace(PTRACE_DETACH, c, 0, 0);
        write(p[1], "x", 1);
        int w = waitpid(c, &st, 0);
        EXPECT("PTRACE_DETACH lets it run on", r3 == 0 && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 8);
    }

    /* PTRACE_KILL and what a tracer may not do. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGUSR2);
            _exit(0);
        }
        int sig = 0, st = 0;
        wait_stop(c, &sig);
        ptrace(PTRACE_KILL, c, 0, 0);
        int w = waitpid(c, &st, 0);
        EXPECT("PTRACE_KILL ends the tracee", w == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
        errno = 0;
        int r = (int)ptrace(PTRACE_PEEKDATA, getpid(), (void *)&cell, 0), e = errno;
        EXPECT("a process that is not a tracee is ESRCH", r == -1 && e == ESRCH);
        errno = 0;
        int r2 = (int)ptrace(PTRACE_ATTACH, getpid(), 0, 0), e2 = errno;
        EXPECT("attaching to itself is EPERM", r2 == -1 && e2 == EPERM);
        errno = 0;
        int r3 = (int)ptrace(PTRACE_CONT, 99999, 0, 0), e3 = errno;
        EXPECT("a process that does not exist is ESRCH", r3 == -1 && e3 == ESRCH);
    }

    /* A tracee that is running, not stopped, cannot be read. */
    {
        int p[2];
        pipe(p);
        pid_t c = fork();
        if (c == 0) {
            close(p[1]);
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            char b;
            read(p[0], &b, 1);
            _exit(0);
        }
        close(p[0]);
        usleep(50000);
        errno = 0;
        long v = ptrace(PTRACE_PEEKDATA, c, (void *)&cell, 0);
        int e = errno;
        EXPECT("a tracee that is running is ESRCH", v == -1 && e == ESRCH);
        write(p[1], "x", 1);
        int st;
        waitpid(c, &st, 0);
    }
    return done();
}
