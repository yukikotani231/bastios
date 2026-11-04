/* Landlock ABI 1: a process restricts itself to the paths it names. */
#include "compat.h"
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define LL_CREATE 444
#define LL_ADD 445
#define LL_RESTRICT 446

struct attr { unsigned long long handled; };
struct beneath { unsigned long long allowed; int parent_fd; } __attribute__((packed));

enum { EXEC = 1, WFILE = 2, RFILE = 4, RDIR = 8, RMDIR = 16, RMFILE = 32, MKDIR = 128, MKREG = 256, MKSYM = 4096 };

static int allow(int rs, const char *dir, unsigned long long acc) {
    int fd = open(dir, O_PATH);
    struct beneath b = { acc, fd };
    int r = syscall(LL_ADD, rs, 1, &b, 0);
    close(fd);
    return r;
}

int main(void) {
    setup();
    mkdir("/tmp/ll", 0755);
    mkdir("/tmp/ll/in", 0755);
    mkdir("/tmp/ll/out", 0755);
    close(open("/tmp/ll/in/f", O_CREAT | O_WRONLY, 0644));
    close(open("/tmp/ll/out/f", O_CREAT | O_WRONLY, 0644));

    EXPECT("the ABI version is 1", syscall(LL_CREATE, NULL, 0, 1) == 1);
    struct attr a = { RFILE | WFILE | MKREG | RMFILE | MKDIR | RMDIR };
    EXPECT("an unknown access is EINVAL", ({ struct attr bad = { 1ULL << 40 }; syscall(LL_CREATE, &bad, 8, 0) == -1 && errno == EINVAL; }));
    int rs = syscall(LL_CREATE, &a, sizeof a, 0);
    EXPECT("a ruleset is made", rs >= 0);
    EXPECT("a rule below a directory", allow(rs, "/tmp/ll/in", RFILE | WFILE | MKREG | RMFILE | MKDIR | RMDIR) == 0);

    pid_t p = fork();
    if (p == 0) {
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (syscall(LL_RESTRICT, rs, 0) != 0)
            _exit(1);
        close(rs);
        int e = 0;
        int fd = open("/tmp/ll/in/f", O_RDONLY);
        if (fd < 0) e |= 1; else close(fd);
        fd = open("/tmp/ll/in/g", O_CREAT | O_WRONLY, 0644);
        if (fd < 0) e |= 2; else close(fd);
        if (unlink("/tmp/ll/in/g") != 0) e |= 4;
        if (mkdir("/tmp/ll/in/d", 0755) != 0 || rmdir("/tmp/ll/in/d") != 0) e |= 8;
        if (open("/tmp/ll/out/f", O_RDONLY) != -1 || errno != EACCES) e |= 16;
        if (open("/tmp/ll/out/g", O_CREAT | O_WRONLY, 0644) != -1 || errno != EACCES) e |= 32;
        if (unlink("/tmp/ll/out/f") != -1 || errno != EACCES) e |= 64;
        if (mkdir("/tmp/ll/out/d", 0755) != -1 || errno != EACCES) e |= 128;
        if (symlink("x", "/tmp/ll/in/s") != -1 && errno != EACCES) e |= 256;
        if (rename("/tmp/ll/in/f", "/tmp/ll/out/f") != -1) e |= 512;
        _exit(e);
    }
    int st = 0;
    waitpid(p, &st, 0);
    EXPECT("a restricted child is held to its rules", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    EXPECT("the parent is not restricted", open("/tmp/ll/out/f", O_RDONLY) >= 0);
    return nfail != 0;
}
