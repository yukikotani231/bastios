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
#include <sys/stat.h>
#include <sys/syscall.h>

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

    /* PTRACE_SYSCALL stops before and after each system call, with the
     * number in orig_rax, -ENOSYS in rax before and the result after. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            umask(0077);
            long old = syscall(SYS_umask, 0022);          /* returns 0077 */
            _exit(old == 0077 ? 0 : 1);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        int entries = 0, exits = 0, enosys_before = 0, result_after = 0, on_umask = 0, bad = 0;
        long want = SYS_umask;
        while (ok) {
            if (ptrace(PTRACE_SYSCALL, c, 0, 0) != 0) { bad = 1; break; }
            if (waitpid(c, &st, 0) != c) { bad = 1; break; }
            if (!WIFSTOPPED(st)) break;
            if (WSTOPSIG(st) != SIGTRAP) continue;
            struct user_regs_struct r;
            if (ptrace(PTRACE_GETREGS, c, 0, &r) != 0) { bad = 1; break; }
            /* Entry and exit alternate, starting with an entry. */
            if (((entries + exits) & 1) == 0) {
                entries++;
                if (r.orig_rax == (unsigned long)want) {
                    on_umask = 1;
                    enosys_before = (long)r.rax == -ENOSYS && r.rdi == 0022;
                }
            } else {
                exits++;
                if (on_umask == 1 && r.orig_rax == (unsigned long)want) { result_after = r.rax == 0077; on_umask = 2; }
            }
        }
        EXPECT("PTRACE_SYSCALL stops before and after a call, in turn", !bad && entries >= 2 && (entries == exits || entries == exits + 1));
        EXPECT("at the entry stop orig_rax is the call and rax is -ENOSYS", enosys_before);
        EXPECT("at the exit stop rax is the result, and the call ran once", result_after && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* PTRACE_SINGLESTEP runs one instruction; int3 is SIGTRAP. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            asm volatile("int3\n\tnop\n\tnop\n\tnop\n\tnop\n" ::: "memory");
            _exit(0);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGTRAP;     /* int3 */
        struct user_regs_struct r0, r;
        int g = ok && ptrace(PTRACE_GETREGS, c, 0, &r0) == 0;
        int steps = 0, exact = 1;
        for (int i = 1; i <= 4 && g; i++) {
            if (ptrace(PTRACE_SINGLESTEP, c, 0, 0) != 0 || !wait_stop(c, &sig) || sig != SIGTRAP) { exact = 0; break; }
            if (ptrace(PTRACE_GETREGS, c, 0, &r) != 0 || r.rip != r0.rip + i) exact = 0;
            steps++;
        }
        ptrace(PTRACE_CONT, c, 0, 0);
        int w = waitpid(c, &st, 0);
        EXPECT("int3 stops the tracee with SIGTRAP", ok && g);
        EXPECT("PTRACE_SINGLESTEP runs one instruction at a time", steps == 4 && exact);
        EXPECT("and the tracee runs freely after PTRACE_CONT", w == c && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* PTRACE_O_TRACEFORK: the parent stops with the event, the child starts
     * attached and stopped by SIGSTOP. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            pid_t g = fork();
            if (g == 0) _exit(21);
            int gst = 0;
            waitpid(g, &gst, 0);
            _exit(WIFEXITED(gst) && WEXITSTATUS(gst) == 21 ? 0 : 1);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        int r1 = (int)ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACEFORK);
        ptrace(PTRACE_CONT, c, 0, 0);
        int w = waitpid(c, &st, 0);
        int ev = w == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP && (st >> 16) == PTRACE_EVENT_FORK;
        unsigned long msg = 0;
        int r2 = (int)ptrace(PTRACE_GETEVENTMSG, c, 0, &msg);
        pid_t g = (pid_t)msg;
        EXPECT("PTRACE_O_TRACEFORK stops the parent with PTRACE_EVENT_FORK and gives the child's id",
               ok && r1 == 0 && ev && r2 == 0 && g > 0 && g != c);
        int st2 = 0;
        int gs = waitpid(g, &st2, 0) == g && WIFSTOPPED(st2) && WSTOPSIG(st2) == SIGSTOP;
        EXPECT("the new child is traced already and stops with SIGSTOP", gs);
        ptrace(PTRACE_CONT, g, 0, 0);
        ptrace(PTRACE_CONT, c, 0, 0);
        int gw = waitpid(g, &st2, 0) == g && WIFEXITED(st2) && WEXITSTATUS(st2) == 21;
        int cw = 0;
        for (int i = 0; i < 4; i++) {       /* the parent hears SIGCHLD as a signal stop */
            if (waitpid(c, &st, 0) != c) break;
            if (WIFEXITED(st)) { cw = WEXITSTATUS(st) == 0; break; }
            ptrace(PTRACE_CONT, c, 0, 0);
        }
        EXPECT("both run on after PTRACE_CONT, the child's end first for its tracer", gw && cw);
    }

    /* PTRACE_O_TRACEEXEC: the stop after execve carries the event. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            execl("/proc/self/exe", "t_ptrace", "exec-child", (char *)0);
            execl(argv[0], "t_ptrace", "exec-child", (char *)0);
            _exit(99);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACEEXEC);
        ptrace(PTRACE_CONT, c, 0, 0);
        int w = waitpid(c, &st, 0);
        int ev = w == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP && (st >> 16) == PTRACE_EVENT_EXEC;
        ptrace(PTRACE_CONT, c, 0, 0);
        w = waitpid(c, &st, 0);
        EXPECT("PTRACE_O_TRACEEXEC reports the exec as PTRACE_EVENT_EXEC",
               ok && ev && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 7);
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
