/* ioctl on a /proc/PID/ns descriptor (ioctl_ns(2)): NS_GET_NSTYPE,
 * NS_GET_USERNS, NS_GET_PARENT and NS_GET_OWNER_UID, and who may ask. */
#define _GNU_SOURCE
#include "compat.h"
#include <dirent.h>
#include <sys/mount.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NSIO 0xb7
#define NS_GET_USERNS _IO(NSIO, 0x1)
#define NS_GET_PARENT _IO(NSIO, 0x2)
#define NS_GET_NSTYPE _IO(NSIO, 0x3)
#define NS_GET_OWNER_UID _IO(NSIO, 0x4)

static int in_child(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) _exit(f() ? 0 : 1);
    int st; return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static ino_t ino_of(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 ? st.st_ino : 0;
}

/* Does ioctl(fd, req) fail with err? */
static int fails(int fd, unsigned long req, int err) {
    errno = 0;
    int r = ioctl(fd, req);
    if (r >= 0) { close(r); return 0; }
    return errno == err;
}

/* A new user namespace, made by the child: its owner is the child's user,
 * which the namespace has no map for. */
static int owner_unmapped(void) {
    if (unshare(CLONE_NEWUSER) != 0) return 0;
    int fd = open("/proc/self/ns/user", O_RDONLY);
    unsigned int uid = 0;
    if (fd < 0 || ioctl(fd, NS_GET_OWNER_UID, &uid) != 0) return 0;
    return uid == 65534;
}

/* ... and the namespace's parent is out of its reach from inside. */
static int own_parent_out_of_reach(void) {
    if (unshare(CLONE_NEWUSER) != 0) return 0;
    int fd = open("/proc/self/ns/user", O_RDONLY);
    return fd >= 0 && fails(fd, NS_GET_USERNS, EPERM) && fails(fd, NS_GET_PARENT, EPERM);
}

/* The PID namespace the children will be in has the first one as parent. */
static int pid_parent(void) {
    if (unshare(CLONE_NEWPID) != 0) return 0;
    int mine = open("/proc/self/ns/pid", O_RDONLY);
    int kids = open("/proc/self/ns/pid_for_children", O_RDONLY);
    if (mine < 0 || kids < 0 || ino_of(mine) == ino_of(kids)) return 0;
    int p = ioctl(kids, NS_GET_PARENT);
    return p >= 0 && ino_of(p) == ino_of(mine);
}

