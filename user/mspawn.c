/* posix_spawn through the Linux system calls, against musl: it runs the
 * child with clone(CLONE_VM | CLONE_VFORK) on its own stack in the
 * parent's memory. A program that starts, its exit status, and one that
 * does not exist (the error comes back through a close-on-exec pipe). */
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/times.h>
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
    /* system and popen run /bin/sh, which must be a POSIX shell. */
    int st = system("exit 4");
    if (st < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 4) {
        printf("mspawn: system gave %d\n", st);
        return 1;
    }
    FILE *f = popen("echo popen $((6 * 7))", "r");
    char line[32] = "";
    if (!f || !fgets(line, sizeof line, f) || strcmp(line, "popen 42\n") != 0 || pclose(f) != 0) {
        printf("mspawn: popen read '%s'\n", line);
        return 1;
    }
    pid_t c = fork();
    if (c == 0)
        _exit(3);
    siginfo_t si;
    if (c < 0 || waitid(P_PID, c, &si, WEXITED) != 0 || si.si_pid != c || si.si_code != CLD_EXITED
        || si.si_status != 3) {
        printf("mspawn: waitid failed (errno %d)\n", errno);
        return 1;
    }
    struct tms t;
    if (times(&t) == (clock_t)-1) {
        printf("mspawn: times failed\n");
        return 1;
    }
    printf("mspawn: ok, kept %d, %s\n", before, line[0] == 'p' ? "sh is POSIX" : "?");
    return 0;
}
