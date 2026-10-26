/* A minimal job-control shell: it runs a child in a group of its own as
 * the terminal's foreground group, and waits for it with WUNTRACED. Ctrl-Z
 * typed at the terminal must stop the child (and not the parent); the
 * parent then writes "stopped", resumes the child with SIGCONT, ends it,
 * and writes "resumed" to the file named by its argument.
 * Used by test/compat/tty.txt. */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 3;
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGTSTP, SIG_DFL);
        setpgid(0, 0);
        for (;;) pause();
    }
    setpgid(pid, pid);
    tcsetpgrp(fd, pid);
    write(fd, "ready\r\n", 7);
    int st;
    if (waitpid(pid, &st, WUNTRACED) != pid || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGTSTP) return 4;
    tcsetpgrp(fd, getpgrp());
    FILE *out = fopen(argv[1], "w");
    if (!out) return 5;
    fputs("stopped\n", out);
    kill(-pid, SIGCONT);
    kill(-pid, SIGTERM);
    if (waitpid(pid, &st, 0) != pid || !WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) return 6;
    fputs("resumed\n", out);
    fclose(out);
    return 0;
}
