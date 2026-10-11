/* Pipes, descriptor juggling, waiting on descriptors, local sockets,
 * event and timer descriptors. */
#include "compat.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>

int main(void) {
    setup();
    int p[2], q[2] = {-1, -1}, fd, st;
    char b[64];
    pid_t c;

    EXPECT("pipe", pipe(p) == 0 && write(p[1], "abc", 3) == 3 && read(p[0], b, 3) == 3 && memcmp(b, "abc", 3) == 0);
    EXPECT("pipe across fork", (c = fork()) >= 0 && (c == 0 ? (write(p[1], "kid", 3), _exit(0), 0) :
           read(p[0], b, 3) == 3 && memcmp(b, "kid", 3) == 0 && waitpid(c, &st, 0) == c));
    EXPECT("EOF once writers close", close(p[1]) == 0 && read(p[0], b, 1) == 0);
    close(p[0]);
    /* Made outside any check, so a rerun past a hang still has it. */
    int q2 = pipe2(q, O_NONBLOCK | O_CLOEXEC);
    EXPECT("pipe2", q2 == 0);
    EXPECT("O_NONBLOCK read of an empty pipe is EAGAIN", read(q[0], b, 1) == -1 && errno == EAGAIN);
    EXPECT("fcntl F_GETFD sees FD_CLOEXEC", fcntl(q[0], F_GETFD) == FD_CLOEXEC);
    EXPECT("fcntl F_GETFL sees O_NONBLOCK", (fcntl(q[0], F_GETFL) & O_NONBLOCK) != 0);
    EXPECT("fcntl F_DUPFD", (fd = fcntl(q[0], F_DUPFD, 10)) >= 10 && close(fd) == 0);
    EXPECT("fcntl F_SETFL", fcntl(q[0], F_SETFL, 0) == 0 && (fcntl(q[0], F_GETFL) & O_NONBLOCK) == 0);
    EXPECT("F_SETPIPE_SZ limits a pipe", ({ int s[2], ok = pipe2(s, O_NONBLOCK) == 0 && fcntl(s[1], F_GETPIPE_SZ) == 65536 &&
        fcntl(s[1], F_SETPIPE_SZ, 5000) == 8192 && fcntl(s[0], F_GETPIPE_SZ) == 8192; char z[8192] = {0};
        ok = ok && write(s[1], z, sizeof z) == 8192 && write(s[1], z, 1) == -1 && errno == EAGAIN &&
            fcntl(s[1], F_SETPIPE_SZ, 4096) == -1 && errno == EBUSY;
        close(s[0]); close(s[1]); ok; }));
    EXPECT("readv/writev check every iovec; 0 vectors is 0", ({ int s[2], ok = pipe(s) == 0; char z[4];
        struct iovec v[2] = {{z, 4}, {z, (size_t)-1}};
        ok = ok && writev(s[1], v, 2) == -1 && errno == EINVAL && writev(s[1], v, 0) == 0 && readv(s[0], v, 0) == 0 &&
            write(s[1], "abcd", 4) == 4 && readv(s[0], v, 2) == -1 && errno == EINVAL;
        close(s[0]); close(s[1]); ok; }));
    if (begin("pipe holds 64 KiB, then EAGAIN")) {
        int big = 0, n;
        char blk[4096] = {0};
        fcntl(q[1], F_SETFL, O_NONBLOCK);
        while (big < (1 << 20) && (n = write(q[1], blk, sizeof blk)) > 0) big += n;
        int e = errno;
        if (big >= 65536 && n == -1 && e == EAGAIN) pass();
        else fail("%d bytes went in, then write returned %d (errno %d)", big, n, e);
    }
    if (q[0] > 2) close(q[0]);
    if (q[1] > 2) close(q[1]);
    EXPECT("dup2 onto stdout and back", (fd = dup(1)) >= 0 && dup2(fd, 1) == 1 && close(fd) == 0);
    EXPECT("poll a readable pipe", pipe(p) == 0 && write(p[1], "x", 1) == 1 &&
           ({ struct pollfd f = {p[0], POLLIN, 0}; poll(&f, 1, 1000) == 1 && (f.revents & POLLIN); }));
    EXPECT("poll times out", ({ char c1; read(p[0], &c1, 1); struct pollfd f = {p[0], POLLIN, 0}; poll(&f, 1, 50) == 0; }));
    EXPECT("select", ({ fd_set r; FD_ZERO(&r); FD_SET(p[0], &r); write(p[1], "y", 1);
           struct timeval tv = {1, 0}; select(p[0] + 1, &r, 0, 0, &tv) == 1 && FD_ISSET(p[0], &r); }));
    EXPECT("pselect timeout", ({ char c1; read(p[0], &c1, 1); fd_set r; FD_ZERO(&r); FD_SET(p[0], &r);
           struct timespec ts = {0, 50000000}; pselect(p[0] + 1, &r, 0, 0, &ts, 0) == 0; }));
    {
        int ep;
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = p[0]}, out;
        EXPECT("epoll", (ep = epoll_create1(0)) >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0 && write(p[1], "z", 1) == 1 &&
               epoll_wait(ep, &out, 1, 1000) == 1 && out.data.fd == p[0]);
    }
    close(p[0]); close(p[1]);
    EXPECT("eventfd", (fd = eventfd(3, 0)) >= 0 && ({ uint64_t v; read(fd, &v, 8) == 8 && v == 3; }));
    EXPECT("timerfd", (fd = timerfd_create(CLOCK_MONOTONIC, 0)) >= 0 && ({ struct itimerspec t = {{0, 0}, {0, 20000000}};
           uint64_t n = 0; timerfd_settime(fd, 0, &t, 0) == 0 && read(fd, &n, 8) == 8 && n == 1; }));
    EXPECT("socketpair AF_UNIX", socketpair(AF_UNIX, SOCK_STREAM, 0, p) == 0 && write(p[0], "sp", 2) == 2 &&
           read(p[1], b, 2) == 2 && memcmp(b, "sp", 2) == 0);
    EXPECT("socketpair both ways, EOF after close", write(p[1], "ba", 2) == 2 && read(p[0], b, 2) == 2 &&
           memcmp(b, "ba", 2) == 0 && close(p[0]) == 0 && read(p[1], b, 2) == 0);
    close(p[1]);
    {
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        strcpy(a.sun_path, "/cu_nobody");
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        EXPECT("AF_UNIX connect to no listener fails", s >= 0 && connect(s, (void *)&a, sizeof a) == -1 && errno == ENOENT);
        close(s);
    }
    {
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        strcpy(a.sun_path, "/cu_sock");
        unlink(a.sun_path);
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        EXPECT("AF_UNIX bind/listen", s >= 0 && bind(s, (void *)&a, sizeof a) == 0 && listen(s, 1) == 0);
        EXPECT("AF_UNIX connect/accept", (c = fork()) >= 0 && (c == 0 ? ({ int t = socket(AF_UNIX, SOCK_STREAM, 0);
               _exit(connect(t, (void *)&a, sizeof a) == 0 && write(t, "un", 2) == 2 ? 0 : 1); 0; }) :
               ({ int t = accept(s, 0, 0); t >= 0 && read(t, b, 2) == 2 && memcmp(b, "un", 2) == 0; }) &&
               waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 0));
        unlink(a.sun_path);
    }
    EXPECT("mkfifo", mkfifo("/cu_fifo", 0644) == 0);
    EXPECT("fstat on an open FIFO shows the file lstat shows", ({ struct stat a, b; int fd = open("/cu_fifo", O_RDONLY | O_NONBLOCK);
           int ok = fd >= 0 && lstat("/cu_fifo", &a) == 0 && fstat(fd, &b) == 0 && S_ISFIFO(b.st_mode) &&
                    a.st_ino == b.st_ino && a.st_dev == b.st_dev && a.st_mode == b.st_mode;
           close(fd); ok; }));
    unlink("/cu_fifo");
    EXPECT("isatty(0)", isatty(0) == 1);
    EXPECT("ttyname(0)", ttyname(0) != NULL);
    return done();
}
