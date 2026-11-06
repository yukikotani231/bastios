/* A TCP connection the peer reset (SO_LINGER 0, then close): a poll that
 * asks only for POLLOUT is woken with POLLOUT, POLLERR and POLLHUP (as
 * Linux), a write fails (EPIPE or ECONNRESET), and a read gives the error
 * once and then end of file. A connection the peer closed after sending data
 * still reads its data and is still writable. */
#define _GNU_SOURCE
#include "compat.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>

static int pair(int *srv, int *cli) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) return -1;
    socklen_t l = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &l);
    *cli = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(*cli, (struct sockaddr *)&a, sizeof a)) return -1;
    *srv = accept(ls, 0, 0);
    close(ls);
    return *srv >= 0 ? 0 : -1;
}

int main(void) {
    setup();
    signal(SIGPIPE, SIG_IGN);
    int s, c;
    EXPECT("a connected pair", pair(&s, &c) == 0);
    if (s < 0) return done();
    struct linger lg = {1, 0};
    setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(c);
    usleep(50000);
    struct pollfd p = {s, POLLOUT, 0};
    EXPECT("a poll for POLLOUT alone returns", poll(&p, 1, 2000) == 1);
    EXPECT("with POLLOUT, POLLERR and POLLHUP", (p.revents & (POLLOUT | POLLERR | POLLHUP)) == (POLLOUT | POLLERR | POLLHUP));
    char b[16] = "x";
    ssize_t w = write(s, b, 1);
    int we = errno;
    EXPECT("a write fails with EPIPE or ECONNRESET", w == -1 && (we == EPIPE || we == ECONNRESET));
    ssize_t r = read(s, b, sizeof b);
    int re = errno;
    EXPECT("a read gives the error or end of file", r == 0 || (r == -1 && re == ECONNRESET));
    close(s);

    EXPECT("another pair", pair(&s, &c) == 0);
    write(c, "bye", 3);
    close(c);
    usleep(50000);
    p.fd = s;
    p.events = POLLOUT;
    p.revents = 0;
    EXPECT("a peer that closed after sending: POLLOUT is still ready", poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT));
    EXPECT("its data is still read", read(s, b, sizeof b) == 3 && !memcmp(b, "bye", 3));
    EXPECT("then end of file", read(s, b, sizeof b) == 0);
    p.events = POLLRDHUP;
    p.revents = 0;
    EXPECT("a poll for POLLRDHUP sees the peer's end", poll(&p, 1, 2000) == 1 && (p.revents & POLLRDHUP));
    close(s);
    return done();
}
