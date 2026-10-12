/* seccomp filters and no_new_privs, against musl. Loads classic BPF programs
 * that musl itself does not wrap: the ones that must be refused at load time
 * (a load past seccomp_data, an unaligned one, a jump out of the program, a
 * division by 0, no return at the end), then ones that answer: an errno for
 * a call chosen by number, one chosen by an argument, two filters stacked
 * (the stricter wins, a child keeps both), SIGSYS for TRAP, death by SIGSYS
 * for KILL_PROCESS, strict mode, and the probes libseccomp makes. Prints
 * "mseccomp: ok", or what failed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* The kernel's headers (linux/filter.h, linux/seccomp.h) are not on musl's
 * include path, so what is used of them is here. */
struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };
#define BPF_LD 0x00
#define BPF_ST 0x02
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_IND 0x40
#define BPF_ABS 0x20
#define BPF_K 0x00
#define BPF_DIV 0x30
#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_STMT(c, k) { (unsigned short)(c), 0, 0, (unsigned)(k) }
#define BPF_JUMP(c, k, jt, jf) { (unsigned short)(c), (jt), (jf), (unsigned)(k) }
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SECCOMP_MODE_STRICT 1
#define SECCOMP_FILTER_FLAG_TSYNC 1
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL_THREAD 0x00000000U
#define SECCOMP_RET_TRAP 0x00030000U
#define SECCOMP_RET_ERRNO 0x00050000U
#define SECCOMP_RET_LOG 0x7ffc0000U
#define SECCOMP_RET_ALLOW 0x7fff0000U
#ifndef SYS_seccomp
#define SYS_seccomp 317
#endif
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#endif
#ifndef PR_SET_SECCOMP
#define PR_SET_SECCOMP 22
#define PR_GET_SECCOMP 21
#endif

#define NR(n) ((unsigned)(n))
#define LD_NR   BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0)
#define LD_ARG0 BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 16)
#define RET(v)  BPF_STMT(BPF_RET | BPF_K, (v))
#define IF_NR(n, then_ret) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR(n), 0, 1), RET(then_ret)

/* musl's getppid does not set errno; syscall() does. */
static long ppid(void) { return syscall(SYS_getppid); }

static int fail(const char *what)
{
    printf("mseccomp: %s (errno %d)\n", what, errno);
    return 1;
}

static int load(struct sock_filter *f, unsigned n)
{
    struct sock_fprog p = { .len = n, .filter = f };
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &p);
}

/* Programs that must be refused when they are loaded. */
static int refused(void)
{
    struct sock_filter past[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 64), RET(SECCOMP_RET_ALLOW) };
    struct sock_filter odd[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 2), RET(SECCOMP_RET_ALLOW) };
    struct sock_filter half[] = { BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 0), RET(SECCOMP_RET_ALLOW) };
    struct sock_filter jump[] = { BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 5, 0), RET(SECCOMP_RET_ALLOW) };
    struct sock_filter ja[] = { BPF_STMT(BPF_JMP | BPF_JA, 9), RET(SECCOMP_RET_ALLOW) };
    struct sock_filter div0[] = { BPF_STMT(BPF_ALU | BPF_DIV | BPF_K, 0), RET(SECCOMP_RET_ALLOW) };
    struct sock_filter noret[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0) };
    struct sock_filter mem[] = { BPF_STMT(BPF_ST, 16), RET(SECCOMP_RET_ALLOW) };
    struct sock_filter ind[] = { BPF_STMT(BPF_LD | BPF_W | BPF_IND, 0), RET(SECCOMP_RET_ALLOW) };
    struct { const char *what; struct sock_filter *f; unsigned n; } bad[] = {
        { "a load past seccomp_data", past, 2 },
        { "an unaligned load", odd, 2 },
        { "a half word load", half, 2 },
        { "a conditional jump out of the program", jump, 2 },
        { "a jump out of the program", ja, 2 },
        { "a division by the constant 0", div0, 2 },
        { "a program with no return at its end", noret, 1 },
        { "scratch memory index 16", mem, 2 },
        { "an indexed load", ind, 2 },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (load(bad[i].f, bad[i].n) != -1 || errno != EINVAL) {
            printf("mseccomp: %s was loaded (errno %d)\n", bad[i].what, errno);
            return 1;
        }
    }
    struct sock_fprog empty = { 0, 0 };
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &empty) != -1 || errno != EINVAL)
        return fail("an empty program");
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, 0) != -1 || errno != EFAULT)
        return fail("no program");
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0x100, &empty) != -1 || errno != EINVAL)
        return fail("an unknown flag");
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, 0) != -1 || errno != EFAULT)
        return fail("TSYNC is not known");
    return 0;
}

