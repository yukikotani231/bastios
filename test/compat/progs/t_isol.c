/* Isolation between users: what an unprivileged process may not do to the
 * rest of the system. Each check drops to nobody in a child: it may not
 * reboot, mount or unmount, write to a file it may only read through a
 * shared mapping made writable with mprotect, connect to a unix socket
 * whose file it may not write, signal a root process through
 * rt_sigqueueinfo, or bind a port below 1024; and on BastiOS it may not
 * go around the Linux layer through the native system calls (int 0x80),
 * whose file, kill and power calls check nothing and so are root's.
 * A set-user-ID file in a loop-mounted image does not raise its runner:
 * the image's maker chose its owners and modes (BastiOS mounts them
 * nosuid). It may not read where a root process's /proc exe and fd links
 * lead, nor open /dev/console, nor SHM_LOCK under a zero RLIMIT_MEMLOCK;
 * and a SysV set's group bits reach it through a supplementary group. */
#include "compat.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <grp.h>
#include <sys/syscall.h>
#include <sys/un.h>
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

/* A file it may only read, mapped shared: mprotect refuses PROT_WRITE,
 * and the file keeps its bytes. */
static int no_mprotect_write(void) {
    if (!become_nobody()) return 0;
    int fd = open("i_public", O_RDONLY);
    char *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (fd < 0 || p == MAP_FAILED || p[0] != 'p') return 0;
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) == 0) {
        p[0] = 'X';
        return 0;
    }
    if (errno != EACCES) return 0;
    munmap(p, 4096);
    char c = 0;
    int ok = pread(fd, &c, 1, 0) == 1 && c == 'p';
    close(fd);
    return ok;
}

/* Unix sockets bound to a path: the socket file's mode says who may
 * connect, as for any file. */
static struct sockaddr_un usock(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    return a;
}

