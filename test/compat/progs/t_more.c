/* System calls the other programs leave out that real programs still make:
 * copying between descriptors, locks, the *at family with flags, vectored
 * and positioned I/O, file times, memfd and the other descriptor kinds,
 * process spawning the way libc does it. */
#include "compat.h"
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>

extern char **environ;

int main(int argc, char **argv) {
    if (argc > 1)
        return 7;                                   /* run by the posix_spawn check */
    setup();
    char b[64];
    int fd = -1, g = -1, p[2], st;
    struct stat s;
    pid_t c;

    EXPECT("source file", (fd = open("cx_a", O_RDWR | O_CREAT | O_TRUNC, 0644)) >= 0 && write(fd, "0123456789", 10) == 10);
    EXPECT("sendfile file to file", (g = open("cx_b", O_RDWR | O_CREAT | O_TRUNC, 0644)) >= 0 &&
           ({ off_t o = 2; sendfile(g, fd, &o, 5) == 5 && o == 7; }) && pread(g, b, 5, 0) == 5 && memcmp(b, "23456", 5) == 0);
    EXPECT("sendfile to a pipe", pipe(p) == 0 && ({ off_t o = 0; sendfile(p[1], fd, &o, 4) == 4; }) &&
           read(p[0], b, 4) == 4 && memcmp(b, "0123", 4) == 0);
    EXPECT("FIONREAD on a pipe", write(p[1], "abc", 3) == 3 && ({ int n = 0; ioctl(p[0], FIONREAD, &n) == 0 && n == 3; }));
    close(p[0]); close(p[1]);
    EXPECT("copy_file_range", ({ off_t i = 0, o = 0; ftruncate(g, 0) == 0 && copy_file_range(fd, &i, g, &o, 10, 0) == 10; }) &&
           pread(g, b, 10, 0) == 10 && memcmp(b, "0123456789", 10) == 0);
    EXPECT("preadv", ({ char x[3], y[4]; struct iovec v[2] = {{x, 3}, {y, 4}};
           preadv(fd, v, 2, 1) == 7 && memcmp(x, "123", 3) == 0 && memcmp(y, "4567", 4) == 0; }));
    EXPECT("pwritev", ({ struct iovec v[2] = {{"AB", 2}, {"CD", 2}}; pwritev(g, v, 2, 3) == 4; }) &&
           pread(g, b, 7, 0) == 7 && memcmp(b, "012ABCD", 7) == 0);
    EXPECT("flock exclusive", flock(fd, LOCK_EX) == 0 && flock(fd, LOCK_UN) == 0);
    EXPECT("fcntl F_SETLK and F_GETLK", ({ struct flock l = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 4};
           fcntl(fd, F_SETLK, &l) == 0 && (l.l_type = F_WRLCK, fcntl(fd, F_GETLK, &l) == 0) && l.l_type == F_UNLCK; }));
    EXPECT("fstatfs", ({ struct statfs f; fstatfs(fd, &f) == 0; }));
    EXPECT("futimens", ({ struct timespec t[2] = {{1000000000, 0}, {1000000000, 0}};
           futimens(fd, t) == 0 && fstat(fd, &s) == 0 && s.st_mtime == 1000000000; }));
    EXPECT("utimensat", ({ struct timespec t[2] = {{0, UTIME_OMIT}, {1100000000, 0}};
           utimensat(AT_FDCWD, "cx_a", t, 0) == 0 && stat("cx_a", &s) == 0 && s.st_mtime == 1100000000; }));
    EXPECT("fallocate grows a file", ({ int r = posix_fallocate(g, 0, 8192); r == 0; }) && fstat(g, &s) == 0 && s.st_size == 8192);
    EXPECT("posix_fadvise", posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) == 0);
    EXPECT("dup3 O_CLOEXEC", ({ int d = dup3(fd, 15, O_CLOEXEC); d == 15 && (fcntl(d, F_GETFD) & FD_CLOEXEC) && close(d) == 0; }));
    EXPECT("open O_CLOEXEC", ({ int d = open("cx_a", O_RDONLY | O_CLOEXEC); d >= 0 && (fcntl(d, F_GETFD) & FD_CLOEXEC) && close(d) == 0; }));
    EXPECT("statx", ({ unsigned long long x[32] = {0};             /* stx_size is the u64 at byte 40 */
           syscall(SYS_statx, AT_FDCWD, "cx_a", 0, 0x200, x) == 0 && x[5] == 10; }));
    EXPECT("fstatat with AT_SYMLINK_NOFOLLOW", fstatat(AT_FDCWD, "cx_a", &s, AT_SYMLINK_NOFOLLOW) == 0 && s.st_size == 10);
    EXPECT("fstatat AT_EMPTY_PATH", fstatat(fd, "", &s, AT_EMPTY_PATH) == 0 && s.st_size == 10);
    EXPECT("mkdirat and unlinkat AT_REMOVEDIR", mkdirat(AT_FDCWD, "cx_d", 0755) == 0 && unlinkat(AT_FDCWD, "cx_d", AT_REMOVEDIR) == 0 &&
           stat("cx_d", &s) == -1 && errno == ENOENT);
    EXPECT("fchmodat", fchmodat(AT_FDCWD, "cx_a", 0600, 0) == 0 && stat("cx_a", &s) == 0 && (s.st_mode & 0777) == 0600);
    EXPECT("renameat2 RENAME_NOREPLACE", syscall(SYS_renameat2, AT_FDCWD, "cx_a", AT_FDCWD, "cx_b", 1) == -1 && errno == EEXIST);
    EXPECT("readdir counts the files", ({ DIR *d = opendir("."); int n = 0; struct dirent *e;
           while (d && (e = readdir(d))) n += strncmp(e->d_name, "cx_", 3) == 0; d && closedir(d) == 0 && n == 2; }));
    close(fd); close(g);
    unlink("cx_a"); unlink("cx_b");
    EXPECT("memfd_create", (fd = memfd_create("cx", MFD_CLOEXEC)) >= 0 && write(fd, "mem", 3) == 3 && pread(fd, b, 3, 0) == 3 &&
           memcmp(b, "mem", 3) == 0 && close(fd) == 0);
    EXPECT("a full nonblocking pipe is EAGAIN", pipe2(p, O_NONBLOCK) == 0 && ({ static char big[4096]; int n = 0, r;
           while ((r = write(p[1], big, sizeof big)) > 0 && n < 1 << 24) n += r; r == -1 && errno == EAGAIN && n >= 4096; }));
    close(p[0]); close(p[1]);
    EXPECT("signalfd", ({ sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR2); sigprocmask(SIG_BLOCK, &m, 0);
           int f = signalfd(-1, &m, 0); struct signalfd_siginfo i;
           f >= 0 && kill(getpid(), SIGUSR2) == 0 && read(f, &i, sizeof i) == sizeof i && i.ssi_signo == SIGUSR2 && close(f) == 0; }));
    EXPECT("prlimit", ({ struct rlimit r; prlimit(0, RLIMIT_NOFILE, 0, &r) == 0 && r.rlim_cur > 0; }));
    EXPECT("sched_getcpu", sched_getcpu() >= 0);
    EXPECT("posix_spawn", ({ char *a[] = {argv[0], "child", 0}; posix_spawn(&c, argv[0], 0, 0, a, environ) == 0 &&
           waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 7; }));
    EXPECT("vfork + _exit", (c = vfork()) >= 0 && (c == 0 ? (_exit(7), 0) : waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 7));
    EXPECT("getrusage children", ({ struct rusage r; getrusage(RUSAGE_CHILDREN, &r) == 0; }));
    return done();
}
