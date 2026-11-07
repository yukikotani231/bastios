/* ptrace, as a debugger or strace uses it: a child that asks to be traced
 * (PTRACE_TRACEME) or a process attached to (PTRACE_ATTACH) stops for each
 * signal and after execve; the tracer reads and writes its memory and
 * registers, resumes it with PTRACE_CONT passing a signal or none, and
 * detaches. The checks stay on what Linux gives every tracer on x86-64. */
#include "compat.h"
#include <signal.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <spawn.h>
#include <pthread.h>
#include <string.h>


/* The classic BPF and seccomp definitions musl does not ship. */
struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };
#define BPF_STMT(c, k) { (c), 0, 0, (k) }
#define BPF_JUMP(c, k, jt, jf) { (c), (jt), (jf), (k) }
#define BPF_LD_W_ABS 0x20
#define BPF_JEQ_K 0x15
#define BPF_RET_K 0x06
#define SECCOMP_MODE_FILTER 2
#define SECCOMP_RET_ALLOW 0x7fff0000u
#define SECCOMP_RET_TRACE 0x7ff00000u
#ifndef PTRACE_O_TRACESECCOMP
#define PTRACE_O_TRACESECCOMP 0x80
#endif
#ifndef PTRACE_EVENT_SECCOMP
#define PTRACE_EVENT_SECCOMP 7
#endif


#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
/* struct ptrace_syscall_info, as linux/ptrace.h has it (musl does not). */
struct sysinfo {
    unsigned char op, pad[3];
    unsigned int arch;
    unsigned long long ip, sp;
    union {
        struct { unsigned long long nr, args[6]; } entry;
        struct { long long rval; unsigned char is_error; } exit;
        struct { unsigned long long nr, args[6]; unsigned int ret_data; } seccomp;
    };
};

/* A filter that answers SECCOMP_RET_TRACE with data 42 to getppid. */
static int trace_getppid_filter(void) {
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD_W_ABS, 0),
        BPF_JUMP(BPF_JEQ_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET_K, SECCOMP_RET_TRACE | 42),
        BPF_STMT(BPF_RET_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { sizeof f / sizeof f[0], f };
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
}

#ifdef __aarch64__
/* AArch64 has no PTRACE_GETREGS: the registers come as NT_PRSTATUS. */
struct regs { unsigned long long x[31], sp, pc, pstate; };
static int get_regs(pid_t c, struct regs *r) {
    struct iovec iov = { r, sizeof *r };
    return (int)ptrace(PTRACE_GETREGSET, c, (void *)1, &iov);
}
static int set_regs(pid_t c, struct regs *r) {
    struct iovec iov = { r, sizeof *r };
    return (int)ptrace(PTRACE_SETREGSET, c, (void *)1, &iov);
}
#define REG_PC(r) ((r).pc)
#define REG_SP(r) ((r).sp)
#define REG_NR(r) ((r).x[8])
#define REG_RET(r) ((r).x[0])
#define REG_ARG0(r) ((r).x[0])
#define ENTRY_RET_IS_ENOSYS 0
#define BREAK_LEN 4
#define BREAK_INSN "brk #0\n\tnop\n\tnop\n\tnop\n\tnop\n"
#else
struct regs { struct user_regs_struct u; };
static int get_regs(pid_t c, struct regs *r) { return (int)ptrace(PTRACE_GETREGS, c, 0, &r->u); }
static int set_regs(pid_t c, struct regs *r) { return (int)ptrace(PTRACE_SETREGS, c, 0, &r->u); }
#define REG_PC(r) ((r).u.rip)
#define REG_SP(r) ((r).u.rsp)
#define REG_NR(r) ((r).u.orig_rax)
#define REG_RET(r) ((r).u.rax)
#define REG_ARG0(r) ((r).u.rdi)
#define ENTRY_RET_IS_ENOSYS 1
#define BREAK_LEN 0
#define BREAK_INSN "int3\n\tnop\n\tnop\n\tnop\n\tnop\n"
#endif

static volatile long cell = 0x1111;
static volatile int handled;
static void on_usr1(int s) { (void)s; handled = 1; }

