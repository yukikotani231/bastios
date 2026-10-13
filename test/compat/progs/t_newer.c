/* Calls newer kernels added beside older ones, the old ones they replaced,
 * and the corners LTP found around them: preadv2 and pwritev2, the old
 * getdents, "." and "..", seeking in a directory, sysfs, ustat, fchmodat2,
 * execveat, O_PATH handles and empty paths; and the calls on other
 * processes: pidfds, kcmp, process_vm_readv and process_vm_writev. */
#include "compat.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#ifndef SYS_fchmodat2
#define SYS_fchmodat2 452
#endif

extern char **environ;

/* The old getdents's record, which libc no longer declares. */
struct old_dirent {
    unsigned long d_ino;
    unsigned long d_off;
    unsigned short d_reclen;
    char d_name[];
};

static int has_name(const char *buf, long n, int old, const char *name, int *type) {
    for (long at = 0; at < n;) {
        unsigned short reclen;
        const char *nm;
        int t;
        if (old) {
            const struct old_dirent *d = (const void *)(buf + at);
            reclen = d->d_reclen;
            nm = d->d_name;
            t = buf[at + reclen - 1];
        } else {
            const struct dirent *d = (const void *)(buf + at);
            reclen = d->d_reclen;
            nm = d->d_name;
            t = d->d_type;
        }
        if (strcmp(nm, name) == 0) {
            *type = t;
            return 1;
        }
        if (reclen == 0) return 0;
        at += reclen;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1)
        return 7;                                   /* run by the execveat checks */
    setup();
    char buf[1024], a[4], b[4];
    struct stat st;
    int fd, t;

    fd = open("nw_f", O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT("pwritev2 at an offset", fd >= 0 && write(fd, "abcdefgh", 8) == 8 &&
           syscall(SYS_pwritev2, fd, &(struct iovec){"XY", 2}, 1, 2L, 0L, 0) == 2 &&
           lseek(fd, 0, SEEK_CUR) == 8 && pread(fd, buf, 8, 0) == 8 && memcmp(buf, "abXYefgh", 8) == 0);
    {
        struct iovec v[2] = {{a, 2}, {b, 2}};
        EXPECT("preadv2 at an offset", syscall(SYS_preadv2, fd, v, 2, 1L, 0L, 0) == 4 &&
               memcmp(a, "bX", 2) == 0 && memcmp(b, "Ye", 2) == 0 && lseek(fd, 0, SEEK_CUR) == 8);
        EXPECT("preadv2 at -1 uses and moves the offset", lseek(fd, 4, SEEK_SET) == 4 &&
               syscall(SYS_preadv2, fd, v, 2, -1L, 0L, 0) == 4 && memcmp(a, "ef", 2) == 0 &&
               lseek(fd, 0, SEEK_CUR) == 8);
        EXPECT("pwritev2 at -1 appends at the offset", syscall(SYS_pwritev2, fd, &(struct iovec){"ij", 2}, 1, -1L, 0L, 0) == 2 &&
               fstat(fd, &st) == 0 && st.st_size == 10);
        EXPECT("preadv2 with RWF_HIPRI", syscall(SYS_preadv2, fd, v, 1, 0L, 0L, 1) == 2 && memcmp(a, "ab", 2) == 0);
        EXPECT("preadv2 with an unknown flag", syscall(SYS_preadv2, fd, v, 1, 0L, 0L, 0x10000) == -1 && errno == EOPNOTSUPP);
        EXPECT("preadv2 at -2", syscall(SYS_preadv2, fd, v, 1, -2L, 0L, 0) == -1 && errno == EINVAL);
    }
    close(fd);

    mkdir("nw_d", 0755);
    close(open("nw_d/one", O_CREAT | O_WRONLY, 0644));
    mkdir("nw_d/sub", 0755);
    fd = open("nw_d", O_RDONLY | O_DIRECTORY);
    long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
    EXPECT("getdents64 lists . and ..", n > 0 && has_name(buf, n, 0, ".", &t) && t == DT_DIR &&
           has_name(buf, n, 0, "..", &t) && t == DT_DIR);
    EXPECT("getdents64 at the end", syscall(SYS_getdents64, fd, buf, sizeof buf) == 0);
    EXPECT("lseek rewinds a directory", lseek(fd, 0, SEEK_SET) == 0);
#ifdef SYS_getdents
    n = syscall(SYS_getdents, fd, buf, sizeof buf);
    EXPECT("getdents (the old one)", n > 0 && has_name(buf, n, 1, "one", &t) && t == DT_REG &&
           has_name(buf, n, 1, "sub", &t) && t == DT_DIR && has_name(buf, n, 1, "..", &t));
#endif
    EXPECT("getdents into a byte", lseek(fd, 0, SEEK_SET) == 0 && syscall(SYS_getdents64, fd, buf, 1) == -1 && errno == EINVAL);
    close(fd);
    {
        DIR *d = opendir("nw_d");
        int first = 0, again = 0;
        struct dirent *e;
        while (d && (e = readdir(d))) first++;
        if (d) rewinddir(d);
        while (d && (e = readdir(d))) again++;
        EXPECT("rewinddir", d && first == 4 && again == 4);
        long pos = 0;
        if (d) {
            rewinddir(d);
            readdir(d);
            pos = telldir(d);
            e = readdir(d);
            strcpy(a, e ? e->d_name : "");
            seekdir(d, pos);
            e = readdir(d);
        }
        EXPECT("telldir and seekdir", d && e && strcmp(a, e->d_name) == 0);
        if (d) closedir(d);
    }

#ifdef SYS_sysfs
    EXPECT("sysfs counts file system types", syscall(SYS_sysfs, 3) >= 3);
    {
        long i = syscall(SYS_sysfs, 1, "tmpfs");
        char name[64] = "";
        EXPECT("sysfs names and numbers agree", i >= 0 && syscall(SYS_sysfs, 2, i, name) == 0 && strcmp(name, "tmpfs") == 0);
    }
    EXPECT("sysfs with an unknown name", syscall(SYS_sysfs, 1, "nofs") == -1 && errno == EINVAL);
    {
        char u[32];
        EXPECT("ustat for /'s device", stat("/", &st) == 0 && syscall(SYS_ustat, (unsigned)st.st_dev, u) == 0);
    }
#endif

    symlink("nw_f", "nw_l");
    EXPECT("fchmodat2 follows a link", syscall(SYS_fchmodat2, AT_FDCWD, "nw_l", 0600, 0) == 0 &&
           stat("nw_f", &st) == 0 && (st.st_mode & 0777) == 0600);
    EXPECT("fchmodat2 on a link itself", syscall(SYS_fchmodat2, AT_FDCWD, "nw_l", 0644, AT_SYMLINK_NOFOLLOW) == -1 && errno == EOPNOTSUPP);
    EXPECT("fchmodat2 with bad flags", syscall(SYS_fchmodat2, AT_FDCWD, "nw_f", 0644, -1) == -1 && errno == EINVAL);
    fd = open("nw_f", O_PATH);
    EXPECT("fchmodat2 with AT_EMPTY_PATH", fd >= 0 && syscall(SYS_fchmodat2, fd, "", 0640, AT_EMPTY_PATH) == 0 &&
           stat("nw_f", &st) == 0 && (st.st_mode & 0777) == 0640);
    EXPECT("O_PATH: read is EBADF", read(fd, buf, 1) == -1 && errno == EBADF);
    EXPECT("O_PATH: fchmod is EBADF", syscall(SYS_fchmod, fd, 0644) == -1 && errno == EBADF);
    EXPECT("O_PATH: fstat works", fstat(fd, &st) == 0 && st.st_size == 10);
    close(fd);
    EXPECT("an empty path is ENOENT", stat("", &st) == -1 && errno == ENOENT);
    EXPECT("a path in unmapped memory is EFAULT", stat((char *)8, &st) == -1 && errno == EFAULT);


    {
        int pfd = syscall(SYS_pidfd_open, getpid(), 0);
        int fdf = fcntl(pfd, F_GETFD);
        EXPECT("pidfd_open of itself is close-on-exec", pfd >= 0 && fdf >= 0 && (fdf & FD_CLOEXEC));
        EXPECT("pidfd_open with bad flags", syscall(SYS_pidfd_open, getpid(), 1) == -1 && errno == EINVAL);
        int pp[2];
        pipe(pp);
        pid_t c = fork();
        if (c == 0) {
            char x;
            read(pp[0], &x, 1);
            _exit(5);
        }
        int cfd = syscall(SYS_pidfd_open, c, 0);
        struct pollfd pf = {cfd, POLLIN, 0};
        EXPECT("poll on a running child's pidfd", cfd >= 0 && poll(&pf, 1, 0) == 0);
        int tfd = syscall(SYS_pidfd_getfd, cfd, pp[1], 0);
        EXPECT("pidfd_getfd takes the child's pipe end", tfd >= 0 && syscall(SYS_kcmp, getpid(), getpid(), 0, tfd, pp[1]) == 0 &&
               syscall(SYS_kcmp, getpid(), c, 0, tfd, pp[0]) != 0);
        EXPECT("pidfd_send_signal 0", syscall(SYS_pidfd_send_signal, cfd, 0, NULL, 0) == 0);
        if (tfd >= 0) write(tfd, "x", 1);
        EXPECT("poll on an ended child's pidfd", poll(&pf, 1, 5000) == 1 && (pf.revents & POLLIN));
        siginfo_t si = {0};
        EXPECT("waitid P_PIDFD", waitid(P_PIDFD, cfd, &si, WEXITED) == 0 && si.si_pid == c && si.si_status == 5);
        EXPECT("pidfd_send_signal to a reaped child", syscall(SYS_pidfd_send_signal, cfd, SIGTERM, NULL, 0) == -1 && errno == ESRCH);
        EXPECT("pidfd_send_signal on a file", syscall(SYS_pidfd_send_signal, pp[0], SIGTERM, NULL, 0) == -1 && errno == EBADF);
        close(pp[0]);
        close(pp[1]);
        close(tfd);
        close(cfd);
        close(pfd);
    }
    {
        char src[32] = "copied across processes", dst[32] = "";
        struct iovec l = {dst, sizeof src}, r = {src, sizeof src};
        EXPECT("process_vm_readv of itself", syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == (long)sizeof src &&
               strcmp(dst, src) == 0);
        pid_t c = fork();
        if (c == 0) {
            pause();
            _exit(0);
        }
        char buf2[32] = "";
        struct iovec l2 = {buf2, sizeof src};
        EXPECT("process_vm_readv of a child", syscall(SYS_process_vm_readv, c, &l2, 1, &r, 1, 0) == (long)sizeof src &&
               strcmp(buf2, src) == 0);
        struct iovec w = {"written", 8};
        EXPECT("process_vm_writev to a child and back", syscall(SYS_process_vm_writev, c, &w, 1, &r, 1, 0) == 8 &&
               syscall(SYS_process_vm_readv, c, &l2, 1, &r, 1, 0) == (long)sizeof src && strcmp(buf2, "written") == 0 &&
               strcmp(src, "copied across processes") == 0);
        EXPECT("process_vm_readv with flags", syscall(SYS_process_vm_readv, c, &l2, 1, &r, 1, 1) == -1 && errno == EINVAL);
        struct iovec bad = {(void *)8, 8};
        EXPECT("process_vm_readv from unmapped memory", syscall(SYS_process_vm_readv, c, &l2, 1, &bad, 1, 0) == -1 && errno == EFAULT);
        kill(c, SIGKILL);
        waitpid(c, NULL, 0);
        EXPECT("process_vm_readv of a reaped child", syscall(SYS_process_vm_readv, c, &l2, 1, &r, 1, 0) == -1 && errno == ESRCH);
    }
    /* A child that has just been forked may still be running on another
     * processor when the parent writes into one of its copy-on-write pages:
     * the write waits for the page to be copied, and the child never takes
     * a fault for the page the kernel has already copied. */
    {
        static char target[64] = "before";
        int good = 0;
        for (int i = 0; i < 120; i++) {
            pid_t c = fork();
            if (c == 0) {
                pause();
                _exit(0);
            }
            struct iovec w = {"after", 6}, r = {target, 6};
            int st = 0;
            long n = syscall(SYS_process_vm_writev, c, &w, 1, &r, 1, 0);
            int alive = waitpid(c, &st, WNOHANG) == 0;
            kill(c, SIGKILL);
            waitpid(c, NULL, 0);
            if (n == 6 && alive && strcmp(target, "before") == 0)
                good++;
        }
        EXPECT("process_vm_writev to a child just forked, repeatedly", good == 120);
    }
    {
        int status = -1;
        pid_t p = fork();
        if (p == 0) {
            char *av[] = {argv[0], "child", NULL};
            syscall(SYS_execveat, AT_FDCWD, argv[0], av, environ, 0);
            _exit(99);
        }
        EXPECT("execveat a path", p > 0 && waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 7);
        fd = open(argv[0], O_PATH | O_CLOEXEC);
        p = fork();
        if (p == 0) {
            char *av[] = {argv[0], "child", NULL};
            syscall(SYS_execveat, fd, "", av, environ, AT_EMPTY_PATH);
            _exit(99);
        }
        EXPECT("execveat a descriptor (fexecve)", fd >= 0 && p > 0 && waitpid(p, &status, 0) == p &&
               WIFEXITED(status) && WEXITSTATUS(status) == 7);
        close(fd);
        EXPECT("execveat with bad flags", syscall(SYS_execveat, AT_FDCWD, argv[0], (char *[]){NULL}, environ, 1) == -1 &&
               errno == EINVAL);
        EXPECT("execveat of a link with AT_SYMLINK_NOFOLLOW", symlink(argv[0], "nw_x") == 0 &&
               syscall(SYS_execveat, AT_FDCWD, "nw_x", (char *[]){NULL}, environ, AT_SYMLINK_NOFOLLOW) == -1 && errno == ELOOP);
    }
    /* membarrier: a query answers with the commands there are (none here,
     * a mask on Linux); an unknown command is EINVAL. */
    EXPECT("membarrier query", syscall(SYS_membarrier, 0, 0) >= 0);
    EXPECT("membarrier unknown command", syscall(SYS_membarrier, 1 << 30, 0) == -1 && errno == EINVAL);
    unlink("nw_x");
    unlink("nw_l");
    unlink("nw_f");
    unlink("nw_d/one");
    rmdir("nw_d/sub");
    rmdir("nw_d");
    return done();
}
