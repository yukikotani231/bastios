/* What crun does to build a container's /dev: a fresh tmpfs, a device node
 * made in it with mknodat, then chmod and chown of that node by name and
 * through the directory's descriptor. */
#include "compat.h"
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

int main(void) {
    setup();
    struct stat st;
    int ok = unshare(CLONE_NEWNS) == 0;
    EXPECT("unshare a mount namespace", ok);
    mkdir("/tmp/dn", 0755);
    EXPECT("mount a tmpfs", mount("tmpfs", "/tmp/dn", "tmpfs", 0, "mode=755") == 0);
    int dfd = open("/tmp/dn", O_RDONLY | O_DIRECTORY);
    EXPECT("open the directory", dfd >= 0);
    EXPECT("mknodat makes a character device in it", mknodat(dfd, "null", S_IFCHR | 0666, makedev(1, 3)) == 0);
    EXPECT("the node stats as a character device", fstatat(dfd, "null", &st, 0) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(1, 3));
    EXPECT("fchmodat by the directory's descriptor works", fchmodat(dfd, "null", 0666, 0) == 0);
    EXPECT("fchownat by the directory's descriptor works", fchownat(dfd, "null", 0, 0, 0) == 0);
    EXPECT("chmod by path works", chmod("/tmp/dn/null", 0660) == 0 && stat("/tmp/dn/null", &st) == 0 && (st.st_mode & 0777) == 0660);
    int f = open("/tmp/dn/null", O_WRONLY);
    EXPECT("the node opens and writes like /dev/null", f >= 0 && write(f, "x", 1) == 1);
    /* crun's way: an O_PATH descriptor of the node, changed through /proc/self/fd */
    {
        char fdp[64];
        int pf = openat(dfd, "null", O_PATH | O_CLOEXEC);
        snprintf(fdp, sizeof fdp, "/proc/self/fd/%d", pf);
        EXPECT("chmod through /proc/self/fd of an O_PATH device node", pf >= 0 && chmod(fdp, 0666) == 0);
        EXPECT("chown through /proc/self/fd of an O_PATH device node", pf >= 0 && chown(fdp, 0, 0) == 0);
        int pr = openat(dfd, "regular", O_RDWR | O_CREAT, 0600);
        close(pr);
        pr = openat(dfd, "regular", O_PATH | O_CLOEXEC);
        snprintf(fdp, sizeof fdp, "/proc/self/fd/%d", pr);
        EXPECT("chmod through /proc/self/fd of an O_PATH regular file", pr >= 0 && chmod(fdp, 0640) == 0);
    }
    /* the fallback for a runtime that cannot mknod: an empty file with the
     * host's device bound over it */
    int bf = open("/tmp/dn/zero", O_WRONLY | O_CREAT, 0666);
    if (bf >= 0) close(bf);
    EXPECT("a host device bound over a tmpfs file", mount("/dev/zero", "/tmp/dn/zero", NULL, MS_BIND, NULL) == 0);
    EXPECT("the bound node stats as the device", stat("/tmp/dn/zero", &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(1, 5));
    EXPECT("chmod of the bound node by the directory's descriptor works", fchmodat(dfd, "zero", 0666, 0) == 0);
    EXPECT("chmod of the bound node by path works", chmod("/tmp/dn/zero", 0666) == 0);
    return 0;
}
