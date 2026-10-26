/* Processes that share a descriptor table (clone with CLONE_FILES) or a working directory and umask (CLONE_FS): what one
 * opens or closes the other sees, until one of them unshares it (unshare,
 * close_range with CLOSE_RANGE_UNSHARE, execve), kcmp says which processes
 * share, and unshare(CLONE_FILES) fails past /proc/sys/fs/nr_open. */
#include "compat.h"
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define STACK 65536
#define KCMP_FILES 2
#define KCMP_FS 3
#ifndef CLOSE_RANGE_UNSHARE
#define CLOSE_RANGE_UNSHARE (1U << 1)
#endif

static int open_fd(int fd) { return fcntl(fd, F_GETFD) >= 0; }
static long kcmp_files(int a, int b) { return syscall(SYS_kcmp, a, b, KCMP_FILES, 0, 0); }

struct arg {
    int mode;
    int fd;          /* a descriptor the parent opened */
    int up, go;      /* the child tells the parent on up, and waits on go */
    int flags;       /* clone flags, besides SIGCHLD */
    pid_t parent;
};

static int child(void *p)
{
    struct arg *a = p;
    char c = 's';
    switch (a->mode) {
    case 0:   /* opens one: the parent has it too */
        return open("/etc/passwd", O_RDONLY) < 0 ? 10 : 0;
    case 1:   /* the parent's descriptor goes, in the shared table */
        return close(a->fd) ? 12 : 0;
    case 2:   /* unshare: later changes stay here */
        if (unshare(CLONE_FILES)) return 13;
        if (close(a->fd)) return 14;
        return open("/etc/passwd", O_RDONLY) < 0 ? 15 : 0;
    case 3:   /* close_range with CLOSE_RANGE_UNSHARE */
        if (syscall(SYS_close_range, 3, ~0U, CLOSE_RANGE_UNSHARE)) return 16;
        return open_fd(a->fd) ? 17 : 0;
    case 4:   /* kcmp: shares, then does not */
        if (kcmp_files(a->parent, getpid()) != 0) return 18;
        write(a->up, &c, 1);
        read(a->go, &c, 1);
        if (unshare(CLONE_FILES)) return 19;
        return kcmp_files(a->parent, getpid()) == 0 ? 20 : 0;
    case 5: { /* execve gives the program a table of its own */
        int x = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
        if (x < 0) return 21;
        char *av[] = { "t_fdshare", "x", NULL };
        execv("/proc/self/exe", av);
        return 22;
    }
    case 7:   /* the working directory is the parent's too */
        return chdir("/etc") ? 30 : 0;
    case 8:
        umask(077);
        return 0;
    case 9:   /* unshare(CLONE_FS): the child's chdir stays with it */
        if (unshare(CLONE_FS)) return 31;
        return chdir("/etc") ? 32 : 0;
    case 10:  /* kcmp(KCMP_FS) */
        return syscall(SYS_kcmp, a->parent, getpid(), KCMP_FS, 0, 0) == 0 ? 0 : 33;
    case 6:   /* a copy would hold a descriptor past nr_open */
        return unshare(CLONE_FILES) == 0 || errno != EMFILE ? 23 : 0;
    }
    return 99;
}

