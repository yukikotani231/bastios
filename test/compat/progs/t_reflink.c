/* The reflink ioctls: FICLONE, FICLONERANGE and FIDEDUPERANGE. Bytes are
 * copied, not shared, so the checks are on what a program can see: equal
 * contents afterwards that then change independently, the errors Linux
 * gives for bad descriptors, kinds, alignment and overlap, and the per
 * destination status of a deduplication. */
#define _GNU_SOURCE
#include "compat.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define FICLONE _IOW(0x94, 9, int)
struct clone_range { int64_t src_fd; uint64_t src_offset, src_length, dest_offset; };
#define FICLONERANGE _IOW(0x94, 13, struct clone_range)
struct dedupe_info { int64_t dest_fd; uint64_t dest_offset, bytes_deduped; int32_t status; uint32_t reserved; };
struct dedupe { uint64_t src_offset, src_length; uint16_t dest_count, reserved1; uint32_t reserved2; struct dedupe_info info[2]; };
#define FIDEDUPERANGE 0xC0189436u   /* _IOWR(0x94, 54, the 24-byte header) */
#define SZ 12288

static int mk(const char *name, int flags, unsigned char seed, int size) {
    int fd = open(name, O_CREAT | O_TRUNC | flags, 0600);
    unsigned char b[SZ];
    for (int i = 0; i < size; i++) b[i] = (unsigned char)(seed + i * 7);
    if (fd >= 0 && size) (void)!write(fd, b, size);
    return fd;
}

static int same(int a, int b, int size) {
    unsigned char x[SZ], y[SZ];
    if (pread(a, x, size, 0) != size || pread(b, y, size, 0) != size) return 0;
    return memcmp(x, y, size) == 0;
}

