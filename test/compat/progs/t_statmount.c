/* statmount and listmount: the mounts of the namespace by id, with their
 * parent, attributes, propagation, mount point and file system type. */
#include "compat.h"
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define STATMOUNT 457
#define LISTMOUNT 458
#define LSMT_ROOT (~0ULL)
struct req { unsigned size, spare; unsigned long long mnt_id, param; };
struct sm {
    unsigned size, spare;
    unsigned long long mask;
    unsigned dev_major, dev_minor;
    unsigned long long magic;
    unsigned sb_flags, fs_type;
    unsigned long long mnt_id, parent;
    unsigned id_old, parent_old;
    unsigned long long attr, prop, peer, master, from;
    unsigned mnt_root, mnt_point;
    unsigned long long spare2[50];
    char str[4096];
};

static long stat_mount(unsigned long long id, unsigned long long mask, struct sm *s) {
    struct req r = { 24, 0, id, mask };
    memset(s, 0, sizeof *s);
    return syscall(STATMOUNT, &r, s, sizeof *s, 0);
}

static unsigned long long find(const char *mp, struct sm *out) {
    unsigned long long ids[64];
    struct req r = { 24, 0, LSMT_ROOT, 0 };
    long n = syscall(LISTMOUNT, &r, ids, 64, 0);
    for (long i = 0; i < n; i++)
        if (stat_mount(ids[i], 0x3b, out) == 0 && !strcmp(out->str + out->mnt_point, mp))
            return ids[i];
    return 0;
}

int main(void) {
    setup();
    struct sm s;
    unsigned long long ids[64];
    struct req r = { 24, 0, LSMT_ROOT, 0 };
    long n = syscall(LISTMOUNT, &r, ids, 64, 0);
    EXPECT("listmount lists the mounts", n >= 2 && n < 64);
    EXPECT("the ids are ascending", n >= 2 && ids[0] < ids[1]);
    long n2 = syscall(LISTMOUNT, &r, ids, 1, 0);
    EXPECT("listmount holds to the room it is given", n2 == 1);
    r.param = ids[0];
    long n3 = syscall(LISTMOUNT, &r, ids, 64, 0);
    EXPECT("listmount starts after the last id", n3 == n - 1);
    r.param = 0;
    long n4 = syscall(LISTMOUNT, &r, ids, 64, 1);
    EXPECT("listmount in reverse starts with the newest", n4 == n && ids[0] > ids[1]);
    EXPECT("an unknown mount is ENOENT", stat_mount(100000, 2, &s) == -1 && errno == ENOENT);
    EXPECT("flags are EINVAL", syscall(STATMOUNT, &r, &s, sizeof s, 1) == -1 && errno == EINVAL);
    EXPECT("a small buffer is EOVERFLOW", syscall(STATMOUNT, &r, &s, 16, 0) == -1 && errno == EOVERFLOW);

    mkdir("/tmp/sm1", 0755);
    EXPECT("mount tmpfs", mount("tmpfs", "/tmp/sm1", "tmpfs", MS_NOSUID | MS_NODEV, "") == 0);
    unsigned long long id = find("/tmp/sm1", &s);
    EXPECT("the new mount is found by its mount point", id != 0);
    EXPECT("its mask has the fields asked for", (s.mask & 0x3b) == 0x3b);
    EXPECT("its id is its own", s.mnt_id == id);
    EXPECT("its file system type is tmpfs", !strcmp(s.str + s.fs_type, "tmpfs"));
    EXPECT("its magic number is tmpfs's", s.magic == 0x01021994);
    EXPECT("its root is /", !strcmp(s.str + s.mnt_root, "/"));
    EXPECT("nosuid and nodev show", (s.attr & 6) == 6 && !(s.attr & 1));
    EXPECT("it is private", s.prop == 0x40000);
    EXPECT("the size covers the strings", s.size > 512 && s.size < sizeof s);
    EXPECT("MNT_BASIC alone leaves out the strings", stat_mount(id, 2, &s) == 0 && s.mask == 2 && s.mnt_id == id);

    mkdir("/tmp/sm2", 0755);
    EXPECT("bind it", mount("/tmp/sm1", "/tmp/sm2", NULL, MS_BIND, NULL) == 0);
    unsigned long long bid = find("/tmp/sm2", &s);
    EXPECT("the bind mount has an id of its own", bid != 0 && bid != id);
    EXPECT("make it shared", mount("", "/tmp/sm2", NULL, MS_SHARED, NULL) == 0);
    EXPECT("statmount shows shared and a peer group", stat_mount(bid, 2, &s) == 0 && s.prop == 0x100000 && s.peer > 0);
    mkdir("/tmp/sm3", 0755);
    EXPECT("bind the shared mount", mount("/tmp/sm2", "/tmp/sm3", NULL, MS_BIND, NULL) == 0);
    EXPECT("make the copy a slave", mount("", "/tmp/sm3", NULL, MS_SLAVE, NULL) == 0);
    unsigned long long sid = find("/tmp/sm3", &s);
    EXPECT("statmount shows the master", sid != 0 && stat_mount(sid, 2, &s) == 0 && s.prop == 0x80000 && s.master > 0);
    EXPECT("unmount the slave", umount("/tmp/sm3") == 0);
    EXPECT("remount read-only", mount("", "/tmp/sm2", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0);
    EXPECT("statmount shows ro", stat_mount(bid, 3, &s) == 0 && (s.attr & 1));
    EXPECT("unmount the bind mount", umount("/tmp/sm2") == 0);
    EXPECT("it is gone", stat_mount(bid, 2, &s) == -1 && errno == ENOENT);
    EXPECT("unmount", umount("/tmp/sm1") == 0);
    EXPECT("the tmpfs is gone", stat_mount(id, 2, &s) == -1 && errno == ENOENT);
    return done();
}
