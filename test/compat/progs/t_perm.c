/* File ownership and permissions: chown keeps owners, new files belong to
 * their maker and take the umask, and an unprivileged process is held to
 * the mode bits: open, creating and removing names, search through
 * directories, sticky directories, chmod and utime for the owner, access,
 * truncate and exec; supplementary groups, and set-user-ID programs. */
#include "compat.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <grp.h>
#include <utime.h>

#define NOBODY 65534
#define USERS 100

/* Runs f in a child (as root, unless it drops) and reports whether it
 * returned nonzero. */
static int in_child(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) _exit(f() ? 0 : 1);
    int st; return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int become_nobody(void) { return setgid(USERS) == 0 && setuid(NOBODY) == 0; }

static int make(const char *path, int mode) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) return 0;
    int ok = write(fd, "data", 4) == 4;
    close(fd);
    return ok && chmod(path, mode) == 0;
}

static int owned(const char *path, uid_t u, gid_t g) {
    struct stat st;
    return lstat(path, &st) == 0 && st.st_uid == u && st.st_gid == g;
}

static int mode_is(const char *path, int mode) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & 07777) == mode;
}

static int umask_applies(void) {
    if (umask(077) != 022) return 0;                                 /* 022 to start with */
    int ok = make("p_um", 0644) && chmod("p_um", 0644) == 0;
    int fd = open("p_um2", O_CREAT | O_WRONLY, 0666);
    ok = ok && fd >= 0 && mode_is("p_um2", 0600) && mkdir("p_umd", 0777) == 0 && mode_is("p_umd", 0700);
    close(fd);
    ok = ok && umask(022) == 077;
    unlink("p_um"); unlink("p_um2"); rmdir("p_umd");
    return ok;
}

static int chown_keeps(void) {
    int ok = make("p_own", 04755) && chown("p_own", NOBODY, USERS) == 0 && owned("p_own", NOBODY, USERS);
    ok = ok && mode_is("p_own", 0755);                               /* the set-user-ID bit goes */
    int fd = open("p_own", O_RDONLY);
    ok = ok && fchown(fd, 3, -1) == 0 && owned("p_own", 3, USERS);
    close(fd);
    ok = ok && symlink("p_own", "p_ownl") == 0 && lchown("p_ownl", 5, 6) == 0 && owned("p_ownl", 5, 6)
         && owned("p_own", 3, USERS);                                /* lchown: the link only */
    ok = ok && fchownat(AT_FDCWD, "p_ownl", 7, 8, 0) == 0 && owned("p_own", 7, 8);
    unlink("p_ownl"); unlink("p_own");
    return ok;
}

static int nobody_makes(void) {
    if (!become_nobody()) return 0;
    int fd = open("pd/mine", O_CREAT | O_WRONLY, 0640);
    int ok = fd >= 0 && owned("pd/mine", NOBODY, USERS) && mkdir("pd/mydir", 0755) == 0 && owned("pd/mydir", NOBODY, USERS);
    close(fd);
    ok = ok && unlink("pd/mine") == 0 && rmdir("pd/mydir") == 0;
    return ok;
}

static int nobody_opens(void) {
    if (!become_nobody()) return 0;
    int ok = open("p_secret", O_RDONLY) == -1 && errno == EACCES;
    int fd = open("p_public", O_RDONLY);
    ok = ok && fd >= 0;
    close(fd);
    ok = ok && open("p_public", O_WRONLY) == -1 && errno == EACCES;
    ok = ok && open("p_public", O_RDONLY | O_TRUNC) == -1 && errno == EACCES;
    ok = ok && truncate("p_public", 0) == -1 && errno == EACCES;
    return ok;
}

static int nobody_dirs(void) {
    if (!become_nobody()) return 0;
    int ok = open("p_rootdir/new", O_CREAT | O_WRONLY, 0644) == -1 && errno == EACCES;
    ok = ok && mkdir("p_rootdir/new", 0755) == -1 && errno == EACCES;
    ok = ok && unlink("p_rootdir/f") == -1 && errno == EACCES;
    ok = ok && rename("p_rootdir/f", "pd/f") == -1 && errno == EACCES;
    ok = ok && symlink("x", "p_rootdir/l") == -1 && errno == EACCES;
    return ok;
}

static int nobody_searches(void) {
    if (!become_nobody()) return 0;
    struct stat st;
    int ok = stat("p_closed/f", &st) == -1 && errno == EACCES;      /* p_closed is 0700 */
    ok = ok && chdir("p_closed") == -1 && errno == EACCES;
    ok = ok && stat("p_closed", &st) == 0;                           /* the directory itself shows */
    return ok;
}

