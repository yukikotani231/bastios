/* Paths and the errors Linux gives for them: unreadable and empty paths,
 * paths up to PATH_MAX, loops of links through a long path, and the
 * smaller rules LTP checks around them (negative offsets, directories
 * opened for writing, O_NOFOLLOW, rmdir("."), set-group-ID at create),
 * chroot, and device nodes. */
#include "compat.h"
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>

#define NOBODY 65534
#define USERS 100

static int in_child(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) _exit(f() ? 0 : 1);
    int st; return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int fails(long r, int e) { return r == -1 && errno == e; }

static int make(const char *path) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return 0;
    int ok = write(fd, "data", 4) == 4;
    close(fd);
    return ok;
}

static int unreadable_is_efault(void) {
    char *bad = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bad == MAP_FAILED) return 0;
    struct stat st;
    int ok = fails(stat(bad, &st), EFAULT) && fails(open(bad, O_RDONLY), EFAULT)
        && fails(mkdir(bad, 0755), EFAULT) && fails(unlink(bad), EFAULT)
        && fails(chdir(bad), EFAULT) && fails(truncate(bad, 0), EFAULT);
    munmap(bad, 4096);
    return ok;
}

static int empty_is_enoent(void) {
    struct stat st;
    return fails(stat("", &st), ENOENT) && fails(unlink(""), ENOENT) && fails(mkdir("", 0755), ENOENT)
        && fails(chmod("", 0644), ENOENT) && fails(truncate("", 0), ENOENT) && fails(open("", O_RDONLY), ENOENT)
        && fstatat(AT_FDCWD, "", &st, AT_EMPTY_PATH) == 0 && S_ISDIR(st.st_mode);
}

static int long_paths(void) {
    static char p[PATH_MAX + 16];
    char here[256];
    if (!getcwd(here, sizeof here)) return 0;
    memset(p, '/', 300);
    strcpy(p + 300, here);                       /* 300 slashes and the working directory */
    int ok = chdir(p) == 0;
    memset(p, 'a', PATH_MAX);                    /* no NUL within PATH_MAX */
    p[PATH_MAX] = 0;
    ok = ok && fails(chdir(p), ENAMETOOLONG);
    return ok && chdir(here) == 0;
}

static int loop_is_eloop(void) {
    static char p[64 * 8];
    struct stat st;
    mkdir("pt_loop", 0755);
    if (symlink("../pt_loop", "pt_loop/pt_loop") != 0) return 0;
    strcpy(p, ".");
    for (int i = 0; i < 43; i++) strcat(p, "/pt_loop");   /* longer than a name BastiOS keeps */
    int ok = fails(stat(p, &st), ELOOP) && fails(lstat(p, &st), ELOOP) && fails(mkdir(p, 0755), ELOOP);
    unlink("pt_loop/pt_loop"); rmdir("pt_loop");
    return ok;
}

static int offsets(void) {
    char b[8];
    struct iovec v = { b, (size_t)-1 };
    int fd = open("pt_off", O_CREAT | O_RDWR, 0644);
    int d = open(".", O_RDONLY);
    int ok = fd >= 0 && d >= 0 && fails(pread(fd, b, 4, -1), EINVAL) && fails(pwrite(fd, b, 4, -1), EINVAL)
        && pwrite(fd, NULL, 0, 0) == 0 && fails(pread(d, b, 4, 0), EISDIR)
        && fails(preadv(fd, &v, 1, 0), EINVAL);
    close(fd); close(d); unlink("pt_off");
    return ok;
}

static int fstat_dir(void) {
    struct stat st;
    mkdir("pt_d", 0700);
    int d = open("pt_d", O_RDONLY);
    int ok = d >= 0 && fchmod(d, 01777) == 0 && fstat(d, &st) == 0 && st.st_mode == (S_IFDIR | 01777);
    close(d); rmdir("pt_d");
    return ok;
}

static int nobody_noatime(void) {
    if (setgid(USERS) != 0 || setuid(NOBODY) != 0) return 0;
    return fails(open("pt_mine", O_RDONLY | O_NOATIME), EPERM);
}