/* From inside the new PID namespace the parent is out of reach. */
static int pid_parent_out_of_reach(void) {
    if (unshare(CLONE_NEWPID) != 0) return 0;
    pid_t c = fork();
    if (c == 0) {
        int fd = open("/proc/self/ns/pid", O_RDONLY);
        _exit(fd >= 0 && fails(fd, NS_GET_PARENT, EPERM) ? 0 : 1);
    }
    int st;
    return c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* Nested user namespaces: from the outer one, the inner one's parent is
 * the outer one; from the inner one, the outer one's parent is out of reach. */
static int user_nested(void) {
    if (unshare(CLONE_NEWUSER) != 0) return 0;
    int outer = open("/proc/self/ns/user", O_RDONLY);
    int pp[2], qq[2];
    if (outer < 0 || pipe(pp) != 0 || pipe(qq) != 0) return 0;
    pid_t c = fork();
    if (c == 0) {
        char b = 0;
        if (unshare(CLONE_NEWUSER) != 0) _exit(1);
        int inner = open("/proc/self/ns/user", O_RDONLY);
        if (inner < 0 || ino_of(inner) == ino_of(outer)) _exit(1);
        if (!fails(inner, NS_GET_PARENT, EPERM)) _exit(1);
        if (!fails(outer, NS_GET_PARENT, EPERM)) _exit(1);
        if (write(pp[1], &b, 1) != 1 || read(qq[0], &b, 1) != 1) _exit(1);
        _exit(0);
    }
    char b;
    if (c < 0 || read(pp[0], &b, 1) != 1) return 0;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/ns/user", (int)c);
    int inner = open(path, O_RDONLY);
    int par = inner >= 0 ? ioctl(inner, NS_GET_PARENT) : -1;
    int ok = par >= 0 && ino_of(par) == ino_of(outer) && ino_of(inner) != ino_of(outer);
    if (write(qq[1], &b, 1) != 1) return 0;
    int st;
    return ok && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* Nested PID namespaces: a child in the first one makes a second, whose
 * parent is the first. */
static int pid_nested(void) {
    if (unshare(CLONE_NEWPID) != 0) return 0;
    pid_t c = fork();
    if (c == 0) {
        int mine = open("/proc/self/ns/pid", O_RDONLY);
        if (mine < 0 || unshare(CLONE_NEWPID) != 0) _exit(1);
        int kids = open("/proc/self/ns/pid_for_children", O_RDONLY);
        int p = kids >= 0 ? ioctl(kids, NS_GET_PARENT) : -1;
        _exit(p >= 0 && ino_of(p) == ino_of(mine) ? 0 : 1);
    }
    int st;
    return c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* What lsns does: open every namespace link of every process and ask
 * its type. Returns the number of descriptors that did not answer. */
static int scan_all(void) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    static const char *names[] = { "mnt", "uts", "ipc", "pid", "net", "user", "cgroup", "time", "pid_for_children", "time_for_children" };
    int bad = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        for (int i = 0; i < 10; i++) {
            char p[64];
            snprintf(p, sizeof p, "/proc/%s/ns/%s", e->d_name, names[i]);
            int f = open(p, O_RDONLY);
            if (f < 0) continue;
            /* lsns opens them relative to a descriptor of /proc/PID. */
            char dp[32], rel[32];
            snprintf(dp, sizeof dp, "/proc/%s", e->d_name);
            snprintf(rel, sizeof rel, "ns/%s", names[i]);
            int pd = open(dp, O_RDONLY | O_DIRECTORY);
            int g = pd >= 0 ? openat(pd, rel, O_RDONLY) : -1;
            if (g < 0 || ioctl(g, NS_GET_NSTYPE) < 0) { printf("# %s: openat from the /proc/PID descriptor: fd %d errno %d\n", p, g, errno); bad++; }
            if (g >= 0) close(g);
            if (pd >= 0) close(pd);
            pd = open(dp, O_PATH | O_DIRECTORY);
            g = pd >= 0 ? openat(pd, rel, O_RDONLY) : -1;
            if (g < 0 || ioctl(g, NS_GET_NSTYPE) < 0) { printf("# %s: openat from an O_PATH /proc/PID descriptor: fd %d errno %d\n", p, g, errno); bad++; }
            if (g >= 0) close(g);
            if (pd >= 0) close(pd);
            char np[64];
            snprintf(np, sizeof np, "/proc/%s/ns", e->d_name);
            pd = open(np, O_PATH | O_DIRECTORY);
            g = pd >= 0 ? openat(pd, names[i], O_RDONLY) : -1;
            if (g < 0 || ioctl(g, NS_GET_NSTYPE) < 0) { printf("# %s: openat from an O_PATH ns directory: fd %d errno %d\n", p, g, errno); bad++; }
            if (g >= 0) close(g);
            if (pd >= 0) close(pd);
            long t = ioctl(f, NS_GET_NSTYPE);
            if (t < 0) { printf("# %s: NS_GET_NSTYPE failed, errno %d\n", p, errno); bad++; }
            /* What lsns does next: the owner of the namespace, and its type. */
            int o = ioctl(f, NS_GET_USERNS);
            if (o >= 0) {
                if (ioctl(o, NS_GET_NSTYPE) != CLONE_NEWUSER) { printf("# %s: its owner has no type, errno %d\n", p, errno); bad++; }
                close(o);
            }
            int par = ioctl(f, NS_GET_PARENT);
            if (par >= 0) {
                if (ioctl(par, NS_GET_NSTYPE) < 0) { printf("# %s: its parent has no type, errno %d\n", p, errno); bad++; }
                close(par);
            }
            close(f);
        }
    }
    if (d) closedir(d);
    return bad;
}

/* A thread's own directory has namespace links too (lsns reads them). */
static int task_ns(void) {
    char p[96];
    int f = open("/proc/self/task", O_RDONLY | O_DIRECTORY);
    if (f < 0) return 0;
    int ok = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/ns/mnt", (int)getpid());
    int g = open(p, O_RDONLY);
    ok = g >= 0 && ioctl(g, NS_GET_NSTYPE) == CLONE_NEWNS;
    if (!ok) printf("# %s: fd %d errno %d\n", p, g, errno);
    if (g >= 0) close(g);
    snprintf(p, sizeof p, "%d/ns/mnt", (int)getpid());
    g = openat(f, p, O_RDONLY);
    int ok2 = g >= 0 && ioctl(g, NS_GET_NSTYPE) == CLONE_NEWNS;
    if (!ok2) printf("# openat(task, %s): fd %d errno %d\n", p, g, errno);
    if (g >= 0) close(g);
    int dd = open("/proc", O_RDONLY | O_DIRECTORY);
    int g2 = openat(dd, "ns/mnt", O_RDONLY);
    int ok3 = g2 < 0 && errno == ENOENT;
    if (!ok3) printf("# openat(/proc, ns/mnt): fd %d errno %d\n", g2, errno);
    return ok && ok2 && ok3;
}

static int zombie_ns(void) {
    pid_t c = fork();
    if (c == 0) _exit(0);
    usleep(100000);
    char b[64];
    snprintf(b, sizeof b, "/proc/%d/ns/pid", c);
    int fd = open(b, O_RDONLY);
    int e = errno;
    char l[64];
    snprintf(b, sizeof b, "/proc/%d/ns/uts", c);
    ssize_t n = readlink(b, l, sizeof l);
    int e2 = errno;
    if (fd >= 0) {
        struct stat st;
        fstat(fd, &st);
        printf("zombie ns: open ok fd=%d mode=%o\n", fd, st.st_mode);
    }
    printf("zombie ns: open e=%d readlink n=%zd e=%d\n", e, n, e2);
    waitpid(c, 0, 0);
    int code = fd < 0 ? (e == ENOENT ? 1 : 100 + e) : 2 + (n < 0 ? 10 + e2 : 0);
    errno = code;
    return code == 1;
}

int main(void) {
    setup();
    int uts = open("/proc/self/ns/uts", O_RDONLY);
    int usr = open("/proc/self/ns/user", O_RDONLY);
    int pid = open("/proc/self/ns/pid", O_RDONLY);
    int dir = open(".", O_RDONLY);
    EXPECT("NS_GET_NSTYPE works on the descriptor NS_GET_USERNS returns", ({ int o = ioctl(uts, NS_GET_USERNS); o >= 0 && ioctl(o, NS_GET_NSTYPE) == CLONE_NEWUSER; }));
    EXPECT("NS_GET_NSTYPE works on an O_PATH descriptor of a ns link", ({ int o = open("/proc/self/ns/mnt", O_PATH); o >= 0 && ioctl(o, NS_GET_NSTYPE) == CLONE_NEWNS; }));
    EXPECT("NS_GET_NSTYPE works on a ns link opened in a chroot", ({
        int st = 0;
        pid_t c = fork();
        if (c == 0) {
            int o = open("/proc/self/ns/mnt", O_RDONLY);
            _exit(o >= 0 && ioctl(o, NS_GET_NSTYPE) == CLONE_NEWNS ? 0 : 3);
        }
        waitpid(c, &st, 0);
        WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }));
    EXPECT("NS_GET_NSTYPE answers on every namespace link of every process", scan_all() == 0);
    EXPECT("NS_GET_NSTYPE answers on every namespace link in a chroot with its own proc", ({
        int st = 0;
        mkdir("/tmp/nsr", 0755);
        mkdir("/tmp/nsr/proc", 0755);
        mount("proc", "/tmp/nsr/proc", "proc", 0, "");
        pid_t c = fork();
        if (c == 0) {
            if (chroot("/tmp/nsr") != 0 || chdir("/") != 0) _exit(2);
            _exit(scan_all() == 0 ? 0 : 3);
        }
        waitpid(c, &st, 0);
        umount("/tmp/nsr/proc");
        WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }));
    EXPECT("namespace files have a device of their own (nsfs), shared by all of them",
           ({ struct stat a, b, c, r;
              stat("/proc/self/ns/pid", &a) == 0 && stat("/proc/self/ns/net", &b) == 0
              && stat("/proc", &c) == 0 && stat("/", &r) == 0
              && a.st_dev != 0 && a.st_dev == b.st_dev && a.st_dev != c.st_dev && a.st_dev != r.st_dev
              && fstat(open("/proc", O_RDONLY), &c) == 0 && a.st_dev != c.st_dev; }));
    EXPECT("a zombie has no namespace links to open", zombie_ns() == 1);
    EXPECT("a thread's directory has namespace links, /proc has none", task_ns());
    EXPECT("NS_GET_NSTYPE gives the CLONE_NEW* flag of the namespace",
           ioctl(uts, NS_GET_NSTYPE) == CLONE_NEWUTS && ioctl(usr, NS_GET_NSTYPE) == CLONE_NEWUSER
           && ioctl(pid, NS_GET_NSTYPE) == CLONE_NEWPID);
    EXPECT("the first user namespace owns the first UTS namespace",
           ({ int o = ioctl(uts, NS_GET_USERNS); o >= 0 && ino_of(o) == ino_of(usr); }));
    EXPECT("the first user namespace has no owner and no parent",
           fails(usr, NS_GET_USERNS, EPERM) && fails(usr, NS_GET_PARENT, EPERM));
    EXPECT("the first PID namespace has no parent", fails(pid, NS_GET_PARENT, EPERM));
    EXPECT("a UTS namespace has no parent", fails(uts, NS_GET_PARENT, EINVAL));
    EXPECT("only a user namespace has an owner uid", ({ unsigned int u; ioctl(uts, NS_GET_OWNER_UID, &u) < 0 && errno == EINVAL; }));
    EXPECT("the first user namespace is owned by uid 0", ({ unsigned int u = 7; ioctl(usr, NS_GET_OWNER_UID, &u) == 0 && u == 0; }));
    EXPECT("an ordinary descriptor says ENOTTY",
           fails(dir, NS_GET_NSTYPE, ENOTTY) && fails(dir, NS_GET_USERNS, ENOTTY) && fails(dir, NS_GET_PARENT, ENOTTY));
    EXPECT("the descriptor NS_GET_USERNS returns is close-on-exec",
           ({ int o = ioctl(uts, NS_GET_USERNS); o >= 0 && (fcntl(o, F_GETFD) & FD_CLOEXEC); }));
    EXPECT("the owner of a user namespace its creator has no map for is 65534", in_child(owner_unmapped));
    EXPECT("from inside a user namespace its parent is out of reach", in_child(own_parent_out_of_reach));
    EXPECT("the children's PID namespace has the first one as its parent", in_child(pid_parent));
    EXPECT("from inside a PID namespace its parent is out of reach", in_child(pid_parent_out_of_reach));
    EXPECT("an inner user namespace's parent is the outer one, seen from outside only", in_child(user_nested));
    EXPECT("a PID namespace made inside another has that one as its parent", in_child(pid_nested));
    return done();
}
