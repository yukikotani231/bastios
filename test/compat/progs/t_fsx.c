/* File system features beyond plain reads and writes: fallocate modes
 * (PUNCH_HOLE, ZERO_RANGE) and extended attributes, on files in /tmp and
 * on the disk. */
#include "compat.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#ifndef FALLOC_FL_COLLAPSE_RANGE
#define FALLOC_FL_COLLAPSE_RANGE 8
#endif
#ifndef FALLOC_FL_ZERO_RANGE
#define FALLOC_FL_ZERO_RANGE 16
#endif

/* Fills a file with 'x' for n bytes, then returns the descriptor. */
static int filled(const char *name, int n) {
    unlink(name);
    int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    char buf[4096];
    memset(buf, 'x', sizeof buf);
    for (int i = 0; i < n; i += sizeof buf)
        if (write(fd, buf, n - i < (int)sizeof buf ? n - i : (int)sizeof buf) < 0) return -1;
    return fd;
}

/* Whether bytes [from, to) of fd are all c. */
static int range_is(int fd, long from, long to, char c) {
    char b[4096];
    for (long p = from; p < to; ) {
        long n = to - p < (long)sizeof b ? to - p : (long)sizeof b;
        if (pread(fd, b, n, p) != n) return 0;
        for (long i = 0; i < n; i++) if (b[i] != c) return 0;
        p += n;
    }
    return 1;
}

/* A name on the disk: /disk, or the working directory on a kernel without one. */
static const char *on_disk(const char *leaf) {
    static char buf[4][64];
    static int n;
    char *b = buf[n++ % 4];
    snprintf(b, 64, "%s%s", access("/disk", W_OK) == 0 ? "/disk/" : "", leaf);
    return b;
}

static int punch(const char *name) {
    struct stat s;
    int fd = filled(name, 20000);
    if (fd < 0) return 0;
    int ok = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 100, 9000) == 0
        && fstat(fd, &s) == 0 && s.st_size == 20000
        && range_is(fd, 0, 100, 'x') && range_is(fd, 100, 9100, 0) && range_is(fd, 9100, 20000, 'x');
    /* a range that runs past the end punches up to the end and keeps the size */
    ok = ok && fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 19000, 100000) == 0
        && fstat(fd, &s) == 0 && s.st_size == 20000 && range_is(fd, 19000, 20000, 0);
    /* PUNCH_HOLE alone is not supported, as on Linux */
    ok = ok && fallocate(fd, FALLOC_FL_PUNCH_HOLE, 0, 10) == -1 && errno == EOPNOTSUPP;
    /* the file offset stays where it was */
    lseek(fd, 123, SEEK_SET);
    ok = ok && fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 50) == 0 && lseek(fd, 0, SEEK_CUR) == 123;
    close(fd);
    unlink(name);
    return ok;
}

static int zero_range(const char *name) {
    struct stat s;
    int fd = filled(name, 10000);
    if (fd < 0) return 0;
    int ok = fallocate(fd, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, 5000, 100000) == 0
        && fstat(fd, &s) == 0 && s.st_size == 10000 && range_is(fd, 0, 5000, 'x') && range_is(fd, 5000, 10000, 0);
    /* without KEEP_SIZE the file grows to the end of the range */
    ok = ok && fallocate(fd, FALLOC_FL_ZERO_RANGE, 8000, 6000) == 0
        && fstat(fd, &s) == 0 && s.st_size == 14000 && range_is(fd, 5000, 14000, 0);
    /* an append-mode descriptor still writes at the offset, not at the end */
    int a = open(name, O_WRONLY | O_APPEND);
    ok = ok && a >= 0 && fallocate(a, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, 0, 10) == 0
        && fstat(fd, &s) == 0 && s.st_size == 14000 && range_is(fd, 0, 10, 0) && range_is(fd, 10, 5000, 'x');
    if (a >= 0) close(a);
    close(fd);
    unlink(name);
    return ok;
}