static int sticky_rules(void) {
    if (!become_nobody()) return 0;
    int ok = unlink("p_sticky/roots") == -1 && errno == EPERM;
    int fd = open("p_sticky/mine", O_CREAT | O_WRONLY, 0644);
    close(fd);
    ok = ok && fd >= 0 && unlink("p_sticky/mine") == 0;
    return ok;
}

static int owner_only(void) {
    if (!become_nobody()) return 0;
    struct utimbuf t = {1000, 1000};
    int ok = chmod("p_public", 0777) == -1 && errno == EPERM;
    ok = ok && chown("p_public", NOBODY, -1) == -1 && errno == EPERM;
    ok = ok && utime("p_public", &t) == -1 && errno == EPERM;
    ok = ok && utime("p_public", NULL) == -1 && errno == EACCES;     /* now: needs write permission */
    ok = ok && utime("p_shared", NULL) == 0;
    int fd = open("pd/m", O_CREAT | O_WRONLY, 0644);
    close(fd);
    ok = ok && chmod("pd/m", 0600) == 0 && mode_is("pd/m", 0600);
    ok = ok && chown("pd/m", -1, USERS) == 0 && chown("pd/m", -1, 0) == -1 && errno == EPERM;
    ok = ok && chmod("p_sgd", 03777) == 0 && mode_is("p_sgd", 01777);    /* not in its group: no S_ISGID */
    unlink("pd/m");
    return ok;
}

static int nobody_access(void) {
    if (setresgid(USERS, USERS, USERS) || setresuid(NOBODY, NOBODY, NOBODY)) return 0;
    return access("p_secret", F_OK) == 0 && access("p_secret", R_OK) == -1 && errno == EACCES
        && access("p_public", R_OK) == 0 && access("p_public", W_OK) == -1 && errno == EACCES
        && access("p_shared", R_OK | W_OK) == 0;
}

static int exec_needs_x(void) {
    /* Root too may run only a file with an execute bit. */
    int ok = make("p_prog", 0644);
    char *argv[] = {"p_prog", 0};
    ok = ok && access("p_prog", X_OK) == -1 && errno == EACCES;
    ok = ok && execv("p_prog", argv) == -1 && errno == EACCES;
    unlink("p_prog");
    return ok;
}

static int setgid_dir(void) {
    int ok = mkdir("p_sg", 02775) == 0 && chown("p_sg", 0, USERS) == 0 && chmod("p_sg", 02775) == 0;
    int fd = open("p_sg/f", O_CREAT | O_WRONLY, 0644);
    close(fd);
    ok = ok && fd >= 0 && owned("p_sg/f", 0, USERS) && mkdir("p_sg/d", 0755) == 0 && owned("p_sg/d", 0, USERS)
         && mode_is("p_sg/d", 02755);
    unlink("p_sg/f"); rmdir("p_sg/d"); rmdir("p_sg");
    return ok;
}

static int disk_keeps_owners(void) {
    struct stat st;
    if (stat("/disk", &st) != 0) return 1;                           /* no disk: nothing to check */
    int ok = make("/disk/p_owner", 0640) && chown("/disk/p_owner", NOBODY, USERS) == 0 && owned("/disk/p_owner", NOBODY, USERS)
             && chmod("/disk/p_owner", 0600) == 0 && owned("/disk/p_owner", NOBODY, USERS) && mode_is("/disk/p_owner", 0600);
    unlink("/disk/p_owner");
    return ok;
}

static int groups_kept(void) {
    gid_t set[3] = {200, 300, 400}, got[3];
    if (setgroups(3, set) || getgroups(0, 0) != 3 || getgroups(3, got) != 3) return 0;
    if (got[0] != 200 || got[1] != 300 || got[2] != 400) return 0;
    if (getgroups(2, got) != -1 || errno != EINVAL) return 0;
    char buf[1024];
    int fd = open("/proc/self/status", O_RDONLY);
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    if (!strstr(buf, "Groups:\t200 300 400")) return 0;
    if (setuid(NOBODY)) return 0;
    return setgroups(0, 0) == -1 && errno == EPERM;
}

static int group_reads(void) {
    gid_t g = 200;
    if (setgroups(1, &g) || !become_nobody()) return 0;
    int fd = open("p_group", O_RDONLY);                              /* 0640, group 200 */
    close(fd);
    int ok = fd >= 0 && access("p_group", R_OK) == 0;
    ok = ok && make("pd/g", 0644) && chown("pd/g", -1, 200) == 0 && owned("pd/g", NOBODY, 200);
    unlink("pd/g");
    return ok;
}