static int open_rules(void) {
    int ok = make("pt_mine") && symlink("pt_mine", "pt_ln") == 0 && mkdir("pt_d2", 0755) == 0;
    ok = ok && fails(open("pt_ln", O_RDONLY | O_NOFOLLOW), ELOOP);
    int fd = open("pt_mine", O_RDONLY | O_NOFOLLOW | O_NOATIME);             /* root may */
    ok = ok && fd >= 0 && in_child(nobody_noatime);
    close(fd);
    ok = ok && fails(open("pt_d2", O_RDWR), EISDIR) && fails(open("pt_d2", O_WRONLY | O_CREAT, 0644), EISDIR);
    ok = ok && fails(rmdir("."), EINVAL) && fails(rmdir("pt_d2/."), EINVAL) && fails(mkdir("/dev/null", 0755), EEXIST);
    unlink("pt_ln"); unlink("pt_mine"); rmdir("pt_d2");
    return ok;
}

static int nobody_sgid(void) {
    if (setgid(USERS) != 0 || setuid(NOBODY) != 0) return 0;
    struct stat st;
    int fd = open("pt_sg/mine", O_CREAT | O_WRONLY, 02755);   /* a group of its own: kept */
    int ok = fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 07777) == 02755;
    close(fd);
    fd = open("pt_sg0/other", O_CREAT | O_WRONLY, 02755);    /* group 0, not its own: dropped */
    ok = ok && fd >= 0 && fstat(fd, &st) == 0 && st.st_gid == 0 && (st.st_mode & 07777) == 0755;
    close(fd);
    return ok;
}

static int setgid_create(void) {
    umask(022);
    mkdir("pt_sg", 0777); chmod("pt_sg", 0777); chown("pt_sg", 0, USERS);
    mkdir("pt_sg0", 0777); chmod("pt_sg0", 02777);
    int ok = in_child(nobody_sgid);
    unlink("pt_sg/mine"); rmdir("pt_sg"); unlink("pt_sg0/other"); rmdir("pt_sg0");
    return ok;
}

