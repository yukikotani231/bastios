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
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* Reads up to n - 1 bytes of path into b as a string; the count, or -1. */
static int slurp(const char *path, char *b, int n) {
    int fd = open(path, O_RDONLY), r;
    if (fd < 0) return -1;
    r = read(fd, b, n - 1);
    close(fd);
    if (r >= 0) b[r] = 0;
    return r;
}

/* Whether the maps text b has a line with permissions perm whose range
 * covers lo .. hi (neighbours with the same permissions merge into one),
 * and that ends in name if that is not NULL. */
static int maps_has(const char *b, unsigned long lo, unsigned long hi, const char *perm, const char *name) {
    unsigned long a, e;
    char pr[8];
    for (; *b; b++) {
        const char *nl = strchr(b, '\n');
        if (sscanf(b, "%lx-%lx %7s", &a, &e, pr) == 3 && a <= lo && e >= hi && !strncmp(pr, perm, 4)
            && (!name || (nl && nl - b >= (long)strlen(name) && !strncmp(nl - strlen(name), name, strlen(name))))) return 1;
        if (!nl) break;
        b = nl;
    }
    return 0;
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
    {
        /* The /proc/PID files with fixed answers, and oom_score_adj, which
         * is kept per process and inherited. */
        char b[64];
        EXPECT("oom_score reads a number", slurp("/proc/self/oom_score", b, sizeof b) > 0 && b[0] >= '0' && b[0] <= '9');
        EXPECT("oom_score_adj starts at 0", slurp("/proc/self/oom_score_adj", b, sizeof b) > 0 && strcmp(b, "0\n") == 0);
        EXPECT("loginuid is unset", slurp("/proc/self/loginuid", b, sizeof b) > 0 && strcmp(b, "4294967295\n") == 0);
        EXPECT("coredump_filter reads its default", slurp("/proc/self/coredump_filter", b, sizeof b) > 0 && strcmp(b, "00000033\n") == 0);
        EXPECT("cpuset is the root", slurp("/proc/self/cpuset", b, sizeof b) > 0 && strcmp(b, "/\n") == 0);
        EXPECT("oom_score_adj reads back a negative value", put("/proc/self/oom_score_adj", "-500\n") && slurp("/proc/self/oom_score_adj", b, sizeof b) > 0 && strcmp(b, "-500\n") == 0);
        EXPECT("a child inherits oom_score_adj", (p = fork()) >= 0 && (p == 0 ? (_exit(slurp("/proc/self/oom_score_adj", b, sizeof b) > 0 && strcmp(b, "-500\n") == 0 ? 0 : 1), 0) :
            waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0));
        EXPECT("oom_score_adj above 1000 is EINVAL", ({ int fd = open("/proc/self/oom_score_adj", O_WRONLY); int r = fd < 0 ? 0 : (write(fd, "1001\n", 5) == -1 && errno == EINVAL); if (fd >= 0) close(fd); r; }));
        EXPECT("oom_score_adj rejects text", ({ int fd = open("/proc/self/oom_score_adj", O_WRONLY); int r = fd < 0 ? 0 : (write(fd, "x\n", 2) == -1 && errno == EINVAL); if (fd >= 0) close(fd); r; }));
        EXPECT("oom_score_adj can be set back to 0", put("/proc/self/oom_score_adj", "0\n") && slurp("/proc/self/oom_score_adj", b, sizeof b) > 0 && strcmp(b, "0\n") == 0);
        EXPECT("/proc/self lists oom_score_adj", ({ DIR *d = opendir("/proc/self"); struct dirent *e; int seen = 0; while (d && (e = readdir(d))) if (!strcmp(e->d_name, "oom_score_adj")) seen = 1; if (d) closedir(d); seen; }));
    }
    {
        /* /proc/PID/fdinfo: position, flags, mount and inode of each open
         * descriptor, and what its kind adds. */
        char b[400], path[64];
        int f = open("/proc/self/exe", O_RDONLY | O_CLOEXEC), e = eventfd(0, 0), n = inotify_init1(0), w = -1;
        char c5[5];
        uint64_t one = 5;
        EXPECT("fdinfo of a file starts at position 0", f >= 0 && (snprintf(path, sizeof path, "/proc/self/fdinfo/%d", f), slurp(path, b, sizeof b)) > 0 && strncmp(b, "pos:\t0\n", 7) == 0);
        EXPECT("fdinfo follows the position", read(f, c5, 5) == 5 && slurp(path, b, sizeof b) > 0 && strncmp(b, "pos:\t5\n", 7) == 0);
        EXPECT("fdinfo flags carry O_CLOEXEC", strstr(b, "flags:\t02000000\n") != NULL);
        EXPECT("fdinfo has mnt_id and ino", strstr(b, "\nmnt_id:\t") != NULL && strstr(b, "\nino:\t") != NULL);
        EXPECT("an eventfd shows its count", e >= 0 && write(e, &one, 8) == 8 && (snprintf(path, sizeof path, "/proc/self/fdinfo/%d", e), slurp(path, b, sizeof b)) > 0 && strstr(b, "eventfd-count: 0000000000000005\n") != NULL);
        EXPECT("an inotify instance lists its watches", n >= 0 && (w = inotify_add_watch(n, ".", IN_MODIFY | IN_ONESHOT)) > 0 && (snprintf(path, sizeof path, "/proc/self/fdinfo/%d", n), slurp(path, b, sizeof b)) > 0
               && ({ int wd = 0; unsigned ino, sdev, mask; char *q = strstr(b, "inotify wd:"); q && sscanf(q, "inotify wd:%d ino:%x sdev:%x mask:%x", &wd, &ino, &sdev, &mask) == 4 && wd == w && mask == (IN_MODIFY | IN_ONESHOT); }));
        EXPECT("a pidfd shows the process", ({ int pf = syscall(434, getpid(), 0), r = 0; char want[32]; if (pf >= 0) { snprintf(path, sizeof path, "/proc/self/fdinfo/%d", pf); snprintf(want, sizeof want, "Pid:\t%d\n", (int)getpid()); r = slurp(path, b, sizeof b) > 0 && strstr(b, want) != NULL; close(pf); } r; }));
        EXPECT("fdinfo lists the open descriptors", ({ DIR *d = opendir("/proc/self/fdinfo"); struct dirent *de; int seen = 0; char nm[16]; snprintf(nm, sizeof nm, "%d", f); while (d && (de = readdir(d))) if (!strcmp(de->d_name, nm)) seen = 1; if (d) closedir(d); seen; }));
        EXPECT("/proc/self lists fdinfo", ({ DIR *d = opendir("/proc/self"); struct dirent *de; int seen = 0; while (d && (de = readdir(d))) if (!strcmp(de->d_name, "fdinfo")) seen = 1; if (d) closedir(d); seen; }));
        if (f >= 0) close(f);
        if (e >= 0) close(e);
        if (n >= 0) close(n);
    }
    {
        /* /proc/PID/maps: the regions the process really has. */
        static char mb[16384];
        char *a = mmap(NULL, 8 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        char *sh = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
                a[0] = 1;                                    /* touch one page; the rest stay untouched */
        EXPECT("maps lists an anonymous mapping", a != MAP_FAILED && slurp("/proc/self/maps", mb, sizeof mb) > 0
               && maps_has(mb, (unsigned long)a, (unsigned long)a + 8 * 4096, "rw-p", NULL));
        EXPECT("maps lists a shared mapping with s", sh != MAP_FAILED && slurp("/proc/self/maps", mb, sizeof mb) > 0
               && maps_has(mb, (unsigned long)sh, (unsigned long)sh + 2 * 4096, "rw-s", NULL));
        EXPECT("maps drops a mapping when it is unmapped", munmap(a, 8 * 4096) == 0 && slurp("/proc/self/maps", mb, sizeof mb) > 0
               && !maps_has(mb, (unsigned long)a, (unsigned long)a + 1, "rw-p", NULL) && !maps_has(mb, (unsigned long)a, (unsigned long)a + 1, "r-xp", NULL));
        EXPECT("maps shows a PROT_NONE page with no permissions", ({ char *g = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); int r = 0; if (g != MAP_FAILED && mprotect(g, 4096, PROT_NONE) == 0 && slurp("/proc/self/maps", mb, sizeof mb) > 0) r = maps_has(mb, (unsigned long)g, (unsigned long)g + 4096, "---p", NULL); r; }));
        EXPECT("maps has a [heap] after brk grows", ({ char *b0 = (char *)syscall(SYS_brk, 0); int r = 0; if ((char *)syscall(SYS_brk, b0 + 3 * 4096) == b0 + 3 * 4096) { b0[0] = 1; r = slurp("/proc/self/maps", mb, sizeof mb) > 0 && maps_has(mb, (unsigned long)b0, (unsigned long)b0 + 3 * 4096, "rw-p", "[heap]"); } r; }));
        EXPECT("maps has [stack] and [vdso]", strstr(mb, "[stack]\n") != NULL && strstr(mb, "[vdso]\n") != NULL);
        EXPECT("maps lines end at column 74 for names", ({ char *q = strstr(mb, "[stack]"); q && q[-1] == ' ' && ({ char *l = q; while (l > mb && l[-1] != '\n') l--; (size_t)(q - l) == 73; }); }));
    }
    EXPECT("readlink of /proc/self is the caller's id", ({ char lb[32], want[32]; ssize_t r = readlink("/proc/self", lb, sizeof lb - 1);
           snprintf(want, sizeof want, "%d", (int)getpid()); r > 0 && (lb[r] = 0, strcmp(lb, want) == 0); }));
    return done();
}
