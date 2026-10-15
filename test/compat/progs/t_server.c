/* What event-driven servers such as nginx do with their sockets: turn on
 * O_NONBLOCK with fcntl or ioctl(FIONBIO), send until the connection is
 * full and wait for room with epoll, hand files to the connection with
 * sendfile, set FIOASYNC and close-on-exec with ioctl, and sit in
 * epoll_wait and sigsuspend between events. */
#include "compat.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>

static volatile int got;
static void h(int s) { got = s; }

static long ms_since(struct timespec *t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* A connected pair of TCP sockets over the loopback: *c the client's end,
 * *s the server's. */
static int tcp_pair(int *c, int *s) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    int one = 1, l = socket(AF_INET, SOCK_STREAM, 0);
    if (l < 0 || setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) || bind(l, (void *)&a, sizeof a) ||
        listen(l, 4) || getsockname(l, (void *)&a, &al))
        return 0;
    *c = socket(AF_INET, SOCK_STREAM, 0);
    if (*c < 0 || connect(*c, (void *)&a, sizeof a)) return 0;
    *s = accept(l, 0, 0);
    close(l);
    return *s >= 0;
}

/* Bytes in order 0, 1, ... 250, 0, 1, ...: what the file holds. */
static int pattern(int i) { return i % 251; }

