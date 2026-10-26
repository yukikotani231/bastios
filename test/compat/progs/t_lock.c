/* File locks between processes and descriptions: fcntl record locks,
 * open file description locks and flock. Each check that needs a second
 * process forks one, which reports its finding in its exit status. */
#include "compat.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>

static int lk(int fd, int cmd, int type, off_t start, off_t len) {
    struct flock l = {.l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len};
    return fcntl(fd, cmd, &l);
}

/* What F_GETLK says about a type lock on [start, start + len): the type in
 * the way (F_UNLCK if none), with its holder in *pid. */
static int getlk(int fd, int cmd, int type, off_t start, off_t len, pid_t *pid) {
    struct flock l = {.l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len};
    if (fcntl(fd, cmd, &l) != 0) return -1;
    if (pid) *pid = l.l_pid;
    return l.l_type;
}

/* Runs f in a child; its result is the child's exit status. */
static int in_child(int (*f)(int), int fd) {
    pid_t c = fork();
    if (c == 0) _exit(f(fd));
    int st;
    if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static pid_t parent;
static int c_blocked(int fd) { return lk(fd, F_SETLK, F_WRLCK, 0, 10) == -1 && (errno == EAGAIN || errno == EACCES); }
static int c_beside(int fd) { return lk(fd, F_SETLK, F_WRLCK, 10, 10) == 0; }
static int c_shared(int fd) { return lk(fd, F_SETLK, F_RDLCK, 0, 10) == 0; }
static int c_getlk(int fd) { pid_t p = 0; return getlk(fd, F_GETLK, F_WRLCK, 5, 1, &p) == F_WRLCK && p == parent; }
static int c_free(int fd) { return lk(fd, F_SETLK, F_WRLCK, 0, 100) == 0; }
static int c_ofd_blocked(int fd) { return lk(fd, F_OFD_SETLK, F_WRLCK, 0, 1) == -1 && errno == EAGAIN; }
static int c_flock_shared(int fd) { return flock(fd, LOCK_SH | LOCK_NB) == 0; }

int main(void) {
    setup();
    int fd = -1, g = -1, st;
    pid_t c = -1;
    parent = getpid();
    EXPECT("lock file", (fd = open("lk_a", O_RDWR | O_CREAT | O_TRUNC, 0644)) >= 0 && write(fd, "0123456789", 10) == 10);

    /* Record locks belong to the process. */
    EXPECT("F_SETLK write lock", lk(fd, F_SETLK, F_WRLCK, 0, 10) == 0);
    EXPECT("another process is kept out", in_child(c_blocked, fd) == 1);
    EXPECT("bytes beside it are free", in_child(c_beside, fd) == 1);
    EXPECT("F_GETLK names the holder", in_child(c_getlk, fd) == 1);
    EXPECT("F_GETLK sees no lock of ours", getlk(fd, F_GETLK, F_WRLCK, 0, 10, 0) == F_UNLCK);
    EXPECT("the owner relocks freely", lk(fd, F_SETLK, F_RDLCK, 0, 10) == 0);
    EXPECT("read locks are shared", in_child(c_shared, fd) == 1);
    EXPECT("unlocking a middle part splits it", lk(fd, F_SETLK, F_UNLCK, 3, 4) == 0 &&
           ({ pid_t p; getlk(fd, F_GETLK, F_WRLCK, 0, 0, &p) == F_UNLCK; }));
    EXPECT("F_UNLCK frees the rest", lk(fd, F_SETLK, F_UNLCK, 0, 0) == 0 && in_child(c_free, fd) == 1);
    EXPECT("a lock to the end of the file", lk(fd, F_SETLK, F_WRLCK, 4, 0) == 0 && in_child(c_blocked, fd) == 1 &&
           lk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
    EXPECT("F_SETLK bad type is EINVAL", lk(fd, F_SETLK, 9, 0, 1) == -1 && errno == EINVAL);
    EXPECT("closing any descriptor drops the locks", ({ int d = open("lk_a", O_RDONLY);
           d >= 0 && lk(fd, F_SETLK, F_WRLCK, 0, 10) == 0 && close(d) == 0 && in_child(c_free, fd) == 1; }));

    /* F_SETLKW waits for the holder. */
    EXPECT("F_SETLKW waits until it is free", ({ int ok = 0, p[2]; char b;
           if (pipe(p) == 0 && (c = fork()) == 0) {
               close(p[0]);
               lk(fd, F_SETLK, F_WRLCK, 0, 10);
               write(p[1], "x", 1);
               struct timespec t = {1, 0};   /* long enough that a slow parent still finds the lock held */
               nanosleep(&t, 0);
               _exit(0);                                    /* exiting drops the lock */
           }
           close(p[1]);
           if (c > 0 && read(p[0], &b, 1) == 1) {
               time_t t0 = time(0);
               ok = lk(fd, F_SETLK, F_WRLCK, 0, 10) == -1 && lk(fd, F_SETLKW, F_WRLCK, 0, 10) == 0 && time(0) - t0 < 5;
               lk(fd, F_SETLK, F_UNLCK, 0, 0);
           }
           close(p[0]);
           waitpid(c, &st, 0);
           ok; }));

    /* Two processes each waiting for the other's lock: the second to wait
     * gets EDEADLK instead of waiting forever. */
    EXPECT("F_SETLKW deadlock is EDEADLK", ({ int ok = 0, p[2]; char b;
           if (lk(fd, F_SETLK, F_WRLCK, 0, 1) == 0 && pipe(p) == 0 && (c = fork()) == 0) {
               close(p[0]);
               lk(fd, F_SETLK, F_WRLCK, 1, 1);
               write(p[1], "x", 1);
               _exit(lk(fd, F_SETLKW, F_WRLCK, 0, 1) == 0 ? 0 : 1);   /* waits for the parent */
           }
           close(p[1]);
           if (c > 0 && read(p[0], &b, 1) == 1) {
               struct timespec t = {0, 300000000};
               nanosleep(&t, 0);                            /* let the child start waiting */
               ok = lk(fd, F_SETLKW, F_WRLCK, 1, 1) == -1 && errno == EDEADLK;
               lk(fd, F_SETLK, F_UNLCK, 0, 0);              /* the child gets its lock */
               ok = waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0 && ok;
           }
           close(p[0]);
           ok; }));

    /* OFD locks belong to the open file description. */
    EXPECT("OFD lock", (g = open("lk_a", O_RDWR)) >= 0 && lk(fd, F_OFD_SETLK, F_WRLCK, 0, 1) == 0);
    EXPECT("another description is kept out", lk(g, F_OFD_SETLK, F_WRLCK, 0, 1) == -1 && errno == EAGAIN);
    EXPECT("OFD and record locks meet", lk(g, F_SETLK, F_RDLCK, 0, 1) == -1);
    EXPECT("a forked child is kept out too", in_child(c_ofd_blocked, g) == 1);
    EXPECT("OFD F_GETLK says l_pid -1", ({ pid_t p = 0; getlk(g, F_OFD_GETLK, F_RDLCK, 0, 1, &p) == F_WRLCK && p == -1; }));
    EXPECT("closing the description drops it", close(fd) == 0 && lk(g, F_OFD_SETLK, F_WRLCK, 0, 1) == 0 &&
           lk(g, F_OFD_SETLK, F_UNLCK, 0, 1) == 0);

    /* flock locks the whole file, also per description. */
    EXPECT("flock LOCK_EX", (fd = open("lk_a", O_RDWR)) >= 0 && flock(fd, LOCK_EX) == 0);
    EXPECT("flock LOCK_NB is EWOULDBLOCK", flock(g, LOCK_SH | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    EXPECT("flock and fcntl locks do not meet", lk(g, F_SETLK, F_WRLCK, 0, 0) == 0 && lk(g, F_SETLK, F_UNLCK, 0, 0) == 0);
    EXPECT("flock LOCK_SH is shared", flock(fd, LOCK_SH) == 0 && in_child(c_flock_shared, g) == 1);
    EXPECT("flock LOCK_UN", flock(fd, LOCK_UN) == 0 && flock(g, LOCK_EX | LOCK_NB) == 0 && flock(g, LOCK_UN) == 0);
    close(fd);
    close(g);
    unlink("lk_a");
    return done();
}
