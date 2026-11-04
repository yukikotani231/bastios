/* sendfile and copy_file_range into a file on a full disk: they fail with
 * ENOSPC, as write(2) does, rather than return 0, which reads as the end
 * of the source (BusyBox tar copies a member's data with sendfile and said
 * "short read" for a full disk). Then, with the room back, both copy the
 * whole source. Takes the directory to work in: a disk with a few MiB
 * free, which it fills and empties again. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

#define SRC_BYTES (1 << 20)

static char path[4][256];
static char buf[65536];

static int fail(const char *what)
{
    printf("fullcopy: FAIL %s (errno %d)\n", what, errno);
    for (int i = 0; i < 4; i++)
        unlink(path[i]);
    return 1;
}

/* Copies from src to a new file `to` until the call returns 0 or fails:
 * the bytes copied, and in *last what the last call returned and *err its
 * errno. */
static long long copy_all(int src, const char *to, int cfr, long long *last, int *err)
{
    int out = open(to, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) {
        *last = -1;
        *err = errno;
        return 0;
    }
    long long done = 0;
    loff_t off = 0;
    for (;;) {
        errno = 0;
        ssize_t r = cfr ? copy_file_range(src, &off, out, NULL, SRC_BYTES, 0)
                        : sendfile(out, src, &off, SRC_BYTES);
        if (r <= 0) {
            *last = r;
            *err = errno;
            break;
        }
        done += r;
    }
    close(out);
    return done;
}

/* Does file p hold the source's bytes? */
static int same(const char *p)
{
    static char a[65536];
    int f = open(p, O_RDONLY), s = open(path[0], O_RDONLY);
    int ok = f >= 0 && s >= 0;
    long long total = 0;
    while (ok) {
        ssize_t n = read(s, buf, sizeof buf);
        if (n <= 0) {
            ok = n == 0 && total == SRC_BYTES && read(f, a, 1) == 0;
            break;
        }
        ok = read(f, a, n) == n && memcmp(a, buf, n) == 0;
        total += n;
    }
    close(f);
    close(s);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return fail("usage");
    const char *names[4] = {"fc_src", "fc_fill", "fc_out1", "fc_out2"};
    for (int i = 0; i < 4; i++)
        snprintf(path[i], sizeof path[i], "%s/%s", argv[1], names[i]);
    int src = open(path[0], O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (src < 0)
        return fail("open the source");
    for (int i = 0; i < SRC_BYTES; i += (int)sizeof buf) {
        for (size_t j = 0; j < sizeof buf; j++)
            buf[j] = (char)((i + j) * 7 + (i + j) / 4093);
        if (write(src, buf, sizeof buf) != (ssize_t)sizeof buf)
            return fail("write the source");
    }
    if (fsync(src))
        return fail("fsync the source");
    /* Fill the disk: whole blocks, then single bytes, until there is no room. */
    int fill = open(path[1], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fill < 0)
        return fail("open the filler");
    memset(buf, 'f', sizeof buf);
    long long filled = 0;
    ssize_t w;
    while ((w = write(fill, buf, 4096)) > 0)
        filled += w;
    if (w != -1 || errno != ENOSPC)
        return fail("a full disk's write is ENOSPC");
    while ((w = write(fill, buf, 1)) > 0)
        filled += w;
    if (w != -1 || errno != ENOSPC)
        return fail("a full disk's one-byte write is ENOSPC");
    fsync(fill);
    long long last;
    int err;
    copy_all(src, path[2], 0, &last, &err);
    if (last != -1 || err != ENOSPC)
        return fail("sendfile onto a full disk is ENOSPC, not 0");
    copy_all(src, path[3], 1, &last, &err);
    if (last != -1 || err != ENOSPC)
        return fail("copy_file_range onto a full disk is ENOSPC, not 0");
    /* Room again: both copy all of it. */
    close(fill);
    if (unlink(path[1]) || unlink(path[2]) || unlink(path[3]))
        return fail("unlink");
    sync();
    if (copy_all(src, path[2], 0, &last, &err) != SRC_BYTES || last != 0 || !same(path[2]))
        return fail("sendfile with room copies it all");
    if (copy_all(src, path[3], 1, &last, &err) != SRC_BYTES || last != 0 || !same(path[3]))
        return fail("copy_file_range with room copies it all");
    close(src);
    for (int i = 0; i < 4; i++)
        unlink(path[i]);
    printf("fullcopy: ok, %lld bytes filled the disk, sendfile and copy_file_range said ENOSPC\n", filled);
    return 0;
}
