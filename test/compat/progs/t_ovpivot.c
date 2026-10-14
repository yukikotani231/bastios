/* An overlay mount as a container root, the way crun enters it: bind the
 * mount onto itself, pivot_root(".", ".") into it and look for a program
 * of the lower layer. */
#include "compat.h"
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>

static int put(const char *path) {
    int f = open(path, O_WRONLY | O_CREAT, 0755);
    if (f < 0) return -1;
    return close(f);
}

int main(void) {
    setup();
    mkdir("/tmp/ov", 0755);
    mkdir("/tmp/ov/lo", 0755);
    mkdir("/tmp/ov/lo/bin", 0755);
    mkdir("/tmp/ov/up", 0755);
    mkdir("/tmp/ov/wk", 0755);
    mkdir("/tmp/ov/m", 0755);
    EXPECT("a program and a link to it in the lower layer", put("/tmp/ov/lo/bin/busybox") == 0 && symlink("busybox", "/tmp/ov/lo/bin/sh") == 0);
    EXPECT("unshare a mount namespace", unshare(CLONE_NEWNS) == 0);
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    EXPECT("mount the overlay", mount("overlay", "/tmp/ov/m", "overlay", 0, "lowerdir=/tmp/ov/lo,upperdir=/tmp/ov/up,workdir=/tmp/ov/wk") == 0);
    EXPECT("access finds the program before the pivot", access("/tmp/ov/m/bin/sh", X_OK) == 0);
    EXPECT("bind the overlay onto itself", mount("/tmp/ov/m", "/tmp/ov/m", NULL, MS_BIND | MS_REC, NULL) == 0);
    EXPECT("access still finds it", access("/tmp/ov/m/bin/sh", X_OK) == 0);
    EXPECT("pivot_root into it", chdir("/tmp/ov/m") == 0 && syscall(SYS_pivot_root, ".", ".") == 0 && umount2(".", MNT_DETACH) == 0);
    EXPECT("eaccess finds /bin/sh in the new root", eaccess("/bin/sh", X_OK) == 0);
    struct stat st;
    EXPECT("stat finds /bin/sh as a regular file", stat("/bin/sh", &st) == 0 && S_ISREG(st.st_mode));
    EXPECT("lstat sees the link", lstat("/bin/sh", &st) == 0 && S_ISLNK(st.st_mode));
    return 0;
}