/* A randomized comparison against a model in memory (as xfstests' fsx
 * does): writes, truncates, hole punches and zeroed ranges, with the whole
 * file compared after every few steps. */
#define FSX_MAX 200000
static unsigned long long rng = 88172645463325252ULL;
static unsigned long rnd(unsigned long n) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned long)(rng % n);
}

static int fsx(const char *name, int steps) {
    static unsigned char model[FSX_MAX], got[FSX_MAX];
    long size = 0;
    unlink(name);
    int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("  open: errno %d\n", errno); return 0; }
    memset(model, 0, sizeof model);
    for (int i = 0; i < steps; i++) {
        long off = rnd(FSX_MAX - 1), len = 1 + rnd(30000);
        if (off + len > FSX_MAX) len = FSX_MAX - off;
        switch (rnd(5)) {
        case 0: case 1: {
            unsigned char buf[30000];
            for (long j = 0; j < len; j++) buf[j] = 1 + rnd(255);
            if (pwrite(fd, buf, len, off) != len) { printf("  pwrite at step %d: %d\n", i, errno); return 0; }
            memcpy(model + off, buf, len);
            if (off + len > size) size = off + len;
            break;
        }
        case 2: {
            long to = rnd(FSX_MAX);
            if (ftruncate(fd, to) != 0) { printf("  ftruncate at step %d: %d\n", i, errno); return 0; }
            if (to < size) memset(model + to, 0, size - to);
            size = to;
            break;
        }
        case 3:
            if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, len) != 0) { printf("  punch at step %d: %d\n", i, errno); return 0; }
            if (off < size) memset(model + off, 0, (off + len > size ? size : off + len) - off);
            break;
        default:
            if (fallocate(fd, FALLOC_FL_ZERO_RANGE, off, len) != 0) { printf("  zero at step %d: %d\n", i, errno); return 0; }
            memset(model + off, 0, len);
            if (off + len > size) size = off + len;
            break;
        }
        if (i % 7 == 6 || i == steps - 1) {
            struct stat s;
            if (fstat(fd, &s) != 0 || s.st_size != size) { printf("size %ld want %ld at step %d\n", (long)s.st_size, size, i); return 0; }
            { ssize_t rd = pread(fd, got, size, 0); if (rd != size) { printf("  pread at step %d: %ld of %ld, errno %d\n", i, (long)rd, size, errno); return 0; } }
            if (memcmp(got, model, size) != 0) { printf("data differs at step %d\n", i); return 0; }
        }
    }
    close(fd);
    unlink(name);
    return 1;
}

/* Whether the attribute names of path are exactly the n names in want
 * (in any order). */
static int names_are(const char *path, const char *const *want, int n) {
    char buf[4096];
    ssize_t len = listxattr(path, buf, sizeof buf);
    if (len < 0) return 0;
    int seen = 0;
    for (char *p = buf; p < buf + len; p += strlen(p) + 1) {
        int hit = 0;
        for (int i = 0; i < n; i++) hit += strcmp(p, want[i]) == 0;
        if (hit != 1) return 0;
        seen++;
    }
    return seen == n;
}

/* One step of a check: the first that fails says where. */
#define S(c) do { if (ok && !(c)) { printf("  step at line %d failed (errno %d %s)\n", __LINE__, errno, strerror(errno)); ok = 0; } } while (0)