static int wait_stop(pid_t c, int *sig) {
    int st = 0;
    if (waitpid(c, &st, 0) != c || !WIFSTOPPED(st)) return 0;
    *sig = WSTOPSIG(st);
    return 1;
}

/* posix_spawn (a clone with CLONE_VM | CLONE_VFORK, which borrows the
 * memory) in a child traced with PTRACE_SYSCALL and PTRACE_O_TRACEVFORK plus
 * `opts`: 1 if the child's stops pair up as entries and exits, and the stop
 * after the vfork event (and vfork-done, if asked for) is the end of the
 * clone, returning the new child's pid. */
static int spawn_traced(char **argv, int opts)
{
    pid_t c = fork();
    if (c == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        pid_t q;
        char *av[] = {"t_ptrace", "exec-child", 0};
        if (posix_spawn(&q, "/proc/self/exe", 0, 0, av, 0) != 0 && posix_spawn(&q, argv[0], 0, 0, av, 0) != 0) _exit(90);
        int qs = 0;
        waitpid(q, &qs, 0);
        _exit(WIFEXITED(qs) && WEXITSTATUS(qs) == 7 ? 0 : 91);
    }
    int st = 0;
    waitpid(c, &st, 0);
    ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)(opts | PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEVFORK));
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    int ok = 1, inside = 0, after = 0, n = 0, cx = -1;
    long kid = 0;
    for (;;) {
        pid_t p = waitpid(-1, &st, __WALL);
        if (p == -1) break;
        if (p != c) {
            if (WIFSTOPPED(st)) ptrace(PTRACE_CONT, p, 0, 0);
            continue;
        }
        if (!WIFSTOPPED(st)) { cx = st; break; }
        if (++n > 2000) { ok = 0; kill(c, SIGKILL); continue; }
        int sig = WSTOPSIG(st), ev = st >> 16;
        if (sig == (SIGTRAP | 0x80)) {
            struct sysinfo si;
            memset(&si, 0, sizeof si);
            ptrace(PTRACE_GET_SYSCALL_INFO, c, (void *)sizeof si, &si);
            if (si.op != (inside ? 2 : 1)) ok = 0;
            if (after && !(si.op == 2 && si.exit.rval == kid)) ok = 0;
            after = 0;
            inside = !inside;
        } else if (ev == PTRACE_EVENT_VFORK) {
            unsigned long m = 0;
            ptrace(PTRACE_GETEVENTMSG, c, 0, &m);
            kid = (long)m;
            after = 1;
        }
        int pass = sig == SIGTRAP || sig == (SIGTRAP | 0x80) ? 0 : sig;
        ptrace(PTRACE_SYSCALL, c, 0, (void *)(long)pass);
    }
    return ok && kid > 0 && WIFEXITED(cx) && WEXITSTATUS(cx) == 0;
}

/* Program text that another process patches: it returns 42 until then. */
__attribute__((noinline)) static int patched(void) { __asm__ volatile(""); return 42; }
static int (*volatile patched_p)(void) = patched;
/* The bytes that make it return 7. */
#if defined(__x86_64__)
static const unsigned char ret7[] = { 0xb8, 7, 0, 0, 0, 0xc3 };      /* mov $7, %eax; ret */
#else
static const unsigned char ret7[] = { 0xe0, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6 };   /* mov w0, #7; ret */
#endif

static int wake_fd[2], tid_fd[2];
static void *text_thread(void *a) {
    (void)a;
    int t = (int)syscall(SYS_gettid);
    write(tid_fd[1], &t, sizeof t);
    char x;
    while (read(wake_fd[0], &x, 1) != 1) {}
    return (void *)(intptr_t)patched_p();
}

/* A child whose second thread is the last of the process to run before
 * the parent patches its program text (by /proc/PID/mem, or by
 * PTRACE_POKETEXT on that thread): the thread then calls the patched
 * function, and the child exits with what it returned (7 if the patch
 * landed, 42 if not). The first thread waits in pthread_join all along, so
 * the process's memory is held by the second one when the patch is made. */
