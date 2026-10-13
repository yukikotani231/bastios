/* Pseudo-terminals, as an SSH server, script(1) or a terminal emulator
 * uses them: openpty (/dev/ptmx, TIOCSPTLCK, TIOCGPTN, /dev/pts/N),
 * ttyname, the line discipline between the two ends (echo, canonical
 * lines, erase, ICRNL and ONLCR, end of file, raw mode), Ctrl-C reaching
 * the foreground group as SIGINT, the window size and SIGWINCH, poll and
 * O_NONBLOCK, epoll, and what each end sees when the other closes. */
#include "compat.h"
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>

/* Reads what the master has for us within about a second. */
static int drain(int m, char *buf, int size) {
    int got = 0;
    for (int tries = 0; tries < 50 && got < size - 1; tries++) {
        struct pollfd p = {.fd = m, .events = POLLIN};
        if (poll(&p, 1, 20) != 1) {
            if (got) break;
            continue;
        }
        ssize_t n = read(m, buf + got, size - 1 - got);
        if (n <= 0) break;
        got += n;
    }
    buf[got] = 0;
    return got;
}

static volatile int winched;
static void on_winch(int s) { (void)s; winched = 1; }

int main(void) {
    setup();
    int m = -1, s = -1;
    char name[64] = "", buf[256];

    EXPECT("openpty", openpty(&m, &s, name, 0, 0) == 0 && m >= 0 && s >= 0);
    EXPECT("its slave is /dev/pts/N", strncmp(name, "/dev/pts/", 9) == 0);
    {
        struct stat a, b;
        EXPECT("stat of the slave's path matches fstat", stat(name, &a) == 0 && fstat(s, &b) == 0 &&
               S_ISCHR(a.st_mode) && a.st_ino == b.st_ino && a.st_dev == b.st_dev);
    }
    {
        char *t = ttyname(s);
        EXPECT("ttyname of the slave", t && strcmp(t, name) == 0);
    }
    EXPECT("both ends are terminals", isatty(m) && isatty(s));
    {
        int n = -1;
        EXPECT("TIOCGPTN on the master", ioctl(m, TIOCGPTN, &n) == 0 && n == atoi(name + 9));
    }

    /* Canonical mode with echo, as a fresh terminal starts. */
    EXPECT("typing echoes back, CR read as LF",
           write(m, "echo hi\r", 8) == 8 && drain(m, buf, sizeof buf) > 0 && strcmp(buf, "echo hi\r\n") == 0);
    EXPECT("the slave reads the finished line", read(s, buf, sizeof buf) == 8 && memcmp(buf, "echo hi\n", 8) == 0);
    {
        int fl = fcntl(s, F_GETFL);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        if (write(m, "abc", 3) != 3) return 1;
        EXPECT("an unfinished line is not readable yet", read(s, buf, sizeof buf) < 0 && errno == EAGAIN);
        if (write(m, "\x7f" "d\n", 3) != 3) return 1;   /* erase c, type d */
        drain(m, buf, sizeof buf);
        ssize_t n = read(s, buf, sizeof buf);
        EXPECT("erase takes back a byte", n == 4 && memcmp(buf, "abd\n", 4) == 0);
        if (write(m, "\x04", 1) != 1) return 1;   /* Ctrl-D on an empty line */
        EXPECT("Ctrl-D on an empty line reads as end of file", read(s, buf, sizeof buf) == 0);
        fcntl(s, F_SETFL, fl);
    }
    EXPECT("output LF goes out as CR LF",
           write(s, "out\n", 4) == 4 && drain(m, buf, sizeof buf) == 5 && strcmp(buf, "out\r\n") == 0);

    /* Raw mode, as an editor sets it. */
    {
        struct termios t, raw;
        int ok = tcgetattr(s, &t) == 0;
        raw = t;
        cfmakeraw(&raw);
        ok = ok && tcsetattr(s, TCSANOW, &raw) == 0;
        EXPECT("raw mode: no echo, bytes readable at once", ok && write(m, "x\r", 2) == 2 &&
               read(s, buf, sizeof buf) == 2 && memcmp(buf, "x\r", 2) == 0 && drain(m, buf, sizeof buf) == 0);
        EXPECT("raw mode: LF goes out alone", write(s, "y\n", 2) == 2 && drain(m, buf, sizeof buf) == 2 && strcmp(buf, "y\n") == 0);
        struct termios back;
        EXPECT("the master sees the settings the slave made", tcgetattr(m, &back) == 0 && !(back.c_lflag & ICANON) &&
               tcsetattr(s, TCSANOW, &t) == 0);
    }

    /* A child in its own session, with the slave as its terminal: the
     * window size set on the master (as an SSH server does) sends it
     * SIGWINCH, and Ctrl-C reaches it. */
    {
        int sync[2];
        if (pipe(sync) != 0) return 1;
        pid_t c = fork();
        if (c == 0) {
            signal(SIGWINCH, on_winch);
            setsid();
            ioctl(s, TIOCSCTTY, 0);
            dup2(s, 0);
            dup2(s, 1);
            close(m);
            int t = open("/dev/tty", O_RDWR);
            char ok = (t >= 0 && isatty(t)) ? 'y' : 'n';
            if (write(sync[1], &ok, 1) != 1) _exit(1);
            for (int i = 0; i < 2000 && !winched; i++) usleep(1000);
            struct winsize r = {0};
            ok = winched && ioctl(t, TIOCGWINSZ, &r) == 0 && r.ws_row == 33 && r.ws_col == 101 ? 'y' : 'n';
            if (write(sync[1], &ok, 1) != 1) _exit(1);
            pause();                         /* until SIGINT */
            _exit(9);
        }
        char ok = 0;
        if (read(sync[0], &ok, 1) != 1) ok = 0;
        EXPECT("/dev/tty in the child's session is its pty", ok == 'y');
        pid_t g = 0;
        EXPECT("TIOCSCTTY made the child's group the foreground", ioctl(m, TIOCGPGRP, &g) == 0 && g == c);
        struct winsize w = {.ws_row = 33, .ws_col = 101};
        ok = 0;
        EXPECT("TIOCSWINSZ on the master: SIGWINCH and the size on the slave",
               ioctl(m, TIOCSWINSZ, &w) == 0 && read(sync[0], &ok, 1) == 1 && ok == 'y');
        if (write(m, "\x03", 1) != 1) return 1;
        int st = 0;
        EXPECT("Ctrl-C sends SIGINT to the foreground group",
               waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGINT);
        close(sync[0]);
        close(sync[1]);
    }

    /* Closing: the master reads EIO once no slave is open. */
    {
        close(s);
        drain(m, buf, sizeof buf);           /* the echoed ^C */
        struct pollfd p = {.fd = m, .events = POLLIN};
        EXPECT("poll on the master says the slave hung up", poll(&p, 1, 1000) == 1 && (p.revents & POLLHUP));
        EXPECT("then a master read fails with EIO", read(m, buf, sizeof buf) < 0 && errno == EIO);
        close(m);
        struct stat a;
        EXPECT("with both ends closed its /dev/pts entry is gone", stat(name, &a) < 0 && errno == ENOENT);
    }
    {
        int m2 = -1, s2 = -1;
        int ok = openpty(&m2, &s2, 0, 0, 0) == 0;
        close(m2);
        EXPECT("a slave read after the master closes is end of file", ok && read(s2, buf, sizeof buf) == 0);
        EXPECT("and a slave write fails with EIO", write(s2, "x", 1) < 0 && errno == EIO);
        close(s2);
    }
    {
        /* A locked pair: the slave cannot be opened until TIOCSPTLCK unlocks it. */
        int mx = posix_openpt(O_RDWR | O_NOCTTY), n = -1, zero = 0, one = 1;
        char path[32];
        int ok = mx >= 0 && ioctl(mx, TIOCGPTN, &n) == 0;
        snprintf(path, sizeof path, "/dev/pts/%d", n);
        int a = ok ? open(path, O_RDWR | O_NOCTTY) : -1, e = errno;
        EXPECT("a locked slave does not open", ok && a < 0 && e == EIO);
        ioctl(mx, TIOCSPTLCK, &zero);
        int b = open(path, O_RDWR | O_NOCTTY);
        EXPECT("unlockpt lets it open", b >= 0);
        ioctl(mx, TIOCSPTLCK, &one);
        close(b);
        close(mx);
    }
    {
        /* An event loop watching a terminal, as libuv does for Neovim over SSH. */
        int m3 = -1, s3 = -1;
        int ok = openpty(&m3, &s3, 0, 0, 0) == 0;
        int ep = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = m3}, out[2];
        int a = ok && ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, m3, &ev) == 0;
        ev.data.fd = s3;
        int b = ok && epoll_ctl(ep, EPOLL_CTL_ADD, s3, &ev) == 0;
        EXPECT("epoll_ctl accepts both ends of a pair", a && b);
        int n = ok && write(s3, "hi", 2) == 2 ? epoll_wait(ep, out, 2, 1000) : -1;
        EXPECT("epoll reports the master readable after a slave write", n == 1 && out[0].data.fd == m3 && (out[0].events & EPOLLIN));
        char junk[16];
        ok = ok && read(m3, junk, sizeof junk) == 2;
        n = ok && write(m3, "z\n", 2) == 2 ? epoll_wait(ep, out, 2, 1000) : -1;
        int slave = 0;
        for (int i = 0; i < n; i++)
            if (out[i].data.fd == s3 && (out[i].events & EPOLLIN)) slave = 1;
        EXPECT("and the slave readable after a master line", slave);
        close(ep);
        close(m3);
        close(s3);
    }
    {
        /* Sixteen pairs at once (a server with many logins). */
        int ms[16], ss[16], made = 0, echoed = 0;
        for (int i = 0; i < 16; i++)
            made += openpty(&ms[i], &ss[i], NULL, NULL, NULL) == 0;
        EXPECT("sixteen pseudo-terminal pairs at once", made == 16);
        for (int i = 0; i < made; i++) {
            char c = 'a' + i, got = 0;
            echoed += write(ss[i], &c, 1) == 1 && read(ms[i], &got, 1) == 1 && got == c;
        }
        EXPECT("each pair carries its own bytes", echoed == made);
        for (int i = 0; i < made; i++) {
            close(ms[i]);
            close(ss[i]);
        }
    }
    return done();
}
