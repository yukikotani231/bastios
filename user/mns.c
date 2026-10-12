/* IPC and PID namespaces and setns, against musl. The parent makes a System V
 * message queue, semaphore set and shared memory segment under fixed keys
 * and a POSIX queue; a child moves to a new IPC namespace (unshare) and
 * must not find them, then makes its own queue under the same key and
 * sends a message to it. The parent cannot see that message until it
 * joins the child's namespace through /proc/PID/ns/ipc (setns), and goes
 * back through a descriptor of its own /proc/self/ns/ipc; then the same
 * through a pidfd. Then PID namespaces: a child made with CLONE_NEWPID is
 * process 1 of its own and sees only its own children by their ids there;
 * the same for the first child after unshare(CLONE_NEWPID); and when that
 * init ends, the rest of its namespace goes with it. Prints "mns: ok", or
 * what failed. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define KEY 0x6d6e73
struct msg { long type; char text[16]; };

static int fail(const char *what)
{
    printf("mns: %s (errno %d)\n", what, errno);
    return 1;
}

static int child(int ready, int go)
{
    if (unshare(CLONE_NEWIPC) != 0)
        return fail("unshare");
    if (msgget(KEY, 0) != -1 || errno != ENOENT)
        return fail("the parent's queue is seen");
    if (semget(KEY, 0, 0) != -1 || errno != ENOENT)
        return fail("the parent's semaphores are seen");
    if (shmget(KEY, 0, 0) != -1 || errno != ENOENT)
        return fail("the parent's segment is seen");
    if (mq_open("/mns", O_RDONLY) != -1 || errno != ENOENT)
        return fail("the parent's POSIX queue is seen");
    int q = msgget(KEY, IPC_CREAT | 0600);
    struct msg m = { 1, "child" };
    if (q < 0 || msgsnd(q, &m, sizeof m.text, 0) != 0)
        return fail("child queue");
    char c = 1;
    write(ready, &c, 1);
    read(go, &c, 1);
    msgctl(q, IPC_RMID, 0);
    return 0;
}

/* The text of a message on the queue under KEY in the current namespace. */
static int peek(char *out)
{
    struct msg m;
    int q = msgget(KEY, 0);
    if (q < 0 || msgrcv(q, &m, sizeof m.text, 0, IPC_NOWAIT) < 0)
        return -1;
    strcpy(out, m.text);
    msgsnd(q, &m, sizeof m.text, 0);       /* put it back */
    return 0;
}