/* What libseccomp asks before it builds a program. */
static int probes(void)
{
    unsigned avail[] = { SECCOMP_RET_KILL_PROCESS, SECCOMP_RET_KILL_THREAD, SECCOMP_RET_TRAP,
                         SECCOMP_RET_ERRNO, SECCOMP_RET_LOG, SECCOMP_RET_ALLOW };
    for (unsigned i = 0; i < sizeof avail / sizeof avail[0]; i++)
        if (syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &avail[i]) != 0)
            return fail("an action that should be available");
    unsigned gone = 0x12340000;
    if (syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &gone) != -1 || errno != EOPNOTSUPP)
        return fail("an unknown action");
    if (prctl(PR_GET_SECCOMP) != 0 || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0)
        return fail("a fresh process has no seccomp or no_new_privs");
    if (prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) != -1 || errno != EINVAL)
        return fail("no_new_privs cannot be cleared");
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 1, 0, 0) != -1 || errno != EINVAL
        || prctl(PR_GET_NO_NEW_PRIVS, 1, 0, 0, 0) != -1 || errno != EINVAL)
        return fail("unused no_new_privs arguments must be 0");
    return 0;
}

static volatile int trapped;
static void on_sys(int s) { (void)s; trapped++; }

static int status_has(const char *line)
{
    char buf[2048];
    int fd = open("/proc/self/status", O_RDONLY);
    int n = fd < 0 ? 0 : read(fd, buf, sizeof buf - 1);
    if (fd >= 0)
        close(fd);
    buf[n > 0 ? n : 0] = 0;
    return strstr(buf, line) != 0;
}

static void *thread_getppid(void *arg)
{
    errno = 0;
    return (void *)(long)(ppid() == -1 && errno == EPERM);
}

/* Runs as a child so the filters die with it. */
static int answers(void)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
        return fail("no_new_privs");
    if (!status_has("NoNewPrivs:\t1") || !status_has("Seccomp:\t0"))
        return fail("the status before a filter");

    /* getppid: EPERM; write to fd 2: EACCES; the rest is allowed. */
    struct sock_filter one[] = {
        LD_NR,
        IF_NR(SYS_getppid, SECCOMP_RET_ERRNO | EPERM),
        IF_NR(SYS_write, SECCOMP_RET_ALLOW),          /* looked at below */
        RET(SECCOMP_RET_ALLOW),
    };
    /* write: the first argument decides (fd 2 refused). */
    struct sock_filter two[] = {
        LD_NR,
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR(SYS_write), 0, 4),
        LD_ARG0,
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 1),
        RET(SECCOMP_RET_ERRNO | EACCES),
        RET(SECCOMP_RET_ALLOW),
        RET(SECCOMP_RET_ALLOW),
    };
    pthread_t th;
    if (pthread_create(&th, 0, thread_getppid, 0) == 0)
        pthread_join(th, 0);          /* only so that a second thread exists */

    if (load(one, sizeof one / sizeof one[0]) != 0)
        return fail("loading the first filter");
    if (prctl(PR_GET_SECCOMP) != 2 || !status_has("Seccomp:\t2") || !status_has("Seccomp_filters:\t1"))
        return fail("the status after one filter");
    errno = 0;
    long gp = ppid();
    if (gp != -1 || errno != EPERM) {
        printf("mseccomp: getppid gave %ld, errno %d\n", gp, errno);
        return fail("getppid is not refused with EPERM");
    }
    if (getpid() <= 0)
        return fail("getpid is refused");

    if (load(two, sizeof two / sizeof two[0]) != 0)
        return fail("loading the second filter");
    if (!status_has("Seccomp_filters:\t2"))
        return fail("the status after two filters");
    if (write(2, "x", 1) != -1 || errno != EACCES)
        return fail("write to fd 2 is not refused with EACCES");
    if (write(1, "", 0) != 0)
        return fail("write to fd 1 is refused");
    errno = 0;
    if (ppid() != -1 || errno != EPERM)
        return fail("the first filter no longer holds");

    /* a child has both */
    pid_t c = fork();
    if (c == 0) {
        errno = 0;
        if (ppid() != -1 || errno != EPERM)
            _exit(1);
        if (write(2, "x", 1) != -1 || errno != EACCES)
            _exit(2);
        if (!status_has("Seccomp_filters:\t2"))
            _exit(3);
        _exit(0);
    }
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("mseccomp: the child's filters said %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }

    /* the stricter of two answers: an older ERRNO beats a newer ALLOW */
    struct sock_filter allow[] = { RET(SECCOMP_RET_ALLOW) };
    if (load(allow, 1) != 0)
        return fail("loading an allow-all filter on top");
    errno = 0;
    if (ppid() != -1 || errno != EPERM)
        return fail("a newer ALLOW overrides an older ERRNO");

    /* a stack deeper than the pool is ENOMEM, not a crash */
    int got = 0;
    for (int i = 0; i < 20 && got == 0; i++)
        got = load(allow, 1);
    if (got != -1 || errno != ENOMEM)
        return fail("a full pool is not ENOMEM");
    return 0;
}

