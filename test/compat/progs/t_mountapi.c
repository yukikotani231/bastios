/* The new mount API: fsopen, fsconfig, fsmount and move_mount make a tmpfs
 * on a directory with the options asked for, and refuse what Linux refuses. */
#include "compat.h"
#include <sys/statvfs.h>
#include <sys/wait.h>
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
#define MOUNT_SETATTR 442
struct mattr { unsigned long long set, clr, prop, userns; };

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

    /* Mount attributes of a bind mount: set when it is made, changed by a remount. */
    mkdir("/tmp/oc_dst", 0755);
    int xf = open("/tmp/oc_src/run", O_CREAT | O_WRONLY, 0755);
    if (xf >= 0) {
        write(xf, "#!/bin/sh\nexit 0\n", 17);
        close(xf);
    }
    EXPECT("a bind mount", mount("/tmp/oc_src", "/tmp/oc_dst", NULL, MS_BIND, NULL) == 0);
    struct statvfs sv;
    EXPECT("it has no attributes", statvfs("/tmp/oc_dst", &sv) == 0 && !(sv.f_flag & (ST_NOSUID | ST_NODEV | ST_NOEXEC | ST_RDONLY)));
    EXPECT("remounting it sets nosuid, nodev, noexec and read-only",
           mount(NULL, "/tmp/oc_dst", NULL, MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, NULL) == 0);
    EXPECT("statvfs reports them", statvfs("/tmp/oc_dst", &sv) == 0 && (sv.f_flag & ST_NOSUID) && (sv.f_flag & ST_NODEV) && (sv.f_flag & ST_NOEXEC) && (sv.f_flag & ST_RDONLY));
    EXPECT("so does /proc/mounts", has("/tmp/oc_dst", "ro") && has("/tmp/oc_dst", "nosuid") && has("/tmp/oc_dst", "nodev") && has("/tmp/oc_dst", "noexec"));
    {
        pid_t xp = fork();
        if (xp == 0) {
            execl("/tmp/oc_dst/run", "run", (char *)0);
            _exit(errno == EACCES ? 0 : 1);
        }
        int xs = 0;
        waitpid(xp, &xs, 0);
        EXPECT("noexec refuses to run a file in it", WIFEXITED(xs) && WEXITSTATUS(xs) == 0);
    }
    EXPECT("the source is not changed", mount("/tmp/oc_src", "/tmp/oc_dst", NULL, MS_BIND | MS_REMOUNT, NULL) == 0 && statvfs("/tmp/oc_src", &sv) == 0 && !(sv.f_flag & ST_NOEXEC));
    EXPECT("a remount without flags clears them", statvfs("/tmp/oc_dst", &sv) == 0 && !(sv.f_flag & (ST_NOSUID | ST_NODEV | ST_NOEXEC | ST_RDONLY)));
    umount("/tmp/oc_dst");

    /* mount_setattr: on a clone before it is attached, and on a mount that exists. */
    struct mattr ma = { 1 | 2 | 8, 0, 0, 0 };           /* MOUNT_ATTR_RDONLY | NOSUID | NOEXEC */
    EXPECT("mount_setattr with a short struct is EINVAL", syscall(MOUNT_SETATTR, AT_FDCWD, "/tmp/oc_src", 0, &ma, 8) == -1 && errno == EINVAL);
    EXPECT("with a bad flag is EINVAL", syscall(MOUNT_SETATTR, AT_FDCWD, "/tmp/oc_src", 0x40, &ma, sizeof ma) == -1 && errno == EINVAL);
    struct mattr bad = { 0x4000, 0, 0, 0 };
    EXPECT("with an unknown attribute is EINVAL", syscall(MOUNT_SETATTR, AT_FDCWD, "/tmp/oc_src", 0, &bad, sizeof bad) == -1 && errno == EINVAL);
    cl = syscall(OPEN_TREE, AT_FDCWD, "/tmp/oc_src", 1 | 0x80000);
    EXPECT("mount_setattr sets attributes on a clone", cl >= 0 && syscall(MOUNT_SETATTR, cl, "", 0x1000, &ma, sizeof ma) == 0);
    EXPECT("then move_mount attaches it with them", cl >= 0 && syscall(MOVE_MOUNT, cl, "", AT_FDCWD, "/tmp/oc_dst", EMPTY) == 0
           && statvfs("/tmp/oc_dst", &sv) == 0 && (sv.f_flag & ST_RDONLY) && (sv.f_flag & ST_NOSUID) && (sv.f_flag & ST_NOEXEC) && !(sv.f_flag & ST_NODEV));
    if (cl >= 0)
        close(cl);
    struct mattr ma2 = { 4, 1 | 8, 0, 0 };              /* set nodev, clear rdonly and noexec */
    EXPECT("mount_setattr changes a mount that exists", syscall(MOUNT_SETATTR, AT_FDCWD, "/tmp/oc_dst", 0, &ma2, sizeof ma2) == 0
           && statvfs("/tmp/oc_dst", &sv) == 0 && !(sv.f_flag & ST_RDONLY) && !(sv.f_flag & ST_NOEXEC) && (sv.f_flag & ST_NODEV) && (sv.f_flag & ST_NOSUID));
    EXPECT("mount_setattr on a path in no mount is EINVAL", syscall(MOUNT_SETATTR, AT_FDCWD, "/tmp/oc_src", 0, &ma2, sizeof ma2) == -1 && errno == EINVAL);
    umount("/tmp/oc_dst");
    /* What crun does to a read-only path: clone it, make the clone read-only, put it back. */
    {
        int pc = syscall(OPEN_TREE, AT_FDCWD, "/proc/sys", 1 | 0x80000 | 0x8000);
        struct mattr pro = { 1, 0, 0, 0 };
        EXPECT("a procfs directory clones", pc >= 0 || errno == ENOSYS);
        if (pc >= 0) {
            EXPECT("and is set read-only", syscall(MOUNT_SETATTR, pc, "", 0x1000, &pro, sizeof pro) == 0);
            EXPECT("and moved back over itself", syscall(MOVE_MOUNT, pc, "", AT_FDCWD, "/proc/sys", EMPTY) == 0);
            umount("/proc/sys");
            close(pc);
        }
        mkdir("/tmp/pp", 0755);
        EXPECT("a proc mount", mount("proc", "/tmp/pp", "proc", 0, NULL) == 0);
        pc = syscall(OPEN_TREE, AT_FDCWD, "/tmp/pp/sys", 1 | 0x80000 | 0x8000);
        EXPECT("clones a directory of it", pc >= 0 || errno == ENOSYS);
        if (pc >= 0) {
            EXPECT("sets it read-only", syscall(MOUNT_SETATTR, pc, "", 0x1000, &pro, sizeof pro) == 0);
            EXPECT("moves it back over itself", syscall(MOVE_MOUNT, pc, "", AT_FDCWD, "/tmp/pp/sys", EMPTY) == 0);
            umount("/tmp/pp/sys");
            close(pc);
        }
        umount("/tmp/pp");
        rmdir("/tmp/pp");
        {
            /* A name through /proc/self/fd/N is followed to the directory the descriptor names, as on Linux: it can be cloned. */
            mkdir("/tmp/pp", 0755);
            mount("proc", "/tmp/pp", "proc", 0, NULL);
            int tfd = open("/tmp/pp/sys", O_PATH | O_DIRECTORY);
            char pfd[64];
            snprintf(pfd, sizeof pfd, "/proc/self/fd/%d", tfd);
            int c2 = syscall(OPEN_TREE, AT_FDCWD, pfd, 1 | 0x80000 | 0x8000);
            EXPECT("a clone through /proc/self/fd/N is a handle on the directory", c2 >= 0);
            if (c2 >= 0) close(c2);
            EXPECT("and mount(2) binds onto it", mount("/tmp/pp/sys", pfd, NULL, MS_BIND, NULL) == 0);
            int c3 = syscall(OPEN_TREE, AT_FDCWD, "/tmp/pp/sys", 1 | 0x80000 | 0x8000);
            EXPECT("move_mount onto a descriptor with T_EMPTY_PATH", c3 >= 0 && syscall(MOVE_MOUNT, c3, "", tfd, "", EMPTY | 0x40) == 0);
            close(tfd);
            umount("/tmp/pp/sys");
            umount("/tmp/pp");
            rmdir("/tmp/pp");
        }
        int rfd = open("/", O_RDONLY | O_DIRECTORY);
        pc = syscall(OPEN_TREE, rfd, "proc/sys", 1 | 0x80000 | 0x8000);
        EXPECT("a relative one clones", pc >= 0);
        EXPECT("and is set read-only (relative)", pc >= 0 && syscall(MOUNT_SETATTR, pc, "", 0x1000, &pro, sizeof pro) == 0);
        EXPECT("and moved back (relative to a descriptor)", pc >= 0 && syscall(MOVE_MOUNT, pc, "", rfd, "proc/sys", EMPTY) == 0);
        umount("/proc/sys");
        if (pc >= 0) close(pc);
        pc = syscall(OPEN_TREE, AT_FDCWD, "proc/sys", 1 | 0x80000 | 0x8000);
        EXPECT("and moved back (relative to the cwd)", pc >= 0 && chdir("/") == 0 && syscall(MOVE_MOUNT, pc, "", AT_FDCWD, "proc/sys", EMPTY) == 0);
        umount("/proc/sys");
    }
    unlink("/tmp/oc_src/run");
    unlink("/tmp/oc_src/f");
    rmdir("/tmp/oc_src");
    rmdir("/tmp/oc_dst");
    return nfail != 0;
}
