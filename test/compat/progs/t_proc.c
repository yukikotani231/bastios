/* Processes: fork, exec, wait, exit status, process groups and sessions,
 * identity, limits, system information. */
#include "compat.h"
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>

/* Reads up to n - 1 bytes of path into b as a string; the count, or -1. */
static int slurp(const char *path, char *b, int n) {
    int fd = open(path, O_RDONLY), r;
    if (fd < 0) return -1;
    r = read(fd, b, n - 1);
    close(fd);
    if (r >= 0) b[r] = 0;
    return r;
}

/* Walks the directory at path: every entry must stat, and the files
 * must open for reading. Returns the number of files, or -1. */
static int walk(const char *path, int depth) {
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;
    if (!d || depth > 8) return -1;
    while ((e = readdir(d))) {
        char sub[600];
        struct stat sb;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (stat(sub, &sb) != 0) { n = -1; break; }
        if (S_ISDIR(sb.st_mode)) {
            int m = walk(sub, depth + 1);
            if (m < 0) { n = -1; break; }
            n += m;
        } else {
            int fd = open(sub, O_RDONLY);
            if (fd < 0) { n = -1; break; }
            close(fd);
            n++;
        }
    }
    closedir(d);
    return n;
}

/* Writes s to a new file at path. */
static int put(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    return f && fputs(s, f) >= 0 && fclose(f) == 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return 7;                                   /* run by the execve check */
    if (argc > 1 && strcmp(argv[1], "bang") == 0)   /* run by the "#!" checks */
        return argc == 4 && strcmp(argv[2], "./cp_bang") == 0 && strcmp(argv[3], "a") == 0 ? 8 : 9;
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
    {
        /* A script runs through the interpreter its "#!" line names, with
         * the line's argument and the script's path before the rest. */
        char self[256], line[300];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        self[n > 0 ? n : 0] = 0;
        snprintf(line, sizeof line, "#! %s  bang \necho not here\n", self);
        EXPECT("execve of a #! script", put("cp_bang", line) && chmod("cp_bang", 0755) == 0
            && (p = fork()) >= 0 && (p == 0 ? (execl("./cp_bang", "./cp_bang", "a", (char *)0), _exit(99), 0) :
            waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 8));
        EXPECT("#! naming no interpreter is ENOEXEC", put("cp_bang", "#!\n") &&
            execl("./cp_bang", "./cp_bang", (char *)0) == -1 && errno == ENOEXEC);
        EXPECT("#! naming a missing interpreter is ENOENT", put("cp_bang", "#!/no/such/sh\n") &&
            execl("./cp_bang", "./cp_bang", (char *)0) == -1 && errno == ENOENT);
        unlink("cp_bang");
    }
    {
        /* The plain sysctl numbers a program reads for a default, and
         * writes back to the same file. */
        char b[64];
        EXPECT("sysctl somaxconn reads 4096", slurp("/proc/sys/net/core/somaxconn", b, sizeof b) > 0 && strcmp(b, "4096\n") == 0);
        EXPECT("sysctl overcommit_memory reads 0", slurp("/proc/sys/vm/overcommit_memory", b, sizeof b) > 0 && strcmp(b, "0\n") == 0);
        EXPECT("sysctl max_map_count reads 65530", slurp("/proc/sys/vm/max_map_count", b, sizeof b) > 0 && strcmp(b, "65530\n") == 0);
        EXPECT("sysctl cap_last_cap reads 40", slurp("/proc/sys/kernel/cap_last_cap", b, sizeof b) > 0 && strcmp(b, "40\n") == 0);
        EXPECT("sysctl ip_local_port_range has two numbers", slurp("/proc/sys/net/ipv4/ip_local_port_range", b, sizeof b) > 0 && strcmp(b, "32768\t60999\n") == 0);
        EXPECT("writing a sysctl number reads back", put("/proc/sys/vm/swappiness", "10\n") && slurp("/proc/sys/vm/swappiness", b, sizeof b) > 0 && strcmp(b, "10\n") == 0);
        EXPECT("a sysctl number rejects text", ({ int fd = open("/proc/sys/vm/swappiness", O_WRONLY); int r = fd < 0 ? 0 : (write(fd, "x\n", 2) == -1 && errno == EINVAL); if (fd >= 0) close(fd); r; }));
        EXPECT("a read-only sysctl is not opened for writing", open("/proc/sys/fs/file-nr", O_WRONLY) == -1);
        EXPECT("a sysctl file is a regular file", ({ struct stat sb; stat("/proc/sys/vm/swappiness", &sb) == 0 && S_ISREG(sb.st_mode) && (sb.st_mode & 0777) == 0644; }));
    }
    {
        /* /proc/sys lists, and everything it lists opens. */
        struct stat sb;
        DIR *d = opendir("/proc");
        struct dirent *e;
        int seen = 0;
        while (d && (e = readdir(d))) if (!strcmp(e->d_name, "sys")) seen = 1;
        if (d) closedir(d);
        EXPECT("/proc lists sys", seen);
        EXPECT("/proc/sys is a directory", stat("/proc/sys", &sb) == 0 && S_ISDIR(sb.st_mode));
        EXPECT("/proc/sys/net/core is a directory", stat("/proc/sys/net/core", &sb) == 0 && S_ISDIR(sb.st_mode));
        EXPECT("a directory under /proc/sys that is not there is ENOENT", stat("/proc/sys/nosuch", &sb) == -1 && errno == ENOENT);
        EXPECT("every file listed under /proc/sys opens", walk("/proc/sys", 0) >= 60);
        d = opendir("/proc/sys/vm");
        seen = 0;
        while (d && (e = readdir(d))) if (!strcmp(e->d_name, "swappiness")) seen = 1;
        if (d) closedir(d);
        EXPECT("/proc/sys/vm lists swappiness", seen);
    }
    return done();
}
