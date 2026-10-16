/* Many namespaces at once, as a host running a few dozen containers needs:
 * twenty-four UTS, user and PID namespaces held open together, each with
 * a name or id of its own, and a namespace freed when its last member
 * leaves, so the slots can be taken again. */
#include "compat.h"
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>

#define N 24

/* Starts a child that makes a namespace of KIND, tells us over UP whether
 * it did (and for UTS, that its name stuck), then waits on DOWN. */
static pid_t hold(int kind, int idx, int up[2], int down[2]) {
    pid_t c = fork();
    if (c != 0) return c;
    char ok = 'n', b;
    char name[16];
    snprintf(name, sizeof name, "ns%d", idx);
    if (unshare(kind) == 0) {
        ok = 'y';
        if (kind == CLONE_NEWUTS) {
            char got[16] = "";
            if (sethostname(name, strlen(name)) != 0 || gethostname(got, sizeof got) != 0 || strcmp(got, name) != 0) ok = 'b';
        }
    }
    if (write(up[1], &ok, 1) != 1) _exit(2);
    if (read(down[0], &b, 1) < 0) _exit(3);
    _exit(ok == 'y' ? 0 : 1);
}

static int many(int kind) {
    int up[2], down[2];
    if (pipe(up) != 0 || pipe(down) != 0) return -1;
    pid_t kids[N];
    int made = 0;
    for (int i = 0; i < N; i++) {
        kids[i] = hold(kind, i, up, down);
        char r;
        if (kids[i] < 0 || read(up[0], &r, 1) != 1) break;
        if (r == 'y') made++;
    }
    char go = 'g';
    for (int i = 0; i < N; i++) (void)!write(down[1], &go, 1);
    int clean = 0;
    for (int i = 0; i < N; i++) {
        int st;
        if (kids[i] > 0 && waitpid(kids[i], &st, 0) == kids[i] && WIFEXITED(st) && WEXITSTATUS(st) == 0) clean++;
    }
    close(up[0]); close(up[1]); close(down[0]); close(down[1]);
    return made == N && clean == N ? N : made;
}

/* 60 tmpfs mounts at once, each with its own files, then all unmounted. */
static int many_tmpfs(void) {
    char d[32], f[48], b[16];
    int n = 0, ok = 1;
    for (int i = 0; i < 60; i++) {
        snprintf(d, sizeof d, "nm_t%d", i);
        mkdir(d, 0755);
        if (mount("none", d, "tmpfs", 0, NULL) != 0) { ok = 0; break; }
        n++;
        snprintf(f, sizeof f, "%s/f", d);
        FILE *fp = fopen(f, "w");
        if (!fp) { ok = 0; break; }
        fprintf(fp, "m%d", i);
        fclose(fp);
    }
    for (int i = 0; ok && i < n; i++) {
        snprintf(f, sizeof f, "nm_t%d/f", i);
        FILE *fp = fopen(f, "r");
        memset(b, 0, sizeof b);
        char want[16];
        snprintf(want, sizeof want, "m%d", i);
        if (!fp || !fgets(b, sizeof b, fp) || strcmp(b, want) != 0) ok = 0;
        if (fp) fclose(fp);
    }
    for (int i = 0; i < n; i++) {
        snprintf(d, sizeof d, "nm_t%d", i);
        snprintf(f, sizeof f, "%s/f", d);
        unlink(f);
        umount(d);
        rmdir(d);
    }
    return ok && n == 60;
}

/* PID namespaces nest six deep: each process makes one and starts the init
 * of the next (id 1 there). The init of the sixth cannot make a seventh
 * (ENOSPC), and from the first namespace its /proc/PID/status lists its id
 * in all seven (NSpid), the last being 1. */
static int nest(int depth, int up, int down) {
    if (depth == 7) {
        char b;
        if (getpid() != 1) return 1;
        if (unshare(CLONE_NEWPID) != -1 || errno != ENOSPC) return 2;
        if (write(up, "r", 1) != 1 || read(down, &b, 1) < 0) return 3;
        return 0;
    }
    if (unshare(CLONE_NEWPID) != 0) return 10 + depth;
    pid_t c = fork();
    if (c < 0) return 20 + depth;
    if (c == 0) _exit(nest(depth + 1, up, down));
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st)) return 30 + depth;
    return WEXITSTATUS(st);
}

static int nested_pidns(void) {
    int up[2], down[2];
    if (pipe(up) != 0 || pipe(down) != 0) return 0;
    pid_t c = fork();
    if (c == 0) _exit(nest(1, up[1], down[0]));
    close(up[1]);
    close(down[0]);
    signal(SIGPIPE, SIG_IGN);
    char r = 0;
    int found = 0;
    if (read(up[0], &r, 1) == 1 && r == 'r') {
        DIR *d = opendir("/proc");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char path[64], line[256];
            snprintf(path, sizeof path, "/proc/%s/status", e->d_name);
            FILE *f = fopen(path, "r");
            while (f && fgets(line, sizeof line, f)) {
                if (strncmp(line, "NSpid:", 6) != 0) continue;
                int fields = 0;
                for (char *q = line + 6; *q; q++)
                    if (*q != ' ' && *q != '\t' && *q != '\n' && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == ':')) fields++;
                if (fields == 7 && strstr(line, "\t1\n")) found = 1;   /* the first and six inside, the init of the last is 1 */
            }
            if (f) fclose(f);
        }
        if (d) closedir(d);
    }
    (void)!write(down[1], "g", 1);
    int st;
    int ok = waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok) errno = WIFEXITED(st) ? 1000 + WEXITSTATUS(st) : 2000;
    return ok && found;
}

int main(void) {
    setup();
    EXPECT("24 UTS namespaces at once, each with its own name", many(CLONE_NEWUTS) == N);
    EXPECT("and 24 more once those are gone", many(CLONE_NEWUTS) == N);
    EXPECT("24 PID namespaces at once", many(CLONE_NEWPID) == N);
    EXPECT("PID namespaces nest six deep, and no deeper", nested_pidns());
    EXPECT("24 IPC namespaces at once", many(CLONE_NEWIPC) == N);
    EXPECT("60 tmpfs mounts at once, each with its own files", many_tmpfs());
    return done();
}
