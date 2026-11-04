/* The new mount API: fsopen, fsconfig, fsmount and move_mount make a tmpfs
 * on a directory with the options asked for, and refuse what Linux refuses. */
#include "compat.h"
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define FSOPEN 430
#define FSCONFIG 431
#define FSMOUNT 432
#define MOVE_MOUNT 429
#define OPEN_TREE 428
#define EMPTY 4

static int has(const char *dir, const char *opt) {
    char line[512], mp[256], opts[256];
    FILE *f = fopen("/proc/mounts", "r");
    int r = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%*s %255s %*s %255s", mp, opts) < 2 || strcmp(mp, dir) != 0)
            continue;
        for (char *t = strtok(opts, ","); t; t = strtok(NULL, ","))
            if (!strcmp(t, opt))
                r = 1;
    }
    if (f)
        fclose(f);
    return r;
}

int main(void) {
    setup();
    mkdir("/tmp/ma", 0755);
    int fs = syscall(FSOPEN, "tmpfs", 1);
    EXPECT("fsopen of tmpfs", fs >= 0);
    EXPECT("fsopen of an unknown type is ENODEV", syscall(FSOPEN, "nosuchfs", 0) == -1 && errno == ENODEV);
    EXPECT("fsopen with a bad flag is EINVAL", syscall(FSOPEN, "tmpfs", 0x80) == -1 && errno == EINVAL);
    EXPECT("fsmount before the context is created is EINVAL", syscall(FSMOUNT, fs, 0, 0) == -1 && errno == EINVAL);
    EXPECT("a flag parameter", syscall(FSCONFIG, fs, 0, "nosuid", NULL, 0) == 0);
    EXPECT("an unknown flag parameter is EINVAL", syscall(FSCONFIG, fs, 0, "nosuchflag", NULL, 0) == -1 && errno == EINVAL);
    EXPECT("a string parameter", syscall(FSCONFIG, fs, 1, "size", "1m", 0) == 0);
    EXPECT("a flag with a value is EINVAL", syscall(FSCONFIG, fs, 0, "ro", "x", 0) == -1 && errno == EINVAL);
    EXPECT("an unknown command is EOPNOTSUPP", syscall(FSCONFIG, fs, 100, NULL, NULL, 0) == -1 && errno == EOPNOTSUPP);
    EXPECT("a context is created", syscall(FSCONFIG, fs, 6, NULL, NULL, 0) == 0);
    EXPECT("a context is created once", syscall(FSCONFIG, fs, 6, NULL, NULL, 0) == -1 && errno == EBUSY);
    EXPECT("fsmount with a bad attribute is EINVAL", syscall(FSMOUNT, fs, 0, 0x100) == -1 && errno == EINVAL);
    EXPECT("fsmount of a closed descriptor is EBADF", syscall(FSMOUNT, 999, 0, 0) == -1 && errno == EBADF);
    int mfd = syscall(FSMOUNT, fs, 1, 0x80 | 0x10);   /* MOUNT_ATTR_NODIRATIME | MOUNT_ATTR_NOATIME */
    EXPECT("fsmount", mfd >= 0);
    EXPECT("a mount object is not a context", syscall(FSCONFIG, mfd, 6, NULL, NULL, 0) == -1 && errno == EINVAL);
    EXPECT("move_mount with a name that is not there is ENOENT", syscall(MOVE_MOUNT, mfd, "nope", AT_FDCWD, "/tmp/ma", EMPTY) == -1 && errno == ENOENT);
    EXPECT("move_mount to a name that is not there is ENOENT", syscall(MOVE_MOUNT, mfd, "", AT_FDCWD, "/tmp/nowhere", EMPTY) == -1 && errno == ENOENT);
    EXPECT("move_mount with a bad flag is EINVAL", syscall(MOVE_MOUNT, mfd, "", AT_FDCWD, "/tmp/ma", EMPTY | 8) == -1 && errno == EINVAL);
    EXPECT("move_mount attaches it", syscall(MOVE_MOUNT, mfd, "", AT_FDCWD, "/tmp/ma", EMPTY) == 0);
    EXPECT("it can be attached once", syscall(MOVE_MOUNT, mfd, "", AT_FDCWD, "/tmp/ma", EMPTY) == -1 && errno == EINVAL);
    close(mfd);
    close(fs);
    EXPECT("the directory is a mount point", has("/tmp/ma", "rw"));
    EXPECT("with the flag from fsconfig", has("/tmp/ma", "nosuid"));
    EXPECT("and the attributes from fsmount", has("/tmp/ma", "noatime") && has("/tmp/ma", "nodiratime"));
    int f = open("/tmp/ma/x", O_CREAT | O_WRONLY, 0644);
    EXPECT("a file can be made in it", f >= 0 && write(f, "hi", 2) == 2);
    if (f >= 0)
        close(f);
    unlink("/tmp/ma/x");
    EXPECT("it unmounts", umount("/tmp/ma") == 0 && !has("/tmp/ma", "rw"));

    /* A read-only mount, and the default atime option. */
    fs = syscall(FSOPEN, "tmpfs", 0);
    EXPECT("a second context", fs >= 0 && syscall(FSCONFIG, fs, 6, NULL, NULL, 0) == 0);
    mfd = syscall(FSMOUNT, fs, 0, 1);                  /* MOUNT_ATTR_RDONLY */
    EXPECT("a read-only mount object", mfd >= 0 && syscall(MOVE_MOUNT, mfd, "", AT_FDCWD, "/tmp/ma", EMPTY) == 0);
    EXPECT("it is read-only", has("/tmp/ma", "ro") && has("/tmp/ma", "relatime"));
    EXPECT("nothing can be made in it", open("/tmp/ma/y", O_CREAT | O_WRONLY, 0644) == -1 && errno == EROFS);
    EXPECT("it unmounts again", umount("/tmp/ma") == 0);

    /* open_tree: arguments are checked in Linux's order; a plain one is a handle. */
    EXPECT("open_tree with a bad flag is EINVAL", syscall(OPEN_TREE, AT_FDCWD, "/tmp", 0xFFFFFFFFu) == -1 && errno == EINVAL);
    EXPECT("open_tree of a missing path is ENOENT", syscall(OPEN_TREE, AT_FDCWD, "/tmp/nosuch", 1) == -1 && errno == ENOENT);
    EXPECT("open_tree on a bad descriptor is EBADF", syscall(OPEN_TREE, -1, "ma", 1) == -1 && errno == EBADF);
    int ot = syscall(OPEN_TREE, AT_FDCWD, "/tmp/ma", 0);
    struct stat st;
    EXPECT("open_tree without a clone is a handle", ot >= 0 && fstat(ot, &st) == 0 && S_ISDIR(st.st_mode));
    if (ot >= 0)
        close(ot);

    /* A clone of a directory attaches as a bind mount of it. */
    mkdir("/tmp/oc_src", 0755);
    mkdir("/tmp/oc_dst", 0755);
    int sf = open("/tmp/oc_src/f", O_CREAT | O_WRONLY, 0644);
    if (sf >= 0) {
        write(sf, "clone", 5);
        close(sf);
    }
    int cl = syscall(OPEN_TREE, AT_FDCWD, "/tmp/oc_src", 1 | 0x80000);   /* OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC */
    EXPECT("open_tree with a clone of a directory is a handle", cl >= 0 && fstat(cl, &st) == 0 && S_ISDIR(st.st_mode));
    char cb[8] = "";
    EXPECT("its files are not in the target yet", access("/tmp/oc_dst/f", F_OK) != 0);
    EXPECT("move_mount attaches the clone", cl >= 0 && syscall(MOVE_MOUNT, cl, "", AT_FDCWD, "/tmp/oc_dst", EMPTY) == 0);
    int cf = open("/tmp/oc_dst/f", O_RDONLY);
    EXPECT("the target shows the files", cf >= 0 && read(cf, cb, sizeof cb) == 5 && memcmp(cb, "clone", 5) == 0);
    if (cf >= 0)
        close(cf);
    EXPECT("the clone is attached once", cl >= 0 && syscall(MOVE_MOUNT, cl, "", AT_FDCWD, "/tmp/oc_dst", EMPTY) == -1);
    if (cl >= 0)
        close(cl);
    EXPECT("it unmounts", umount("/tmp/oc_dst") == 0 && access("/tmp/oc_dst/f", F_OK) != 0);
    unlink("/tmp/oc_src/f");
    rmdir("/tmp/oc_src");
    rmdir("/tmp/oc_dst");
    return nfail != 0;
}
