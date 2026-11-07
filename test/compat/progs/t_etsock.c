/* Edge-triggered epoll on loopback TCP, as the Dart VM's event handler uses
 * it: sockets registered with EPOLLET and EPOLLRDHUP, the interest changed
 * with EPOLL_CTL_MOD, and a peer that writes and closes at once. */
#include "compat.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int pair(int *srv) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t n = sizeof a;
    if (bind(l, (void *)&a, sizeof a) || listen(l, 4) || getsockname(l, (void *)&a, &n)) return -1;
    int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (connect(c, (void *)&a, sizeof a) && errno != EINPROGRESS) return -1;
    *srv = accept(l, 0, 0);
    close(l);
    return c;
}

/* The events of fd that include `want`, waiting up to `ms` for them. */
static int waitfor(int ep, int fd, unsigned want, int ms) {
    struct epoll_event e[4];
    for (int left = ms; left > 0; left -= 10) {
        int n = epoll_wait(ep, e, 4, 10);
        for (int i = 0; i < n; i++)
            if (e[i].data.fd == fd && (e[i].events & want)) return e[i].events;
    }
    return 0;
}

/* Data and a FIN arrive after the client registered its socket. */
static int data_then_fin(unsigned mask) {
    int s, c = pair(&s);
    if (c < 0 || s < 0) return 0;
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = mask, .data.fd = c};
    epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);
    waitfor(ep, c, EPOLLOUT, 1000);                    /* connected */
    write(s, "echo\n", 5);
    close(s);                                           /* write, then close at once */
    int got = waitfor(ep, c, EPOLLIN, 2000);
    char b[16];
    int r = read(c, b, sizeof b);
    close(c); close(ep);
    return (got & EPOLLIN) && r == 5;
}

/* The data is already there when the interest is changed to include EPOLLIN:
 * a modification of an edge-triggered descriptor reports what is ready. */
static int mod_reports_ready(int fin) {
    int s, c = pair(&s);
    if (c < 0 || s < 0) return 0;
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLOUT | EPOLLRDHUP | EPOLLET, .data.fd = c};
    epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);
    waitfor(ep, c, EPOLLOUT, 1000);
    write(s, "echo\n", 5);
    if (fin) close(s);
    usleep(100000);
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    epoll_ctl(ep, EPOLL_CTL_MOD, c, &ev);
    int got = waitfor(ep, c, EPOLLIN, 2000);
    if (!fin) close(s);
    close(c); close(ep);
    return (got & EPOLLIN) != 0;
}

/* A thread already asleep in epoll_wait when the peer's data and FIN arrive
 * is woken, however the interest was set (the Dart VM's server and client
 * share one event loop, so the client's wait begins before the echo). */
static int asleep_then_data(int via_mod) {
    int s, c = pair(&s);
    if (c < 0 || s < 0) return 0;
    int ep = epoll_create1(0);
    unsigned first = via_mod ? EPOLLOUT : EPOLLIN;
    struct epoll_event ev = {.events = first | EPOLLRDHUP | EPOLLET, .data.fd = c};
    epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);
    waitfor(ep, c, EPOLLOUT, 1000);
    if (via_mod) {
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        epoll_ctl(ep, EPOLL_CTL_MOD, c, &ev);
    }
    pid_t k = fork();
    if (k == 0) {
        usleep(200000);
        write(s, "echo\n", 5);
        close(s);
        _exit(0);
    }
    close(s);
    struct epoll_event out[4];
    int n = epoll_wait(ep, out, 4, 5000);
    int ok = 0;
    for (int i = 0; i < n; i++) ok |= (out[i].events & EPOLLIN) != 0;
    waitpid(k, 0, 0);
    close(c); close(ep);
    return ok;
}

/* shutdown(SHUT_WR) right after a write, the socket itself kept open (the
 * Dart VM's Socket.close() on a connection it is still reading): the data
 * still arrives, then the end of file. */
static int write_then_shutdown(void) {
    int s, c = pair(&s);
    if (c < 0 || s < 0) return 0;
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP | EPOLLET, .data.fd = c};
    epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);
    write(s, "echo\n", 5);
    shutdown(s, SHUT_WR);
    int got = waitfor(ep, c, EPOLLIN, 2000);
    char b[16];
    int r = read(c, b, sizeof b);
    int r2 = -2;
    for (int i = 0; i < 100 && (r2 = read(c, b, sizeof b)) < 0 && errno == EAGAIN; i++) usleep(10000);
    close(s); close(c); close(ep);
    return got && r == 5 && r2 == 0;
}

