/* File system features beyond plain reads and writes: fallocate modes
 * (PUNCH_HOLE, ZERO_RANGE), on a file in /tmp and on the disk. */
#include "compat.h"
#include <fcntl.h>
#include <sys/stat.h>

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
    /* modes that move data are not supported */
    ok = ok && fallocate(fd, FALLOC_FL_COLLAPSE_RANGE, 0, 4096) == -1 && errno == EOPNOTSUPP;
    close(fd);
    unlink(name);
    return ok;
}

int main(void) {
    setup();
    EXPECT("fallocate PUNCH_HOLE|KEEP_SIZE zeroes a range in /tmp", punch("/tmp/fsx_a"));
    EXPECT("fallocate ZERO_RANGE zeroes (and extends) a range in /tmp", zero_range("/tmp/fsx_b"));
    EXPECT("fallocate PUNCH_HOLE|KEEP_SIZE zeroes a range on the disk (the working directory)", punch("fsx_a"));
    EXPECT("fallocate ZERO_RANGE zeroes (and extends) a range on the disk (the working directory)", zero_range("fsx_b"));
    return done();
}
