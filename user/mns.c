/* IPC namespaces and setns, against musl. The parent makes a System V
 * message queue, semaphore set and shared memory segment under fixed keys
 * and a POSIX queue; a child moves to a new IPC namespace (unshare) and
 * must not find them, then makes its own queue under the same key and
 * sends a message to it. The parent cannot see that message until it
 * joins the child's namespace through /proc/PID/ns/ipc (setns), and goes
 * back through a descriptor of its own /proc/self/ns/ipc; then the same
 * through a pidfd. Prints "mns: ok", or what failed. */
#define _GNU_SOURCE
#include <errno.h>
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
    puts("mns: ok");
    return 0;
}