static int try_connect(const char *path) {
    struct sockaddr_un a = usock(path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    int e = errno;
    close(s);
    errno = e;
    return r;
}

static int nobody_connect_refused(void) {
    return become_nobody() && try_connect("i_usock") == -1 && errno == EACCES;
}

static int nobody_connects(void) {
    return become_nobody() && try_connect("i_usock") == 0;
}

static int nobody_binds_in_root_dir(void) {
    if (!become_nobody()) return 0;
    struct sockaddr_un a = usock("i_rdir/s");
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    int ok = bind(s, (struct sockaddr *)&a, sizeof a) == -1 && errno == EACCES;
    close(s);
    return ok;
}

static int unix_socket_files(void) {
    struct sockaddr_un a = usock("i_usock");
    unlink("i_usock");
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct stat st;
    int ok = s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 4) == 0
        && lstat("i_usock", &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == 0;
    ok = ok && open("i_usock", O_RDONLY) == -1 && errno == ENXIO;
    ok = ok && chmod("i_usock", 0700) == 0 && in_child(nobody_connect_refused);
    ok = ok && chmod("i_usock", 0777) == 0 && in_child(nobody_connects);
    close(s);
    int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    ok = ok && bind(s2, (struct sockaddr *)&a, sizeof a) == -1 && errno == EADDRINUSE;   /* the file stays */
    ok = ok && try_connect("i_usock") == -1 && errno == ECONNREFUSED;
    close(s2);
    mkdir("i_rdir", 0755);
    ok = ok && in_child(nobody_binds_in_root_dir);
    rmdir("i_rdir");
    unlink("i_usock");
    return ok;
}

/* Loop devices are root's and group disk's (6). */
static int nobody_no_loop(void) {
    return become_nobody() && open("/dev/loop0", O_RDONLY) == -1 && errno == EACCES;
}

static int disk_member_loop(void) {
    gid_t disk = 6;
    if (setgroups(1, &disk) != 0 || !become_nobody()) return 0;
    int fd = open("/dev/loop0", O_RDONLY);
    close(fd);
    return fd >= 0;
}

static int loop_devices(void) {
    struct stat st;
    if (stat("/dev/loop0", &st) != 0) return 1;                      /* none here */
    int ok = S_ISBLK(st.st_mode) && (st.st_mode & 07) == 0 && in_child(nobody_no_loop);
    if (st.st_gid == 6 && (st.st_mode & 060) == 060)                 /* BastiOS, and most Linux systems */
        ok = ok && in_child(disk_member_loop);
    return ok;
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

/* Run as `t_isol euid`, the program reports its effective user id. */
static int run_euid(const char *path) {
    pid_t c = fork();
    if (c == 0) {
        execl(path, path, "euid", (char *)0);
        _exit(9);
    }
    int st;
    return waitpid(c, &st, 0) == c && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *self;                /* argv[0] */

static int copy_self(const char *to) {
    int in = open(self, O_RDONLY), out = open(to, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    char buf[4096];
    ssize_t n;
    int ok = in >= 0 && out >= 0;
    while (ok && (n = read(in, buf, sizeof buf)) > 0) ok = write(out, buf, n) == n;
    close(in);
    close(out);
    return ok && chmod(to, 04755) == 0;
}

static int nobody_runs_suid(void) {     /* exit 0 if the copy outside the image runs as root */
    return become_nobody() && run_euid("/tmp/i_suid") == 0 && run_euid("/tmp/i_lm/suid") == 65534 % 256;
}

static int loop_nosuid(void) {      /* in memory: a loop mount takes an in-memory image */
    int fd = open("/tmp/i_img", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    int ok = fd >= 0 && ftruncate(fd, 1 << 20) == 0;
    close(fd);
    pid_t c = fork();
    if (c == 0) {
        execl("/bin/mkfs.bstdisk3", "mkfs.bstdisk3", "/tmp/i_img", (char *)0);
        _exit(9);
    }
    int st;
    ok = ok && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    mkdir("/tmp/i_lm", 0755);
    ok = ok && mount("/tmp/i_img", "/tmp/i_lm", "bstdisk3", 0, NULL) == 0;
    ok = ok && copy_self("/tmp/i_suid") && copy_self("/tmp/i_lm/suid") && in_child(nobody_runs_suid);
    unlink("/tmp/i_lm/suid");
    umount2("/tmp/i_lm", 0);
    rmdir("/tmp/i_lm");
    unlink("/tmp/i_suid");
    unlink("/tmp/i_img");
    return ok;
}

#define OTHERS 200

/* The links of the root parent are hidden; its own are not. */
static int no_proc_links(void) {
    char p[64], buf[256];
    pid_t root = getppid();
    if (!become_nobody()) return 0;
    snprintf(p, sizeof p, "/proc/%d/exe", (int)root);
    int ok = readlink(p, buf, sizeof buf) == -1 && errno == EACCES;
    snprintf(p, sizeof p, "/proc/%d/fd/0", (int)root);
    ok = ok && readlink(p, buf, sizeof buf) == -1 && errno == EACCES;
    return ok && readlink("/proc/self/exe", buf, sizeof buf) > 0;
}

static int no_console(void) {
    if (!become_nobody()) return 0;
    return open("/dev/console", O_RDWR) == -1 && errno == EACCES;
}

/* A set of group OTHERS, mode 040: readable by a member of OTHERS only
 * through its supplementary groups. */
static int sysv_msq;
static int reads_through_group(void) {
    gid_t g = OTHERS;
    if (setgroups(1, &g) != 0 || !become_nobody()) return 0;
    struct msqid_ds ds;
    return msgctl(sysv_msq, IPC_STAT, &ds) == 0;
}
static int stranger_cannot_read(void) {
    if (setgroups(0, NULL) != 0 || !become_nobody()) return 0;
    struct msqid_ds ds;
    return msgctl(sysv_msq, IPC_STAT, &ds) == -1 && errno == EACCES;
}
static int sysv_groups(void) {
    sysv_msq = msgget(IPC_PRIVATE, 0040);
    if (sysv_msq < 0) return 0;
    struct msqid_ds ds;
    int ok = msgctl(sysv_msq, IPC_STAT, &ds) == 0;
    ds.msg_perm.gid = OTHERS;
    ok = ok && msgctl(sysv_msq, IPC_SET, &ds) == 0;
    ok = ok && in_child(reads_through_group) && in_child(stranger_cannot_read);
    msgctl(sysv_msq, IPC_RMID, NULL);
    return ok;
}

/* The segment's owner, under a zero RLIMIT_MEMLOCK. */
static int no_shm_lock(void) {
    if (!become_nobody()) return 0;
    int id = shmget(IPC_PRIVATE, 4096, 0600);
    if (id < 0) return 0;
    struct rlimit rl = { 0, 0 };
    int ok = setrlimit(RLIMIT_MEMLOCK, &rl) == 0;
    ok = ok && shmctl(id, SHM_LOCK, NULL) == -1 && errno == EPERM;
    shmctl(id, IPC_RMID, NULL);
    return ok;
}

static int bastios(void) {
    struct utsname u;
    return uname(&u) == 0 && strstr(u.release, "bastios") != NULL;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "euid") == 0) return geteuid() % 256;
    self = argv[0];
    setup();
    mkdir("i_mnt", 0777);
    chmod("i_mnt", 0777);
    int fd = open("i_secret", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    close(fd);
    fd = open("i_public", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    (void)!write(fd, "public", 6);
    close(fd);

    EXPECT("reboot is root's", in_child(no_reboot));
    EXPECT("mount and umount2 are root's", in_child(no_mount));
    EXPECT("rt_sigqueueinfo follows kill's rule", in_child(no_sigqueue));
    EXPECT("a shared mapping of a read-only file stays read-only", in_child(no_mprotect_write));
    EXPECT("a unix socket's file says who may connect", unix_socket_files());
    EXPECT("loop devices are root's and group disk's", loop_devices());
    EXPECT("ports below 1024 are root's", in_child(low_ports));
    EXPECT("root binds a port below 1024", root_binds_low());
    EXPECT("another user's /proc exe and fd links are hidden", in_child(no_proc_links));
    EXPECT("/dev/console is root's", in_child(no_console));
    EXPECT("SysV group bits count supplementary groups", sysv_groups());
    EXPECT("SHM_LOCK needs RLIMIT_MEMLOCK or root", in_child(no_shm_lock));
    if (bastios())
        EXPECT("native system calls are root's", in_child(no_native));
    if (bastios())
        EXPECT("a loop-mounted image is nosuid", loop_nosuid());

    unlink("i_secret");
    unlink("i_public");
    rmdir("i_mnt");
    return done();
}
