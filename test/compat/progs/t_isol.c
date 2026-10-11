/* Isolation between users: what an unprivileged process may not do to the
 * rest of the system. Each check drops to nobody in a child: it may not
 * reboot, mount or unmount, signal a root process through
 * rt_sigqueueinfo, or bind a port below 1024; and on BastiOS it may not
 * go around the Linux layer through the native system calls (int 0x80),
 * whose file, kill and power calls check nothing and so are root's. */
#include "compat.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <netinet/in.h>

/* reboot(2)'s magic numbers, and a command that changes nothing. */
#define RB_MAGIC1 0xfee1dead
#define RB_MAGIC2 672274793
#define RB_CAD_ON 0x89abcdef

#define NOBODY 65534
#define USERS 100

static int in_child(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) _exit(f() ? 0 : 1);
    int st; return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int become_nobody(void) { return setgid(USERS) == 0 && setuid(NOBODY) == 0; }

static int no_reboot(void) {
    if (!become_nobody()) return 0;
    /* RB_CAD_ON changes nothing even if it got through. */
    return syscall(SYS_reboot, RB_MAGIC1, RB_MAGIC2, RB_CAD_ON, 0) == -1
        && errno == EPERM;
}

static int no_mount(void) {
    if (!become_nobody()) return 0;
    int ok = mount("none", "i_mnt", "tmpfs", 0, NULL) == -1 && errno == EPERM;
    return ok && umount2("i_mnt", 0) == -1 && errno == EPERM;
}

static int no_sigqueue(void) {
    pid_t root = getppid();
    if (!become_nobody()) return 0;
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = SIGUSR1;
    si.si_code = SI_QUEUE;
    si.si_pid = getpid();
    si.si_uid = NOBODY;
    int ok = syscall(SYS_rt_sigqueueinfo, root, SIGUSR1, &si) == -1 && errno == EPERM;
    signal(SIGUSR2, SIG_IGN);
    si.si_signo = SIGUSR2;
    return ok && syscall(SYS_rt_sigqueueinfo, getpid(), SIGUSR2, &si) == 0;   /* its own: fine */
}

static int bind_port(int type, int port) {
    int s = socket(AF_INET, type, 0);
    if (s < 0) return -2;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int r = bind(s, (struct sockaddr *)&a, sizeof a);
    int e = errno;
    close(s);
    errno = e;
    return r;
}

static int low_ports(void) {
    if (!become_nobody()) return 0;
    int ok = bind_port(SOCK_DGRAM, 81) == -1 && errno == EACCES;
    ok = ok && bind_port(SOCK_STREAM, 82) == -1 && errno == EACCES;
    return ok && bind_port(SOCK_DGRAM, 5381) == 0 && bind_port(SOCK_STREAM, 5382) == 0;
}

static int root_binds_low(void) {
    return bind_port(SOCK_DGRAM, 83) == 0 && bind_port(SOCK_STREAM, 84) == 0;
}

/* BastiOS's own system calls (src/syscall.bastio), raised with int 0x80. */
static long native(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

static int no_native(void) {
    pid_t root = getppid();
    if (!become_nobody()) return 0;
    const char *name = "i_secret";
    int ok = native(11, 0, (long)name, strlen(name)) == -1;           /* SYS_OPEN */
    ok = ok && native(15, 0, (long)name, strlen(name)) == -1;         /* SYS_UNLINK */
    ok = ok && native(9, root, 0, 0) == 1;                            /* SYS_KILL of root */
    struct stat st;
    return ok && stat(name, &st) == 0;                                /* still there */
}

static int bastios(void) {
    struct utsname u;
    return uname(&u) == 0 && strstr(u.release, "bastios") != NULL;
}

int main(void) {
    setup();
    mkdir("i_mnt", 0777);
    chmod("i_mnt", 0777);
    int fd = open("i_secret", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    close(fd);

    EXPECT("reboot is root's", in_child(no_reboot));
    EXPECT("mount and umount2 are root's", in_child(no_mount));
    EXPECT("rt_sigqueueinfo follows kill's rule", in_child(no_sigqueue));
    EXPECT("ports below 1024 are root's", in_child(low_ports));
    EXPECT("root binds a port below 1024", root_binds_low());
    if (bastios())
        EXPECT("native system calls are root's", in_child(no_native));

    unlink("i_secret");
    rmdir("i_mnt");
    return done();
}
