/* More system call corners LTP's tests found: O_PATH descriptors given to
 * epoll_ctl and the socket calls, readlinkat of an O_PATH handle on a
 * link, fchdir into a directory without search permission,
 * fallocate(FALLOC_FL_KEEP_SIZE), file handles (name_to_handle_at and
 * open_by_handle_at), unlinkat's flag check, and fchmod and fchown on a
 * closed descriptor, on a file of a read-only mount, and chmod of a file
 * that keeps no mode. */
#include "compat.h"
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

struct fh { unsigned int bytes; int type; unsigned char h[128]; };

/* Runs f in a child that has dropped to user nobody; its exit status. */
static int as_nobody(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) {
        if (setgid(65534) || setuid(65534)) _exit(2);
        _exit(f());
    }
    int st = 0;
    waitpid(c, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}

static int nosearch_fd = -1;
static int fchdir_nosearch(void) { return fchdir(nosearch_fd) == -1 && errno == EACCES ? 0 : 1; }
static int chmod_devnull(void) { return chmod("/dev/null", 0666) == -1 && errno == EPERM ? 0 : 1; }
static int ro_fd = -1;
static int fchmod_ro(void) { return fchmod(ro_fd, 0600) == -1 && errno == EROFS ? 0 : 1; }

int main(void) {
    setup();
    mkdir("l3_d", 0755);
    close(open("l3_d/f", O_CREAT | O_WRONLY, 0644));
    unlink("l3_link");
    if (symlink("l3_d/f", "l3_link")) perror("symlink");

    int p = open("l3_d/f", O_PATH);
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN };
    EXPECT("epoll_ctl of an O_PATH descriptor is EBADF",
           p >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, p, &ev) == -1 && errno == EBADF);
    EXPECT("accept on an O_PATH descriptor is EBADF",
           accept(p, NULL, NULL) == -1 && errno == EBADF);
    EXPECT("getsockopt on an O_PATH descriptor is EBADF",
           getsockopt(p, SOL_SOCKET, SO_TYPE, &(int){0}, &(socklen_t){sizeof(int)}) == -1 && errno == EBADF);
    close(ep);
    close(p);

    int l = open("l3_link", O_PATH | O_NOFOLLOW);
    char buf[64] = {0};
    struct stat st;
    EXPECT("O_PATH|O_NOFOLLOW opens the link itself",
           l >= 0 && fstatat(l, "", &st, AT_EMPTY_PATH) == 0 && S_ISLNK(st.st_mode));
    EXPECT("readlinkat of that descriptor and \"\" reads the link",
           readlinkat(l, "", buf, sizeof buf - 1) == 6 && strcmp(buf, "l3_d/f") == 0);
    close(l);

    mkdir("l3_nx", 0700);
    nosearch_fd = open("l3_nx", O_RDONLY | O_DIRECTORY);
    EXPECT("fchdir into a directory without search permission is EACCES",
           nosearch_fd >= 0 && as_nobody(fchdir_nosearch) == 0);
    close(nosearch_fd);

    int f = open("l3_d/f", O_RDWR);
    EXPECT("fallocate with KEEP_SIZE succeeds and keeps the size",
           f >= 0 && fallocate(f, FALLOC_FL_KEEP_SIZE, 0, 4096) == 0 && fstat(f, &st) == 0 && st.st_size == 0);
    EXPECT("fallocate without it grows the file",
           fallocate(f, 0, 0, 100) == 0 && fstat(f, &st) == 0 && st.st_size == 100);
    close(f);

    struct fh h = { .bytes = 0 };
    int mnt = -1;
    EXPECT("name_to_handle_at with no room is EOVERFLOW and says how much it needs",
           syscall(SYS_name_to_handle_at, AT_FDCWD, "l3_d/f", &h, &mnt, 0) == -1 && errno == EOVERFLOW &&
           h.bytes > 0 && h.bytes <= 128);
    unsigned int need = h.bytes;
    EXPECT("with that room it gives the handle and a mount id",
           syscall(SYS_name_to_handle_at, AT_FDCWD, "l3_d/f", &h, &mnt, 0) == 0 && h.bytes == need && mnt >= 0);
    struct fh hd = { .bytes = 128 };
    EXPECT("a directory has a handle too",
           syscall(SYS_name_to_handle_at, AT_FDCWD, "l3_d", &hd, &mnt, 0) == 0);
    EXPECT("bad flags are EINVAL",
           syscall(SYS_name_to_handle_at, AT_FDCWD, "l3_d/f", &h, &mnt, 0x8000) == -1 && errno == EINVAL);
    int mfd = open("l3_d", O_RDONLY | O_DIRECTORY);
    int g = syscall(SYS_open_by_handle_at, mfd, &h, O_RDONLY);
    EXPECT("open_by_handle_at opens the same file",
           g >= 0 && fstat(g, &st) == 0 && st.st_size == 100);
    close(g);
    g = syscall(SYS_open_by_handle_at, mfd, &hd, O_RDONLY | O_DIRECTORY);
    EXPECT("and the same directory", g >= 0 && fstatat(g, "f", &st, 0) == 0);
    close(g);
    rename("l3_d/f", "l3_d/f2");
    g = syscall(SYS_open_by_handle_at, mfd, &h, O_RDONLY);
    EXPECT("a handle still opens its file after a rename", g >= 0 && fstat(g, &st) == 0 && st.st_size == 100);
    close(g);
    struct fh bad = { .bytes = 0 };
    EXPECT("a handle of no bytes is EINVAL",
           syscall(SYS_open_by_handle_at, mfd, &bad, O_RDONLY) == -1 && errno == EINVAL);
    close(mfd);

    EXPECT("unlinkat with an unknown flag is EINVAL, before the path",
           unlinkat(AT_FDCWD, "l3_no_such", 9999) == -1 && errno == EINVAL);
    EXPECT("unlinkat with AT_REMOVEDIR of a file is ENOTDIR",
           unlinkat(AT_FDCWD, "l3_d/f2", AT_REMOVEDIR) == -1 && errno == ENOTDIR);

    int c = open("l3_d/f2", O_RDONLY);
    close(c);
    EXPECT("fchmod of a closed descriptor is EBADF", fchmod(c, 0600) == -1 && errno == EBADF);
    EXPECT("fchown of a closed descriptor is EBADF", fchown(c, 0, 0) == -1 && errno == EBADF);
    EXPECT("chmod of /dev/null by another user is EPERM", as_nobody(chmod_devnull) == 0);

    mkdir("l3_ro", 0755);
    int m = mount("none", "l3_ro", "tmpfs", 0, NULL);
    close(open("l3_ro/f", O_CREAT | O_WRONLY, 0644));
    m = m || mount("none", "l3_ro", "tmpfs", MS_REMOUNT | MS_RDONLY, NULL);
    ro_fd = open("l3_ro/f", O_RDONLY);
    EXPECT("fchmod of a file on a read-only mount is EROFS",
           m == 0 && ro_fd >= 0 && fchmod(ro_fd, 0600) == -1 && errno == EROFS);
    EXPECT("fchown of it is EROFS", fchown(ro_fd, 1, 1) == -1 && errno == EROFS);
    EXPECT("and EROFS comes before EPERM for another user", as_nobody(fchmod_ro) == 0);
    close(ro_fd);
    umount("l3_ro");
    rmdir("l3_ro");

    unlink("l3_d/f2");
    unlink("l3_link");
    rmdir("l3_d");
    rmdir("l3_nx");
    return done();
}