static int in_child(int (*f)(void), const char *what)
{
    pid_t c = fork();
    if (c == 0)
        _exit(f());
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("mseccomp: %s (status 0x%x)\n", what, st);
        return 1;
    }
    return 0;
}

static int trap_it(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sys;
    sigaction(SIGSYS, &sa, 0);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct sock_filter f[] = { LD_NR, IF_NR(SYS_getppid, SECCOMP_RET_TRAP), RET(SECCOMP_RET_ALLOW) };
    if (load(f, sizeof f / sizeof f[0]) != 0)
        return 1;
    ppid();
    return trapped == 1 ? 0 : 2;
}

static int kill_it(void)
{
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct sock_filter f[] = { LD_NR, IF_NR(SYS_getppid, SECCOMP_RET_KILL_PROCESS), RET(SECCOMP_RET_ALLOW) };
    if (load(f, sizeof f / sizeof f[0]) != 0)
        return 1;
    ppid();
    return 0;                       /* not reached */
}

static int strict_it(void)
{
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
        return 1;
    write(1, "", 0);
    getpid();                       /* killed by SIGKILL */
    return 0;
}

static int unprivileged(void)
{
    if (setuid(1000) != 0)
        return 1;
    struct sock_filter allow[] = { RET(SECCOMP_RET_ALLOW) };
    if (load(allow, 1) != -1 || errno != EACCES)
        return 2;                   /* needs no_new_privs or CAP_SYS_ADMIN */
    if (prctl(PR_SET_SECCOMP, 2, (unsigned long)-4096, 0, 0) != -1 || errno != EFAULT)
        return 2;                   /* but a bad program pointer is EFAULT first */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || load(allow, 1) != 0)
        return 3;
    return 0;
}

static int killed_by(int (*f)(void), int sig, const char *what)
{
    pid_t c = fork();
    if (c == 0)
        _exit(f());
    int st;
    if (waitpid(c, &st, 0) != c || !WIFSIGNALED(st) || WTERMSIG(st) != sig) {
        printf("mseccomp: %s (status %d)\n", what, st);
        return 1;
    }
    return 0;
}

static volatile int tsync_ok;

static void *tsync_thread(void *arg)
{
    int *go = arg;
    while (!*go)
        usleep(1000);
    errno = 0;
    tsync_ok = ppid() == -1 && errno == EPERM;
    return 0;
}

static int tsync_it(void)
{
    static int go;
    pthread_t th;
    if (pthread_create(&th, 0, tsync_thread, &go))
        return 1;
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct sock_filter f[] = { LD_NR, IF_NR(SYS_getppid, SECCOMP_RET_ERRNO | EPERM), RET(SECCOMP_RET_ALLOW) };
    struct sock_fprog p = { .len = sizeof f / sizeof f[0], .filter = f };
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &p) != 0)
        return 2;
    go = 1;
    pthread_join(th, 0);
    return tsync_ok ? 0 : 3;
}

int main(void)
{
    if (refused() || probes())
        return 1;
    if (in_child(answers, "the filters did not answer"))
        return 1;
    if (in_child(trap_it, "TRAP did not raise SIGSYS") || in_child(unprivileged, "an unprivileged user and a filter"))
        return 1;
    if (killed_by(kill_it, SIGSYS, "KILL_PROCESS did not end the process with SIGSYS"))
        return 1;
    if (killed_by(strict_it, SIGKILL, "strict mode did not kill a getpid"))
        return 1;
    if (in_child(tsync_it, "TSYNC did not reach another thread"))
        return 1;
    puts("mseccomp: ok");
    return 0;
}