int main(void) {
    setup();
    signal(SIGPIPE, SIG_IGN);
    int c = -1, s = -1, sv[2], on = 1, off = 0;
    char b[4096];

    EXPECT("a TCP connection over the loopback", tcp_pair(&c, &s));
    EXPECT("F_SETFL O_NONBLOCK on a TCP socket shows in F_GETFL", fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) == 0 &&
           (fcntl(s, F_GETFL) & O_NONBLOCK));
    EXPECT("and makes an empty read EAGAIN", read(s, b, sizeof b) == -1 && errno == EAGAIN);
    EXPECT("ioctl FIONBIO 0 turns it off", ioctl(s, FIONBIO, &off) == 0 && !(fcntl(s, F_GETFL) & O_NONBLOCK));
    EXPECT("ioctl FIONBIO 1 turns it on", ioctl(s, FIONBIO, &on) == 0 && (fcntl(s, F_GETFL) & O_NONBLOCK) &&
           recv(s, b, sizeof b, 0) == -1 && errno == EAGAIN);
    EXPECT("FIONBIO on an AF_UNIX socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && ioctl(sv[0], FIONBIO, &on) == 0 &&
           (fcntl(sv[0], F_GETFL) & O_NONBLOCK) && read(sv[0], b, 1) == -1 && errno == EAGAIN);
    EXPECT("FIOASYNC is accepted", ioctl(sv[0], FIOASYNC, &on) == 0 && ioctl(sv[0], FIOASYNC, &off) == 0);
    EXPECT("FIOCLEX and FIONCLEX set and clear close-on-exec", ioctl(sv[1], FIOCLEX) == 0 && fcntl(sv[1], F_GETFD) == FD_CLOEXEC &&
           ioctl(sv[1], FIONCLEX) == 0 && fcntl(sv[1], F_GETFD) == 0);
    close(sv[0]); close(sv[1]);

    // Nobody reads the client's end: sending fills the connection, and a
    // nonblocking send then says EAGAIN rather than waiting. (Room can come
    // back while the peer still takes in what is in flight, so this sends
    // until poll says no room is coming.)
    long sent = 0;
    EXPECT("a nonblocking send fills the connection, then is EAGAIN", ({
           int r = 0, again = 1, rounds = 0; struct pollfd p = {s, POLLOUT, 0};
           do { while ((r = send(s, b, sizeof b, 0)) > 0) sent += r; again = again && r == -1 && errno == EAGAIN; }
           while (again && poll(&p, 1, 100) == 1 && ++rounds < 1000);
           again && sent > 0 && rounds < 1000; }));
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLOUT | EPOLLET, .data.fd = s}, out;
    EXPECT("epoll_wait for room times out while the peer does not read", ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev) == 0 &&
           epoll_wait(ep, &out, 1, 50) == 0);
    long drained = 0;
    EXPECT("reading the other end makes room: EPOLLOUT", ({
           int r; while (drained < sent && (r = read(c, b, sizeof b)) > 0) drained += r;
           drained == sent && epoll_wait(ep, &out, 1, 1000) == 1 && (out.events & EPOLLOUT); }));
    close(ep);
    close(c); close(s);

    // sendfile from a file to a connection, as nginx serves a file.
    const char *path = "t_server.dat";
    enum { SIZE = 300000 };
    int f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT("a file to serve", f >= 0 && ({
           int ok = 1; for (int i = 0; i < SIZE && ok; i += sizeof b) {
               int n = SIZE - i < (int)sizeof b ? SIZE - i : (int)sizeof b;
               for (int j = 0; j < n; j++) b[j] = pattern(i + j);
               ok = write(f, b, n) == n; }
           ok; }));
    EXPECT("sendfile to a TCP connection moves the whole file, in order", tcp_pair(&c, &s) && ({
           int p[2]; pipe(p); pid_t k = fork();
           if (k == 0) {                     // the client: count what arrives, check every byte
               close(s); long n = 0, bad = 0; int r; char x[4096];
               while ((r = read(c, x, sizeof x)) > 0) { for (int j = 0; j < r; j++) bad += (unsigned char)x[j] != pattern(n + j); n += r; }
               long res[2] = {n, bad}; write(p[1], res, sizeof res); _exit(0);
           }
           close(c);
           off_t at = 0; long total = 0; ssize_t r;
           while (total < SIZE && (r = sendfile(s, f, &at, SIZE - total)) > 0) total += r;
           close(s);
           long res[2] = {-1, -1}; read(p[0], res, sizeof res); waitpid(k, 0, 0); close(p[0]); close(p[1]);
           total == SIZE && at == SIZE && res[0] == SIZE && res[1] == 0; }));
    // (Linux's loopback takes the whole file; BastiOS's 256 KiB rings may not.)
    EXPECT("sendfile to a full nonblocking connection is EAGAIN after what fits", tcp_pair(&c, &s) &&
           ioctl(s, FIONBIO, &on) == 0 && ({
           off_t at = 0; ssize_t r; long total = 0;
           while ((r = sendfile(s, f, &at, SIZE - total)) > 0) total += r;
           ((r == -1 && errno == EAGAIN) || (r == 0 && total == SIZE)) && total > 0 && at == total; }));
    close(c); close(s);
    close(f); unlink(path);

    EXPECT("PR_SET_DUMPABLE and PR_GET_DUMPABLE", prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == 0 && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 1);

    // The waits a server sits in: they end on time, on input from another
    // process, and on a signal.
    struct timespec t0;
    EXPECT("epoll_wait with nothing ready ends at its timeout", ({
           int e = epoll_create1(0), p[2]; pipe(p); struct epoll_event pe = {.events = EPOLLIN};
           epoll_ctl(e, EPOLL_CTL_ADD, p[0], &pe); clock_gettime(CLOCK_MONOTONIC, &t0);
           int r = epoll_wait(e, &pe, 1, 60); long ms = ms_since(&t0);
           close(e); close(p[0]); close(p[1]); r == 0 && ms >= 50 && ms < 1000; }));
    EXPECT("poll with nothing ready ends at its timeout", ({
           int p[2]; pipe(p); struct pollfd pf = {p[0], POLLIN, 0}; clock_gettime(CLOCK_MONOTONIC, &t0);
           int r = poll(&pf, 1, 60); long ms = ms_since(&t0);
           close(p[0]); close(p[1]); r == 0 && ms >= 50 && ms < 1000; }));
    EXPECT("epoll_wait wakes for a pipe another process writes", ({
           int e = epoll_create1(0), p[2]; pipe(p); struct epoll_event pe = {.events = EPOLLIN};
           epoll_ctl(e, EPOLL_CTL_ADD, p[0], &pe);
           pid_t k = fork(); if (k == 0) { usleep(100000); write(p[1], "x", 1); _exit(0); }
           clock_gettime(CLOCK_MONOTONIC, &t0);
           int r = epoll_wait(e, &pe, 1, 5000); long ms = ms_since(&t0); waitpid(k, 0, 0);
           close(e); close(p[0]); close(p[1]); r == 1 && ms < 2000; }));
    EXPECT("sigsuspend ends when another process signals", ({
           signal(SIGUSR1, h); sigset_t m, old; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, &old);
           pid_t me = getpid(), k = fork(); if (k == 0) { usleep(100000); kill(me, SIGUSR1); _exit(0); }
           got = 0; sigset_t none; sigemptyset(&none);
           int r = sigsuspend(&none); int e = errno; waitpid(k, 0, 0); sigprocmask(SIG_SETMASK, &old, 0);
           r == -1 && e == EINTR && got == SIGUSR1; }));
    EXPECT("FIONREAD on a TCP socket counts the bytes waiting", ({
           int c2, s2; tcp_pair(&c2, &s2); int n = -1;
           write(c2, "hello", 5);
           for (int i = 0; i < 100 && n != 5; i++) { n = -1; ioctl(s2, FIONREAD, &n); if (n != 5) usleep(10000); }
           int r = n == 5; read(s2, b, 5); n = -1; ioctl(s2, FIONREAD, &n);
           close(c2); close(s2); r && n == 0; }));
    EXPECT("a nonblocking connect to a port nobody listens on ends in ECONNREFUSED (SO_ERROR)", ({
           struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
           socklen_t al = sizeof a; int l = socket(AF_INET, SOCK_STREAM, 0);
           bind(l, (void *)&a, sizeof a); getsockname(l, (void *)&a, &al); close(l);
           int f = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
           int r = connect(f, (void *)&a, sizeof a), ok;
           if (r == 0) ok = 0;
           else if (errno == ECONNREFUSED) ok = 1;
           else {
               struct pollfd pf = {f, POLLOUT, 0}; int err = -1; socklen_t el = sizeof err;
               ok = errno == EINPROGRESS && poll(&pf, 1, 2000) == 1 &&
                    getsockopt(f, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == ECONNREFUSED;
           }
           close(f); ok; }));
    EXPECT("an IPv6-only listener and an IPv4 one share a port number", ({
           struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
           socklen_t al = sizeof a; int one = 1;
           int l4 = socket(AF_INET, SOCK_STREAM, 0);
           int ok = bind(l4, (void *)&a, sizeof a) == 0 && listen(l4, 4) == 0 && getsockname(l4, (void *)&a, &al) == 0;
           int l6 = socket(AF_INET6, SOCK_STREAM, 0);
           struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_ANY_INIT, .sin6_port = a.sin_port};
           ok = ok && l6 >= 0 && setsockopt(l6, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one) == 0 &&
                bind(l6, (void *)&a6, sizeof a6) == 0 && listen(l6, 4) == 0;
           int nov6 = l6 < 0 && errno == EAFNOSUPPORT;   /* a host without IPv6 */
           close(l4); if (l6 >= 0) close(l6); ok || nov6; }));
    return done();
}
