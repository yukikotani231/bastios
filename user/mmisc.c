/* Small Linux system calls, against musl: uname and gethostname, readv,
 * pread and pwrite, limits, sysinfo, statfs, getrandom, priorities, umask,
 * sessions and sched_yield. */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("mmisc: %s failed (errno %d)\n", what, errno);
    return 1;
}

int main(void)
{
    struct utsname u;
    char host[64];
    if (uname(&u) != 0 || strcmp(u.sysname, "Linux") != 0 || strcmp(u.machine, "x86_64") != 0)
        return fail("uname");
    if (gethostname(host, sizeof host) != 0 || strcmp(host, "bastios") != 0)
        return fail("gethostname");

    int fd = open("mm", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "abcdefgh", 8) != 8)
        return fail("write");
    char x[4] = { 0 }, y[4] = { 0 };
    if (pread(fd, x, 3, 2) != 3 || strcmp(x, "cde") != 0 || lseek(fd, 0, SEEK_CUR) != 8)
        return fail("pread");
    if (pwrite(fd, "XY", 2, 0) != 2 || lseek(fd, 0, SEEK_CUR) != 8)
        return fail("pwrite");
    lseek(fd, 0, SEEK_SET);
    struct iovec iov[2] = { { x, 0 }, { y, 3 } };
    if (readv(fd, iov, 2) <= 0 || strncmp(y, "XYc", 3) != 0)
        return fail("readv");
    if (fsync(fd) != 0)
        return fail("fsync");
    close(fd);
    if (chmod("mm", 0600) != 0 || unlink("mm") != 0)
        return fail("chmod");

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == 0)
        return fail("getrlimit");
    struct sysinfo si;
    if (sysinfo(&si) != 0 || si.totalram == 0 || si.procs == 0)
        return fail("sysinfo");
    struct statfs sf;
    if (statfs("/", &sf) != 0 || sf.f_bsize != 4096)
        return fail("statfs");
    unsigned char r[16];
    if (getrandom(r, sizeof r, 0) != sizeof r)
        return fail("getrandom");
    errno = 0;
    if (getpriority(PRIO_PROCESS, 0) != 0 || errno != 0 || setpriority(PRIO_PROCESS, 0, 5) != 0)
        return fail("priorities");
    umask(022);
    if (sched_yield() != 0)
        return fail("sched_yield");
    if (setsid() != getpid() || getsid(0) != getpid() || getpgrp() != getpid())
        return fail("sessions");
    printf("mmisc: ok on %s %s\n", u.sysname, u.machine);
    return 0;
}
