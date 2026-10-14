/* crun's read-only root, in its own order: bind the rootfs onto itself, mount
 * what the container needs below it, remount the rootfs read-only through a
 * descriptor, and only then open the rootfs again and pivot_root(".", ".")
 * into it. The root must still be read-only afterwards. */
#include "compat.h"
#ifndef ROOT
#define ROOT "/tmp/rr"
#endif
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* openat2(dirfd, name, O_PATH | O_CLOEXEC, RESOLVE_IN_ROOT): how crun finds a mount point. */
static int sub(int dirfd, const char *name) {
    struct { unsigned long long flags, mode, resolve; } how = { O_RDONLY | O_CLOEXEC | O_PATH, 0, 8 };
    return syscall(SYS_openat2, dirfd, name, &how, sizeof how);
}

/* mount(src, "/proc/self/fd/N" of the named subdirectory of dirfd, ...) */
static int mount_in(int dirfd, const char *name, const char *src, const char *type, unsigned long fl, const char *data) {
    int f = sub(dirfd, name);
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", f);
    int r = f >= 0 ? mount(src, p, type, fl, data) : -1;
    if (f >= 0) close(f);
    return r;
}

int main(void) {
    setup();
    EXPECT("unshare a mount namespace", unshare(CLONE_NEWNS) == 0);
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    mkdir(ROOT "", 0755);
    mkdir(ROOT "/proc", 0755);
    mkdir(ROOT "/w", 0755);
    mkdir(ROOT "/dev", 0755);
    mkdir(ROOT "/sys", 0755);
    EXPECT("bind the rootfs onto itself", mount(ROOT "", ROOT "", NULL, MS_BIND | MS_REC, NULL) == 0);
    EXPECT("make the bind private", mount(NULL, ROOT "", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
    EXPECT("remount it with no change", mount(NULL, ROOT "", NULL, MS_REMOUNT | MS_BIND, NULL) == 0);
    int rfd = open(ROOT "", O_PATH | O_CLOEXEC);
    int dfd = rfd >= 0 ? dup(rfd) : -1;
    EXPECT("mount /proc below it", mount_in(rfd, "proc", "proc", "proc", 0, NULL) == 0);
    EXPECT("mount a tmpfs on /dev below it", mount_in(rfd, "dev", "tmpfs", "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=65536k") == 0);
    EXPECT("mount sysfs below it", mount_in(rfd, "sys", "sysfs", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0);
    {
        int f = sub(rfd, "sys");
        char p[64];
        snprintf(p, sizeof p, "/proc/self/fd/%d", f);
        EXPECT("remount /sys read-only", mount(NULL, p, NULL, MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_REMOUNT | MS_BIND, NULL) == 0);
        close(f);
    }
    char fdp[64];
    snprintf(fdp, sizeof fdp, "/proc/self/fd/%d", dfd);
    EXPECT("remount the rootfs read-only through its descriptor", mount(NULL, fdp, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0);
    EXPECT("a file cannot be created in it before the pivot", open(ROOT "/w/a", O_WRONLY | O_CREAT, 0600) == -1 && errno == EROFS);
    int old = open("/", O_PATH | O_CLOEXEC | O_DIRECTORY);
    int nfd = open(ROOT "", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    EXPECT("pivot_root(\".\", \".\") into it", fchdir(nfd) == 0 && syscall(SYS_pivot_root, ".", ".") == 0 && fchdir(old) == 0
           && mount(NULL, ".", NULL, MS_REC | MS_PRIVATE, NULL) == 0 && umount2(".", MNT_DETACH) == 0);
    /* crun runs umount2 again until it fails: the second one finds the old
     * root already gone, and must not take the new root's mount (and its
     * read-only flag) with it. */
    EXPECT("umount2 of \".\" again fails with EINVAL", umount2(".", MNT_DETACH) == -1 && errno == EINVAL);
    EXPECT("chdir to the new root", chdir("/") == 0);
    EXPECT("make the new root private", mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
    EXPECT("a file cannot be created in the new root", open("/w/b", O_WRONLY | O_CREAT, 0600) == -1 && errno == EROFS);
    EXPECT("nor a file in the root directory itself", open("/x", O_WRONLY | O_CREAT, 0600) == -1 && errno == EROFS);
    EXPECT("nor a directory", mkdir("/w/d", 0755) == -1 && errno == EROFS);
    return 0;
}
