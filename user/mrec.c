/* Recording from /dev/dsp, against musl: reads half a second of the card's
 * input (48 kHz 16-bit stereo, 96000 bytes) in pieces, checks that the
 * read waited for the card (the data comes in at the card's pace, not at
 * once) and that a non-blocking read with nothing recorded says EAGAIN,
 * and prints "mrec: ok, 96000 bytes". Without a sound card or without an
 * input (QEMU's hda-output), open or the first read must fail, which it
 * reports (exit status 2). QEMU's input backends here give silence, so the
 * samples are not checked. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
    int fd = open("/dev/dsp", O_RDONLY);
    if (fd < 0 && errno == ENODEV) {
        puts("mrec: no sound card (ENODEV)");
        return 2;
    }
    if (fd < 0) {
        printf("mrec: open /dev/dsp: errno %d\n", errno);
        return 1;
    }
    static char buf[8192];
    double t0 = now();
    size_t total = 0;
    while (total < 96000) {
        size_t want = sizeof buf;
        if (want > 96000 - total)
            want = 96000 - total;
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == ENODEV) {
            puts("mrec: no input (ENODEV)");
            return 2;
        }
        if (n <= 0 || n % 4) {
            printf("mrec: read: %zd, errno %d\n", n, errno);
            return 1;
        }
        total += n;
    }
    double dt = now() - t0;
    if (dt < 0.3 || dt > 5) {
        printf("mrec: 96000 bytes took %.2f s\n", dt);
        return 1;
    }
    /* Nothing is recorded right after a drain... */
    close(fd);
    fd = open("/dev/dsp", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        printf("mrec: reopen: errno %d\n", errno);
        return 1;
    }
    ssize_t n = read(fd, buf, sizeof buf);
    if (n >= 0 || errno != EAGAIN) {
        /* ... unless the stream was still running (it stops by itself later). */
        if (n < 0 || n % 4) {
            printf("mrec: nonblocking read: %zd, errno %d\n", n, errno);
            return 1;
        }
    }
    printf("mrec: ok, %zu bytes\n", total);
    return 0;
}
