/* The USB serial port (/dev/ttyUSB0, also /dev/ttyACM0), against musl.
 * Opened O_NONBLOCK with nothing received yet, a read fails with EAGAIN
 * and poll says the port is writable only. Its termios can be read and set.
 * Then it writes "ping\n", says "mserial: ready", and reads the line the
 * host answers with; reads 1500 bytes the host sends in pieces (a pattern,
 * which it sums); and writes 2000 bytes of a pattern, more than the ring
 * holds, so a write waits for room. Without a port, opening the device fails
 * with ENODEV, which it reports (exit status 2).
 * test/shell_test.sh is the other end: a QEMU usb-serial on a socket. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int writeall(int fd, const unsigned char *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w <= 0) {
            printf("mserial: write: %zd, errno %d\n", w, errno);
            return -1;
        }
        done += w;
    }
    return 0;
}

int main(void)
{
    int fd = open("/dev/ttyUSB0", O_RDWR | O_NONBLOCK);
    if (fd < 0 && errno == ENODEV) {
        puts("mserial: no serial port (ENODEV)");
        return 2;
    }
    if (fd < 0) {
        printf("mserial: open: errno %d\n", errno);
        return 1;
    }
    unsigned char buf[2048];
    struct pollfd pf = { .fd = fd, .events = POLLIN | POLLOUT };
    if (read(fd, buf, sizeof buf) != -1 || errno != EAGAIN) {
        printf("mserial: expected nothing waiting (errno %d)\n", errno);
        return 1;
    }
    if (poll(&pf, 1, 0) != 1 || pf.revents != POLLOUT) {
        printf("mserial: poll said %x, not writable only\n", pf.revents);
        return 1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) < 0 || (t.c_cflag & CSIZE) != CS8) {
        printf("mserial: tcgetattr: errno %d, cflag %x\n", errno, t.c_cflag);
        return 1;
    }
    cfmakeraw(&t);
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        printf("mserial: tcsetattr: errno %d\n", errno);
        return 1;
    }
    close(fd);
    fd = open("/dev/ttyACM0", O_RDWR);
    if (fd < 0) {
        printf("mserial: open /dev/ttyACM0: errno %d\n", errno);
        return 1;
    }
    if (writeall(fd, (const unsigned char *)"ping\n", 5) < 0)
        return 1;
    puts("mserial: ready");
    fflush(stdout);
    char line[64];
    size_t n = 0;
    while (n < sizeof line - 1) {
        if (read(fd, line + n, 1) != 1) {
            printf("mserial: read: errno %d\n", errno);
            return 1;
        }
        if (line[n++] == '\n')
            break;
    }
    line[n - 1] = 0;
    printf("mserial: got %s\n", line);
    puts("mserial: rx ready");
    fflush(stdout);
    size_t got = 0;
    unsigned sum = 0;
    while (got < 1500) {
        ssize_t r = read(fd, buf, sizeof buf < 1500 - got ? sizeof buf : 1500 - got);
        if (r <= 0) {
            printf("mserial: read: %zd, errno %d\n", r, errno);
            return 1;
        }
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] != (unsigned char)((got + i) * 7 + 3)) {
                printf("mserial: byte %zu is %d, expected %d\n", got + i, buf[i], (unsigned char)((got + i) * 7 + 3));
                return 1;
            }
            sum += buf[i];
        }
        got += r;
    }
    printf("mserial: rx %zu bytes, sum %u\n", got, sum);
    fflush(stdout);
    for (int i = 0; i < 2000; i++)
        buf[i] = (unsigned char)(i * 5 + 1);
    if (writeall(fd, buf, 2000) < 0)
        return 1;
    puts("mserial: tx done");
    close(fd);
    return 0;
}