int main(void) {
    int s = mk("/tmp/rl_src", O_RDWR, 1, SZ);
    int d = mk("/tmp/rl_dst", O_RDWR, 9, 5000);
    EXPECT("FICLONE makes the destination a copy of the source", ioctl(d, FICLONE, s) == 0 && lseek(d, 0, SEEK_END) == SZ && same(s, d, SZ));
    EXPECT("the copy changes independently of the source", ({
        unsigned char z = 0xEE, y;
        (void)!pwrite(d, &z, 1, 100);
        (void)!pread(s, &y, 1, 100);
        y != 0xEE;
    }));
    EXPECT("FICLONE does not move the destination's offset", ({
        lseek(d, 77, SEEK_SET);
        ioctl(d, FICLONE, s);
        lseek(d, 0, SEEK_CUR) == 77;
    }));
    EXPECT("FICLONE of a file onto itself is a no-op", ioctl(s, FICLONE, s) == 0 && same(s, s, SZ));
    EXPECT("FICLONE from a bad descriptor is EBADF", ioctl(d, FICLONE, 999) == -1 && errno == EBADF);
    EXPECT("FICLONE onto a descriptor opened read-only is EBADF", ({
        int ro = open("/tmp/rl_dst", O_RDONLY);
        int r = ioctl(ro, FICLONE, s);
        int e = errno;
        close(ro);
        r == -1 && e == EBADF;
    }));
    EXPECT("FICLONE onto an O_APPEND descriptor is EBADF", ({
        int ap = open("/tmp/rl_dst", O_WRONLY | O_APPEND);
        int r = ioctl(ap, FICLONE, s);
        int e = errno;
        close(ap);
        r == -1 && e == EBADF;
    }));
    EXPECT("FICLONE from a directory is EISDIR", ({
        int dir = open("/tmp", O_RDONLY | O_DIRECTORY);
        int r = ioctl(d, FICLONE, dir);
        int e = errno;
        close(dir);
        r == -1 && e == EISDIR;
    }));
    EXPECT("FICLONE from a pipe is EINVAL", ({
        int p[2];
        int r = pipe(p) == 0 ? ioctl(d, FICLONE, p[0]) : 0;
        int e = errno;
        r == -1 && e == EINVAL;
    }));
    EXPECT("FICLONERANGE copies a block-aligned range", ({
        int t = mk("/tmp/rl_t", O_RDWR, 3, SZ);
        struct clone_range r = { s, 4096, 4096, 8192 };
        int ok = ioctl(t, FICLONERANGE, &r) == 0;
        unsigned char a[4096], b[4096];
        ok = ok && pread(s, a, 4096, 4096) == 4096 && pread(t, b, 4096, 8192) == 4096 && memcmp(a, b, 4096) == 0;
        unsigned char c, e2;
        ok = ok && pread(t, &c, 1, 0) == 1 && (e2 = (unsigned char)(3), c == e2);
        close(t);
        ok;
    }));
    EXPECT("FICLONERANGE with a length of 0 copies to the end of the source", ({
        int t = mk("/tmp/rl_t", O_RDWR, 3, 0);
        struct clone_range r = { s, 4096, 0, 4096 };
        int ok = ioctl(t, FICLONERANGE, &r) == 0 && lseek(t, 0, SEEK_END) == SZ;
        close(t);
        ok;
    }));
    EXPECT("FICLONERANGE with an unaligned offset is EINVAL", ({
        struct clone_range r = { s, 100, 4096, 0 };
        ioctl(d, FICLONERANGE, &r) == -1 && errno == EINVAL;
    }));
    EXPECT("FICLONERANGE past the end of the source is EINVAL", ({
        struct clone_range r = { s, 8192, 8192, 0 };
        ioctl(d, FICLONERANGE, &r) == -1 && errno == EINVAL;
    }));
    EXPECT("FICLONERANGE with overlapping ranges in one file is EINVAL", ({
        struct clone_range r = { s, 0, 8192, 4096 };
        ioctl(s, FICLONERANGE, &r) == -1 && errno == EINVAL;
    }));
    EXPECT("FICLONERANGE with a bad pointer is EFAULT", ioctl(d, FICLONERANGE, (void *)8) == -1 && errno == EFAULT);
    EXPECT("FIDEDUPERANGE says SAME for equal ranges", ({
        ioctl(d, FICLONE, s);
        struct dedupe q;
        memset(&q, 0, sizeof q);
        q.src_offset = 0; q.src_length = SZ; q.dest_count = 1;
        q.info[0].dest_fd = d; q.info[0].dest_offset = 0;
        int r = ioctl(s, FIDEDUPERANGE, &q);
        r == 0 && q.info[0].status == 0 && q.info[0].bytes_deduped == SZ;
    }));
    EXPECT("FIDEDUPERANGE says DIFFERS for ranges that differ", ({
        unsigned char z = 0x55;
        (void)!pwrite(d, &z, 1, 9000);
        struct dedupe q;
        memset(&q, 0, sizeof q);
        q.src_offset = 0; q.src_length = SZ; q.dest_count = 1;
        q.info[0].dest_fd = d; q.info[0].dest_offset = 0;
        int r = ioctl(s, FIDEDUPERANGE, &q);
        r == 0 && q.info[0].status == 1 && q.info[0].bytes_deduped == 0;
    }));
    EXPECT("FIDEDUPERANGE gives each destination its own status", ({
        int ro = open("/tmp/rl_dst", O_RDONLY);
        struct dedupe q;
        memset(&q, 0, sizeof q);
        q.src_offset = 0; q.src_length = 4096; q.dest_count = 2;
        q.info[0].dest_fd = d; q.info[0].dest_offset = 0;
        q.info[1].dest_fd = ro; q.info[1].dest_offset = 0;
        int r = ioctl(s, FIDEDUPERANGE, &q);
        close(ro);
        r == 0 && q.info[0].status == 0 && q.info[1].status == -EBADF;
    }));
    EXPECT("FIDEDUPERANGE with an unaligned offset is EINVAL for that destination", ({
        struct dedupe q;
        memset(&q, 0, sizeof q);
        q.src_offset = 1; q.src_length = 4096; q.dest_count = 1;
        q.info[0].dest_fd = d;
        ioctl(s, FIDEDUPERANGE, &q) == 0 && q.info[0].status == -EINVAL;
    }));
    EXPECT("FIDEDUPERANGE with a reserved field set is EINVAL", ({
        struct dedupe q;
        memset(&q, 0, sizeof q);
        q.src_length = 4096; q.dest_count = 1; q.reserved1 = 1;
        q.info[0].dest_fd = d;
        ioctl(s, FIDEDUPERANGE, &q) == -1 && errno == EINVAL;
    }));
    EXPECT("FIDEDUPERANGE with no destinations does nothing", ({
        struct dedupe q;
        memset(&q, 0, sizeof q);
        q.src_length = 4096;
        ioctl(s, FIDEDUPERANGE, &q) == 0;
    }));
    EXPECT("an ordinary ioctl number is still ENOTTY on a file", ioctl(s, _IOW(0x94, 99, int), 0) == -1 && errno == ENOTTY);
    close(s); close(d);
    unlink("/tmp/rl_src"); unlink("/tmp/rl_dst"); unlink("/tmp/rl_t");
    return done();
}
