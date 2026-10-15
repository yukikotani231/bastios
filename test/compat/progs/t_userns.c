/* What an unprivileged user needs for a rootless container (runc, crun):
 * a user namespace the user maps itself into, the mounts and capabilities
 * its root has there, and a few smaller things that runc leans on:
 * chown of its own pipes, a hard descriptor limit above the table's size,
 * /dev/full, and /proc/self/fd/N of a file in a tmpfs it mounted. */
#define _GNU_SOURCE
#include "compat.h"
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

#define NOBODY 65534
#define MNT "/tmp/userns_t"

static int in_child(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) _exit(f() ? 0 : 1);
    int st; return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int as_nobody(void) {
    return setresgid(NOBODY, NOBODY, NOBODY) == 0 && setresuid(NOBODY, NOBODY, NOBODY) == 0;
}

static int write_str(const char *path, const char *s, size_t n) {
    int f = open(path, O_WRONLY);
    if (f < 0) return -1;
    int r = write(f, s, n);
    close(f);
    return r;
}

/* The way crun opens them, and a shell's > does: O_CREAT and O_TRUNC on a
 * file that is already there. */
static int write_trunc(const char *path, const char *s, size_t n) {
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    if (f < 0) return -1;
    int r = write(f, s, n);
    close(f);
    return r;
}

/* Runs f as nobody in a child that has its own user namespace, mapped the
 * way runc maps a rootless container: this process (the parent, in the
 * first namespace) writes "0 65534 1" to the child's uid_map and gid_map
 * after "deny" to setgroups, counting a NUL at the end of the text. The
 * child then becomes root there, with f as what it does next. */