/* The init of a new PID namespace and its children, as they see themselves. */
static int pidns(void)
{
    int up[2], st;
    if (pipe(up))
        return fail("pipe");
    pid_t c = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
    if (c < 0)
        return fail("clone(CLONE_NEWPID)");
    if (c == 0) {
        if (getpid() != 1)
            _exit(10);
        if (getppid() != 0)
            _exit(11);
        pid_t g = fork();
        if (g == 0) {                       /* held until the namespace ends */
            if (getpid() != 2 || getppid() != 1)
                _exit(12);
            close(up[0]);
            for (;;)
                pause();
        }
        if (g != 2)
            _exit(13);
        if (kill(2, 0) != 0 || kill(3, 0) != -1 || errno != ESRCH)
            _exit(14);
        pid_t h = fork();
        if (h == 0)
            _exit(7);
        if (h != 3 || waitpid(h, &st, 0) != h || WEXITSTATUS(st) != 7)
            _exit(15);
        if (waitpid(-1, &st, WNOHANG) != 0)
            _exit(16);                      /* g still runs */
        close(up[1]);
        _exit(0);                           /* g goes with it */
    }
    close(up[1]);
    if (c == 1)
        return fail("the namespace's init is process 1 outside it too");
    char b;
    if (waitpid(c, &st, 0) != c)
        return fail("waitpid on the init");
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("mns: the init said %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }
    if (read(up[0], &b, 1) != 0)
        return fail("the init's namespace outlived it");
    close(up[0]);

    /* unshare: the caller stays where it is; its first child is process 1 */
    pid_t d = fork();
    if (d == 0) {
        pid_t me = getpid();
        if (unshare(CLONE_NEWPID) != 0)
            _exit(20);
        if (getpid() != me)
            _exit(21);
        if (unshare(CLONE_NEWPID) != -1)
            _exit(22);                      /* it has children of a new one already */
        pid_t e = fork();
        if (e == 0)
            _exit(getpid() == 1 ? 0 : 23);
        if (e == 1 || waitpid(e, &st, 0) != e)
            _exit(24);
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 25);
    }
    if (waitpid(d, &st, 0) != d || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("mns: unshare(CLONE_NEWPID) said %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }
    return 0;
}

/* A child in a UTS namespace of its own goes back to the first one through
 * a descriptor opened before (what LTP's setns02 does). */
static int utsback(void)
{
    char before[64], after[64];
    int fd = open("/proc/self/ns/uts", O_RDONLY);
    if (fd < 0 || gethostname(before, sizeof before))
        return fail("the uts link");
    pid_t c = fork();
    if (c == 0) {
        if (unshare(CLONE_NEWUTS) != 0 || sethostname("mns-box", 7) != 0)
            _exit(30);
        if (gethostname(after, sizeof after) || strcmp(after, "mns-box"))
            _exit(31);
        if (setns(fd, CLONE_NEWUTS) != 0)
            _exit(32);
        if (gethostname(after, sizeof after) || strcmp(after, before))
            _exit(33);
        _exit(0);
    }
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("mns: setns back to the first UTS namespace said %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }
    close(fd);
    return 0;
}

/* Writes str to a /proc/self file: the write's result. */
static int proc_put(const char *name, const char *str)
{
    int fd = open(name, O_WRONLY);
    if (fd < 0)
        return -1;
    int r = write(fd, str, strlen(str));
    close(fd);
    return r;
}

/* A user namespace as root makes it: maps are checked and written once,
 * ids go through them, and an id with no map is refused. */
static int userns(void)
{
    pid_t c = fork();
    if (c == 0) {
        if (unshare(CLONE_NEWUSER) != 0)
            _exit(40);
        if (getuid() != 65534)
            _exit(41);                      /* no map yet */
        if (proc_put("/proc/self/uid_map", "0 0 0\n") != -1 || errno != EINVAL)
            _exit(42);                      /* a count of 0 */
        if (proc_put("/proc/self/uid_map", "0 0 1\n0 5 1\n") != -1 || errno != EINVAL)
            _exit(43);                      /* overlapping */
        if (proc_put("/proc/self/uid_map", "0 0 1\n10 5000 5\n") < 0)
            _exit(44);
        if (proc_put("/proc/self/uid_map", "1 1 1\n") != -1 || errno != EPERM)
            _exit(45);                      /* once */
        if (getuid() != 0 || setuid(12) != 0 || getuid() != 12 || geteuid() != 12)
            _exit(46);
        if (setuid(7) != -1 || errno != EINVAL || getuid() != 12)
            _exit(47);                      /* 7 has no map */
        struct stat st;
        if (stat("/proc/self/uid_map", &st) || st.st_uid != 12)
            _exit(48);                      /* owned by its process: 5002 outside, 12 inside */
        if (proc_put("/proc/self/setgroups", "deny\n") < 0 || proc_put("/proc/self/setgroups", "allow\n") != -1)
            _exit(49);
        if (unshare(CLONE_NEWUSER) != -1 || errno != ENOSPC)
            _exit(50);                      /* two levels */
        _exit(0);
    }
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("mns: user namespace said %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }
    return 0;
}

int main(void)
{
    int q = msgget(KEY, IPC_CREAT | IPC_EXCL | 0600);
    int s = semget(KEY, 1, IPC_CREAT | IPC_EXCL | 0600);
    int sh = shmget(KEY, 4096, IPC_CREAT | IPC_EXCL | 0600);
    struct mq_attr at = { .mq_maxmsg = 4, .mq_msgsize = 16 };
    mqd_t pq = mq_open("/mns", O_CREAT | O_EXCL | O_RDWR, 0600, &at);
    if (q < 0 || s < 0 || sh < 0 || pq == (mqd_t)-1)
        return fail("making the objects");
    int up[2], down[2];
    pipe(up);
    pipe(down);
    pid_t c = fork();
    if (c == 0)
        _exit(child(up[1], down[0]));
    char b = 0, text[16];
    int bad = 0;
    if (read(up[0], &b, 1) != 1)
        bad = fail("the child gave up");
    if (!bad && peek(text) == 0)
        bad = fail("a message from the child's namespace is seen");
    int self = open("/proc/self/ns/ipc", O_RDONLY);
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/ns/ipc", c);
    int other = open(path, O_RDONLY);
    struct stat a, z;
    if (!bad && (self < 0 || other < 0 || fstat(self, &a) || fstat(other, &z) || a.st_ino == z.st_ino))
        bad = fail("the ns links");
    if (!bad && setns(other, CLONE_NEWUTS) != -1)
        bad = fail("setns with the wrong type");
    if (!bad && (setns(other, CLONE_NEWIPC) != 0 || peek(text) != 0 || strcmp(text, "child")))
        bad = fail("setns into the child's namespace");
    if (!bad && (setns(self, 0) != 0 || msgget(KEY, 0) != q))
        bad = fail("setns back");
    int pfd = syscall(SYS_pidfd_open, c, 0);
    if (!bad && (pfd < 0 || setns(pfd, CLONE_NEWIPC | CLONE_NEWUTS) != 0 || peek(text) != 0))
        bad = fail("setns through a pidfd");
    if (!bad && (setns(self, CLONE_NEWIPC) != 0 || msgget(KEY, 0) != q))
        bad = fail("setns back again");
    write(down[1], &b, 1);
    int st = 0;
    waitpid(c, &st, 0);
    msgctl(q, IPC_RMID, 0);
    semctl(s, 0, IPC_RMID);
    shmctl(sh, IPC_RMID, 0);
    mq_close(pq);
    mq_unlink("/mns");
    if (bad)
        return 1;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return 1;                           /* the child said what */
    if (utsback() || pidns() || userns())
        return 1;
    puts("mns: ok");
    return 0;
}
