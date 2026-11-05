/* Mount propagation: private, shared, slave and unbindable mounts, what
 * mountinfo says about them, and how a mount or an unmount below a shared
 * mount reaches its peers and their slaves, in the same mount namespace and
 * in another, as a Docker or Podman volume with rshared or rslave
 * propagation needs. */
#include "compat.h"
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifndef MS_REC
#define MS_REC 0x4000
#endif
#ifndef MS_UNBINDABLE
#define MS_UNBINDABLE (1 << 17)
#define MS_PRIVATE (1 << 18)
#define MS_SLAVE (1 << 19)
#define MS_SHARED (1 << 20)
#endif

#define D "/tmp/mp"

/* The optional fields (between the mount options and " - ") of the mountinfo
 * line of mount point mp, in a static buffer; NULL if there is no such line. */
static const char *opt_fields(const char *mp) {
    static char out[256];
    static char buf[16384];
    int fd = open("/proc/self/mountinfo", O_RDONLY);
    if (fd < 0) return NULL;
    int n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    for (char *l = buf; l && *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        /* ID PARENT MAJ:MIN ROOT MOUNTPOINT OPTIONS [FIELDS] - ... */
        char *p = l;
        int sp = 0;
        char *mpos = NULL, *opos = NULL;
        for (; *p; p++) {
            if (*p == ' ') {
                sp++;
                if (sp == 4) mpos = p + 1;
                if (sp == 5) opos = p + 1;
                if (sp == 5) break;
            }
        }
        if (mpos && opos && (size_t)(opos - 1 - mpos) == strlen(mp) && strncmp(mpos, mp, strlen(mp)) == 0) {
            char *dash = strstr(opos, " - ");
            char *f = strchr(opos, ' ');
            if (dash && f && f < dash) {
                size_t m = dash - f;
                if (m >= sizeof out) m = sizeof out - 1;
                memcpy(out, f, m);
                out[m] = 0;
            } else {
                out[0] = 0;
            }
            return out;
        }
        l = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/* "shared:N" of mount point mp: N, or -1. */
static int group_of(const char *mp) {
    const char *f = opt_fields(mp);
    const char *s = f ? strstr(f, "shared:") : NULL;
    return s ? atoi(s + 7) : -1;
}

static int has_field(const char *mp, const char *what) {
    const char *f = opt_fields(mp);
    return f && strstr(f, what) != NULL;
}

static int prop(const char *p, unsigned long fl) { return mount(NULL, p, NULL, fl, NULL) == 0; }

/* One command, run here or in the helper: returns a number (1/0 for yes/no). */
static int cmd(const char *line) {
    char op[16], a[128], b[128];
    a[0] = b[0] = 0;
    if (sscanf(line, "%15s %127s %127s", op, a, b) < 1) return 0;
    if (!strcmp(op, "unshare")) return unshare(CLONE_NEWNS) == 0;
    if (!strcmp(op, "tmpfs")) return mount("tmpfs", a, "tmpfs", 0, NULL) == 0;
    if (!strcmp(op, "umount")) return umount(a) == 0;
    if (!strcmp(op, "bind")) return mount(a, b, NULL, MS_BIND, NULL) == 0;
    if (!strcmp(op, "rbind")) return mount(a, b, NULL, MS_BIND | MS_REC, NULL) == 0;
    if (!strcmp(op, "shared")) return prop(a, MS_SHARED);
    if (!strcmp(op, "rshared")) return prop(a, MS_SHARED | MS_REC);
    if (!strcmp(op, "slave")) return prop(a, MS_SLAVE);
    if (!strcmp(op, "rslave")) return prop(a, MS_SLAVE | MS_REC);
    if (!strcmp(op, "private")) return prop(a, MS_PRIVATE);
    if (!strcmp(op, "rprivate")) return prop(a, MS_PRIVATE | MS_REC);
    if (!strcmp(op, "unbindable")) return prop(a, MS_UNBINDABLE);
    if (!strcmp(op, "mkdir")) return mkdir(a, 0755) == 0 || errno == EEXIST;
    if (!strcmp(op, "mkfile")) {
        int fd = open(a, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) return 0;
        close(fd);
        return 1;
    }
    if (!strcmp(op, "exists")) return access(a, F_OK) == 0;
    if (!strcmp(op, "gid")) return group_of(a);
    if (!strcmp(op, "has")) return has_field(a, b);
    if (!strcmp(op, "mounted")) return opt_fields(a) != NULL;
    return 0;
}

static int cw = -1, rr = -1;

/* The same command in the helper process, which has a mount namespace of its own. */
static int far(const char *line) {
    char b[200];
    snprintf(b, sizeof b, "%s\n", line);
    if (write(cw, b, strlen(b)) < 0) return -9;
    int n = read(rr, b, sizeof b - 1);
    if (n <= 0) return -9;
    b[n] = 0;
    return atoi(b);
}

static void helper(int cr, int rw) {
    char line[200];
    for (;;) {
        int n = 0;
        char c;
        while (n < (int)sizeof line - 1 && read(cr, &c, 1) == 1 && c != '\n') line[n++] = c;
        if (n == 0) _exit(0);
        line[n] = 0;
        char r[32];
        snprintf(r, sizeof r, "%d\n", cmd(line));
        if (write(rw, r, strlen(r)) < 0) _exit(1);
    }
}

#define C(...) ({ char _l[200]; snprintf(_l, sizeof _l, __VA_ARGS__); cmd(_l); })
#define F(...) ({ char _l[200]; snprintf(_l, sizeof _l, __VA_ARGS__); far(_l); })

int main(void) {
    setup();
    mkdir(D, 0755);
    mkdir(D "/a", 0755);
    mkdir(D "/b", 0755);
    mkdir(D "/c", 0755);
    mkdir(D "/v", 0755);
    EXPECT("unshare a mount namespace", cmd("unshare"));
    EXPECT("make the namespace private", cmd("rprivate /"));

    /* mountinfo and the four types */
    EXPECT("a bind is private at first", cmd("bind " D "/a " D "/a") && opt_fields(D "/a") && !has_field(D "/a", "shared") && !has_field(D "/a", "master"));
    EXPECT("MS_SHARED shows shared:N", cmd("shared " D "/a") && has_field(D "/a", "shared:") && group_of(D "/a") > 0);
    EXPECT("MS_SLAVE leaves the group of the last peer", cmd("slave " D "/a") && !has_field(D "/a", "shared:") && !has_field(D "/a", "master:"));
    EXPECT("MS_SHARED again is a new group", cmd("shared " D "/a") && group_of(D "/a") > 0);
    EXPECT("MS_UNBINDABLE shows unbindable", cmd("unbindable " D "/a") && has_field(D "/a", "unbindable") && !has_field(D "/a", "shared:"));
    EXPECT("an unbindable mount cannot be bound", !cmd("bind " D "/a " D "/b") && errno == EINVAL);
    EXPECT("MS_PRIVATE clears it", cmd("private " D "/a") && !has_field(D "/a", "unbindable") && cmd("bind " D "/a " D "/b") && cmd("umount " D "/b"));
    EXPECT("the propagation of a plain directory is EINVAL", !prop(D "/c", MS_SHARED) && errno == EINVAL);
    EXPECT("two types at once are EINVAL", !prop(D "/a", MS_SHARED | MS_PRIVATE) && errno == EINVAL);
    EXPECT("the root can be made shared", cmd("shared /") && has_field("/", "shared:"));
    EXPECT("and private again", cmd("private /") && !has_field("/", "shared:"));

    /* peers in one namespace */
    EXPECT("make a shared", cmd("shared " D "/a"));
    EXPECT("a bind of a shared mount is its peer", cmd("bind " D "/a " D "/b") && group_of(D "/b") == group_of(D "/a") && group_of(D "/a") > 0);
    EXPECT("make the sub directory", cmd("mkdir " D "/a/s"));
    EXPECT("mount a tmpfs below a", cmd("tmpfs " D "/a/s") && cmd("mkfile " D "/a/s/f"));
    EXPECT("it shows up below b", cmd("exists " D "/b/s/f") && cmd("mounted " D "/b/s"));
    EXPECT("both are in one group", group_of(D "/a/s") > 0 && group_of(D "/a/s") == group_of(D "/b/s"));
    EXPECT("unmounting below b unmounts below a", cmd("umount " D "/b/s") && !cmd("mounted " D "/a/s") && !cmd("exists " D "/a/s/f"));
    EXPECT("private b: nothing passes", cmd("private " D "/b") && cmd("tmpfs " D "/a/s") && cmd("mkfile " D "/a/s/g") && !cmd("exists " D "/b/s/g") && cmd("umount " D "/a/s"));
    EXPECT("a slave receives", cmd("shared " D "/a") && cmd("umount " D "/b") && cmd("bind " D "/a " D "/b") && cmd("slave " D "/b") && has_field(D "/b", "master:"));
    EXPECT("a mount below the master shows below the slave", cmd("tmpfs " D "/a/s") && cmd("mkfile " D "/a/s/h") && cmd("exists " D "/b/s/h"));
    EXPECT("the copy is a slave of the mount it came from", has_field(D "/b/s", "master:"));
    EXPECT("a mount below the slave stays there", cmd("umount " D "/b/s") && cmd("exists " D "/a/s/h") && cmd("tmpfs " D "/b/s") && cmd("mkfile " D "/b/s/i") && !cmd("exists " D "/a/s/i") && cmd("umount " D "/b/s"));
    EXPECT("clean up", cmd("umount " D "/a/s") && cmd("umount " D "/b") && cmd("umount " D "/a"));

    /* recursive bind */
    EXPECT("rbind: set up a with a mount in it", cmd("bind " D "/a " D "/a") && cmd("tmpfs " D "/a/s") && cmd("mkfile " D "/a/s/r"));
    EXPECT("a plain bind does not bring the mount along", cmd("bind " D "/a " D "/c") && !cmd("exists " D "/c/s/r") && cmd("umount " D "/c"));
    EXPECT("a recursive bind does", cmd("rbind " D "/a " D "/c") && cmd("exists " D "/c/s/r") && cmd("mounted " D "/c/s"));
    EXPECT("rbind of an unbindable mount below is left out", cmd("unbindable " D "/a/s") && cmd("umount " D "/c/s") && cmd("umount " D "/c") && cmd("rbind " D "/a " D "/c") && !cmd("exists " D "/c/s/r"));
    EXPECT("clean up the binds", cmd("umount " D "/c") && cmd("umount " D "/a/s") && cmd("umount " D "/a"));

    /* another mount namespace */
    int p1[2], p2[2];
    EXPECT("start a helper process", pipe(p1) == 0 && pipe(p2) == 0);
    pid_t hp = fork();
    if (hp == 0) {
        close(p1[1]);
        close(p2[0]);
        helper(p1[0], p2[1]);
    }
    close(p1[0]);
    close(p2[1]);
    cw = p1[1];
    rr = p2[0];

    /* a shared volume dir v, as a container runtime makes for a rshared bind */
    EXPECT("v is a shared mount", cmd("bind " D "/v " D "/v") && cmd("shared " D "/v"));
    EXPECT("the helper gets a copy of the namespace", F("unshare") == 1);
    EXPECT("v is shared with the copy: the same group", F("gid " D "/v") == group_of(D "/v"));
    EXPECT("make a directory to mount on", cmd("mkdir " D "/v/m") && cmd("mkdir " D "/v/n"));
    EXPECT("the helper mounts a tmpfs below v", F("tmpfs " D "/v/m") == 1 && F("mkfile " D "/v/m/x") == 1);
    EXPECT("this namespace sees it", cmd("exists " D "/v/m/x") && cmd("mounted " D "/v/m"));
    EXPECT("this namespace mounts one below v", cmd("tmpfs " D "/v/n") && cmd("mkfile " D "/v/n/y"));
    EXPECT("the helper sees it", F("exists " D "/v/n/y") == 1);
    EXPECT("the helper unmounts its mount", F("umount " D "/v/m") == 1 && !cmd("exists " D "/v/m/x") && !cmd("mounted " D "/v/m"));
    EXPECT("this namespace unmounts its one", cmd("umount " D "/v/n") && F("exists " D "/v/n/y") == 0);

    /* a container's rshared bind of the volume: a peer of it */
    EXPECT("the helper binds v into d", F("mkdir " D "/d") == 1 && F("rbind " D "/v " D "/d") == 1 && F("gid " D "/d") == F("gid " D "/v"));
    EXPECT("a mount in the container reaches the host", F("tmpfs " D "/d/m") == 1 && F("mkfile " D "/d/m/k") == 1 && cmd("exists " D "/v/m/k") && cmd("mounted " D "/v/m"));
    EXPECT("and goes when the container unmounts it", F("umount " D "/d/m") == 1 && !cmd("exists " D "/v/m/k") && !cmd("mounted " D "/v/m"));
    EXPECT("the container lets go of d", F("umount " D "/d") == 1);

    /* the helper makes it a slave: it still gets mounts, and gives none */
    EXPECT("the helper makes v a slave", F("rslave /") == 1 && F("has " D "/v master:") == 1 && F("has " D "/v shared:") == 0);
    EXPECT("the helper's mount stays in its namespace", F("tmpfs " D "/v/m") == 1 && F("mkfile " D "/v/m/z") == 1 && !cmd("exists " D "/v/m/z") && !cmd("mounted " D "/v/m"));
    EXPECT("a mount here still shows there", cmd("tmpfs " D "/v/n") && cmd("mkfile " D "/v/n/w") && F("exists " D "/v/n/w") == 1);
    EXPECT("an unmount here takes it from the slave", cmd("umount " D "/v/n") && F("exists " D "/v/n/w") == 0);
    EXPECT("the helper unmounts its own", F("umount " D "/v/m") == 1);

    /* a container's bind of the volume, with rslave propagation */
    EXPECT("the helper binds v into c, a slave like v", F("mkdir " D "/c") == 1 && F("rbind " D "/v " D "/c") == 1 && F("has " D "/c master:") == 1);
    EXPECT("a mount on the host appears in the container", cmd("tmpfs " D "/v/n") && cmd("mkfile " D "/v/n/q") && F("exists " D "/c/n/q") == 1);
    EXPECT("and goes when the host unmounts it", cmd("umount " D "/v/n") && F("exists " D "/c/n/q") == 0);
    EXPECT("rshared on a slave keeps its master", F("rshared " D "/c") == 1 && F("has " D "/c shared:") == 1 && F("has " D "/c master:") == 1);
    EXPECT("its mounts reach its peers, not its master", F("tmpfs " D "/c/m") == 1 && F("mkfile " D "/c/m/k") == 1 && !cmd("exists " D "/v/m/k") && F("umount " D "/c/m") == 1);
    EXPECT("the container lets go of c", F("umount " D "/c") == 1);

    close(cw);
    waitpid(hp, NULL, 0);
    cmd("umount " D "/v");
    return done();
}