/* An edge is reported once: a connected socket that stays writable, or has
 * unread data nobody has taken, is not reported again until something new
 * happens to it (Linux; the Dart VM counts events against a token budget
 * and stops listening to a socket that floods it). */
static int edge_once(void) {
    int s, c = pair(&s);
    if (c < 0 || s < 0) return 0;
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data.fd = c}, out[4];
    epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);
    int ok = waitfor(ep, c, EPOLLOUT, 1000) != 0;            /* the first edge: connected */
    ok &= epoll_wait(ep, out, 4, 100) == 0;                  /* nothing new: silence */
    write(s, "echo\n", 5);
    ok &= (waitfor(ep, c, EPOLLIN, 1000) & EPOLLIN) != 0;    /* data: an edge */
    ok &= epoll_wait(ep, out, 4, 100) == 0;                  /* still unread, nothing new */
    write(s, "more\n", 5);
    ok &= (waitfor(ep, c, EPOLLIN, 1000) & EPOLLIN) != 0;    /* more data: another edge */
    close(s); close(c); close(ep);
    return ok;
}

/* FIONREAD (SIOCINQ) on a TCP connection: the bytes waiting to be read (the
 * Dart VM sizes its reads by it and reads one byte at a time when it fails). */
static int tcp_fionread(void) {
    int s, c = pair(&s);
    if (c < 0 || s < 0) return 0;
    int n = -1;
    int ok = ioctl(c, FIONREAD, &n) == 0 && n == 0;
    write(s, "hello", 5);
    for (int i = 0; i < 100 && ioctl(c, FIONREAD, &n) == 0 && n != 5; i++) usleep(10000);
    ok &= n == 5;
    char b[8];
    read(c, b, 2);
    ok &= ioctl(c, FIONREAD, &n) == 0 && n == 3;
    close(s); close(c);
    return ok;
}

/* The same on a unix stream socket. */
static int unix_fionread(void) {
    int sv[2], n = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return 0;
    int ok = ioctl(sv[1], FIONREAD, &n) == 0 && n == 0;
    write(sv[0], "abcd", 4);
    ok &= ioctl(sv[1], FIONREAD, &n) == 0 && n == 4;
    close(sv[0]); close(sv[1]);
    return ok;
}

/* A listening socket has nothing to count. */
static int listener_fionread(void) {
    int l = socket(AF_INET, SOCK_STREAM, 0), n = -1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(l, (void *)&a, sizeof a);
    listen(l, 1);
    int r = ioctl(l, FIONREAD, &n);
    int e = errno;
    close(l);
    return r == -1 && e == EINVAL;
}

int main(void) {
    setup();
    EXPECT("EPOLLIN arrives for data followed by a close (edge-triggered, with EPOLLRDHUP)", data_then_fin(EPOLLIN | EPOLLRDHUP | EPOLLOUT | EPOLLET));
    EXPECT("EPOLLIN arrives for data followed by a close (level-triggered)", data_then_fin(EPOLLIN));
    EXPECT("EPOLL_CTL_MOD of an edge-triggered socket reports data that is already there", mod_reports_ready(0));
    EXPECT("the same when the peer has closed after writing", mod_reports_ready(1));
    EXPECT("a thread asleep in epoll_wait is woken by data and a close (edge-triggered)", asleep_then_data(0));
    EXPECT("the same when the interest was changed with EPOLL_CTL_MOD", asleep_then_data(1));
    EXPECT("data written before shutdown(SHUT_WR) arrives, then the end of file", write_then_shutdown());
    EXPECT("an edge-triggered TCP socket is reported once per edge, not on every wait", edge_once());
    EXPECT("FIONREAD on a TCP connection counts the bytes waiting", tcp_fionread());
    EXPECT("FIONREAD on a unix stream socket counts the bytes waiting", unix_fionread());
    EXPECT("FIONREAD on a listening socket fails with EINVAL", listener_fionread());
    return done();
}