static int sendfile_statx(void) {
    int in = open("pt_src", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    int out = open("pt_dst", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    int ro = open("pt_src", O_RDONLY);
    off_t neg = -3;
    unsigned long long sx[32];
    int ok = in >= 0 && out >= 0 && ro >= 0 && fails(sendfile(out, in, NULL, 4), EBADF)
        && fails(sendfile(ro, ro, NULL, 4), EBADF) && fails(sendfile(-1, ro, NULL, 4), EBADF)
        && fails(sendfile(out, ro, &neg, 4), EINVAL)
        && fails(syscall(SYS_statx, AT_FDCWD, "pt_src", -1, 0, sx), EINVAL)
        && fails(syscall(SYS_statx, AT_FDCWD, "pt_src", 0, 0x80000000u, sx), EINVAL);
    close(in); close(out); close(ro); unlink("pt_src"); unlink("pt_dst");
    return ok;
}

/* A socket bound by a name in a chroot reads back by that name, not by the
 * path from the top (multiprocessing's managers connect to what they read). */
static int jail_socket_name(void) {
    struct sockaddr_un a = { .sun_family = AF_UNIX }, got;
    socklen_t len = sizeof got;
    int s = socket(AF_UNIX, SOCK_STREAM, 0), c = socket(AF_UNIX, SOCK_STREAM, 0);
    strcpy(a.sun_path, "/sub/js");
    int ok = s >= 0 && c >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 1) == 0
        && getsockname(s, (struct sockaddr *)&got, &len) == 0 && strcmp(got.sun_path, "/sub/js") == 0
        && connect(c, (struct sockaddr *)&a, sizeof a) == 0;
    close(s); close(c);
    return ok;
}

static int in_jail(void) {
    struct stat st;
    char here[64];
    if (chroot("pt_jail") != 0) return 0;
    return stat("/f", &st) == 0 && stat("/abs", &st) == 0 && stat("/../../f", &st) == 0
        && fails(stat("/pt_jail", &st), ENOENT)
        && chdir("/sub") == 0 && getcwd(here, sizeof here) && strcmp(here, "/sub") == 0
        && stat("../f", &st) == 0 && stat("../../../f", &st) == 0 && stat("up", &st) == 0
        && chdir("/") == 0 && getcwd(here, sizeof here) && strcmp(here, "/") == 0
        && jail_socket_name();
}

static int nobody_chroot(void) {
    if (setgid(USERS) != 0 || setuid(NOBODY) != 0) return 0;
    return fails(chroot("pt_jail"), EPERM);
}

static int chroot_jail(void) {
    mkdir("pt_jail", 0755); mkdir("pt_jail/sub", 0755);
    int ok = make("pt_jail/f") && symlink("/f", "pt_jail/abs") == 0 && symlink("../../../f", "pt_jail/sub/up") == 0
        && fails(chroot("pt_jail/f"), ENOTDIR) && fails(chroot("pt_none"), ENOENT)
        && in_child(nobody_chroot) && in_child(in_jail);
    unlink("pt_jail/sub/js"); unlink("pt_jail/sub/up"); unlink("pt_jail/abs"); unlink("pt_jail/f"); rmdir("pt_jail/sub"); rmdir("pt_jail");
    return ok;
}

static int nobody_mknod(void) {
    if (setgid(USERS) != 0 || setuid(NOBODY) != 0) return 0;
    return fails(mknod("pt_nd/x", S_IFCHR | 0666, makedev(1, 3)), EPERM);   /* the directory allows it */
}

static int device_nodes(void) {
    struct stat st;
    unsigned int sx[64];                         /* struct statx: mode at 28, rdev at 128 */
    char b[4] = "xxx";
    int ok = mknod("pt_null", S_IFCHR | 0666, makedev(1, 3)) == 0 && stat("pt_null", &st) == 0
        && S_ISCHR(st.st_mode) && st.st_rdev == makedev(1, 3) && st.st_size == 0;
    int fd = open("pt_null", O_RDWR | O_TRUNC);
    ok = ok && fd >= 0 && write(fd, "data", 4) == 4 && read(fd, b, 4) == 0;
    close(fd);
    int one = 0;
    fd = mknod("pt_ptmx", S_IFCHR | 0666, makedev(5, 2)) == 0 ? open("pt_ptmx", O_RDWR | O_NOCTTY) : -1;   /* a /dev/ptmx made in a chroot */
    ok = ok && fd >= 0 && ioctl(fd, 0x80045430, &one) == 0 && one == 0;                                  /* TIOCGPTN */
    if (fd >= 0) close(fd);
    unlink("pt_ptmx");
    ok = ok && mknod("pt_zero", S_IFCHR | 0444, makedev(1, 5)) == 0;
    fd = open("pt_zero", O_RDONLY);
    ok = ok && fd >= 0 && read(fd, b, 4) == 4 && b[0] == 0 && b[3] == 0;
    close(fd);
    ok = ok && mknod("pt_blk", S_IFBLK | 0600, makedev(8, 1)) == 0
        && syscall(SYS_statx, AT_FDCWD, "pt_blk", 0, 0x7ff, sx) == 0 && S_ISBLK(sx[7] & 0xffff)
        && sx[32] == 8 && sx[33] == 1
        && mknod("pt_sock", S_IFSOCK | 0600, 0) == 0 && stat("pt_sock", &st) == 0 && S_ISSOCK(st.st_mode)
        && fails(open("pt_sock", O_RDONLY), ENXIO)
        && fails(mknod("pt_bad", S_IFMT | 0600, 0), EINVAL) && fails(mknod("pt_null", S_IFCHR, 0), EEXIST)
        && mkdir("pt_nd", 0777) == 0 && chmod("pt_nd", 0777) == 0 && in_child(nobody_mknod);
    rmdir("pt_nd"); unlink("pt_null"); unlink("pt_zero"); unlink("pt_blk"); unlink("pt_sock");
    return ok;
}

/* A name ending in "/" or "/." must be a directory: a file there is
 * ENOTDIR, and open(O_CREAT) does not make a file under such a name
 * (shutil.copyfile to "missing_dir/" relies on the EISDIR). */
static int trailing_slash(void) {
    struct stat st;
    int ok = make("pt_f") && mkdir("pt_d", 0755) == 0
        && fails(open("pt_new/", O_WRONLY | O_CREAT, 0644), EISDIR) && fails(access("pt_new", F_OK), ENOENT)
        && fails(open("pt_new/", O_RDONLY), ENOENT)
        && fails(open("pt_f/", O_RDONLY), ENOTDIR) && fails(open("pt_f/.", O_RDONLY), ENOTDIR)
        && fails(stat("pt_f/", &st), ENOTDIR) && fails(lstat("pt_f/", &st), ENOTDIR)
        && fails(unlink("pt_f/"), ENOTDIR) && fails(rename("pt_f", "pt_g/"), ENOTDIR) && access("pt_f", F_OK) == 0
        && stat("pt_d/", &st) == 0 && S_ISDIR(st.st_mode) && stat("pt_d/.", &st) == 0
        && mkdir("pt_e/", 0755) == 0 && rmdir("pt_e/") == 0;
    unlink("pt_f"); rmdir("pt_d"); rmdir("pt_e"); unlink("pt_new");
    return ok;
}

static int deep_names(void) {
    struct stat st;
    int ok = mkdir("pd_a", 0755) == 0 && mkdir("pd_a/b", 0755) == 0 && make("pd_a/b/f");
    ok = ok && symlink("pd_a/b", "pd_ln") == 0;
    ok = ok && stat("pd_a/b/f", &st) == 0 && stat("pd_ln/f", &st) == 0 && stat("pd_a/b/../b/./f", &st) == 0;
    ok = ok && fails(stat("pd_a/b/f/x", &st), ENOTDIR) && fails(stat("pd_a/nob/f", &st), ENOENT);
    ok = ok && fails(open("pd_a/b/f/x", O_RDONLY), ENOTDIR) && fails(open("pd_ln/nof", O_RDONLY), ENOENT);
    unlink("pd_a/b/f"); rmdir("pd_a/b");
    ok = ok && symlink("../pd_a", "pd_a/b") == 0 && make("pd_a/f");   /* a link where a directory was */
    ok = ok && stat("pd_a/b/f", &st) == 0 && stat("pd_a/b/b/b/f", &st) == 0 && fails(stat("pd_a/b/nof", &st), ENOENT);
    unlink("pd_a/f"); unlink("pd_a/b"); unlink("pd_ln"); rmdir("pd_a");
    return ok;
}

int main(void) {
    setup();
    EXPECT("a path in unmapped memory is EFAULT", unreadable_is_efault());
    EXPECT("an empty path is ENOENT, except with AT_EMPTY_PATH", empty_is_enoent());
    EXPECT("paths are read up to PATH_MAX", long_paths());
    EXPECT("a loop of links through a long path is ELOOP", loop_is_eloop());
    EXPECT("pread and pwrite take no negative offset", offsets());
    EXPECT("fstat of a directory gives its mode", fstat_dir());
    EXPECT("O_NOFOLLOW, O_NOATIME, EISDIR, rmdir of . and mkdir over a name", open_rules());
    EXPECT("a set-group-ID bit at create is kept only for a member of the group", setgid_create());
    EXPECT("sendfile checks its descriptors and offset; statx its flags", sendfile_statx());
    EXPECT("chroot keeps absolute paths, links and .. inside the new root", chroot_jail());
    EXPECT("mknod makes device and socket nodes; a device node opens its device", device_nodes());
    EXPECT("a name ending in / or /. must be a directory", trailing_slash());
    EXPECT("a name below a directory, a file or a link that replaced a directory", deep_names());
    EXPECT("execve of a missing /bin program is ENOENT", ({ char *av[] = {"nope", 0};
           fails(execve("/bin/no_such_program", av, 0), ENOENT); }));
    return done();
}
