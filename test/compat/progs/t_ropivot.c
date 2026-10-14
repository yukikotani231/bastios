/* How crun makes a container's root read-only: it keeps an O_PATH descriptor
 * of the rootfs, pivots into it, and remounts "/proc/self/fd/N" read-only
 * (MS_REMOUNT | MS_BIND | MS_RDONLY) once everything else is mounted. */
#include "compat.h"
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>

int main(void) {
    setup();
    char fdp[64];
    int ok = unshare(CLONE_NEWNS) == 0;
    EXPECT("unshare a mount namespace", ok);
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    mkdir("/tmp/ro", 0755);
    EXPECT("bind the root directory onto itself", mount("/tmp/ro", "/tmp/ro", NULL, MS_BIND | MS_REC, NULL) == 0);
    {
        /* crun remounts before it pivots: the descriptor's path is the bound directory */
        char pp[64];
        mkdir("/tmp/ro/pre", 0755);
        int pfd = open("/tmp/ro", O_PATH | O_CLOEXEC);
        snprintf(pp, sizeof pp, "/proc/self/fd/%d", pfd);
        EXPECT("before the pivot, remount the descriptor's path read-only", pfd >= 0 && mount(NULL, pp, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0);
        EXPECT("then creating a file below it is EROFS", open("/tmp/ro/pre/x", O_WRONLY | O_CREAT, 0600) == -1 && errno == EROFS);
        EXPECT("and writable again", mount(NULL, pp, NULL, MS_REMOUNT | MS_BIND, NULL) == 0 && open("/tmp/ro/pre/y", O_WRONLY | O_CREAT, 0600) >= 0);
        close(pfd);
    }
    mkdir("/tmp/ro/proc", 0755);
    mkdir("/tmp/ro/w", 0755);
    int rfd = open("/tmp/ro", O_PATH | O_CLOEXEC);
    EXPECT("open the root directory O_PATH", rfd >= 0);
    EXPECT("mount /proc in it", mount("proc", "/tmp/ro/proc", "proc", 0, NULL) == 0);
    EXPECT("pivot_root into it", chdir("/tmp/ro") == 0 && syscall(SYS_pivot_root, ".", ".") == 0 && umount2(".", MNT_DETACH) == 0);
    snprintf(fdp, sizeof fdp, "/proc/self/fd/%d", rfd);
    char lnk[128] = {0};
    long ln = readlink(fdp, lnk, sizeof lnk - 1);
    EXPECT("the root directory's descriptor reads as \"/\"", ln == 1 && lnk[0] == '/');
    EXPECT("stat of the descriptor's path is the root directory", ({ struct stat a, b; stat(fdp, &a) == 0 && stat("/", &b) == 0 && a.st_ino == b.st_ino; }));
    int w = open("/w/a", O_WRONLY | O_CREAT, 0600);
    EXPECT("before the remount a file can be created", w >= 0);
    if (w >= 0) close(w);
    EXPECT("remount the descriptor's path read-only", mount(NULL, fdp, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0);
    EXPECT("after it, creating a file is EROFS", open("/w/b", O_WRONLY | O_CREAT, 0600) == -1 && errno == EROFS);
    return 0;
}
