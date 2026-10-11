/* openat2: the struct open_how checks and the resolve flags, as LTP's
 * openat201-203 and programs like container runtimes use them. */
#include "compat.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/syscall.h>

struct how {
    uint64_t flags, mode, resolve, pad;
};

#define NO_XDEV 1
#define NO_MAGICLINKS 2
#define NO_SYMLINKS 4
#define BENEATH 8
#define IN_ROOT 0x10
#define CACHED 0x20

static long o2(int dfd, const char *path, uint64_t flags, uint64_t mode, uint64_t resolve) {
    struct how h = {flags, mode, resolve, 0};
    return syscall(437, dfd, path, &h, 24);
}

/* Opened, and closed again. */
static int opens(long fd) {
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int fails(long r, int e) { return r == -1 && errno == e; }

int main(void) {
    setup();
    struct how h = {O_RDONLY, 0, 0, 0};
    mkdir("o2_d", 0755);
    mkdir("o2_d/sub", 0755);
    close(open("o2_d/f", O_CREAT | O_WRONLY, 0644));
    close(open("o2_top", O_CREAT | O_WRONLY, 0644));
    symlink("f", "o2_d/lf");
    symlink("../o2_top", "o2_d/lout");
    symlink("/f", "o2_d/labs");
    int d = open("o2_d", O_DIRECTORY);

    EXPECT("openat2 opens a file", opens(syscall(437, d, "f", &h, 24)));
    EXPECT("a size below 24 is EINVAL", fails(syscall(437, d, "f", &h, 23), EINVAL));
    EXPECT("a size above a page is E2BIG", fails(syscall(437, d, "f", &h, 4097), E2BIG));
    EXPECT("a larger struct with zeros past it opens", opens(syscall(437, d, "f", &h, 32)));
    h.pad = 1;
    EXPECT("a larger struct with a nonzero tail is E2BIG", fails(syscall(437, d, "f", &h, 32), E2BIG));
    EXPECT("an unmapped struct is EFAULT", fails(syscall(437, d, "f", (void *)8, 24), EFAULT));
    EXPECT("unknown flags are EINVAL", fails(o2(d, "f", 1UL << 40, 0, 0), EINVAL));
    EXPECT("a mode without O_CREAT is EINVAL", fails(o2(d, "f", O_RDONLY, 0600, 0), EINVAL));
    EXPECT("a mode above 07777 is EINVAL", fails(o2(d, "n", O_RDWR | O_CREAT, 010000, 0), EINVAL));
    EXPECT("O_PATH with O_RDWR is EINVAL", fails(o2(d, "f", O_PATH | O_RDWR, 0, 0), EINVAL));
    EXPECT("unknown resolve flags are EINVAL", fails(o2(d, "f", O_RDONLY, 0, 0x40), EINVAL));
    EXPECT("BENEATH with IN_ROOT is EINVAL", fails(o2(d, "f", O_RDONLY, 0, BENEATH | IN_ROOT), EINVAL));
    EXPECT("CACHED with O_CREAT is EAGAIN", fails(o2(d, "f", O_RDWR | O_CREAT, 0600, CACHED), EAGAIN));
    EXPECT("CACHED alone opens", opens(o2(d, "f", O_RDONLY, 0, CACHED)));
    EXPECT("a bad dirfd is EBADF", fails(o2(-1, "f", O_RDONLY, 0, 0), EBADF));
    EXPECT("a NULL path is EFAULT", fails(o2(d, NULL, O_RDONLY, 0, 0), EFAULT));
    {
        long fd = o2(d, "new", O_RDWR | O_CREAT | O_EXCL, 0640, 0);
        struct stat st;
        EXPECT("O_CREAT with a mode makes the file", fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0640);
        if (fd >= 0) close(fd);
    }

    EXPECT("NO_SYMLINKS on a plain name opens", opens(o2(d, "f", O_RDONLY, 0, NO_SYMLINKS)));
    EXPECT("NO_SYMLINKS through a link is ELOOP", fails(o2(d, "lf", O_RDONLY, 0, NO_SYMLINKS), ELOOP));
    EXPECT("NO_SYMLINKS with O_PATH|O_NOFOLLOW opens the link", opens(o2(d, "lf", O_PATH | O_NOFOLLOW, 0, NO_SYMLINKS)));
    EXPECT("a link opens without it", opens(o2(d, "lf", O_RDONLY, 0, 0)));

    {
        char b[4] = {0};
        long fd = o2(AT_FDCWD, "/proc/self/exe", O_RDONLY, 0, 0);
        EXPECT("/proc/self/exe opens the program", fd >= 0 && read(fd, b, 4) == 4 && memcmp(b, "\177ELF", 4) == 0);
        if (fd >= 0) close(fd);
    }
    EXPECT("NO_MAGICLINKS on /proc/self/exe is ELOOP",
           fails(o2(AT_FDCWD, "/proc/self/exe", O_RDONLY, 0, NO_MAGICLINKS), ELOOP));
    EXPECT("NO_SYMLINKS on /proc/self/exe is ELOOP",
           fails(o2(AT_FDCWD, "/proc/self/exe", O_RDONLY, 0, NO_SYMLINKS), ELOOP));
    EXPECT("open of /proc/self/exe with O_NOFOLLOW is ELOOP", fails(open("/proc/self/exe", O_RDONLY | O_NOFOLLOW), ELOOP));
    EXPECT("NO_MAGICLINKS on /proc/version opens", opens(o2(AT_FDCWD, "/proc/version", O_RDONLY, 0, NO_MAGICLINKS)));

    EXPECT("BENEATH inside opens", opens(o2(d, "sub/../f", O_RDONLY, 0, BENEATH)));
    EXPECT("BENEATH with an absolute path is EXDEV", fails(o2(d, "/proc/version", O_RDONLY, 0, BENEATH), EXDEV));
    EXPECT("BENEATH with .. out is EXDEV", fails(o2(d, "../o2_top", O_RDONLY, 0, BENEATH), EXDEV));
    EXPECT("BENEATH with .. out and back is EXDEV", fails(o2(d, "../o2_d/f", O_RDONLY, 0, BENEATH), EXDEV));
    EXPECT("BENEATH through a link out is EXDEV", fails(o2(d, "lout", O_RDONLY, 0, BENEATH), EXDEV));
    EXPECT("BENEATH through a link inside opens", opens(o2(d, "lf", O_RDONLY, 0, BENEATH)));

    {
        struct stat a, b;
        long fd = o2(d, "/f", O_RDONLY, 0, IN_ROOT);
        EXPECT("IN_ROOT: / is the directory", fd >= 0 && fstat(fd, &a) == 0 && stat("o2_d/f", &b) == 0 && a.st_ino == b.st_ino);
        if (fd >= 0) close(fd);
        fd = o2(d, "../../f", O_RDONLY, 0, IN_ROOT);
        EXPECT("IN_ROOT: .. stops at it", fd >= 0 && fstat(fd, &a) == 0 && a.st_ino == b.st_ino);
        if (fd >= 0) close(fd);
        fd = o2(d, "labs", O_RDONLY, 0, IN_ROOT);
        EXPECT("IN_ROOT: an absolute link starts at it", fd >= 0 && fstat(fd, &a) == 0 && a.st_ino == b.st_ino);
        if (fd >= 0) close(fd);
    }
    EXPECT("IN_ROOT: /proc/version is not there", fails(o2(d, "/proc/version", O_RDONLY | O_CREAT, 0400, IN_ROOT), ENOENT));
    EXPECT("IN_ROOT leaves the process's root alone", access("/proc/version", R_OK) == 0);

    EXPECT("NO_XDEV inside one file system opens", opens(o2(d, "f", O_RDONLY, 0, NO_XDEV)));
    EXPECT("NO_XDEV into /proc is EXDEV", fails(o2(AT_FDCWD, "/proc/version", O_RDONLY, 0, NO_XDEV), EXDEV));

    close(d);
    unlink("o2_d/new");
    unlink("o2_d/lf");
    unlink("o2_d/lout");
    unlink("o2_d/labs");
    unlink("o2_d/f");
    rmdir("o2_d/sub");
    rmdir("o2_d");
    unlink("o2_top");
    return done();
}