static int xattrs(const char *name, const char *lnk, const char *moved, int rich) {
    char v[4096], w[4096], nm[32];
    int ok = 1;
    int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    close(fd);
    S(setxattr(name, "user.a", "one", 3, 0) == 0);
    S(getxattr(name, "user.a", v, sizeof v) == 3 && memcmp(v, "one", 3) == 0);
    S(getxattr(name, "user.a", NULL, 0) == 3);
    S(getxattr(name, "user.a", v, 2) == -1 && errno == ERANGE);
    S(getxattr(name, "user.none", v, sizeof v) == -1 && errno == ENODATA);
    /* XATTR_CREATE and XATTR_REPLACE */
    S(setxattr(name, "user.a", "x", 1, XATTR_CREATE) == -1 && errno == EEXIST);
    S(setxattr(name, "user.b", "x", 1, XATTR_REPLACE) == -1 && errno == ENODATA);
    S(setxattr(name, "user.a", "replaced", 8, XATTR_REPLACE) == 0);
    S(getxattr(name, "user.a", v, sizeof v) == 8 && memcmp(v, "replaced", 8) == 0);
    /* a value that fills most of the file's block, then replaced by a short one */
    memset(w, 'q', sizeof w);
    if (rich) {
        S(setxattr(name, "user.big", w, 3000, 0) == 0);
        S(getxattr(name, "user.big", v, sizeof v) == 3000 && memcmp(v, w, 3000) == 0);
        S(setxattr(name, "user.big2", w, 3000, 0) == -1 && errno == ENOSPC);
    }
    S(setxattr(name, "user.big", "s", 1, 0) == 0 && getxattr(name, "user.big", v, sizeof v) == 1);
    /* an empty value is a value */
    S(setxattr(name, "user.empty", "", 0, 0) == 0 && getxattr(name, "user.empty", v, sizeof v) == 0);
    const char *want[] = {"user.a", "user.big", "user.empty"};
    S(names_are(name, want, 3));
    /* the attributes are the file's, not the name's */
    S(link(name, lnk) == 0 && getxattr(lnk, "user.a", v, sizeof v) == 8);
    S(rename(name, moved) == 0 && getxattr(moved, "user.a", v, sizeof v) == 8);
    S(getxattr(lnk, "user.a", v, sizeof v) == 8);
    /* many small attributes */
    for (int i = 0; ok && i < (rich ? 60 : 10); i++) {
        snprintf(nm, sizeof nm, "user.n%02d", i);
        S(setxattr(moved, nm, nm, strlen(nm), 0) == 0);
    }
    for (int i = 0; ok && i < (rich ? 60 : 10); i++) {
        snprintf(nm, sizeof nm, "user.n%02d", i);
        S(getxattr(moved, nm, v, sizeof v) == (ssize_t)strlen(nm) && memcmp(v, nm, strlen(nm)) == 0);
    }
    for (int i = 0; ok && i < (rich ? 60 : 10); i += 2) {
        snprintf(nm, sizeof nm, "user.n%02d", i);
        S(removexattr(moved, nm) == 0);
    }
    S(getxattr(moved, "user.n00", v, sizeof v) == -1 && errno == ENODATA);
    S(getxattr(moved, "user.n01", v, sizeof v) == 8);
    /* removing them all leaves none */
    for (int i = 1; ok && i < (rich ? 60 : 10); i += 2) {
        snprintf(nm, sizeof nm, "user.n%02d", i);
        S(removexattr(moved, nm) == 0);
    }
    S(removexattr(moved, "user.a") == 0 && removexattr(moved, "user.big") == 0 && removexattr(moved, "user.empty") == 0);
    S(listxattr(moved, v, sizeof v) == 0);
    /* a directory holds them too, and they go with a rename */
    char d1[64], d2[64];
    snprintf(d1, sizeof d1, "%s.d", name);
    snprintf(d2, sizeof d2, "%s.e", name);
    mkdir(d1, 0755);
    S(setxattr(d1, "user.dir", "d", 1, 0) == 0 && rename(d1, d2) == 0);
    S(getxattr(d2, "user.dir", v, sizeof v) == 1);
    rmdir(d2);
    unlink(lnk);
    unlink(moved);
    unlink(name);
    return ok;
}

/* On the disk the attributes are in the file system, so they are still
 * there when the file system is mounted again. */