/* Runs child(a) with CLONE_FILES to its end. */
static int run(struct arg *a)
{
    int st;
    char *stack = malloc(STACK);
    pid_t c = stack ? clone(child, stack + STACK, a->flags | SIGCHLD, a) : -1;
    if (c < 0 || waitpid(c, &st, 0) != c) return -1;
    free(stack);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) return 0;                /* the program mode 5 runs */
    setup();
    int up[2], go[2];
    if (pipe(up) || pipe(go)) return 1;
    struct arg a = { 0, -1, up[1], go[0], CLONE_FILES, getpid() };

    /* What the child opens, the parent has: its descriptor is the lowest free one. */
    int first = open("/etc/passwd", O_RDONLY);
    close(first);
    a.mode = 0;
    EXPECT("a descriptor the child opens is open in the parent", run(&a) == 0 && open("/etc/passwd", O_RDONLY) == first + 1);
    close(first + 1);
    close(first);

    a.mode = 1;
    a.fd = open("/etc/passwd", O_RDONLY);
    EXPECT("a descriptor the child closes is closed in the parent", run(&a) == 0 && !open_fd(a.fd));

    a.mode = 2;
    a.fd = open("/etc/passwd", O_RDONLY);
    EXPECT("after unshare(CLONE_FILES) the child's changes stay with it", run(&a) == 0 && open_fd(a.fd));
    close(a.fd);

    a.mode = 3;
    a.fd = open("/etc/passwd", O_RDONLY);
    EXPECT("close_range with CLOSE_RANGE_UNSHARE closes only the caller's copy", run(&a) == 0 && open_fd(a.fd));
    close(a.fd);

    if (begin("kcmp(KCMP_FILES) tells sharing processes from the rest")) {
        a.mode = 4;
        char *stack = malloc(STACK), b;
        pid_t c = clone(child, stack + STACK, CLONE_FILES | SIGCHLD, &a);
        int ok = c > 0 && read(up[0], &b, 1) == 1 && kcmp_files(getpid(), c) == 0;
        write(go[1], "g", 1);
        int st;
        ok = ok && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0 && kcmp_files(getpid(), 1) != 0;
        if (ok) pass(); else fail("kcmp");
    }

    if (begin("/proc/PID/fd/N of a process that shares the table reads the shared descriptors")) {
        /* The descriptors sit in the row of whichever sharer made the last call; the
         * parent reads the child's links after a call of its own. */
        a.mode = 4;
        char *stack = malloc(STACK), b, link[64], path[64];
        pid_t c = clone(child, stack + STACK, CLONE_FILES | SIGCHLD, &a);
        int ok = c > 0 && read(up[0], &b, 1) == 1;
        snprintf(path, sizeof path, "/proc/%d/fd/%d", (int)c, go[0]);
        ssize_t n = ok ? readlink(path, link, sizeof link - 1) : -1;
        ok = ok && n > 5 && !strncmp(link, "pipe:[", 6);
        write(go[1], "g", 1);
        int st;
        ok = c > 0 && waitpid(c, &st, 0) == c && ok;
        if (ok) pass(); else fail("proc fd of a sharer");
    }

    a.mode = 5;
    int x = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
    close(x);
    EXPECT("execve gives the program a table of its own", run(&a) == 0 && open("/etc/passwd", O_RDONLY) == x + 1);
    close(x + 1);
    close(x);

    if (begin("unshare(CLONE_FILES) is EMFILE past nr_open")) {
        char old[32];
        int nf = open("/proc/sys/fs/nr_open", O_RDWR);
        int n = nf < 0 ? -1 : read(nf, old, sizeof old - 1);
        int ok = n > 0 && write(nf, "64", 2) == 2 && dup2(2, 65) == 65;
        a.mode = 6;
        ok = ok && run(&a) == 0;
        if (n > 0) {
            old[n] = 0;
            lseek(nf, 0, SEEK_SET);
            write(nf, old, strlen(old));
        }
        close(65);
        if (ok) pass(); else fail("nr_open");
    }
    {
        char cwd[64], here[64];
        a.flags = CLONE_FS;
        getcwd(here, sizeof here);
        a.mode = 7;
        EXPECT("a working directory the child sets is the parent's", run(&a) == 0 && getcwd(cwd, sizeof cwd) && strstr(cwd, "etc"));
        chdir(here);
        mode_t old = umask(022);
        a.mode = 8;
        EXPECT("a umask the child sets is the parent's", run(&a) == 0 && umask(old) == 077);
        a.mode = 9;
        EXPECT("after unshare(CLONE_FS) the child's chdir stays with it", run(&a) == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, here));
        a.mode = 10;
        EXPECT("kcmp(KCMP_FS) says processes that share a working directory do", run(&a) == 0 && syscall(SYS_kcmp, getpid(), 1, KCMP_FS, 0, 0) != 0);
        chdir(here);
    }
    return done();
}