static int in_userns(int (*f)(void), int mntns) {
    int up[2], down[2];
    if (pipe(up) || pipe(down)) return 0;
    pid_t c = fork();
    if (c == 0) {
        char b;
        if (!as_nobody() || unshare(CLONE_NEWUSER) != 0) _exit(10);
        if (write(up[1], "x", 1) != 1 || read(down[0], &b, 1) != 1) _exit(11);
        if (setresuid(0, 0, 0) != 0 || setresgid(0, 0, 0) != 0) _exit(12);
        if (mntns && unshare(CLONE_NEWNS) != 0) _exit(13);
        _exit(f() ? 0 : 1);
    }
    char b, path[64], map[32];
    if (read(up[0], &b, 1) != 1) return 0;
    snprintf(path, sizeof path, "/proc/%d/setgroups", c);
    int ok = write_str(path, "deny", 4) == 4;
    int n = snprintf(map, sizeof map, "0 %d 1\n", NOBODY) + 1;      /* the NUL counts, as runc's length does */
    snprintf(path, sizeof path, "/proc/%d/uid_map", c);
    ok = ok && write_str(path, map, n) == n;
    snprintf(path, sizeof path, "/proc/%d/gid_map", c);
    ok = ok && write_str(path, map, n) == n;
    ok = ok && write(down[1], "y", 1) == 1;
    int st;
    return waitpid(c, &st, 0) == c && ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* crun unshares the user namespace in the process that later maps it, so
 * the file it opens for writing belongs to the caller itself, in the
 * namespace the caller just made. */
static int map_own_after_unshare(void) {
    if (!as_nobody() || unshare(CLONE_NEWUSER) != 0) return 0;
    char map[32];
    int n = snprintf(map, sizeof map, "0 %d 1\n", NOBODY);
    return write_trunc("/proc/self/setgroups", "deny", 4) == 4 && write_trunc("/proc/self/uid_map", map, n) == n
        && write_trunc("/proc/self/gid_map", map, n) == n;
}

/* A user namespace inside one: its map names ids of the namespace around
 * it (an id that one does not map is refused), and reads back the same way
 * while the host ids stay what permissions use. */
static int nested_userns(void) {
    if (!map_own_after_unshare() || getuid() != 0) return 0;
    if (unshare(CLONE_NEWUSER) != 0) return 0;
    if (write_trunc("/proc/self/setgroups", "deny", 4) != 4) return 0;
    if (write_trunc("/proc/self/uid_map", "0 5 1\n", 6) != -1 || errno != EPERM) return 0;
    if (write_trunc("/proc/self/uid_map", "7 0 1\n", 6) != 6) return 0;
    if (write_trunc("/proc/self/gid_map", "7 0 1\n", 6) != 6) return 0;
    char t[64];
    int f = open("/proc/self/uid_map", O_RDONLY);
    int n = f < 0 ? -1 : read(f, t, sizeof t - 1);
    if (f >= 0) close(f);
    if (n <= 0) return 0;
    t[n] = 0;
    return getuid() == 7 && strstr(t, "         7          0          1") != NULL;
}

/* After pivot_root, on a directory of the disk, runc's check that a directory descriptor reads back as
 * the path it opened (/proc/self/fd/N names the path from the new root). */
static char pivot_dir[256];

static int fdlink_after_pivot(void) {
    int pd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
    if (pd < 0 || mount(pivot_dir, pivot_dir, 0, MS_BIND | MS_REC, 0) != 0) return 0;
    if (chdir(pivot_dir) != 0 || syscall(SYS_pivot_root, ".", ".") != 0) return 0;
    if (umount2(".", MNT_DETACH) != 0 || chdir("/") != 0) return 0;
    mkdir("/m", 0755);
    mkdir("/b", 0755);
    if (mkdir("/sub/q", 0755) != 0) { printf("mkdir sub/q %d\n", errno); return 0; }
    if (mount("t", "/m", "tmpfs", 0, 0) != 0) { printf("tmpfs m %d\n", errno); return 0; }
    mkdir("/m/p", 0755);
    if (mount("/sub/q", "/b", 0, MS_BIND, 0) != 0) { printf("bind b %d\n", errno); return 0; }
    mkdir("/sub/pts", 0755);
    if (mount("devpts", "/sub/pts", "devpts", 0, 0) != 0) { printf("devpts %d\n", errno); return 0; }
    close(open("/sub/f1", O_CREAT | O_WRONLY, 0644));
    close(open("/m/f2", O_CREAT | O_WRONLY, 0644));
    const char *names[9] = {"/", "/sub", "/m", "/m/p", "/b", "/b", "/sub/pts", "/sub/f1", "/m/f2"};
    for (int i = 0; i < 9; i++) {
        int fd = i < 7 ? open(names[i], O_PATH | O_DIRECTORY | O_CLOEXEC) : open(names[i], O_RDONLY);
        char num[16], got[64];
        snprintf(num, sizeof num, "%d", fd);
        ssize_t n = fd < 0 ? -1 : readlinkat(pd, num, got, sizeof got - 1);
        if (n < 0) { printf("readlink %s %d\n", names[i], errno); return 0; }
        got[n] = 0;
        if (strcmp(got, names[i]) != 0) {
            printf("fd of %s reads %s\n", names[i], got);
            return 0;
        }
    }
    return 1;
}

/* The user who made a user namespace joins it again with setns (rootless
 * podman's way back into the namespace its pause process holds), and is
 * root there, but cannot go to the first one. */
static int join_userns(void) {
    int up[2], down[2];
    if (pipe(up) || pipe(down)) return 0;
    pid_t c = fork();
    if (c == 0) {
        char b;
        if (!as_nobody() || unshare(CLONE_NEWUSER) != 0) _exit(10);
        char map[32];
        int n = snprintf(map, sizeof map, "0 %d 1\n", NOBODY);
        if (write_trunc("/proc/self/setgroups", "deny", 4) != 4 || write_trunc("/proc/self/uid_map", map, n) != n
            || write_trunc("/proc/self/gid_map", map, n) != n) _exit(11);
        if (write(up[1], "x", 1) != 1 || read(down[0], &b, 1) != 1) _exit(12);
        _exit(0);
    }
    char b, path[64];
    int ok = as_nobody() && read(up[0], &b, 1) == 1;
    snprintf(path, sizeof path, "/proc/%d/ns/user", c);
    int f = open(path, O_RDONLY);
    int first = open("/proc/1/ns/user", O_RDONLY);
    ok = ok && f >= 0 && first >= 0 && getuid() == NOBODY;
    ok = ok && setns(f, CLONE_NEWUSER) == 0 && getuid() == 0;
    ok = ok && setns(first, CLONE_NEWUSER) == -1 && errno == EPERM;
    ok = ok && write(down[1], "y", 1) == 1;
    int st;
    return waitpid(c, &st, 0) == c && ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* The root of a user namespace that made its own network namespace brings
 * its loopback up (crun does at start), but not in the first one. */
static int lo_up(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq r;
    memset(&r, 0, sizeof r);
    strcpy(r.ifr_name, "lo");
    r.ifr_flags = IFF_UP;
    int e = ioctl(s, SIOCSIFFLAGS, &r);
    close(s);
    return e;
}
static int lo_up_in_own_netns(void) { return unshare(CLONE_NEWNET) == 0 && lo_up() == 0; }
static int lo_up_in_first_netns(void) { return lo_up() == -1 && errno == EPERM; }

/* A network namespace made by a process in the first user namespace: the
 * root of a user namespace made inside it does not own it. */
static int lo_up_in_foreign_netns(void) {
    return unshare(CLONE_NEWNET) == 0 && in_userns(lo_up_in_first_netns, 0);
}

/* statfs of /proc/self/fd/N is the file system of the file N holds. */
static int statfs_fd_link(void) {
    int f = open("/tmp", O_PATH | O_DIRECTORY);
    char p[40];
    struct statfs a, b;
    snprintf(p, sizeof p, "/proc/self/fd/%d", f);
    int ok = f >= 0 && statfs(p, &a) == 0 && fstatfs(f, &b) == 0 && a.f_type == b.f_type;
    close(f);
    return ok;
}

static int own_pipe(void) {
    int p[2];
    if (!as_nobody() || pipe(p)) return 0;
    if (fchown(p[0], NOBODY, NOBODY) != 0) return 0;                /* to itself: nothing to change */
    if (fchown(p[0], -1, -1) != 0) return 0;
    return fchown(p[0], 0, 0) == -1 && errno == EPERM;              /* to anyone else is root's */
}

static int hard_nofile(void) {
    struct rlimit r = {1024, 1024}, got;
    if (!as_nobody() || setrlimit(RLIMIT_NOFILE, &r) != 0) return 0;
    return getrlimit(RLIMIT_NOFILE, &got) == 0 && got.rlim_max == 1024;
}

static int dev_full(void) {
    char b[4] = {1, 1, 1, 1};
    struct stat st;
    int f = open("/dev/full", O_RDWR);
    if (f < 0 || fstat(f, &st) != 0 || major(st.st_rdev) != 1 || minor(st.st_rdev) != 7) return 0;
    return read(f, b, 4) == 4 && !b[0] && !b[3] && write(f, "x", 1) == -1 && errno == ENOSPC;
}

/* The ids a call that takes one argument leaves in the other registers
 * must not matter. */
static int ids_with_junk(void) {
    long junk1 = 0x123456789L, junk2 = 0xabcdefL;
    return syscall(SYS_setgid, 0L, junk1, junk2) == 0 && syscall(SYS_setuid, 0L, junk1, junk2) == 0
        && getuid() == 0 && getgid() == 0;
}

static int map_text(void) {
    char buf[64];
    int f = open("/proc/self/uid_map", O_RDONLY);
    int n = f < 0 ? -1 : read(f, buf, sizeof buf - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    long a, b, c;
    return sscanf(buf, "%ld %ld %ld", &a, &b, &c) == 3 && a == 0 && b == NOBODY && c == 1 && getuid() == 0;
}

static int mounts_in_own_ns(void) {
    char link[128], want[] = MNT "/f";
    if (mount("t", MNT, "tmpfs", 0, 0) != 0) return 0;
    int f = open(want, O_CREAT | O_RDWR, 0600);
    if (f < 0 || write(f, "in", 2) != 2) return 0;
    struct stat st;
    if (fstat(f, &st) != 0 || st.st_uid != 0) return 0;              /* its own: root there */
    char p[32];
    snprintf(p, sizeof p, "/proc/self/fd/%d", f);
    int n = readlink(p, link, sizeof link - 1);
    if (n != (int)strlen(want)) return 0;
    link[n] = 0;
    if (strcmp(link, want) != 0) return 0;                           /* the name it has in the namespace */
    if (mount(MNT, MNT, 0, MS_BIND, 0) != 0) return 0;
    return umount2(MNT, 0) == 0;
}

static int no_mounts_without_ns(void) {
    return mount("t", MNT, "tmpfs", 0, 0) == -1 && errno == EPERM;
}

static int caps_there(void) {
    struct { unsigned v; int pid; } h = {0x20080522, 0};
    struct { unsigned e, p, i; } d[2];
    memset(d, 0, sizeof d);
    if (syscall(SYS_capget, &h, d) != 0 || !d[0].e) return 0;        /* it holds capabilities in its namespace */
    if (prctl(PR_CAPBSET_DROP, 21 /* CAP_SYS_ADMIN */, 0, 0, 0) != 0) return 0;
    memset(d, 0, sizeof d);
    d[0].e = d[0].p = 1u << 7;                                       /* CAP_SETUID only */
    return syscall(SYS_capset, &h, d) == 0 && prctl(PR_CAPBSET_READ, 21, 0, 0, 0) == 0;
}

static int caps_not_outside(void) {
    return as_nobody() && mount("t", MNT, "tmpfs", 0, 0) == -1 && errno == EPERM;
}

int main(void) {
    setup();
    mkdir(MNT, 0755);
    if (getcwd(pivot_dir, sizeof pivot_dir - 8)) {
        strcat(pivot_dir, "/ud_pv");
        mkdir(pivot_dir, 0777);
        chmod(pivot_dir, 0777);
        char sub[300];
        snprintf(sub, sizeof sub, "%s/sub", pivot_dir);
        mkdir(sub, 0777);
        chmod(sub, 0777);
    }
    EXPECT("a process may chown its own pipe to itself, not to others", in_child(own_pipe));
    EXPECT("an unprivileged process may raise its hard descriptor limit", in_child(hard_nofile));
    EXPECT("/dev/full reads zeros and writes ENOSPC", dev_full());
    EXPECT("the first ids of a user namespace are set from one register", in_userns(ids_with_junk, 0));
    EXPECT("uid_map takes text that ends in a NUL", in_userns(map_text, 0));
    EXPECT("a process maps the user namespace it has just made itself", in_child(map_own_after_unshare));
    EXPECT("a user namespace is made inside another and mapped through it", in_child(nested_userns));
    EXPECT("whoever made a user namespace joins it with setns, and not the first one", in_child(join_userns));
    EXPECT("a directory descriptor reads back from the new root after pivot_root", in_userns(fdlink_after_pivot, 1));
    EXPECT("the root of a user namespace brings loopback up in a network namespace of its own", in_userns(lo_up_in_own_netns, 0));
    EXPECT("and not in the first one", in_userns(lo_up_in_first_netns, 0));
    EXPECT("nor in one that another user namespace made", in_child(lo_up_in_foreign_netns));
    EXPECT("statfs of /proc/self/fd/N answers for the file N holds", statfs_fd_link());
    EXPECT("the root of a user namespace mounts in a mount namespace of its own", in_userns(mounts_in_own_ns, 1));
    EXPECT("and not without one", in_userns(no_mounts_without_ns, 0));
    EXPECT("the root of a user namespace gives up capabilities", in_userns(caps_there, 0));
    EXPECT("an unprivileged user mounts nothing", in_child(caps_not_outside));
    char sub[300];
    snprintf(sub, sizeof sub, "%s/sub", pivot_dir);
    rmdir(sub);
    rmdir(pivot_dir);
    return done();
}