static int xattrs_remount(void) {
    if (access("/bin/mkfs.bstdisk3", X_OK) != 0) return 1;   /* BastiOS's own file system, not on another kernel */
    int fd = open("/tmp/x_img", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    int ok = fd >= 0 && ftruncate(fd, 1 << 20) == 0;
    close(fd);
    pid_t c = fork();
    if (c == 0) {
        execl("/bin/mkfs.bstdisk3", "mkfs.bstdisk3", "/tmp/x_img", (char *)0);
        _exit(9);
    }
    int st;
    ok = ok && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    mkdir("/tmp/x_mnt", 0755);
    ok = ok && mount("/tmp/x_img", "/tmp/x_mnt", "bstdisk3", 0, NULL) == 0;
    ok = ok && xattrs("/tmp/x_mnt/f", "/tmp/x_mnt/l", "/tmp/x_mnt/m", 1);
    fd = open("/tmp/x_mnt/keep", O_CREAT | O_WRONLY, 0644);
    ok = ok && fd >= 0 && close(fd) == 0 && setxattr("/tmp/x_mnt/keep", "user.k", "kept", 4, 0) == 0
        && mkdir("/tmp/x_mnt/kd", 0755) == 0 && setxattr("/tmp/x_mnt/kd", "user.k", "dir", 3, 0) == 0
        && umount2("/tmp/x_mnt", 0) == 0
        && mount("/tmp/x_img", "/tmp/x_mnt", "bstdisk3", 0, NULL) == 0;
    char v[16];
    ok = ok && getxattr("/tmp/x_mnt/keep", "user.k", v, sizeof v) == 4 && memcmp(v, "kept", 4) == 0
        && getxattr("/tmp/x_mnt/kd", "user.k", v, sizeof v) == 3 && memcmp(v, "dir", 3) == 0;
    /* a new file reusing a freed entry and blocks has none */
    ok = ok && unlink("/tmp/x_mnt/keep") == 0;
    fd = open("/tmp/x_mnt/again", O_CREAT | O_WRONLY, 0644);
    ok = ok && fd >= 0 && close(fd) == 0 && listxattr("/tmp/x_mnt/again", v, sizeof v) == 0;
    /* the freed attribute blocks are free: fill most of the image with files and attributes */
    for (int i = 0; ok && i < 100; i++) {
        char f[64];
        snprintf(f, sizeof f, "/tmp/x_mnt/n%d", i);
        fd = open(f, O_CREAT | O_WRONLY, 0644);
        ok = fd >= 0 && close(fd) == 0 && setxattr(f, "user.i", f, strlen(f), 0) == 0;
        unlink(f);
    }
    umount2("/tmp/x_mnt", 0);
    rmdir("/tmp/x_mnt");
    unlink("/tmp/x_img");
    return ok;
}

int main(void) {
    setup();
    EXPECT("fallocate PUNCH_HOLE|KEEP_SIZE zeroes a range in /tmp", punch("/tmp/fsx_a"));
    EXPECT("fallocate ZERO_RANGE zeroes (and extends) a range in /tmp", zero_range("/tmp/fsx_b"));
    EXPECT("fallocate PUNCH_HOLE|KEEP_SIZE zeroes a range on the disk", punch(on_disk("fsx_a")));
    EXPECT("fallocate ZERO_RANGE zeroes (and extends) a range on the disk", zero_range(on_disk("fsx_b")));
    EXPECT("random writes, truncates, holes and zeroed ranges match a model in /tmp", fsx("/tmp/fsx_c", 150));
    EXPECT("random writes, truncates, holes and zeroed ranges match a model on the disk", fsx(on_disk("fsx_c"), 150));
    EXPECT("setxattr, getxattr, listxattr and removexattr on a file in /tmp", xattrs("/tmp/fsx_x", "/tmp/fsx_xl", "/tmp/fsx_xm", 0));
    EXPECT("extended attributes on the disk: flags, 3000-byte values, 60 names, links, renames, directories", xattrs(on_disk("fsx_x"), on_disk("fsx_xl"), on_disk("fsx_xm"), 1));
    EXPECT("extended attributes stay on a bstdisk3 file system when it is mounted again", xattrs_remount());
    return done();
}
