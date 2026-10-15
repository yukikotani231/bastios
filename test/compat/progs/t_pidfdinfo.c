/* CLONE_PIDFD (clone and clone3) and ioctl(PIDFD_GET_INFO) on the pidfd:
 * the ids and credentials of a running child, its wait status once it has
 * ended (as a zombie and after it was reaped), and what is refused. */
#define _GNU_SOURCE
#include "compat.h"
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x1000
#endif
#ifndef CLONE_PARENT_SETTID
#define CLONE_PARENT_SETTID 0x100000
#endif
struct clone_args {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls;
};

struct info {
    uint64_t mask, cgroupid;
    uint32_t pid, tgid, ppid, ruid, rgid, euid, egid, suid, sgid, fsuid, fsgid;
    int32_t exit_code;
    uint32_t coredump_mask, spare;
};
#define PIDFD_GET_INFO _IOWR(0xFF, 11, struct info)
#define INFO_PID 1
#define INFO_CREDS 2
#define INFO_EXIT 8

/* A child that waits for a byte on GO, then exits with CODE (or is killed
 * if CODE is negative). Returns its pidfd (made by clone3) and pid. */
static int spawn(int go[2], int code, pid_t *pid, int use_clone3) {
    int pidfd = -1;
    long r;
    if (use_clone3) {
        struct clone_args a = {0};
        a.flags = CLONE_PIDFD;
        a.pidfd = (uint64_t)(uintptr_t)&pidfd;
        a.exit_signal = SIGCHLD;
        r = syscall(SYS_clone3, &a, sizeof a);
    } else {
        r = syscall(SYS_clone, SIGCHLD | CLONE_PIDFD, 0, &pidfd, 0, 0);
    }
    if (r == 0) {
        char b;
        if (read(go[0], &b, 1) != 1) _exit(99);
        if (code < 0) { pause(); _exit(98); }
        _exit(code);
    }
    *pid = r;
    return r > 0 ? pidfd : -1;
}

static void release(int go[2]) { char b = 'g'; (void)!write(go[1], &b, 1); }

static int live_info(int use_clone3) {
    int go[2]; pid_t c;
    if (pipe(go) != 0) return 0;
    int fd = spawn(go, 7, &c, use_clone3);
    if (fd < 0) return 0;
    struct info i = { .mask = INFO_PID | INFO_CREDS | INFO_EXIT };
    int ok = ioctl(fd, PIDFD_GET_INFO, &i) == 0 && (i.mask & INFO_PID) && (i.mask & INFO_CREDS) && !(i.mask & INFO_EXIT)
        && i.pid == (uint32_t)c && i.tgid == (uint32_t)c && i.ppid == (uint32_t)getpid()
        && i.ruid == getuid() && i.euid == geteuid() && i.rgid == getgid() && i.egid == getegid()
        && i.exit_code == 0;
    struct info j = { .mask = INFO_PID };
    ok = ok && ioctl(fd, PIDFD_GET_INFO, &j) == 0 && !(j.mask & INFO_CREDS) && j.ruid == 0;
    release(go);
    int st;
    ok = ok && waitpid(c, &st, 0) == c;
    close(fd); close(go[0]); close(go[1]);
    return ok;
}

static int exit_status(int code, int zombie_first) {
    int go[2]; pid_t c;
    if (pipe(go) != 0) return 0;
    int fd = spawn(go, code, &c, 1);
    if (fd < 0) return 0;
    struct info i = { .mask = INFO_EXIT };
    int ok = ioctl(fd, PIDFD_GET_INFO, &i) == 0 && !(i.mask & INFO_EXIT);     /* still running */
    if (code < 0) kill(c, SIGKILL); else release(go);
    struct pollfd p = { .fd = fd, .events = POLLIN };
    ok = ok && poll(&p, 1, 5000) == 1;
    int st = 0;
    if (zombie_first) {
        /* ended but not reaped: the status is there already */
        memset(&i, 0, sizeof i); i.mask = INFO_EXIT;
        ok = ok && ioctl(fd, PIDFD_GET_INFO, &i) == 0 && (i.mask & INFO_EXIT);
    }
    ok = ok && waitpid(c, &st, 0) == c;
    memset(&i, 0, sizeof i); i.mask = INFO_EXIT;
    ok = ok && ioctl(fd, PIDFD_GET_INFO, &i) == 0 && (i.mask & INFO_EXIT) && i.exit_code == st;
    if (code >= 0) ok = ok && WIFEXITED(i.exit_code) && WEXITSTATUS(i.exit_code) == code;
    else ok = ok && WIFSIGNALED(i.exit_code) && WTERMSIG(i.exit_code) == SIGKILL;
    close(fd); close(go[0]); close(go[1]);
    return ok;
}

static int reaped_without_exit(void) {
    int go[2]; pid_t c;
    if (pipe(go) != 0) return 0;
    int fd = spawn(go, 0, &c, 0);
    release(go);
    int st;
    if (fd < 0 || waitpid(c, &st, 0) != c) return 0;
    struct info i = { .mask = INFO_PID };
    errno = 0;
    int ok = ioctl(fd, PIDFD_GET_INFO, &i) == -1 && errno == ESRCH;
    close(fd); close(go[0]); close(go[1]);
    return ok;
}

static int refusals(void) {
    int fd = -1;
    errno = 0;
    long r = syscall(SYS_clone, SIGCHLD | CLONE_PIDFD | CLONE_PARENT_SETTID, 0, &fd, 0, 0);
    if (r == 0) _exit(0);
    if (r > 0) { int st; waitpid(r, &st, 0); return 0; }
    return errno == EINVAL;
}

static int bad_requests(void) {
    int go[2]; pid_t c;
    if (pipe(go) != 0) return 0;
    int fd = spawn(go, 0, &c, 1);
    struct info i = { .mask = INFO_PID };
    errno = 0;
    int ok = ioctl(fd, PIDFD_GET_INFO, NULL) == -1 && errno == EINVAL;
    struct { uint32_t dummy; } small = { 1 };
    errno = 0;
    ok = ok && ioctl(fd, _IOWR(0xFF, 11, small), &small) == -1 && (errno == EINVAL || errno == ENOTTY);
    errno = 0;
    ok = ok && ioctl(fd, _IOWR(0xFF, 12, struct info), &i) == -1 && errno == ENOTTY;
    errno = 0;
    ok = ok && ioctl(go[0], PIDFD_GET_INFO, &i) == -1 && errno == ENOTTY;
    release(go);
    int st;
    waitpid(c, &st, 0);
    close(fd); close(go[0]); close(go[1]);
    return ok;
}

int main(void) {
    setup();
    EXPECT("clone with CLONE_PIDFD: ids and credentials of a running child", live_info(0));
    EXPECT("clone3 with CLONE_PIDFD: ids and credentials of a running child", live_info(1));
    EXPECT("the exit status is there once the child has been reaped", exit_status(100, 0));
    EXPECT("and while it is still a zombie", exit_status(3, 1));
    EXPECT("a child a signal ended has the signal in its status", exit_status(-1, 1));
    EXPECT("a reaped child is ESRCH unless the exit status is asked for", reaped_without_exit());
    EXPECT("CLONE_PIDFD with CLONE_PARENT_SETTID is EINVAL", refusals());
    EXPECT("a null or short info, an unknown request and a pipe are refused", bad_requests());
    return done();
}
