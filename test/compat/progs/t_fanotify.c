/* fanotify (notification class): marks on files, directories and mounts,
 * the events they report with a descriptor of the file, merging, the
 * ignore mask that a modification ends, and what fanotify_init refuses. */
#include "compat.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/fanotify.h>

static int fan_mark(int fd, unsigned flags, unsigned long long mask, const char *path) {
    return syscall(SYS_fanotify_mark, fd, flags, mask, AT_FDCWD, path);
}

/* Reads one event into *m; returns 1 if there was one. */
static int next(int fd, struct fanotify_event_metadata *m) {
    long n = read(fd, m, sizeof *m);
    return n == (long)sizeof *m;
}

int main(void) {
    setup();
    unlink("fa_f");
    rmdir("fa_d");
    int c = open("fa_f", O_CREAT | O_WRONLY, 0644);
    write(c, "hello", 5);
    close(c);
    mkdir("fa_d", 0755);

    int fd = fanotify_init(FAN_CLASS_NOTIF | FAN_NONBLOCK | FAN_CLOEXEC, O_RDONLY);
    EXPECT("fanotify_init makes a descriptor", fd >= 0);
    EXPECT("the content classes are accepted, both at once are refused, and a permission mask is refused", ({
           int c = fanotify_init(FAN_CLASS_CONTENT, O_RDONLY), p = fanotify_init(FAN_CLASS_PRE_CONTENT, O_RDONLY);
           int both = fanotify_init(FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT, O_RDONLY);
           int eb = errno;
           int perm = c >= 0 ? fanotify_mark(c, FAN_MARK_ADD, FAN_OPEN_PERM, AT_FDCWD, ".") : 0;
           int ep = errno;
           if (c >= 0) close(c);
           if (p >= 0) close(p);
           c >= 0 && p >= 0 && both < 0 && eb == EINVAL && perm < 0 && ep == EINVAL; }));
    EXPECT("a mark with no operation is EINVAL", fan_mark(fd, 0, FAN_OPEN, "fa_f") < 0 && errno == EINVAL);
    EXPECT("a mark on a missing file is ENOENT", fan_mark(fd, FAN_MARK_ADD, FAN_OPEN, "fa_none") < 0 && errno == ENOENT);
    EXPECT("an empty queue is EAGAIN", ({ char b[64]; read(fd, b, sizeof b) < 0 && errno == EAGAIN; }));

    EXPECT("mark a file for access, modify, open and close",
           fan_mark(fd, FAN_MARK_ADD, FAN_ACCESS | FAN_MODIFY | FAN_OPEN | FAN_CLOSE, "fa_f") == 0);
    int r = open("fa_f", O_RDONLY);
    char b[16];
    read(r, b, sizeof b);
    close(r);
    struct fanotify_event_metadata m;
    EXPECT("open, access and close of a file are one event", next(fd, &m) && m.mask == (FAN_OPEN | FAN_ACCESS | FAN_CLOSE_NOWRITE));
    EXPECT("the event says who did it", m.pid == getpid() && m.vers == FANOTIFY_METADATA_VERSION && m.metadata_len == sizeof m);
    EXPECT("the event's descriptor reads the file", m.fd >= 0 && read(m.fd, b, sizeof b) == 5 && memcmp(b, "hello", 5) == 0);
    EXPECT("reading the event's descriptor makes no event", ({ close(m.fd); struct fanotify_event_metadata x; !next(fd, &x); }));

    int w = open("fa_f", O_WRONLY);
    write(w, "x", 1);
    close(w);
    EXPECT("a write is modify and close-write", next(fd, &m) && m.mask == (FAN_OPEN | FAN_MODIFY | FAN_CLOSE_WRITE));
    if (m.fd >= 0) close(m.fd);

    EXPECT("ignore access", fan_mark(fd, FAN_MARK_ADD | FAN_MARK_IGNORED_MASK, FAN_ACCESS, "fa_f") == 0);
    r = open("fa_f", O_RDONLY);
    read(r, b, sizeof b);
    close(r);
    EXPECT("an ignored event is left out", next(fd, &m) && m.mask == (FAN_OPEN | FAN_CLOSE_NOWRITE));
    if (m.fd >= 0) close(m.fd);
    w = open("fa_f", O_WRONLY);
    write(w, "y", 1);
    close(w);
    next(fd, &m);
    if (m.fd >= 0) close(m.fd);
    r = open("fa_f", O_RDONLY);
    read(r, b, sizeof b);
    close(r);
    EXPECT("a modification ends the ignore mask", next(fd, &m) && (m.mask & FAN_ACCESS));
    if (m.fd >= 0) close(m.fd);
    EXPECT("remove the mark", fan_mark(fd, FAN_MARK_REMOVE, FAN_ACCESS | FAN_MODIFY | FAN_OPEN | FAN_CLOSE, "fa_f") == 0);

    EXPECT("a directory mark with FAN_EVENT_ON_CHILD",
           fan_mark(fd, FAN_MARK_ADD, FAN_OPEN | FAN_EVENT_ON_CHILD, ".") == 0);
    r = open("fa_f", O_RDONLY);
    close(r);
    EXPECT("a child's event comes to its directory", next(fd, &m) && m.mask == FAN_OPEN);
    if (m.fd >= 0) close(m.fd);
    int d = open("fa_d", O_RDONLY | O_DIRECTORY);
    close(d);
    EXPECT("a subdirectory's event needs FAN_ONDIR", !next(fd, &m));
    EXPECT("add FAN_ONDIR", fan_mark(fd, FAN_MARK_ADD, FAN_ONDIR, ".") == 0);
    d = open("fa_d", O_RDONLY | O_DIRECTORY);
    close(d);
    EXPECT("now it comes, without the flag in its mask", next(fd, &m) && m.mask == FAN_OPEN);
    if (m.fd >= 0) close(m.fd);
    EXPECT("flush the marks", fan_mark(fd, FAN_MARK_FLUSH, 0, ".") == 0);

    EXPECT("a mount mark covers files under the directory",
           fan_mark(fd, FAN_MARK_ADD | FAN_MARK_MOUNT, FAN_OPEN, ".") == 0);
    r = open("fa_f", O_RDONLY);
    close(r);
    EXPECT("and reports them", next(fd, &m) && m.mask == FAN_OPEN);
    if (m.fd >= 0) close(m.fd);

    fan_mark(fd, FAN_MARK_ADD, FAN_OPEN, "fa_f");
    char info[64], path[64];
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    EXPECT("fdinfo lists the inode marks", ({ FILE *f = fopen(path, "r"); int ok = 0;
        while (f && fgets(info, sizeof info, f)) ok |= strncmp(info, "fanotify ino:", 13) == 0;
        if (f) fclose(f); ok; }));
    close(fd);

    unlink("fa_f");
    rmdir("fa_d");
    return done();
}
