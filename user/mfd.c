/* More open file descriptions than the table once held (1024): several
 * processes each keep about 240 files open at once, so together they hold
 * far more than that, and each also opens directories, whose names the
 * kernel keeps in a table that grows with the description numbers
 * (dpath). A directory opened this late must still answer an openat
 * relative to it. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define KIDS 8
#define FILES 220
#define DIRS 12

static int fail(const char *what)
{
    printf("mfd: FAIL %s (errno %d)\n", what, errno);
    return 1;
}

/* Opens FILES files and DIRS directories and checks them, tells the parent
 * on `up`, then waits for the parent to close `go`. */
static int kid(int up, int go)
{
    int opened = 0;
    for (int i = 0; i < FILES; i++) {
        if (open("/etc/passwd", O_RDONLY) < 0 && open("/", O_RDONLY | O_DIRECTORY) < 0)
            return fail("opening a file");
        opened++;
    }
    for (int i = 0; i < DIRS; i++) {
        DIR *d = opendir("/");
        if (!d)
            return fail("opendir");
        int sub = openat(dirfd(d), "etc", O_RDONLY | O_DIRECTORY);
        if (sub < 0)
            return fail("openat relative to a late directory");
        close(sub);
    }
    char c = 'k';
    if (write(up, &c, 1) != 1)
        return fail("telling the parent");
    char b;
    while (read(go, &b, 1) > 0)
        ;
    return 0;
}

int main(void)
{
    int up[2], go[2];
    if (pipe(up) || pipe(go))
        return fail("pipe");
    pid_t pids[KIDS];
    for (int i = 0; i < KIDS; i++) {
        pids[i] = fork();
        if (pids[i] < 0)
            return fail("fork");
        if (pids[i] == 0) {
            close(up[0]);
            close(go[1]);
            _exit(kid(up[1], go[0]));
        }
    }
    close(up[1]);
    close(go[0]);
    int ready = 0;
    char c;
    while (ready < KIDS && read(up[0], &c, 1) == 1)
        ready++;
    if (ready != KIDS) {
        printf("mfd: FAIL only %d of %d processes got their files\n", ready, KIDS);
        close(go[1]);
        for (int i = 0; i < KIDS; i++)
            waitpid(pids[i], 0, 0);
        return 1;
    }
    close(go[1]);
    for (int i = 0; i < KIDS; i++) {
        int st = 0;
        if (waitpid(pids[i], &st, 0) != pids[i] || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
            return fail("a child's status");
    }
    printf("mfd: ok, %d processes held %d descriptions\n", KIDS, KIDS * (FILES + DIRS));
    return 0;
}
