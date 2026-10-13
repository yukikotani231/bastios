/* What crun does to protect its own binary: copy /proc/self/exe into a
 * sealed memfd and run that (a bind mount of /proc/self/exe is its first
 * choice), then find in the copy that /proc/self/exe is a sealed memfd. */
#include "compat.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#endif
#define SEALS 15 /* SEAL | SHRINK | GROW | WRITE */

extern char **environ;

int main(int argc, char **argv) {
    if (argc > 1) {
        /* the sealed copy: /proc/self/exe is the memfd, with all seals */
        int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        return fd >= 0 && fcntl(fd, F_GET_SEALS) == SEALS ? 7 : 8;
    }
    setup();
    struct stat st;
    int fd, bin, st_ok;

    bin = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    EXPECT("/proc/self/exe opens and is not sealed", bin >= 0 && fcntl(bin, F_GET_SEALS) == -1);
    st_ok = bin >= 0 && fstat(bin, &st) == 0 && st.st_size > 0;
    EXPECT("fstat of /proc/self/exe has the size", st_ok);
    fd = memfd_create("crun_cloned:/proc/self/exe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    EXPECT("memfd_create with sealing", fd >= 0);
    long sent = 0;
    while (st_ok && fd >= 0 && sent < st.st_size) {
        long n = sendfile(fd, bin, NULL, st.st_size - sent);
        if (n <= 0) break;
        sent += n;
    }
    EXPECT("sendfile copies the binary into the memfd", st_ok && sent == st.st_size);
    EXPECT("F_ADD_SEALS seals it", fd >= 0 && fcntl(fd, F_ADD_SEALS, SEALS) == 0 && fcntl(fd, F_GET_SEALS) == SEALS);
    EXPECT("a write to the sealed memfd is EPERM", fd >= 0 && write(fd, "x", 1) == -1 && errno == EPERM);
    int status = 0;
    pid_t p = fork();
    if (p == 0) {
        char *av[] = {argv[0], "child", NULL};
        fexecve(fd, av, environ);
        _exit(99);
    }
    EXPECT("fexecve of the sealed memfd runs it, and its /proc/self/exe is the sealed memfd",
           p > 0 && waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 7);
    return 0;
}
