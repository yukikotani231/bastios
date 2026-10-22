/* Descriptors past the first few dozen, and the record-lock and lease
 * rules LTP checks: F_GETLK reports the conflicting lock that starts
 * first, a lock needs the matching access mode, malformed requests on a
 * pipe are EINVAL, and leases need a file nobody writes. Also the owner
 * of a description (F_SETOWN, F_SETOWN_EX, F_SETSIG) and the signal it
 * gets when input arrives with O_ASYNC. */
#include "compat.h"
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

static int lock(int fd, int cmd, int type, off_t start, off_t len) {
    struct flock fl = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return fcntl(fd, cmd, &fl);
}

static int many_descriptors(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur < 256) return 0;
    int fd = open("fl_f", O_CREAT | O_RDWR, 0644);
    int ok = fd >= 0 && dup2(fd, 200) == 200 && fcntl(fd, F_DUPFD, 100) == 100;
    close(200); close(100);
    /* 200 files open at once, each its own open file. */
    int fds[200], n = 0;
    while (ok && n < 200 && (fds[n] = open("fl_f", O_RDONLY)) >= 0) n++;
    ok = ok && n == 200;
    while (n > 0) close(fds[--n]);
    close(fd); unlink("fl_f");
    return ok;
}

static int getlk_first(void) {
    int fd = open("fl_l", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "0123456789abcdefghij", 20) != 20) return 0;
    int ok = lock(fd, F_SETLK, F_WRLCK, 10, 5) == 0 && lock(fd, F_SETLK, F_RDLCK, 1, 5) == 0;
    int p[2];
    ok = ok && pipe(p) == 0;
    pid_t c = fork();
    if (c == 0) {
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        int r = fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_RDLCK && fl.l_start == 1 && fl.l_len == 5 && fl.l_pid == getppid();
        struct flock f2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 6, .l_len = 0 };
        r = r && fcntl(fd, F_GETLK, &f2) == 0 && f2.l_type == F_WRLCK && f2.l_start == 10 && f2.l_len == 5;
        _exit(r ? 0 : 1);
    }
    int st;
    ok = ok && c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    close(fd); close(p[0]); close(p[1]); unlink("fl_l");
    return ok;
}

