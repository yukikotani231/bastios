/* /dev/input/mice, against musl. Opened O_NONBLOCK with no motion yet, a
 * read fails with EAGAIN; poll says nothing is waiting. Then it says
 * "mmouse: ready", reads two 3-byte packets with blocking reads and prints
 * each as buttons, dx and dy (up positive). Then it switches the device to
 * ImPS/2 with the IntelliMouse sequence, checks the answers (0xFA, then
 * id 3), says "mmouse: wheel ready" and reads 4-byte packets until one
 * moves the wheel, which it prints (up negative). Then it switches to
 * ImExPS/2 (sample rates 200, 200, 80), checks that the id is 4, says
 * "mmouse: imex ready" and reads packets until one has a side button down,
 * printing the buttons in bits 4 and 5. Without a mouse, opening the
 * device fails with ENODEV, which it reports (exit status 2).
 * test/shell_test.sh moves the mouse, presses a button and turns the wheel
 * through QEMU's monitor and checks what it prints. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

static int readn(int fd, unsigned char *p, size_t want)
{
    size_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, p + got, want - got);
        if (n <= 0) {
            printf("mmouse: read: %zd, errno %d\n", n, errno);
            return -1;
        }
        got += n;
    }
    return 0;
}

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
    unsigned char p[4];
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
        if (readn(fd, p, 3) < 0)
            return 1;
        if (!(p[0] & 8)) {
            printf("mmouse: bad packet %02x %02x %02x\n", p[0], p[1], p[2]);
            return 1;
        }
        int dx = p[0] & 0x10 ? p[1] - 256 : p[1];
        int dy = p[0] & 0x20 ? p[2] - 256 : p[2];
        printf("mmouse: buttons %d dx %d dy %d\n", p[0] & 7, dx, dy);
    }
    /* Only the answer to the last byte written is kept, as in Linux. */
    static const unsigned char imps[] = { 0xF3, 200, 0xF3, 100, 0xF3, 80 };
    if (write(fd, imps, sizeof imps) != sizeof imps || readn(fd, p, 1) < 0 || p[0] != 0xFA) {
        printf("mmouse: no answer to the IntelliMouse sequence (%02x)\n", p[0]);
        return 1;
    }
    if (write(fd, "\xF2", 1) != 1 || readn(fd, p, 2) < 0 || p[0] != 0xFA || p[1] != 3) {
        printf("mmouse: id %02x %02x, not 3\n", p[0], p[1]);
        return 1;
    }
    puts("mmouse: wheel ready");
    fflush(stdout);
    for (int i = 0; i < 8; i++) {
        if (readn(fd, p, 4) < 0)
            return 1;
        if (p[3] != 0) {
            printf("mmouse: wheel %d\n", (signed char)p[3]);
            break;
        }
    }
    static const unsigned char imex[] = { 0xF3, 200, 0xF3, 200, 0xF3, 80 };
    if (write(fd, imex, sizeof imex) != sizeof imex || readn(fd, p, 1) < 0 || p[0] != 0xFA) {
        printf("mmouse: no answer to the ImExPS/2 sequence (%02x)\n", p[0]);
        return 1;
    }
    if (write(fd, "\xF2", 1) != 1 || readn(fd, p, 2) < 0 || p[0] != 0xFA || p[1] != 4) {
        printf("mmouse: id %02x %02x, not 4\n", p[0], p[1]);
        return 1;
    }
    puts("mmouse: imex ready");
    fflush(stdout);
    for (int i = 0; i < 8; i++) {
        if (readn(fd, p, 4) < 0)
            return 1;
        if (p[3] & 0x30) {
            printf("mmouse: imex buttons %d wheel %d\n", p[3] >> 4 & 3, p[3] & 8 ? (p[3] & 15) - 16 : p[3] & 15);
            break;
        }
    }
    close(fd);
    return 0;
}
