/* posix_spawn through the Linux system calls, against musl: it runs the
 * child with clone(CLONE_VM | CLONE_VFORK) on its own stack in the
 * parent's memory. A program that starts, its exit status, and one that
 * does not exist (the error comes back through a close-on-exec pipe). */
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(void)
{
    int before = 1234;
    pid_t child;
    char *ok[] = { "ash", "-c", "echo spawned $((2 + 3)); exit 6", 0 };
    int r = posix_spawn(&child, "/bin/ash", 0, 0, ok, environ);
    if (r != 0) {
        printf("mspawn: posix_spawn failed (%d)\n", r);
        return 1;
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 6) {
        printf("mspawn: wrong status %d\n", status);
        return 1;
    }
    char *none[] = { "nothere", 0 };
    r = posix_spawn(&child, "/nothere", 0, 0, none, environ);
    if (r != ENOENT) {
        printf("mspawn: spawning nothing gave %d\n", r);
        return 1;
    }
    printf("mspawn: ok, kept %d\n", before);
    return 0;
}
