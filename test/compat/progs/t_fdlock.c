/* Descriptors past the first few dozen, and the record-lock and lease
 * rules LTP checks: F_GETLK reports the conflicting lock that starts
 * first, a lock needs the matching access mode, malformed requests on a
 * pipe are EINVAL, and leases need a file nobody writes. */
#include "compat.h"
#include <fcntl.h>
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

int main(void) {
    setup();
    EXPECT("200 descriptors, dup2 to 200 and F_DUPFD from 100", many_descriptors());
    EXPECT("F_GETLK reports the conflicting lock that starts first", getlk_first());
    EXPECT("a lock needs the matching access mode; bad requests are EINVAL", lock_modes());
    EXPECT("F_SETLEASE and F_GETLEASE", leases());
    return done();
}
