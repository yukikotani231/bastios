/* Checks a mounted ext4 image (test/ext4img.py makes the images).
 *
 *   mext4 tree IMG DIR      mount IMG (type TYPE, default ext4) on DIR, check the tree
 *                           test/ext4img.py put in it, then unmount
 *   mext4 refuse IMG DIR    the mount must fail with EINVAL
 *
 * The tree: small (11 bytes, mode 0640, two names with dir/hard), big
 * (700000 bytes of a pattern), sparse (3 MiB, zero but for ten bytes at
 * 2 MiB), zero (empty), short and long (symbolic links), pipe, empty/,
 * dir/ with 300 files f000.. whose contents are their numbers, one with a
 * 200-byte name, sub/deep, and lost+found.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int bad;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("mext4: FAIL %s\n", what);
        bad++;
    }
}

static int slurp(const char *path, unsigned char **out, long *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    fstat(fd, &st);
    unsigned char *b = malloc(st.st_size + 1);
    long n = 0, r;
    while (n < st.st_size && (r = read(fd, b + n, st.st_size - n)) > 0)
        n += r;
    close(fd);
    *out = b;
    *len = n;
    return n == st.st_size ? 0 : -1;
}

static void path(char *buf, const char *dir, const char *name)
{
    snprintf(buf, 1024, "%s/%s", dir, name);
}

static void tree(const char *d)
{
    char p[1024], t[1024];
    unsigned char *b;
    long n;
    struct stat st, st2;

    path(p, d, "small");
    check(slurp(p, &b, &n) == 0 && n == 11 && memcmp(b, "hello ext4\n", 11) == 0, "small contents");
    check(stat(p, &st) == 0 && (st.st_mode & 07777) == 0640 && S_ISREG(st.st_mode), "small mode");
    path(t, d, "dir/hard");
    check(stat(t, &st2) == 0 && st2.st_ino == st.st_ino && st.st_nlink == 2, "hard link shares the file");

    path(p, d, "big");
    int ok = slurp(p, &b, &n) == 0 && n == 700000;
    for (long i = 0; ok && i < n; i++)
        ok = b[i] == (unsigned char)((i * 7 + i / 251) % 256);
    check(ok, "big contents");

    path(p, d, "sparse");
    ok = slurp(p, &b, &n) == 0 && n == (3 << 20);
    for (long i = 0; ok && i < n; i++) {
        long k = i - (2 << 20);
        unsigned char want = (k >= 0 && k < 10) ? "tail-data\n"[k] : 0;
        ok = b[i] == want;
    }
    check(ok, "sparse contents");

    path(p, d, "zero");
    check(stat(p, &st) == 0 && st.st_size == 0 && S_ISREG(st.st_mode), "zero");
    path(p, d, "short");
    long l = readlink(p, t, sizeof t);
    check(l == 5 && memcmp(t, "small", 5) == 0, "short link");
    path(p, d, "long");
    l = readlink(p, t, sizeof t);
    check(l == 86 && memcmp(t, "dir/sub/x", 9) == 0 && memcmp(t + l - 8, "/../deep", 8) == 0, "long link");
    path(p, d, "pipe");
    check(stat(p, &st) == 0 && S_ISFIFO(st.st_mode), "pipe");
    path(p, d, "empty");
    check(stat(p, &st) == 0 && S_ISDIR(st.st_mode), "empty is a directory");
    path(p, d, "lost+found");
    check(stat(p, &st) == 0 && S_ISDIR(st.st_mode), "lost+found");
    path(p, d, "dir/sub/deep");
    check(slurp(p, &b, &n) == 0 && n == 10 && memcmp(b, "deep file\n", 10) == 0, "deep");

    for (int i = 0; i < 300; i++) {
        char nm[32], want[16];
        snprintf(nm, sizeof nm, "dir/f%03d", i);
        path(p, d, nm);
        int w = snprintf(want, sizeof want, "%d\n", i);
        if (!(slurp(p, &b, &n) == 0 && n == w && memcmp(b, want, w) == 0)) {
            check(0, "numbered file");
            break;
        }
    }
    char longp[512];
    memcpy(longp, "dir/", 4);
    memset(longp + 4, 'n', 200);
    longp[204] = 0;
    path(p, d, longp);
    check(slurp(p, &b, &n) == 0 && n == 10 && memcmp(b, "long name\n", 10) == 0, "200-byte name");

    path(p, d, "dir");
    DIR *dp = opendir(p);
    int count = 0;
    struct dirent *e;
    while (dp && (e = readdir(dp)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            count++;
    if (dp)
        closedir(dp);
    check(count == 303, "dir has 303 entries");
    path(p, d, "empty");
    dp = opendir(p);
    count = 0;
    while (dp && (e = readdir(dp)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            count++;
    if (dp)
        closedir(dp);
    check(count == 0, "empty has nothing");

    /* Read-only: every way of changing it fails with EROFS. */
    path(p, d, "new");
    check(open(p, O_WRONLY | O_CREAT, 0644) < 0 && errno == EROFS, "create is refused");
    path(p, d, "small");
    check(open(p, O_WRONLY) < 0 && errno == EROFS, "open for writing is refused");
    check(unlink(p) < 0 && errno == EROFS, "unlink is refused");
    path(p, d, "newdir");
    check(mkdir(p, 0755) < 0 && errno == EROFS, "mkdir is refused");
    path(t, d, "small2");
    path(p, d, "small");
    check(rename(p, t) < 0 && errno == EROFS, "rename is refused");
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: mext4 tree|refuse IMG DIR [TYPE]\n");
        return 2;
    }
    const char *type = argc > 4 ? argv[4] : "ext4";
    int r = mount(argv[2], argv[3], type, MS_RDONLY, 0);
    if (strcmp(argv[1], "refuse") == 0) {
        check(r < 0 && errno == EINVAL, "the image is refused with EINVAL");
        if (r == 0)
            umount(argv[3]);
    } else {
        if (r < 0) {
            printf("mext4: FAIL mount: %s\n", strerror(errno));
            return 1;
        }
        tree(argv[3]);
        check(umount(argv[3]) == 0, "umount");
        check(rmdir(argv[3]) == 0, "mount point is empty again");
    }
    printf(bad ? "mext4: %d failed\n" : "mext4: clean\n", bad);
    return bad != 0;
}
