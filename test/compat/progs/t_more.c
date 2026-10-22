/* System calls the other programs leave out that real programs still make:
 * copying between descriptors, locks, the *at family with flags, vectored
 * and positioned I/O, file times, memfd and the other descriptor kinds,
 * process spawning the way libc does it. */
#include "compat.h"
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>

extern char **environ;

/* Half a page of file, mapped MAP_SHARED, the rest of the page written
 * through the mapping (past the end of the file), unmapped and closed;
 * then the file grows by ftruncate (how 0), a write at the end of the
 * page (1) or a write past it (2), or stays as it is (3). The bytes past
 * the old end must read as zeros, by read and by a new mapping: they were
 * never the file's (POSIX: the partial page is zero-filled, and what is
 * written past the end is never written out). */
static int grow_zero(int how) {
    const long ps = 4096;
    char buf[4096];
    int ok = 1, fd = open("cm_grow", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    memset(buf, 'a', ps / 2);
    char *m = write(fd, buf, ps / 2) == ps / 2 ? mmap(0, ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (m == MAP_FAILED) { close(fd); unlink("cm_grow"); return 0; }
    memset(m + ps / 2, 'b', ps / 2);
    munmap(m, ps);
    close(fd);
    fd = open("cm_grow", O_RDWR);
    if (how == 0) ok = ftruncate(fd, ps) == 0;
    else if (how == 1) ok = pwrite(fd, "", 1, ps - 1) == 1;
    else if (how == 2) ok = pwrite(fd, "z", 1, 2 * ps) == 1;
    if (how != 3) {
        ok = ok && pread(fd, buf, ps - 1, 0) == ps - 1;
        for (long i = ps / 2; ok && i < ps - 1; i++) ok = buf[i] == 0;
    }
    m = mmap(0, ps, PROT_READ, MAP_SHARED, fd, 0);
    ok = ok && m != MAP_FAILED;
    for (long i = ps / 2; ok && i < ps - 1; i++) ok = m[i] == 0;
    if (m != MAP_FAILED) munmap(m, ps);
    close(fd);
    unlink("cm_grow");
    return ok;
}

/* Eighty named semaphores, each mapped with its descriptor closed at once
 * (as sem_open does), then the last one mapped again by name: the new
 * mapping must be the same page. Past a process's 64 recorded shared
 * mappings the page cache used to drop the page at that close, and
 * another process opening the semaphore by name (multiprocessing's spawn)
 * got a page of its own. */
static int sem_many(void) {
    enum { N = 80 };
    sem_t *s[N];
    char n[32];
    int made = 0, ok = 1;
    for (; made < N && ok; made++) {
        snprintf(n, sizeof n, "/cm_many%d", made);
        sem_unlink(n);
        s[made] = sem_open(n, O_CREAT | O_EXCL, 0600, 0);
        if (s[made] == SEM_FAILED) { ok = 0; break; }
    }
    if (ok) {
        int f = open("/dev/shm/cm_many79", O_RDWR), v = -1;
        sem_t *m = f >= 0 ? mmap(0, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED, f, 0) : MAP_FAILED;
        if (f >= 0) close(f);
        ok = m != MAP_FAILED && sem_post(s[N - 1]) == 0 && sem_getvalue(m, &v) == 0 && v == 1 &&
             sem_trywait(m) == 0 && sem_trywait(s[N - 1]) == -1 && errno == EAGAIN;
        if (m != MAP_FAILED) munmap(m, sizeof(sem_t));
    }
    for (int i = 0; i < made; i++) {
        snprintf(n, sizeof n, "/cm_many%d", i);
        sem_close(s[i]);
        sem_unlink(n);
    }
    return ok;
}

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
    EXPECT("splice a pipe into a file", ({ loff_t o = 20; splice(p[0], 0, g, &o, 3, 0) == 3 && o == 23; }) &&
           pread(g, b, 3, 20) == 3 && memcmp(b, "abc", 3) == 0);
    EXPECT("splice a file into a pipe", ({ loff_t o = 5; splice(fd, &o, p[1], 0, 3, 0) == 3 && o == 8; }) &&
           ({ int q[2]; pipe(q) == 0 && tee(p[0], q[1], 3, 0) == 3 && read(q[0], b, 3) == 3 && memcmp(b, "567", 3) == 0 &&
              close(q[0]) == 0 && close(q[1]) == 0; }) && read(p[0], b, 3) == 3 && memcmp(b, "567", 3) == 0);
    EXPECT("splice rejects two files and a pipe offset", ({ loff_t o = 0; splice(fd, 0, g, 0, 1, 0) == -1 && errno == EINVAL &&
           splice(p[0], &o, g, 0, 1, 0) == -1 && errno == ESPIPE; }));
    EXPECT("vmsplice into a pipe, EAGAIN on an empty tee", ({ struct iovec v = {"xy", 2}; int q[2]; pipe(q);
           int r = vmsplice(p[1], &v, 1, 0) == 2 && read(p[0], b, 2) == 2 && memcmp(b, "xy", 2) == 0 &&
                   tee(p[0], q[1], 1, SPLICE_F_NONBLOCK) == -1 && errno == EAGAIN; close(q[0]); close(q[1]); r; }));
    EXPECT("sendfile needs a readable source", ({ int w = open("cx_a", O_WRONLY); off_t o = 0;
           int r = sendfile(g, w, &o, 1) == -1 && errno == EBADF; close(w); r; }));
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
    EXPECT("utimensat and llistxattr act on a dangling link itself", ({ struct timespec t[2] = {{1200000000, 0}, {1200000000, 0}};
           char x[64]; unlink("cx_dl"); int ok = symlink("cx_none", "cx_dl") == 0
               && utimensat(AT_FDCWD, "cx_dl", t, AT_SYMLINK_NOFOLLOW) == 0 && lstat("cx_dl", &s) == 0 && s.st_mtime == 1200000000
               && llistxattr("cx_dl", x, sizeof x) == 0 && utimensat(AT_FDCWD, "cx_dl", t, 0x2) == -1 && errno == EINVAL;
           unlink("cx_dl"); ok; }));
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
    /* numpy and psutil: ru_maxrss, the memory policy calls, the memory and context-switch lines of /proc/PID/status, and SIOCETHTOOL. */
    EXPECT("getrusage self reports a maximum resident size", ({ struct rusage r; getrusage(RUSAGE_SELF, &r) == 0 && r.ru_maxrss > 0; }));
    EXPECT("mbind and set_mempolicy succeed; get_mempolicy says MPOL_DEFAULT", ({ char *pg = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); int mode = -1;
           pg != MAP_FAILED && syscall(SYS_mbind, (long)pg, 4096L, 0L, 0L, 0L, 0L) == 0 && syscall(SYS_set_mempolicy, 0L, 0L, 0L) == 0
           && syscall(SYS_get_mempolicy, (long)&mode, 0L, 0L, 0L, 0L) == 0 && mode == 0; }));
    EXPECT("/proc/PID/status has VmRSS and the context switch counts", ({ char b[4096]; int f = open("/proc/self/status", O_RDONLY);
           int n = f < 0 ? -1 : read(f, b, sizeof b - 1); if (n > 0) b[n] = 0; if (f >= 0) close(f);
           n > 0 && strstr(b, "\nVmRSS:\t") && strstr(b, "\nvoluntary_ctxt_switches:\t") && strstr(b, "\nnonvoluntary_ctxt_switches:\t"); }));
    EXPECT("/proc/meminfo has Active, Inactive and Shmem", ({ char b[4096]; int f = open("/proc/meminfo", O_RDONLY);
           int n = f < 0 ? -1 : read(f, b, sizeof b - 1); if (n > 0) b[n] = 0; if (f >= 0) close(f);
           n > 0 && strstr(b, "\nActive:") && strstr(b, "\nInactive:") && strstr(b, "\nShmem:"); }));
    EXPECT("SIOCETHTOOL on a socket is EOPNOTSUPP", ({ int s = socket(AF_INET, SOCK_DGRAM, 0); unsigned ec[16] = { 1 }; /* ETHTOOL_GSET */ char ifr[40] = "lo"; void *d = ec;
           memcpy(ifr + 16, &d, sizeof d); s >= 0 && ioctl(s, 0x8946, ifr) == -1 && errno == EOPNOTSUPP && close(s) == 0; }));
    /* What CPython's socket, os and shutil modules lean on. */
    EXPECT("ioctl FIONBIO makes a read EAGAIN", ({ int one = 1; char ch; pipe(p) == 0 && ioctl(p[0], FIONBIO, &one) == 0 &&
           (fcntl(p[0], F_GETFL) & O_NONBLOCK) && read(p[0], &ch, 1) == -1 && errno == EAGAIN; }));
    close(p[0]); close(p[1]);
    EXPECT("ioctl FIOCLEX and FIONCLEX", ({ int f = dup(1); f >= 0 && ioctl(f, FIOCLEX) == 0 && fcntl(f, F_GETFD) == FD_CLOEXEC &&
           ioctl(f, FIONCLEX) == 0 && fcntl(f, F_GETFD) == 0 && close(f) == 0; }));
    EXPECT("bind to port 0 names a port before listen", ({ struct sockaddr_in a = {.sin_family = AF_INET,
           .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; socklen_t l = sizeof a; int s = socket(AF_INET, SOCK_STREAM, 0);
           s >= 0 && bind(s, (void *)&a, sizeof a) == 0 && getsockname(s, (void *)&a, &l) == 0 && a.sin_port != 0 && close(s) == 0; }));
    EXPECT("O_NONBLOCK by fcntl on a listening socket", ({ struct sockaddr_in a = {.sin_family = AF_INET,
           .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; int s = socket(AF_INET, SOCK_STREAM, 0); s >= 0 &&
           bind(s, (void *)&a, sizeof a) == 0 && listen(s, 1) == 0 && fcntl(s, F_SETFL, O_NONBLOCK) == 0 &&
           (fcntl(s, F_GETFL) & O_NONBLOCK) && accept(s, 0, 0) == -1 && errno == EAGAIN && close(s) == 0; }));
    EXPECT("fstat on a directory agrees with stat", ({ struct stat a, b; mkdir("cm_fsd", 0755) == 0 &&
           ({ int d = open("cm_fsd", O_RDONLY | O_DIRECTORY); d >= 0 && fstat(d, &a) == 0 && close(d) == 0; }) &&
           stat("cm_fsd", &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev && rmdir("cm_fsd") == 0; }));
    EXPECT("sem_open in /dev/shm", ({ sem_t *m = sem_open("/cm_sem", O_CREAT | O_EXCL, 0600, 1);
           m != SEM_FAILED && sem_wait(m) == 0 && sem_post(m) == 0 && sem_close(m) == 0 && sem_unlink("/cm_sem") == 0; }));
    EXPECT("sem_open past 64 mappings: mapping one again by name gives the same page", sem_many());
    EXPECT("what a mapping wrote past the end of a file reads as zeros", grow_zero(0) && grow_zero(1) && grow_zero(2) && grow_zero(3));
    return done();
}