static int lock_modes(void) {
    int w = open("fl_m", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    int r = open("fl_m", O_RDONLY);
    int p[2];
    struct flock bad = { .l_type = F_WRLCK, .l_whence = -1 };
    int ok = w >= 0 && r >= 0 && pipe(p) == 0
        && lock(w, F_SETLK, F_RDLCK, 0, 0) == -1 && errno == EBADF
        && lock(r, F_SETLK, F_WRLCK, 0, 0) == -1 && errno == EBADF
        && fcntl(p[1], F_SETLK, &bad) == -1 && errno == EINVAL
        && fcntl(w, 999) == -1 && errno == EINVAL;
    close(w); close(r); close(p[0]); close(p[1]); unlink("fl_m");
    return ok;
}

static int leases(void) {
    int fd = open("fl_s", O_CREAT | O_RDONLY, 0644);
    int ok = fd >= 0 && fcntl(fd, F_SETLEASE, F_RDLCK) == 0 && fcntl(fd, F_GETLEASE) == F_RDLCK
        && fcntl(fd, F_SETLEASE, F_UNLCK) == 0 && fcntl(fd, F_GETLEASE) == F_UNLCK;
    close(fd);
    fd = open("fl_s", O_RDWR);
    ok = ok && fd >= 0 && fcntl(fd, F_SETLEASE, F_RDLCK) == -1 && errno == EAGAIN;
    close(fd); unlink("fl_s");
    return ok;
}

static int owners(void) {
    int p[2];
    struct f_owner_ex ex = { 0 }, bad = { 3, 0 }, none = { F_OWNER_PID, 0 };
    pid_t me = getpid(), g = getpgrp();
    int ok = pipe(p) == 0 && fcntl(p[0], F_GETOWN) == 0
        && fcntl(p[0], F_GETOWN_EX, &ex) == 0 && ex.pid == 0
        && fcntl(p[0], F_SETOWN, me) == 0 && fcntl(p[0], F_GETOWN) == me
        && fcntl(p[0], F_SETOWN, -g) == 0 && fcntl(p[0], F_GETOWN) == -g
        && fcntl(p[0], F_GETOWN_EX, &ex) == 0 && ex.type == F_OWNER_PGRP && ex.pid == g;
    ex.type = F_OWNER_TID; ex.pid = me;
    ok = ok && fcntl(p[0], F_SETOWN_EX, &ex) == 0 && fcntl(p[0], F_GETOWN_EX, &ex) == 0
        && ex.type == F_OWNER_TID && ex.pid == me
        && fcntl(p[0], F_SETOWN_EX, &bad) == -1 && errno == EINVAL
        && fcntl(p[0], F_SETOWN, INT_MAX) == -1 && errno == ESRCH
        && fcntl(p[0], F_SETOWN_EX, &none) == 0 && fcntl(p[0], F_GETOWN) == 0
        && fcntl(p[0], F_GETOWN_EX, &ex) == 0 && ex.type == F_OWNER_PID && ex.pid == 0
        && fcntl(p[0], F_GETSIG) == 0 && fcntl(p[0], F_SETSIG, 65) == -1 && errno == EINVAL
        && fcntl(p[0], F_SETSIG, SIGUSR1) == 0 && fcntl(p[0], F_GETSIG) == SIGUSR1;
    close(p[0]); close(p[1]);
    return ok;
}

static int io_signal(void) {
    int p[2];
    char c;
    sigset_t set, old;
    struct timespec t = { 2, 0 };
    sigemptyset(&set); sigaddset(&set, SIGIO); sigaddset(&set, SIGUSR1);
    int ok = pipe(p) == 0 && sigprocmask(SIG_BLOCK, &set, &old) == 0
        && fcntl(p[0], F_SETFL, O_ASYNC) == 0 && (fcntl(p[0], F_GETFL) & O_ASYNC)
        && fcntl(p[0], F_SETOWN, getpid()) == 0
        && write(p[1], "x", 1) == 1 && sigtimedwait(&set, NULL, &t) == SIGIO
        && read(p[0], &c, 1) == 1
        && fcntl(p[0], F_SETSIG, SIGUSR1) == 0
        && write(p[1], "y", 1) == 1 && sigtimedwait(&set, NULL, &t) == SIGUSR1
        && read(p[0], &c, 1) == 1
        && fcntl(p[0], F_SETFL, 0) == 0 && write(p[1], "z", 1) == 1
        && sigtimedwait(&set, NULL, &(struct timespec){ 0, 100000000 }) == -1 && errno == EAGAIN;
    sigprocmask(SIG_SETMASK, &old, NULL);
    close(p[0]); close(p[1]);
    return ok;
}

static int many_locks(void) {
    int fd = open("fl_many", O_CREAT | O_RDWR, 0644);
    if (fd < 0) return 0;
    int ok = ftruncate(fd, 1 << 20) == 0;
    for (int i = 0; ok && i < 200; i++) {
        struct flock f = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = i * 512, .l_len = 256 };
        ok = fcntl(fd, F_OFD_SETLK, &f) == 0;
    }
    struct flock g = { .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    ok = ok && fcntl(fd, F_OFD_SETLK, &g) == 0;
    close(fd);
    unlink("fl_many");
    return ok;
}

static int memfd_sealing_alone(void) {
    int fd = memfd_create("seal_alone", MFD_ALLOW_SEALING);
    int ok = fd >= 0 && ftruncate(fd, 4096) == 0 && fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0
        && (fcntl(fd, F_GET_SEALS) & F_SEAL_SHRINK) && ftruncate(fd, 0) == -1 && errno == EPERM;
    if (fd >= 0) close(fd);
    return ok && memfd_create("bad", 4) == -1 && errno == EINVAL;
}

int main(void) {
    setup();
    EXPECT("memfd_create takes MFD_ALLOW_SEALING without MFD_CLOEXEC", memfd_sealing_alone());
    EXPECT("200 separate byte-range locks on one file", many_locks());
    EXPECT("200 descriptors, dup2 to 200 and F_DUPFD from 100", many_descriptors());
    EXPECT("F_GETLK reports the conflicting lock that starts first", getlk_first());
    EXPECT("a lock needs the matching access mode; bad requests are EINVAL", lock_modes());
    EXPECT("F_SETLEASE and F_GETLEASE", leases());
    EXPECT("F_SETOWN, F_SETOWN_EX and F_SETSIG set a description's owner", owners());
    EXPECT("O_ASYNC on a pipe signals its owner when input arrives", io_signal());
    return done();
}
