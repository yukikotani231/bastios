/* Processes: fork, exec, wait, exit status, process groups and sessions,
 * identity, limits, system information. */
#include "compat.h"
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sched.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return 7;                                   /* run by the execve check */
    setup();
    int st = 0;
    pid_t p;

    EXPECT("getpid", getpid() > 0);
    EXPECT("getppid", getppid() > 0);
    EXPECT("fork + exit status", (p = fork()) >= 0 && (p == 0 ? (_exit(5), 0) :
           waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 5));
    pid_t me = getpid();
    EXPECT("child's getppid is the parent", (p = fork()) >= 0 && (p == 0 ? (_exit(getppid() == me ? 0 : 1), 0) :
           waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 0));
    EXPECT("fork copies memory", ({ static int v = 1; (p = fork()) >= 0 && (p == 0 ? (v = 2, _exit(v), 0) :
           waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 2 && v == 1); }));
    EXPECT("execve", (p = fork()) >= 0 && (p == 0 ? (execl(argv[0], argv[0], "child", (char *)0), _exit(99), 0) :
           waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 7));
    EXPECT("execve missing is ENOENT", execl("/no/such", "x", (char *)0) == -1 && errno == ENOENT);
    EXPECT("waitpid WNOHANG", (p = fork()) >= 0 && (p == 0 ? (sleep(1), _exit(0), 0) :
           waitpid(p, &st, WNOHANG) == 0 && waitpid(p, &st, 0) == p));
    EXPECT("wait with no children is ECHILD", wait(&st) == -1 && errno == ECHILD);
    EXPECT("killed child's status", (p = fork()) >= 0 && (p == 0 ? (pause(), _exit(0), 0) :
           kill(p, 9) == 0 && waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == 9));
    EXPECT("waitid", (p = fork()) >= 0 && (p == 0 ? (_exit(3), 0) : ({ siginfo_t si;
           waitid(P_PID, p, &si, WEXITED) == 0 && si.si_status == 3; })));
    EXPECT("getpgrp", getpgrp() > 0);
    EXPECT("setpgid", (p = fork()) >= 0 && (p == 0 ? (_exit(setpgid(0, 0) == 0 && getpgrp() == getpid() ? 0 : 1), 0) :
           waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 0));
    EXPECT("getsid", getsid(0) > 0);
    EXPECT("setsid in a child", (p = fork()) >= 0 && (p == 0 ? (_exit(setsid() == getpid() ? 0 : 1), 0) :
           waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 0));
    EXPECT("uid/gid", getuid() == geteuid() && getgid() == getegid());
    {
        gid_t g[16];
        EXPECT("getgroups", getgroups(16, g) >= 0);
    }
    EXPECT("setuid to self", setuid(getuid()) == 0);
    {
        struct utsname u;
        EXPECT("uname", uname(&u) == 0 && u.sysname[0] && u.machine[0]);
    }
    {
        struct rlimit r;
        EXPECT("getrlimit", getrlimit(RLIMIT_NOFILE, &r) == 0 && r.rlim_cur > 0);
        EXPECT("setrlimit", getrlimit(RLIMIT_CORE, &r) == 0 && setrlimit(RLIMIT_CORE, &r) == 0);
    }
    {
        struct rusage u;
        EXPECT("getrusage", getrusage(RUSAGE_SELF, &u) == 0);
    }
    {
        struct tms t;
        EXPECT("times", times(&t) != (clock_t)-1);
    }
    {
        struct sysinfo si;
        EXPECT("sysinfo", sysinfo(&si) == 0 && si.totalram > 0);
    }
    EXPECT("umask", (umask(022), umask(022) == 022));
    EXPECT("getpriority", (errno = 0, getpriority(PRIO_PROCESS, 0), errno == 0));
    EXPECT("nice", (errno = 0, nice(0) != -1 || errno == 0));
    EXPECT("sched_yield", sched_yield() == 0);
    {
        char h[65];
        EXPECT("gethostname", gethostname(h, sizeof h) == 0);
    }
    EXPECT("sysconf(_SC_NPROCESSORS_ONLN)", sysconf(_SC_NPROCESSORS_ONLN) >= 1);
    EXPECT("environment", setenv("COMPAT_X", "1", 1) == 0 && getenv("COMPAT_X") && strcmp(getenv("COMPAT_X"), "1") == 0);
    EXPECT("system()", system("exit 4") >= 0 && WEXITSTATUS(system("exit 4")) == 4);
    {
        FILE *f = popen("echo popen", "r");
        char b[16] = "";
        EXPECT("popen", f && fgets(b, sizeof b, f) && strcmp(b, "popen\n") == 0 && pclose(f) == 0);
    }
    {
        /* ps opens /proc/PID/stat, then reads it; a process that ended in
           between gives ESRCH, not an empty file (BusyBox's ps trusts it). */
        char path[32], b[64];
        int fd = -1;
        EXPECT("/proc/PID/stat of an ended process is ESRCH", (p = fork()) >= 0 && (p == 0 ? (pause(), _exit(0), 0) :
            (snprintf(path, sizeof path, "/proc/%d/stat", p), (fd = open(path, O_RDONLY)) >= 0 &&
             kill(p, SIGKILL) == 0 && waitpid(p, &st, 0) == p &&
             read(fd, b, sizeof b) == -1 && errno == ESRCH && close(fd) == 0)));
    }
    return done();
}
