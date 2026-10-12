/* A program that reads the terminal the way libuv does (Neovim's event
 * loop): it opens /dev/tty again, puts it in raw mode and in O_NONBLOCK,
 * and then waits for keys in epoll_wait with no timeout. It writes each
 * key it reads to the file named by its argument, until it reads a 'q'.
 * Used by test/compat/tty.txt. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    struct termios t;
    if (fd < 0 || tcgetattr(fd, &t)) return 3;
    cfmakeraw(&t);
    if (tcsetattr(fd, TCSADRAIN, &t)) return 3;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev)) return 4;
    FILE *out = fopen(argv[1], "w");
    if (!out) return 5;
    write(fd, "ready\r\n", 7);
    for (;;) {
        struct epoll_event got;
        if (epoll_wait(ep, &got, 1, -1) != 1) continue;
        char c;
        while (read(fd, &c, 1) == 1) {
            fputc(c, out);
            if (c == 'q') {
                fclose(out);
                return 0;
            }
        }
    }
}