static int no_group(void) {
    if (setgroups(0, 0) || !become_nobody()) return 0;
    return open("p_group", O_RDONLY) == -1 && errno == EACCES;
}

static const char *self;

/* Copies this program to p_suid, owned by 7:300, with `mode`. */
static int copy_self(int mode) {
    int in = open(self, O_RDONLY), out = open("p_suid", O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (in < 0 || out < 0) return 0;
    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) return 0;
    close(in); close(out);
    return chown("p_suid", 7, 300) == 0 && chmod("p_suid", mode) == 0;
}

/* Runs p_suid as nobody; it checks that it runs with effective user id
 * euid and effective group id egid. */
static int run_as_nobody(const char *euid, const char *egid) {
    pid_t c = fork();
    if (c == 0) {
        if (!become_nobody()) _exit(2);
        execl("p_suid", "p_suid", "ids", euid, egid, (char *)0);
        _exit(3);
    }
    int st; return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int setuid_exec(void) {
    int ok = copy_self(04755) && run_as_nobody("7", "100");
    ok = ok && chmod("p_suid", 02755) == 0 && run_as_nobody("65534", "300");
    ok = ok && chmod("p_suid", 0755) == 0 && run_as_nobody("65534", "100");
    unlink("p_suid");
    return ok;
}

int main(int argc, char **argv) {
    self = argv[0];
    if (argc == 4 && strcmp(argv[1], "ids") == 0) {
        /* Started by setuid_exec: the real ids stay nobody's, the
         * effective and saved ones are the file's where its bits say. */
        uid_t r, e, s;
        gid_t gr, ge, gs;
        getresuid(&r, &e, &s);
        getresgid(&gr, &ge, &gs);
        return r == NOBODY && e == (uid_t)atoi(argv[2]) && s == e && gr == USERS && ge == (gid_t)atoi(argv[3]) && gs == ge ? 0 : 1;
    }
    setup();
    umask(022);
    /* The fixtures, made by root: pd (0777) for others to use; p_rootdir
     * (0755) with f; p_closed (0700) with f; p_sticky (01777) with
     * roots; p_secret (0600), p_public (0644) and p_shared (0666). */
    mkdir("pd", 0777); chmod("pd", 0777);
    mkdir("p_rootdir", 0755); make("p_rootdir/f", 0644);
    mkdir("p_closed", 0700); make("p_closed/f", 0644);
    mkdir("p_sticky", 0777); chmod("p_sticky", 01777); make("p_sticky/roots", 0666);
    make("p_secret", 0600); make("p_public", 0644); make("p_shared", 0666);
    make("p_group", 0640); chown("p_group", 0, 200);
    mkdir("p_sgd", 0755); chown("p_sgd", NOBODY, 0);

    EXPECT("umask applies to new files and directories", umask_applies());
    EXPECT("chown, fchown, lchown and fchownat keep the owner", chown_keeps());
    EXPECT("a new file belongs to its maker", in_child(nobody_makes));
    EXPECT("open checks the mode bits", in_child(nobody_opens));
    EXPECT("names in a directory need write permission on it", in_child(nobody_dirs));
    EXPECT("a path needs search permission on its directories", in_child(nobody_searches));
    EXPECT("a sticky directory keeps others' names", in_child(sticky_rules));
    EXPECT("chmod, chown and utime are for the owner", in_child(owner_only));
    EXPECT("access checks the real ids", in_child(nobody_access));
    EXPECT("exec needs an execute bit", exec_needs_x());
    EXPECT("a set-group-ID directory passes on its group", setgid_dir());
    EXPECT("the disk keeps owners", disk_keeps_owners());
    EXPECT("setgroups and getgroups keep supplementary groups", in_child(groups_kept));
    EXPECT("a supplementary group gives the group's bits", in_child(group_reads));
    EXPECT("without the group, the group's bits do not apply", in_child(no_group));
    EXPECT("exec honours set-user-ID and set-group-ID bits", setuid_exec());

    unlink("p_rootdir/f"); rmdir("p_rootdir");
    unlink("p_closed/f"); rmdir("p_closed");
    unlink("p_sticky/roots"); rmdir("p_sticky");
    unlink("p_secret"); unlink("p_public"); unlink("p_shared"); unlink("p_group"); rmdir("p_sgd"); rmdir("pd");
    return done();
}