static int patch_text_in_thread(int by_ptrace) {
    if (pipe(wake_fd) != 0 || pipe(tid_fd) != 0) return 0;
    pid_t c = fork();
    if (c == 0) {
        pthread_t th;
        void *r = 0;
        if (pthread_create(&th, 0, text_thread, 0) != 0) _exit(1);
        pthread_join(th, &r);
        _exit((int)(intptr_t)r);
    }
    int tid = 0;
    int ok = c > 0 && read(tid_fd[0], &tid, sizeof tid) == sizeof tid;
    usleep(300000);                         /* the thread waits in read */
    if (ok && !by_ptrace) {
        char path[40];
        snprintf(path, sizeof path, "/proc/%d/mem", (int)c);
        int fd = open(path, O_RDWR);
        ok = fd >= 0 && pwrite(fd, ret7, sizeof ret7, (off_t)(uintptr_t)patched) == (ssize_t)sizeof ret7;
        if (fd >= 0) close(fd);
    } else if (ok) {
        int st = 0;
        ok = ptrace(PTRACE_ATTACH, tid, 0, 0) == 0 && waitpid(tid, &st, __WALL) == tid && WIFSTOPPED(st);
        errno = 0;
        long w = ok ? ptrace(PTRACE_PEEKTEXT, tid, (void *)patched, 0) : 0;
        ok = ok && errno == 0;
        memcpy(&w, ret7, sizeof ret7);
        ok = ok && ptrace(PTRACE_POKETEXT, tid, (void *)patched, (void *)w) == 0;
        ptrace(PTRACE_DETACH, tid, 0, 0);
    }
    write(wake_fd[1], "w", 1);
    int st = 0;
    ok = c > 0 && waitpid(c, &st, 0) == c && ok && WIFEXITED(st) && WEXITSTATUS(st) == 7;
    close(wake_fd[0]); close(wake_fd[1]); close(tid_fd[0]); close(tid_fd[1]);
    return ok;
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
        struct regs regs;
        int r3 = get_regs(c, &regs);
        EXPECT("PTRACE_GETREGS gives a user rip and rsp", r3 == 0 && REG_PC(regs) != 0 && REG_SP(regs) != 0);
#ifndef __aarch64__
        errno = 0;
        long ru = ptrace(PTRACE_PEEKUSER, c, (void *)(offsetof(struct user, regs.rip)), 0);
        int e4 = errno;
        EXPECT("PTRACE_PEEKUSER reads a register", ru == (long)regs.u.rip && e4 == 0);
#endif
        int r4 = set_regs(c, &regs);
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
            struct regs r;
            if (get_regs(c, &r) != 0) { bad = 1; break; }
            /* Entry and exit alternate, starting with an entry. */
            if (((entries + exits) & 1) == 0) {
                entries++;
                if (REG_NR(r) == (unsigned long)want) {
                    on_umask = 1;
                    enosys_before = (!ENTRY_RET_IS_ENOSYS || (long)REG_RET(r) == -ENOSYS) && REG_ARG0(r) == 0022;
                }
            } else {
                exits++;
                if (on_umask == 1 && REG_NR(r) == (unsigned long)want) { result_after = REG_RET(r) == 0077; on_umask = 2; }
            }
        }
        EXPECT("PTRACE_SYSCALL stops before and after a call, in turn", !bad && entries >= 2 && (entries == exits || entries == exits + 1));
        EXPECT("at the entry stop orig_rax is the call and rax is -ENOSYS", enosys_before);
        EXPECT("at the exit stop rax is the result, and the call ran once", result_after && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* getpid and gettid stop too, like every other call. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            long a = syscall(SYS_getpid), b = syscall(SYS_gettid);
            _exit(a == getpid() && b == a ? 0 : 1);
        }
        int sig = 0, st = 0, bad = 0, pid_seen = 0, tid_seen = 0, n = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        while (ok) {
            if (ptrace(PTRACE_SYSCALL, c, 0, 0) != 0) { bad = 1; break; }
            if (waitpid(c, &st, 0) != c) { bad = 1; break; }
            if (!WIFSTOPPED(st)) break;
            if (WSTOPSIG(st) != SIGTRAP) continue;
            struct regs r;
            if (get_regs(c, &r) != 0) { bad = 1; break; }
            if ((n++ & 1) == 0) {                   /* an entry stop */
                if (REG_NR(r) == SYS_getpid) pid_seen = 1;
                if (REG_NR(r) == SYS_gettid) tid_seen = 1;
            }
        }
        EXPECT("PTRACE_SYSCALL stops at getpid and gettid", !bad && pid_seen && tid_seen && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* PTRACE_SINGLESTEP runs one instruction; int3 is SIGTRAP. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            asm volatile(BREAK_INSN ::: "memory");
            _exit(0);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGTRAP;     /* int3 */
        struct regs r0, r;
        int g = ok && get_regs(c, &r0) == 0;
        if (g && BREAK_LEN) {                    /* a breakpoint instruction stops at itself: step over it */
            REG_PC(r0) += BREAK_LEN;
            g = set_regs(c, &r0) == 0;
        }
        int steps = 0, exact = 1;
        for (int i = 1; i <= 4 && g; i++) {
            if (ptrace(PTRACE_SINGLESTEP, c, 0, 0) != 0 || !wait_stop(c, &sig) || sig != SIGTRAP) { exact = 0; break; }
            if (get_regs(c, &r) != 0 || REG_PC(r) != REG_PC(r0) + (BREAK_LEN ? 4 : 1) * i) exact = 0;
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

    /* PTRACE_SEIZE does not stop the tracee; PTRACE_INTERRUPT does, with an event stop. */
    {
        int p[2];
        pipe(p);
        pid_t c = fork();
        if (c == 0) {
            close(p[1]);
            char b;
            read(p[0], &b, 1);
            _exit(7);
        }
        close(p[0]);
        usleep(50000);
        long r = ptrace(PTRACE_SEIZE, c, 0, 0);
        int st = 0;
        int w = waitpid(c, &st, WNOHANG);
        EXPECT("PTRACE_SEIZE attaches without stopping the tracee", r == 0 && w == 0);
        long ri = ptrace(PTRACE_INTERRUPT, c, 0, 0);
        w = waitpid(c, &st, 0);
        EXPECT("PTRACE_INTERRUPT stops it with PTRACE_EVENT_STOP and SIGTRAP",
               ri == 0 && w == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP && (st >> 16) == 128);
        long rc = ptrace(PTRACE_CONT, c, 0, 0);
        write(p[1], "x", 1);
        w = waitpid(c, &st, 0);
        EXPECT("a seized tracee resumed with PTRACE_CONT runs to its end", rc == 0 && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 7);
    }


    /* PTRACE_GET_SYSCALL_INFO: the entry of a call has its number and
     * arguments, its exit the result and whether it is an error, and a stop
     * that is no system call stop has op 0. */
    {
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            syscall(SYS_lseek, 99, 5, 0);
            _exit(0);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        struct sysinfo si;
        memset(&si, 0, sizeof si);
        long r0 = ptrace(PTRACE_GET_SYSCALL_INFO, c, (void *)sizeof si, &si);
        EXPECT("PTRACE_GET_SYSCALL_INFO at a signal stop is op 0, with the header only", ok && r0 == 24 && si.op == 0);
        int found = 0, bad = 0;
        long size = 0;
        for (int i = 0; i < 200 && !found; i++) {
            if (ptrace(PTRACE_SYSCALL, c, 0, 0) != 0) { bad = 1; break; }
            int w = waitpid(c, &st, 0);
            if (w != c || !WIFSTOPPED(st)) { bad = 1; break; }
            memset(&si, 0, sizeof si);
            size = ptrace(PTRACE_GET_SYSCALL_INFO, c, (void *)sizeof si, &si);
            if (si.op == 1 && si.entry.nr == SYS_lseek) found = 1;
        }
        EXPECT("PTRACE_GET_SYSCALL_INFO at a call's entry gives its number and arguments",
               !bad && found && size >= 80 && si.entry.args[0] == 99 && si.entry.args[1] == 5 && si.entry.args[2] == 0 && si.ip != 0 && si.sp != 0 && si.arch != 0);
        struct sysinfo small;
        memset(&small, 0xAA, sizeof small);
        long sz2 = ptrace(PTRACE_GET_SYSCALL_INFO, c, (void *)8, &small);
        EXPECT("a buffer that is too small gets what fits, and the full size is the result",
               sz2 == size && small.op == 1 && small.ip == 0xAAAAAAAAAAAAAAAAull);
        ptrace(PTRACE_SYSCALL, c, 0, 0);
        int w = waitpid(c, &st, 0);
        memset(&si, 0, sizeof si);
        long r3 = ptrace(PTRACE_GET_SYSCALL_INFO, c, (void *)sizeof si, &si);
        EXPECT("and at its exit the result, as an error",
               w == c && WIFSTOPPED(st) && r3 >= 33 && si.op == 2 && si.exit.rval == -EBADF && si.exit.is_error == 1);
        ptrace(PTRACE_CONT, c, 0, 0);
        w = waitpid(c, &st, 0);
        EXPECT("the tracee goes on to its end", w == c && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* SECCOMP_RET_TRACE: with PTRACE_O_TRACESECCOMP the tracee stops before
     * the call with the event and the filter's data, and the call runs when
     * it goes on, without the filters again. */
    {
        pid_t c = fork();
        if (c == 0) {
            pid_t pp = getppid();
            if (!trace_getppid_filter()) _exit(10);
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            _exit(syscall(SYS_getppid) == pp ? 0 : 11);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        int r1 = (int)ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACESECCOMP);
        ptrace(PTRACE_CONT, c, 0, 0);
        int w = waitpid(c, &st, 0);
        int ev = w == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP && (st >> 16) == PTRACE_EVENT_SECCOMP;
        unsigned long msg = 0;
        int r2 = (int)ptrace(PTRACE_GETEVENTMSG, c, 0, &msg);
        EXPECT("SECCOMP_RET_TRACE stops the tracee with PTRACE_EVENT_SECCOMP and the filter's data",
               ok && r1 == 0 && ev && r2 == 0 && msg == 42);
        ptrace(PTRACE_CONT, c, 0, 0);
        w = waitpid(c, &st, 0);
        EXPECT("the call runs once the tracer goes on, and the filter is not asked again",
               w == c && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* At the SECCOMP_RET_TRACE stop the info is that of a seccomp stop. */
    {
        pid_t c = fork();
        if (c == 0) {
            if (!trace_getppid_filter()) _exit(10);
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            syscall(SYS_getppid);
            _exit(0);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        ptrace(PTRACE_SETOPTIONS, c, 0, (void *)(long)PTRACE_O_TRACESECCOMP);
        ptrace(PTRACE_CONT, c, 0, 0);
        int w = waitpid(c, &st, 0);
        struct sysinfo si;
        memset(&si, 0, sizeof si);
        long size = ptrace(PTRACE_GET_SYSCALL_INFO, c, (void *)sizeof si, &si);
        EXPECT("PTRACE_GET_SYSCALL_INFO at a SECCOMP_RET_TRACE stop gives the call and the filter's data",
               ok && w == c && (st >> 16) == PTRACE_EVENT_SECCOMP && size >= 84 && si.op == 3 &&
               si.seccomp.nr == SYS_getppid && si.seccomp.ret_data == 42);
        ptrace(PTRACE_CONT, c, 0, 0);
        waitpid(c, &st, 0);
    }

    /* Without PTRACE_O_TRACESECCOMP, or without a tracer, the call fails
     * with ENOSYS. */
    {
        pid_t c = fork();
        if (c == 0) {
            if (!trace_getppid_filter()) _exit(10);
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            _exit(syscall(SYS_getppid) == -1 && errno == ENOSYS ? 0 : 11);
        }
        int sig = 0, st = 0;
        int ok = wait_stop(c, &sig) && sig == SIGSTOP;
        ptrace(PTRACE_CONT, c, 0, 0);
        int w = waitpid(c, &st, 0);
        EXPECT("SECCOMP_RET_TRACE with a tracer that did not ask for it is ENOSYS",
               ok && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        pid_t d = fork();
        if (d == 0) {
            if (!trace_getppid_filter()) _exit(10);
            _exit(syscall(SYS_getppid) == -1 && errno == ENOSYS ? 0 : 11);
        }
        EXPECT("and without a tracer", waitpid(d, &st, 0) == d && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }


    /* PTRACE_SEIZE and PTRACE_INTERRUPT on a tracee that sleeps in a system call. */
    {
        pid_t c = fork();
        if (c == 0) {
            struct timespec ts = {30, 0};
            nanosleep(&ts, 0);
            _exit(0);
        }
        usleep(100000);
        long r = ptrace(PTRACE_SEIZE, c, 0, PTRACE_O_TRACESYSGOOD);
        long ri = ptrace(PTRACE_INTERRUPT, c, 0, 0);
        int st = 0;
        int w = waitpid(c, &st, 0);
        EXPECT("a sleeping seized tracee is stopped by PTRACE_INTERRUPT",
               r == 0 && ri == 0 && w == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP && (st >> 16) == 128);
        long rs = ptrace(PTRACE_SYSCALL, c, 0, 0);
        kill(c, SIGKILL);
        w = waitpid(c, &st, 0);
        int ok = 0;
        while (w == c && WIFSTOPPED(st)) {
            ptrace(PTRACE_SYSCALL, c, 0, 0);
            w = waitpid(c, &st, 0);
        }
        ok = w == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
        EXPECT("it can be resumed and then killed", rs == 0 && ok);
    }

    /* /proc/PID/mem: a process's own memory, and another's, read and written at an address. */
    {
        static volatile int v = 1234;
        int fd = open("/proc/self/mem", O_RDWR);
        int x = 0, y = 77;
        ssize_t a = fd >= 0 ? pread(fd, &x, sizeof x, (off_t)(uintptr_t)&v) : -1;
        ssize_t b = fd >= 0 ? pwrite(fd, &y, sizeof y, (off_t)(uintptr_t)&v) : -1;
        int z = 0;
        ssize_t c2 = fd >= 0 && lseek(fd, (off_t)(uintptr_t)&v, SEEK_SET) == (off_t)(uintptr_t)&v ? read(fd, &z, sizeof z) : -1;
        char junk[8];
        ssize_t d = fd >= 0 ? pread(fd, junk, sizeof junk, 16) : 0;
        EXPECT("/proc/self/mem reads and writes the program's own memory",
               fd >= 0 && a == 4 && x == 1234 && b == 4 && v == 77 && c2 == 4 && z == 77 && d < 0);
        if (fd >= 0) close(fd);

        static volatile int w = 55;
        pid_t c = fork();
        if (c == 0) {
            usleep(400000);
            _exit(w == 99 ? 0 : 1);
        }
        usleep(100000);
        char path[40];
        snprintf(path, sizeof path, "/proc/%d/mem", (int)c);
        int cf = open(path, O_RDWR);
        int cur = 0, nv = 99;
        ssize_t e = cf >= 0 ? pread(cf, &cur, sizeof cur, (off_t)(uintptr_t)&w) : -1;
        ssize_t f = cf >= 0 ? pwrite(cf, &nv, sizeof nv, (off_t)(uintptr_t)&w) : -1;
        int st = 0;
        waitpid(c, &st, 0);
        EXPECT("/proc/PID/mem reads and writes another process's memory",
               cf >= 0 && e == 4 && cur == 55 && f == 4 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        if (cf >= 0) close(cf);
    }
    EXPECT("/proc/PID/mem writes program text while another thread of the process holds its memory",
           patch_text_in_thread(0));
    EXPECT("PTRACE_POKETEXT on a thread other than the first writes program text",
           patch_text_in_thread(1));
    EXPECT("strace -f style tracing of posix_spawn: the clone ends with a stop after the vfork event, and stops pair up",
           spawn_traced(argv, 0));
    EXPECT("the same with PTRACE_O_TRACEVFORKDONE: the stop after the clone follows the vfork-done stop",
           spawn_traced(argv, PTRACE_O_TRACEVFORKDONE));
    return done();
}
