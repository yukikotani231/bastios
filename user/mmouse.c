/* /dev/input/mice, against musl. Opened O_NONBLOCK with no motion yet, a
 * read fails with EAGAIN; poll says nothing is waiting. Then it says
 * "mmouse: ready", reads two 3-byte packets with blocking reads and prints
 * each as buttons, dx and dy (up positive). Without a mouse, opening the
 * device fails with ENODEV, which it reports (exit status 2).
 * test/shell_test.sh moves the mouse and presses a button through QEMU's
 * monitor and checks what it prints. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
    if (fd < 0 && errno == ENODEV) {
        puts("mmouse: no mouse (ENODEV)");
        return 2;
    }
    if (fd < 0) {
        printf("mmouse: open: errno %d\n", errno);
        return 1;
    }
    unsigned char p[3];
    struct pollfd pf = { .fd = fd, .events = POLLIN };
    if (read(fd, p, 3) != -1 || errno != EAGAIN || poll(&pf, 1, 0) != 0) {
        printf("mmouse: expected nothing waiting (errno %d)\n", errno);
        return 1;
    }
    close(fd);
    fd = open("/dev/psaux", O_RDONLY);
    if (fd < 0) {
        printf("mmouse: open /dev/psaux: errno %d\n", errno);
        return 1;
    }
    puts("mmouse: ready");
    fflush(stdout);
    for (int i = 0; i < 2; i++) {
        size_t got = 0;
        while (got < 3) {
            ssize_t n = read(fd, p + got, 3 - got);
            if (n <= 0) {
                printf("mmouse: read: %zd, errno %d\n", n, errno);
                return 1;
            }
            got += n;
        }
        if (!(p[0] & 8)) {
            printf("mmouse: bad packet %02x %02x %02x\n", p[0], p[1], p[2]);
            return 1;
        }
        int dx = p[0] & 0x10 ? p[1] - 256 : p[1];
        int dy = p[0] & 0x20 ? p[2] - 256 : p[2];
        printf("mmouse: buttons %d dx %d dy %d\n", p[0] & 7, dx, dy);
    }
    close(fd);
    return 0;
}
