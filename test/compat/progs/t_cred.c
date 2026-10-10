/* User and group ids: /etc/passwd names nobody, the set*id calls change
 * the ids and follow Linux's rules once the process is no longer root,
 * a child inherits them, and /proc/self/status shows them. */
#include "compat.h"
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/fsuid.h>
#include <sys/wait.h>
#include <unistd.h>

/* Runs f in a child and reports whether it returned nonzero. */
static int in_child(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) _exit(f() ? 0 : 1);
    int st; return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int drop_to_nobody(void) {
    if (setresgid(65534, 65534, 65534) || setresuid(65534, 65534, 0)) return 0;
    uid_t r, e, s; getresuid(&r, &e, &s);
    if (r != 65534 || e != 65534 || s != 0 || getgid() != 65534 || getegid() != 65534) return 0;
    if (setuid(1) != -1 || errno != EPERM) return 0;                 /* not an id it holds */
    if (setgroups(0, 0) != -1 || errno != EPERM) return 0;
    if (seteuid(0) || geteuid() != 0 || getuid() != 65534) return 0;  /* back through the saved id */
    return setuid(0) == 0 && getuid() == 0 && geteuid() == 0;
}

static int reuid_rules(void) {
    if (setreuid(1, 2)) return 0;                                    /* root: anything */
    uid_t r, e, s; getresuid(&r, &e, &s);
    if (r != 1 || e != 2 || s != 2) return 0;                        /* saved follows effective */
    if (setreuid(-1, 3) != -1 || errno != EPERM) return 0;
    if (setreuid(2, 1) || getuid() != 2 || geteuid() != 1) return 0; /* swapping is allowed */
    return setfsuid(-1) == 1 && setfsuid(5) == 1 && setfsuid(-1) == 1;  /* 5 is not held */
}

static int child_sees(void) { return getuid() == 7 && geteuid() == 7; }

static int status_shows(void) {
    char b[1024] = {0};
    int fd = open("/proc/self/status", O_RDONLY); int n = read(fd, b, sizeof b - 1); close(fd);
    return n > 0 && strstr(b, "Uid:\t7\t7\t7\t7\n") != 0;
}

static int inherited(void) {
    return setuid(7) == 0 && in_child(child_sees) && status_shows();
}

int main(void) {
    setup();
    EXPECT("getpwnam finds nobody", ({ struct passwd *p = getpwnam("nobody"); p && p->pw_uid == 65534; }));
    EXPECT("getgrnam finds users", ({ struct group *g = getgrnam("users"); g && g->gr_gid == 100; }));
    EXPECT("root starts as uid and gid 0", getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);
    EXPECT("an unprivileged process keeps to the ids it holds", in_child(drop_to_nobody));
    EXPECT("setreuid and setfsuid follow Linux's rules", in_child(reuid_rules));
    EXPECT("a child inherits ids; /proc/self/status shows them", in_child(inherited));
    EXPECT("the parent is still root", getuid() == 0 && geteuid() == 0);
    return done();
}
