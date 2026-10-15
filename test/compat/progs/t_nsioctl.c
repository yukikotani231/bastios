/* ioctl on a /proc/PID/ns descriptor (ioctl_ns(2)): NS_GET_NSTYPE,
 * NS_GET_USERNS, NS_GET_PARENT and NS_GET_OWNER_UID, and who may ask. */
#define _GNU_SOURCE
#include "compat.h"
#include <errno.h>
#include <fcntl.h>
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

int main(void) {
    setup();
    int uts = open("/proc/self/ns/uts", O_RDONLY);
    int usr = open("/proc/self/ns/user", O_RDONLY);
    int pid = open("/proc/self/ns/pid", O_RDONLY);
    int dir = open(".", O_RDONLY);
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
    return done();
}
