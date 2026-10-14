/* Files on a BastiOS disk past 1 GiB: written at offsets from a few bytes
 * to 40 GiB (a super map of root maps, one per GiB), across the 1 GiB
 * boundaries, read back, cut and grown again, and freed. Takes the
 * directory to work in (a disk with a block count of at least 41 GiB). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define GIB (1LL << 30)

static int fail(const char *what)
{
    printf("mbig: FAIL %s (errno %d)\n", what, errno);
    return 1;
}

static int put(int fd, long long at, const char *s)
{
    size_t n = strlen(s);
    return pwrite(fd, s, n, at) == (ssize_t)n ? 0 : -1;
}

/* Does the file hold s at `at`? */
static int has(int fd, long long at, const char *s)
{
    char buf[64];
    size_t n = strlen(s);
    return pread(fd, buf, n, at) == (ssize_t)n && memcmp(buf, s, n) == 0;
}

/* Are n bytes at `at` all zero? */
static int zeros(int fd, long long at, size_t n)
{
    char buf[64];
    if (n > sizeof buf)
        return 0;
    if (pread(fd, buf, n, at) != (ssize_t)n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (buf[i])
            return 0;
    return 1;
}

static long long size_of(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? -1 : (long long)st.st_size;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return fail("usage");
    char path[256];
    snprintf(path, sizeof path, "%s/huge", argv[1]);
    struct statfs fs0, fs1;
    sync();
    if (statfs(argv[1], &fs0))
        return fail("statfs");
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("open");
    if (put(fd, 0, "alpha") || put(fd, GIB + 100, "beta")
        || put(fd, 5 * GIB - 3, "gamma")          /* across the 5 GiB boundary */
        || put(fd, 40 * GIB + 7, "delta"))
        return fail("write");
    if (size_of(fd) != 40 * GIB + 12)
        return fail("size");
    if (!has(fd, 0, "alpha") || !has(fd, GIB + 100, "beta") || !has(fd, 5 * GIB - 3, "gamma")
        || !has(fd, 40 * GIB + 7, "delta"))
        return fail("read back");
    if (!zeros(fd, 2 * GIB, 64) || !zeros(fd, 20 * GIB + 4096, 64) || !zeros(fd, GIB + 104, 32))
        return fail("holes");
    if (fsync(fd) || close(fd))
        return fail("close");
    fd = open(path, O_RDWR);
    if (fd < 0 || !has(fd, 40 * GIB + 7, "delta") || !has(fd, GIB + 100, "beta"))
        return fail("reopen");
    /* Cut at 6 GiB: delta goes, gamma stays, and growing back reads zeros. */
    if (ftruncate(fd, 6 * GIB) || size_of(fd) != 6 * GIB || !has(fd, 5 * GIB - 3, "gamma"))
        return fail("cut to 6 GiB");
    if (ftruncate(fd, 40 * GIB + 12) || !zeros(fd, 40 * GIB + 7, 5) || !has(fd, 5 * GIB - 3, "gamma"))
        return fail("grow back");
    /* Cut in the middle of beta: its tail reads as zeros after growing. */
    if (ftruncate(fd, GIB + 102) || size_of(fd) != GIB + 102 || ftruncate(fd, 6 * GIB))
        return fail("cut inside beta");
    if (!has(fd, GIB + 100, "be") || !zeros(fd, GIB + 102, 2) || !zeros(fd, 5 * GIB - 3, 5) || !has(fd, 0, "alpha"))
        return fail("after the cut");
    /* Back to one segment: a write past the end makes a second one again. */
    if (ftruncate(fd, 1000) || put(fd, 2 * GIB + 1, "again") || !has(fd, 2 * GIB + 1, "again") || !has(fd, 0, "alpha"))
        return fail("one segment and back");
    if (ftruncate(fd, 0) || size_of(fd) != 0)
        return fail("empty");
    if (put(fd, 3 * GIB, "last") || !has(fd, 3 * GIB, "last"))
        return fail("rewrite");
    if (close(fd) || unlink(path))
        return fail("unlink");
    sync();
    if (statfs(argv[1], &fs1) || fs1.f_bfree != fs0.f_bfree)
        return fail("blocks not freed");
    printf("mbig: ok, files past 1 GiB in %s\n", argv[1]);
    return 0;
}
