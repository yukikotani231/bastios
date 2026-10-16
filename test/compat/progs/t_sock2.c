/* What servers and network tools do with sockets beyond connect and
 * accept: shutdown, MSG_PEEK and MSG_DONTWAIT, socket options, polling
 * sockets, nonblocking connect, sendmsg/recvmsg, passing descriptors over
 * AF_UNIX, and the signals they lean on. */
#include "compat.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int got, val;
static void h(int s, siginfo_t *i, void *c) { (void)c; got = s; val = i->si_value.sival_int; }

/* Stream socketpairs until one fails (64 fit), each carrying its own byte. */
static int many_pairs(void) {
    int sv[65][2], n = 0, ok = 1;
    for (int i = 0; i < 65; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) != 0) break;
        n++;
    }
    ok = n >= 56 && n < 65;
    for (int i = 0; ok && i < n; i++) ok = write(sv[i][0], &(char){'a' + i % 26}, 1) == 1;
    for (int i = 0; ok && i < n; i++) { char ch = 0; ok = read(sv[i][1], &ch, 1) == 1 && ch == 'a' + i % 26; }
    for (int i = 0; i < n; i++) { close(sv[i][0]); close(sv[i][1]); }
    return ok;
}

/* Listening sockets until a bind fails (32 fit), each with a connection through it. */
static int many_listeners(void) {
    int ls[33], cs[32], as[32], n = 0, ok = 1;
    char path[40];
    struct sockaddr_un u = {.sun_family = AF_UNIX};
    for (int i = 0; i < 33; i++) {
        ls[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        snprintf(u.sun_path, sizeof u.sun_path, "/tmp/ml%d", i);
        unlink(u.sun_path);
        if (ls[i] < 0 || bind(ls[i], (void *)&u, sizeof u) != 0 || listen(ls[i], 2) != 0) break;
        n++;
    }
    ok = n >= 28 && n < 33;
    for (int i = 0; ok && i < n; i++) {
        snprintf(u.sun_path, sizeof u.sun_path, "/tmp/ml%d", i);
        cs[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        ok = cs[i] >= 0 && connect(cs[i], (void *)&u, sizeof u) == 0 && (as[i] = accept(ls[i], 0, 0)) >= 0 &&
             write(cs[i], &(char){'A' + i % 26}, 1) == 1;
    }
    for (int i = 0; ok && i < n; i++) { char ch = 0; ok = read(as[i], &ch, 1) == 1 && ch == 'A' + i % 26; }
    for (int i = 0; i < 33; i++) {
        if (ls[i] >= 0) close(ls[i]);
        snprintf(path, sizeof path, "/tmp/ml%d", i);
        unlink(path);
    }
    return ok;
}

/* 24 bound datagram sockets send around a ring and see who sent each. */
static int many_datagrams(void) {
    int d[24], ok = 1;
    struct sockaddr_un u[24];
    for (int i = 0; i < 24; i++) {
        memset(&u[i], 0, sizeof u[i]);
        u[i].sun_family = AF_UNIX;
        snprintf(u[i].sun_path, sizeof u[i].sun_path, "/tmp/md%d", i);
        unlink(u[i].sun_path);
        d[i] = socket(AF_UNIX, SOCK_DGRAM, 0);
        ok = ok && d[i] >= 0 && bind(d[i], (void *)&u[i], sizeof u[i]) == 0;
    }
    for (int i = 0; ok && i < 24; i++) {
        char ch = 'a' + i;
        ok = sendto(d[i], &ch, 1, 0, (void *)&u[(i + 1) % 24], sizeof u[0]) == 1;
    }
    for (int i = 0; ok && i < 24; i++) {
        char ch = 0;
        struct sockaddr_un from;
        socklen_t fl = sizeof from;
        int from_i = (i + 23) % 24;
        ok = recvfrom(d[i], &ch, 1, 0, (void *)&from, &fl) == 1 && ch == 'a' + from_i && strcmp(from.sun_path, u[from_i].sun_path) == 0;
    }
    for (int i = 0; i < 24; i++) {
        if (d[i] >= 0) close(d[i]);
        unlink(u[i].sun_path);
    }
    return ok;
}

int main(void) {
    setup();
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    char b[64];
    int l = -1, c = -1, s = -1, sv[2], one = 1;

    EXPECT("listener", (l = socket(AF_INET, SOCK_STREAM, 0)) >= 0 && setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0 &&
           bind(l, (void *)&a, sizeof a) == 0 && listen(l, 4) == 0 && getsockname(l, (void *)&a, &al) == 0);
    EXPECT("poll a listener for a connection", (c = socket(AF_INET, SOCK_STREAM, 0)) >= 0 && connect(c, (void *)&a, sizeof a) == 0 &&
           ({ struct pollfd p = {l, POLLIN, 0}; poll(&p, 1, 1000) == 1 && (p.revents & POLLIN); }) && (s = accept(l, 0, 0)) >= 0);
    EXPECT("MSG_PEEK leaves the bytes", send(c, "peek", 4, 0) == 4 && recv(s, b, 4, MSG_PEEK) == 4 && recv(s, b, 4, 0) == 4 &&
           memcmp(b, "peek", 4) == 0);
    EXPECT("MSG_DONTWAIT on an empty socket is EAGAIN", recv(s, b, 4, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    EXPECT("sendmsg/recvmsg with two iovecs", ({ struct iovec v[2] = {{"ab", 2}, {"cd", 2}}; struct msghdr m = {.msg_iov = v, .msg_iovlen = 2};
           sendmsg(c, &m, 0) == 4; }) && ({ char x[2], y[2]; struct iovec v[2] = {{x, 2}, {y, 2}}; struct msghdr m = {.msg_iov = v, .msg_iovlen = 2};
           recvmsg(s, &m, 0) == 4 && memcmp(x, "ab", 2) == 0 && memcmp(y, "cd", 2) == 0; }));
    EXPECT("SO_RCVBUF and SO_SNDBUF", ({ int v = 0; socklen_t vl = sizeof v; getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &vl) == 0 && v > 0 &&
           (vl = sizeof v, getsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, &vl)) == 0 && v > 0; }));
    EXPECT("SO_KEEPALIVE", setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) == 0);
    EXPECT("SO_ERROR is 0", ({ int v = -1; socklen_t vl = sizeof v; getsockopt(c, SOL_SOCKET, SO_ERROR, &v, &vl) == 0 && v == 0; }));
    EXPECT("getpeername", ({ struct sockaddr_in p; socklen_t pl = sizeof p; getpeername(c, (void *)&p, &pl) == 0 && p.sin_port == a.sin_port; }));
    EXPECT("shutdown SHUT_WR gives the peer EOF", shutdown(c, SHUT_WR) == 0 && recv(s, b, sizeof b, 0) == 0);
    EXPECT("write after SHUT_WR is EPIPE", ({ signal(SIGPIPE, SIG_IGN); send(c, "x", 1, 0) == -1 && errno == EPIPE; }));
    close(c); close(s);
    EXPECT("nonblocking connect", (c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) >= 0 &&
           (connect(c, (void *)&a, sizeof a) == 0 || errno == EINPROGRESS) &&
           ({ struct pollfd p = {c, POLLOUT, 0}; poll(&p, 1, 1000) == 1 && (p.revents & POLLOUT); }) && (s = accept(l, 0, 0)) >= 0);
    EXPECT("poll sees a peer close", close(s) == 0 && ({ struct pollfd p = {c, POLLIN, 0}; poll(&p, 1, 1000) == 1; }) && read(c, b, 1) == 0);
    close(c); close(l);
    EXPECT("connect to a closed port is ECONNREFUSED", (c = socket(AF_INET, SOCK_STREAM, 0)) >= 0 &&
           connect(c, (void *)&a, sizeof a) == -1 && errno == ECONNREFUSED);
    close(c);
    EXPECT("a nonblocking connect to a closed port leaves ECONNREFUSED in SO_ERROR, once", (c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) >= 0 &&
           connect(c, (void *)&a, sizeof a) == -1 && (errno == EINPROGRESS || errno == ECONNREFUSED) &&
           ({ struct pollfd p = {c, POLLOUT, 0}; poll(&p, 1, 1000) == 1 && (p.revents & POLLERR); }) &&
           ({ int v = -1; socklen_t vl = sizeof v; getsockopt(c, SOL_SOCKET, SO_ERROR, &v, &vl) == 0 && v == ECONNREFUSED; }) &&
           ({ int v = -1; socklen_t vl = sizeof v; getsockopt(c, SOL_SOCKET, SO_ERROR, &v, &vl) == 0 && v == 0; }));
    close(c);
    EXPECT("SCM_RIGHTS passes a descriptor", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && ({
           int p[2]; pipe(p); char cb[CMSG_SPACE(sizeof(int))] = {0}; struct iovec v = {"f", 1};
           struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb};
           struct cmsghdr *h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
           memcpy(CMSG_DATA(h), &p[1], sizeof(int));
           int ok = sendmsg(sv[0], &m, 0) == 1;
           char rb[CMSG_SPACE(sizeof(int))]; char x; struct iovec rv = {&x, 1};
           struct msghdr r = {.msg_iov = &rv, .msg_iovlen = 1, .msg_control = rb, .msg_controllen = sizeof rb};
           int fd = -1;
           ok = ok && recvmsg(sv[1], &r, 0) == 1 && (h = CMSG_FIRSTHDR(&r)) && h->cmsg_type == SCM_RIGHTS && (memcpy(&fd, CMSG_DATA(h), sizeof fd), fd >= 0) &&
                write(fd, "z", 1) == 1 && read(p[0], &x, 1) == 1 && x == 'z';
           ok; }));
    EXPECT("SCM_RIGHTS passes several descriptors in one message", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && ({
           int p[3][2]; int fds[3]; for (int i = 0; i < 3; i++) { pipe(p[i]); fds[i] = p[i][1]; }
           char cb[CMSG_SPACE(3 * sizeof(int))] = {0}; struct iovec v = {"f", 1};
           struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb};
           struct cmsghdr *h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof fds);
           memcpy(CMSG_DATA(h), fds, sizeof fds);
           int ok = sendmsg(sv[0], &m, 0) == 1;
           char rb[CMSG_SPACE(3 * sizeof(int))]; char x; struct iovec rv = {&x, 1};
           struct msghdr r = {.msg_iov = &rv, .msg_iovlen = 1, .msg_control = rb, .msg_controllen = sizeof rb};
           int got[3] = {-1, -1, -1};
           ok = ok && recvmsg(sv[1], &r, 0) == 1 && (h = CMSG_FIRSTHDR(&r)) && h->cmsg_type == SCM_RIGHTS && h->cmsg_len == CMSG_LEN(sizeof got) &&
                !(r.msg_flags & MSG_CTRUNC) && (memcpy(got, CMSG_DATA(h), sizeof got), 1);
           for (int i = 0; i < 3; i++)
               ok = ok && got[i] >= 0 && write(got[i], "0123" + i, 1) == 1 && read(p[i][0], &x, 1) == 1 && x == "0123"[i];
           ok; }));
    EXPECT("SCM_RIGHTS with a short buffer closes the rest and says MSG_CTRUNC", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && ({
           int p[3][2]; int fds[3]; for (int i = 0; i < 3; i++) { pipe(p[i]); fds[i] = p[i][1]; }
           char cb[CMSG_SPACE(3 * sizeof(int))] = {0}; struct iovec v = {"f", 1};
           struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb};
           struct cmsghdr *h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof fds);
           memcpy(CMSG_DATA(h), fds, sizeof fds);
           int ok = sendmsg(sv[0], &m, 0) == 1;
           char rb[CMSG_SPACE(2 * sizeof(int))]; char x; struct iovec rv = {&x, 1};
           struct msghdr r = {.msg_iov = &rv, .msg_iovlen = 1, .msg_control = rb, .msg_controllen = sizeof rb};
           ok = ok && recvmsg(sv[1], &r, 0) == 1 && (r.msg_flags & MSG_CTRUNC) && (h = CMSG_FIRSTHDR(&r)) && h->cmsg_len == CMSG_LEN(2 * sizeof(int));
           ok; }));
    EXPECT("bytes sent before a message with descriptors are read first, and leave the descriptors for the recvmsg", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && ({
           int p[2]; pipe(p);
           char cb[CMSG_SPACE(sizeof(int))] = {0}; struct iovec v = {"c", 1};
           struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb};
           struct cmsghdr *h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
           memcpy(CMSG_DATA(h), &p[1], sizeof(int));
           char x[4];
           int ok = write(sv[0], "ab", 2) == 2 && sendmsg(sv[0], &m, 0) == 1 &&
                    read(sv[1], x, 1) == 1 && x[0] == 'a' &&      // a plain read before the message
                    read(sv[1], x, 3) == 1 && x[0] == 'b';        // and one that would run into it stops short
           char rb[CMSG_SPACE(sizeof(int))]; struct iovec rv = {x, 1};
           struct msghdr r = {.msg_iov = &rv, .msg_iovlen = 1, .msg_control = rb, .msg_controllen = sizeof rb};
           int fd = -1;
           ok = ok && recvmsg(sv[1], &r, 0) == 1 && x[0] == 'c' && (h = CMSG_FIRSTHDR(&r)) && h->cmsg_type == SCM_RIGHTS && (memcpy(&fd, CMSG_DATA(h), sizeof fd), fd >= 0) &&
                write(fd, "z", 1) == 1 && read(p[0], x, 1) == 1 && x[0] == 'z';
           ok; }));
    EXPECT("SCM_RIGHTS with a bad descriptor among several is EBADF", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && ({
           int p[2]; pipe(p); int fds[2] = {p[1], 999};
           char cb[CMSG_SPACE(2 * sizeof(int))] = {0}; struct iovec v = {"f", 1};
           struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb};
           struct cmsghdr *h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof fds);
           memcpy(CMSG_DATA(h), fds, sizeof fds);
           sendmsg(sv[0], &m, 0) == -1 && errno == EBADF; }));
    EXPECT("SCM_RIGHTS from two clients that sent and closed before the accept, and a child that closes the listener", ({
           struct sockaddr_un ua = {.sun_family = AF_UNIX}; strcpy(ua.sun_path, "/tmp/t_scm.sock"); unlink(ua.sun_path);
           int ls = socket(AF_UNIX, SOCK_STREAM, 0); int ok = ls >= 0 && bind(ls, (void *)&ua, sizeof ua) == 0 && listen(ls, 4) == 0;
           int cl[2], rp[2][2], wp[2][2];
           for (int i = 0; i < 2 && ok; i++) {
               cl[i] = socket(AF_UNIX, SOCK_STREAM, 0); ok = ok && connect(cl[i], (void *)&ua, sizeof ua) == 0;
               pipe(rp[i]); pipe(wp[i]);
               int fds[4] = {rp[i][0], wp[i][1], rp[i][1], wp[i][0]};
               char cb[CMSG_SPACE(sizeof fds)] = {0}; struct iovec v = {"f", 1};
               struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb};
               struct cmsghdr *h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof fds);
               memcpy(CMSG_DATA(h), fds, sizeof fds);
               ok = ok && sendmsg(cl[i], &m, 0) == 1;
               close(rp[i][0]); close(wp[i][1]); close(rp[i][1]); close(wp[i][0]);   // the message holds the only references
               close(cl[i]);
           }
           pid_t kid = fork();                 // a child that only closes the listener: the connections wait on
           if (kid == 0) { close(ls); _exit(0); }
           int kst; waitpid(kid, &kst, 0);
           for (int i = 0; i < 2 && ok; i++) {
               int a = accept(ls, 0, 0); char rb[CMSG_SPACE(4 * sizeof(int))]; char x; struct iovec rv = {&x, 1};
               struct msghdr r = {.msg_iov = &rv, .msg_iovlen = 1, .msg_control = rb, .msg_controllen = sizeof rb};
               int got[4] = {-1, -1, -1, -1}; struct cmsghdr *h;
               ok = ok && a >= 0 && recvmsg(a, &r, 0) == 1 && (h = CMSG_FIRSTHDR(&r)) && h->cmsg_len == CMSG_LEN(sizeof got) && (memcpy(got, CMSG_DATA(h), sizeof got), 1);
               ok = ok && write(got[1], "q", 1) == 1 && read(got[3], &x, 1) == 1 && x == 'q' && write(got[2], "r", 1) == 1 && read(got[0], &x, 1) == 1 && x == 'r';
               close(a);
           }
           ok; }));
    EXPECT("AF_UNIX SOCK_DGRAM socketpair", socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0 && send(sv[0], "dg", 2, 0) == 2 &&
           recv(sv[1], b, sizeof b, 0) == 2);
    {
        struct sigaction sa = {.sa_sigaction = h, .sa_flags = SA_SIGINFO};
        sigemptyset(&sa.sa_mask);
        sigaction(SIGRTMIN, &sa, 0);
        EXPECT("sigqueue carries a value", (got = 0, sigqueue(getpid(), SIGRTMIN, (union sigval){.sival_int = 42}) == 0) && got == SIGRTMIN && val == 42);
        EXPECT("SA_RESTART restarts a read", ({ int p[2]; pipe(p); pid_t k = fork();
               k == 0 ? (usleep(100000), kill(getppid(), SIGRTMIN), usleep(100000), write(p[1], "r", 1), _exit(0), 0) : 0;
               struct sigaction r = {.sa_sigaction = h, .sa_flags = SA_SIGINFO | SA_RESTART}; sigemptyset(&r.sa_mask); sigaction(SIGRTMIN, &r, 0);
               char x = 0; int n = read(p[0], &x, 1); int st; waitpid(k, &st, 0); n == 1 && x == 'r'; }));
        EXPECT("a read without SA_RESTART is EINTR", ({ int p[2]; pipe(p); sigaction(SIGRTMIN, &sa, 0); pid_t k = fork();
               k == 0 ? (usleep(100000), kill(getppid(), SIGRTMIN), _exit(0), 0) : 0;
               char x; int n = read(p[0], &x, 1); int e = errno; int st; waitpid(k, &st, 0); n == -1 && e == EINTR; }));
    }
    {
        sigset_t m; sigemptyset(&m); sigaddset(&m, SIGCHLD); sigaddset(&m, SIGUSR1);
        sigprocmask(SIG_BLOCK, &m, 0);
        int f = signalfd(-1, &m, SFD_CLOEXEC);
        struct signalfd_siginfo i;
        EXPECT("signalfd sees a blocked SIGCHLD", f >= 0 && ({ pid_t k = fork(); k == 0 ? (_exit(3), 0) : 0;
               struct pollfd p = {f, POLLIN, 0}; int st; poll(&p, 1, 2000) == 1 && read(f, &i, sizeof i) == sizeof i &&
               i.ssi_signo == SIGCHLD && (pid_t)i.ssi_pid == k && waitpid(k, &st, 0) == k; }));
        EXPECT("signalfd carries a sigqueue value", sigqueue(getpid(), SIGUSR1, (union sigval){.sival_int = 7}) == 0 &&
               read(f, &i, sizeof i) == sizeof i && i.ssi_signo == SIGUSR1 && i.ssi_code == SI_QUEUE && i.ssi_int == 7);
        close(f);
    }
    EXPECT("56 or more AF_UNIX socketpairs at once, each with its own byte; the 65th fails", many_pairs());
    EXPECT("at least 28 AF_UNIX listeners, each accepting a connection; the 33rd is refused", many_listeners());
    EXPECT("24 bound AF_UNIX datagram sockets send around a ring and name their senders", many_datagrams());
    return done();
}
