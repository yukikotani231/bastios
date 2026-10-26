/* What dockerd, containerd and runc 1.5 do that a smaller program does not:
 * keep a boltdb file in a mapping that reaches past its end while it grows
 * by ftruncate and pwrite, treat a peer's half-closed socket as still
 * writable, watch /proc and cgroup files with inotify, write a number with
 * a trailing NUL to oom_score_adj, and read a /proc/self/fd link through
 * an O_PATH handle. */
#include "compat.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#ifndef O_PATH
#define O_PATH 010000000
#endif

/* A read-only mapping 64 pages long of a file that is 4 pages long, then
 * the file grows by ftruncate and pwrite: the mapping must see every
 * write. */
static int grow_seen(const char *path) {
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return 0;
    if (ftruncate(fd, 4 * 4096) != 0) return 0;
    char *m = mmap(NULL, 64 * 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return 0;
    static char buf[4096];
    int bad = 0;
    for (int it = 0; it < 400; it++) {
        long pg = it % 60;
        off_t need = (pg + 2) * 4096;
        if (lseek(fd, 0, SEEK_END) < need && ftruncate(fd, need + 4096 * (it % 4)) != 0) return 0;
        memset(buf, (it & 0xff) | 1, sizeof buf);
        if (pwrite(fd, buf, sizeof buf, pg * 4096) != sizeof buf) return 0;
        if (m[pg * 4096] != buf[0] || m[pg * 4096 + 4095] != buf[0]) bad++;
    }
    munmap(m, 64 * 4096);
    close(fd);
    unlink(path);
    return bad == 0;
}

int main(void) {
    setup();

    EXPECT("a mapping past the end of a growing file sees pwrite (tmpfs)", grow_seen("/tmp/t_dockerd.db"));

    EXPECT("a peer's shutdown(SHUT_WR) is POLLRDHUP, not POLLHUP, and this end can still write", ({
        int sv[2], ok = 0;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            shutdown(sv[0], SHUT_WR);
            struct pollfd p = {sv[1], POLLIN | POLLRDHUP, 0};
            char c = 0;
            ok = poll(&p, 1, 1000) == 1 && (p.revents & POLLRDHUP) && !(p.revents & POLLHUP)
                 && write(sv[1], "x", 1) == 1 && read(sv[0], &c, 1) == 1 && c == 'x';
            close(sv[0]);
            close(sv[1]);
        }
        ok; }));

    EXPECT("inotify_add_watch on /proc/self/status succeeds", ({
        int in = inotify_init1(0);
        int ok = in >= 0 && inotify_add_watch(in, "/proc/self/status", IN_MODIFY) >= 0;
        if (in >= 0) close(in);
        ok; }));

    EXPECT("oom_score_adj takes a number with a trailing NUL", ({
        int fd = open("/proc/self/oom_score_adj", O_WRONLY);
        int ok = fd >= 0 && write(fd, "100\0", 4) == 4;
        if (fd >= 0) close(fd);
        char b[16] = "";
        fd = open("/proc/self/oom_score_adj", O_RDONLY);
        if (fd >= 0) { ssize_t n = read(fd, b, sizeof b - 1); if (n > 0) b[n] = 0; close(fd); }
        write(1, "", 0);
        ok = ok && atoi(b) == 100;
        fd = open("/proc/self/oom_score_adj", O_WRONLY);
        if (fd >= 0) { write(fd, "0\n", 2); close(fd); }
        ok; }));

    EXPECT("readlinkat of an empty path on an O_PATH|O_NOFOLLOW handle of a /proc/self/fd link is its target", ({
        int ok = 0;
        int f = open("/tmp/t_dockerd.f", O_RDWR | O_CREAT, 0600);
        if (f >= 0) {
            char name[64], t1[256], t2[256];
            snprintf(name, sizeof name, "/proc/self/fd/%d", f);
            int h = open(name, O_PATH | O_NOFOLLOW);
            ssize_t a = readlink(name, t1, sizeof t1 - 1);
            ssize_t b = h >= 0 ? readlinkat(h, "", t2, sizeof t2 - 1) : -1;
            ok = a > 0 && a == b && memcmp(t1, t2, a) == 0;
            if (h >= 0) close(h);
            close(f);
            unlink("/tmp/t_dockerd.f");
        }
        ok; }));
    return done();
}
