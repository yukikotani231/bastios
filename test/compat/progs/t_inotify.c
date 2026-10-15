/* inotify: watches on files and directories and the events they report,
 * in the order and shape LTP's inotify tests read them: a file's own
 * events, a directory's events with names, renames tied by a cookie,
 * deletes, coalescing, overflow, one-shot watches, and the limits in
 * /proc/sys/fs/inotify. */
#include "compat.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>

struct ev {
    int wd;
    unsigned mask, cookie;
    char name[64];
};

static struct ev evs[64];

/* Reads what is queued (without waiting) into evs; returns how many. */
static int drain(int fd) {
    static char buf[4096];
    int n = 0;
    long len = read(fd, buf, sizeof buf);
    for (long at = 0; len > 0 && at < len && n < 64;) {
        struct inotify_event *e = (void *)(buf + at);
        evs[n].wd = e->wd;
        evs[n].mask = e->mask;
        evs[n].cookie = e->cookie;
        snprintf(evs[n].name, sizeof evs[n].name, "%.*s", (int)e->len, e->len ? e->name : "");
        n++;
        at += sizeof *e + e->len;
    }
    return n;
}

/* Whether the n events read are exactly these masks (and names: "" for
 * none), in order. */
static int are(int n, int want, const unsigned *masks, const char *const *names) {
    if (n != want) {
        printf("# got %d events, wanted %d:", n, want);
        for (int i = 0; i < n; i++) printf(" %x'%s'", evs[i].mask, evs[i].name);
        printf("\n");
        return 0;
    }
    for (int i = 0; i < n; i++)
        if (evs[i].mask != masks[i] || strcmp(evs[i].name, names[i]) != 0) {
            printf("# event %d is %x '%s', wanted %x '%s'\n", i, evs[i].mask, evs[i].name, masks[i], names[i]);
            return 0;
        }
    return 1;
}

static long proc_num(const char *path) {
    char b[32] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, b, sizeof b - 1);
    close(fd);
    return n > 0 ? atol(b) : -1;
}

int main(void) {
    setup();
    char buf[256] = "data";
    int in, fd, wd, n;

    EXPECT("inotify_init1 rejects unknown flags", inotify_init1(1) == -1 && errno == EINVAL);
    in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    EXPECT("inotify_init1 with NONBLOCK and CLOEXEC", in >= 0 && (fcntl(in, F_GETFD) & FD_CLOEXEC) &&
           (fcntl(in, F_GETFL) & O_NONBLOCK) && (fcntl(in, F_GETFL) & O_ACCMODE) == O_RDONLY);
    EXPECT("an empty queue reads EAGAIN", read(in, buf, sizeof buf) == -1 && errno == EAGAIN);
    EXPECT("a watch on a missing file is ENOENT", inotify_add_watch(in, "in_nothing", IN_ALL_EVENTS) == -1 && errno == ENOENT);
    EXPECT("a watch with no events is EINVAL", inotify_add_watch(in, ".", 0) == -1 && errno == EINVAL);
    EXPECT("a watch on a non-inotify fd is EINVAL", inotify_add_watch(0, ".", IN_OPEN) == -1 && errno == EINVAL);
    EXPECT("a watch on a closed fd is EBADF", inotify_add_watch(99, ".", IN_OPEN) == -1 && errno == EBADF);

    /* A file's own events (LTP inotify01). */
    fd = open("in_f", O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, buf, 4);
    close(fd);
    wd = inotify_add_watch(in, "in_f", IN_ALL_EVENTS);
    EXPECT("inotify_add_watch on a file", wd >= 1);
    EXPECT("adding it again gives the same wd", inotify_add_watch(in, "in_f", IN_ALL_EVENTS) == wd);
    EXPECT("IN_MASK_CREATE on a watched file is EEXIST",
           inotify_add_watch(in, "in_f", IN_ACCESS | IN_MASK_CREATE) == -1 && errno == EEXIST);
    EXPECT("IN_ONLYDIR on a file is ENOTDIR", inotify_add_watch(in, "in_f", IN_ACCESS | IN_ONLYDIR) == -1 && errno == ENOTDIR);
    chmod("in_f", 0600);
    fd = open("in_f", O_RDONLY);
    read(fd, buf, 4);
    close(fd);
    fd = open("in_f", O_RDWR);
    write(fd, buf, 4);
    close(fd);
    {
        struct pollfd p = {in, POLLIN, 0};
        EXPECT("poll says POLLIN once an event is queued", poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    }
    {
        int ep = epoll_create1(0);
        struct epoll_event e = {.events = EPOLLIN}, got;
        EXPECT("epoll sees it readable too", ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, in, &e) == 0 &&
               epoll_wait(ep, &got, 1, 0) == 1 && (got.events & EPOLLIN));
        close(ep);
    }
    n = drain(in);
    {
        unsigned m[] = {IN_ATTRIB, IN_OPEN, IN_ACCESS, IN_CLOSE_NOWRITE, IN_OPEN, IN_MODIFY, IN_CLOSE_WRITE};
        const char *nm[] = {"", "", "", "", "", "", ""};
        EXPECT("a file's events: attrib, open, access, close, open, modify, close-write", are(n, 7, m, nm) &&
               evs[0].wd == wd && evs[0].cookie == 0);
    }
    EXPECT("inotify_rm_watch", inotify_rm_watch(in, wd) == 0);
    n = drain(in);
    {
        unsigned m[] = {IN_IGNORED};
        const char *nm[] = {""};
        EXPECT("removing a watch reports IN_IGNORED", are(n, 1, m, nm) && evs[0].wd == wd);
    }
    EXPECT("removing it twice is EINVAL", inotify_rm_watch(in, wd) == -1 && errno == EINVAL);

    /* A directory's events, with names (LTP inotify02). */
    mkdir("in_d", 0755);
    int wdd = inotify_add_watch(in, "in_d", IN_ALL_EVENTS);
    EXPECT("inotify_add_watch on a directory", wdd > wd);
    chmod("in_d", 0700);
    fd = open("in_d/a", O_RDWR | O_CREAT, 0644);
    close(fd);
    rename("in_d/a", "in_d/b");
    mkdir("in_d/sub", 0755);
    rmdir("in_d/sub");
    unlink("in_d/b");
    n = drain(in);
    {
        unsigned m[] = {IN_ATTRIB | IN_ISDIR, IN_CREATE, IN_OPEN, IN_CLOSE_WRITE, IN_MOVED_FROM, IN_MOVED_TO,
                        IN_CREATE | IN_ISDIR, IN_DELETE | IN_ISDIR, IN_DELETE};
        const char *nm[] = {"", "a", "a", "a", "a", "b", "sub", "sub", "b"};
        EXPECT("a directory's events name the entry", are(n, 9, m, nm));
        EXPECT("a rename's two events share a cookie", n >= 6 && evs[4].cookie != 0 && evs[4].cookie == evs[5].cookie &&
               evs[1].cookie == 0);
    }

    /* Events on a child go to its directory and then to the child (LTP inotify10). */
    close(open("in_d/c", O_CREAT | O_WRONLY, 0644));
    int wdc = inotify_add_watch(in, "in_d/c", IN_ATTRIB);
    drain(in);
    chmod("in_d/c", 0600);
    n = drain(in);
    {
        unsigned m[] = {IN_ATTRIB, IN_ATTRIB};
        const char *nm[] = {"c", ""};
        EXPECT("a child's event: to the directory, then to the child", are(n, 2, m, nm) &&
               evs[0].wd == wdd && evs[1].wd == wdc);
    }

    /* Deleting watched things (LTP inotify04). */
    mkdir("in_d/gone", 0755);
    int wdg = inotify_add_watch(in, "in_d/gone", IN_ALL_EVENTS);
    drain(in);
    rmdir("in_d/gone");
    unlink("in_d/c");
    n = drain(in);
    {
        unsigned m[] = {IN_DELETE_SELF, IN_IGNORED, IN_DELETE | IN_ISDIR, IN_ATTRIB, IN_IGNORED, IN_DELETE};
        const char *nm[] = {"", "", "gone", "", "", "c"};
        EXPECT("rmdir: delete-self, ignored, then delete in the directory", are(n, 6, m, nm) &&
               evs[0].wd == wdg && evs[2].wd == wdd);
        EXPECT("unlink: attrib, ignored, then delete in the directory", n == 6 && evs[3].wd == wdc &&
               evs[4].wd == wdc && evs[5].wd == wdd);
    }

    /* The same event twice in a row is one; a one-shot watch reports once. */
    fd = open("in_f", O_RDWR);
    int wd2 = inotify_add_watch(in, "in_f", IN_MODIFY | IN_ONESHOT);
    write(fd, "x", 1);
    write(fd, "y", 1);
    n = drain(in);
    {
        unsigned m[] = {IN_MODIFY, IN_IGNORED};
        const char *nm[] = {"", ""};
        EXPECT("IN_ONESHOT: one event, then the watch goes", are(n, 2, m, nm) && evs[0].wd == wd2);
    }
    wd2 = inotify_add_watch(in, "in_f", IN_MODIFY);
    write(fd, "x", 1);
    write(fd, "y", 1);
    n = drain(in);
    {
        unsigned m[] = {IN_MODIFY};
        const char *nm[] = {""};
        EXPECT("two equal events in a row coalesce", are(n, 1, m, nm));
    }

    /* Overflow (LTP inotify05). */
    long max = proc_num("/proc/sys/fs/inotify/max_queued_events");
    EXPECT("/proc/sys/fs/inotify/max_queued_events", max > 0);
    EXPECT("/proc/sys/fs/inotify/max_user_instances", proc_num("/proc/sys/fs/inotify/max_user_instances") > 0);
    EXPECT("/proc/sys/fs/inotify/max_user_watches", proc_num("/proc/sys/fs/inotify/max_user_watches") > 0);
    {
        /* root may raise a limit; it is kept and read back */
        long old = proc_num("/proc/sys/fs/inotify/max_user_instances");
        int lf = open("/proc/sys/fs/inotify/max_user_instances", O_WRONLY);
        int okw = lf >= 0 && dprintf(lf, "%ld\n", old + 500) > 0;
        if (lf >= 0)
            close(lf);
        EXPECT("max_user_instances is writable and reads back", okw && proc_num("/proc/sys/fs/inotify/max_user_instances") == old + 500);
        lf = open("/proc/sys/fs/inotify/max_user_instances", O_WRONLY);
        if (lf >= 0) {
            dprintf(lf, "%ld\n", old);
            close(lf);
        }
    }
    {
        /* a process that just ended does not give its id to the next fork */
        int p1 = fork();
        if (p1 == 0)
            _exit(0);
        waitpid(p1, 0, 0);
        int p2 = fork();
        if (p2 == 0)
            _exit(0);
        waitpid(p2, 0, 0);
        EXPECT("two forks in a row get different pids", p1 > 0 && p2 > 0 && p1 != p2);
    }
    {
        /* pid_max: kept and read back; Linux's bounds are 301 and 4194304 */
        long old = proc_num("/proc/sys/kernel/pid_max");
        int pf = open("/proc/sys/kernel/pid_max", O_WRONLY);
        int okw = pf >= 0 && dprintf(pf, "32768\n") > 0;
        int bad = pf >= 0 && write(pf, "5\n", 2) < 0 && errno == EINVAL;
        if (pf >= 0)
            close(pf);
        EXPECT("pid_max is writable, kept and bounded", okw && bad && proc_num("/proc/sys/kernel/pid_max") == 32768);
        pf = open("/proc/sys/kernel/pid_max", O_WRONLY);
        if (pf >= 0) {
            dprintf(pf, "%ld\n", old);
            close(pf);
        }
    }
    inotify_add_watch(in, "in_f", IN_ACCESS | IN_MODIFY);
    for (long i = 0; i < max && i < 100000; i++) {
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, 1);
        write(fd, buf, 1);
    }
    {
        static char big[65536];
        long total = 0, len, last = -1;
        unsigned lastmask = 0;
        int count = 0;
        while ((len = read(in, big, sizeof big)) > 0) {
            for (long at = 0; at < len;) {
                struct inotify_event *e = (void *)(big + at);
                lastmask = e->mask;
                last = e->wd;
                count++;
                at += sizeof *e + e->len;
            }
            total += len;
        }
        EXPECT("a full queue ends with IN_Q_OVERFLOW (wd -1)", lastmask == IN_Q_OVERFLOW && last == -1 &&
               count == max + 1);
    }
    close(fd);

    {
        char small[8];
        fd = open("in_f", O_RDONLY);
        read(fd, buf, 1);
        close(fd);
        EXPECT("a buffer too small for an event is EINVAL", read(in, small, sizeof small) == -1 && errno == EINVAL);
    }
    EXPECT("closing the descriptor", close(in) == 0);

    /* A blocking read waits for an event. */
    in = inotify_init();
    inotify_add_watch(in, ".", IN_CREATE);
    pid_t child = fork();
    if (child == 0) {
        usleep(100000);
        close(open("in_late", O_CREAT | O_WRONLY, 0644));
        _exit(0);
    }
    n = drain(in);
    EXPECT("a blocking read waits for the event", n == 1 && evs[0].mask == IN_CREATE && strcmp(evs[0].name, "in_late") == 0);
    waitpid(child, NULL, 0);
    close(in);

    /* Many watches on one instance (a file watcher over a source tree). */
    in = inotify_init();
    int watched = 0;
    for (int i = 0; i < 60; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "in_w%d", i);
        close(open(nm, O_CREAT | O_WRONLY, 0644));
        watched += inotify_add_watch(in, nm, IN_MODIFY) >= 0;
    }
    EXPECT("sixty watches on one instance", watched == 60);
    close(in);
    for (int i = 0; i < 60; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "in_w%d", i);
        unlink(nm);
    }

    /* epoll: thirty-two instances, and two hundred descriptors on one. */
    int eps[32], nep = 0;
    for (int i = 0; i < 32; i++) {
        eps[i] = epoll_create1(0);
        nep += eps[i] >= 0;
    }
    EXPECT("thirty-two epoll instances", nep == 32);
    int efd[200], nadd = 0;
    for (int i = 0; i < 200; i++) {
        struct epoll_event ev = { .events = EPOLLIN, .data.u32 = i };
        efd[i] = eventfd(0, 0);
        nadd += efd[i] >= 0 && eps[0] >= 0 && epoll_ctl(eps[0], EPOLL_CTL_ADD, efd[i], &ev) == 0;
    }
    EXPECT("two hundred descriptors on one epoll instance", nadd == 200);
    uint64_t one = 1;
    struct epoll_event got[8];
    int ready = efd[199] >= 0 && write(efd[199], &one, 8) == 8 ? epoll_wait(eps[0], got, 8, 0) : -1;
    EXPECT("the last one is the one reported", ready == 1 && got[0].data.u32 == 199);
    for (int i = 0; i < 200; i++)
        close(efd[i]);
    for (int i = 0; i < 32; i++)
        close(eps[i]);

    /* timerfd: sixty at once. */
    int tfd[60], ntf = 0;
    for (int i = 0; i < 60; i++) {
        tfd[i] = timerfd_create(CLOCK_MONOTONIC, 0);
        ntf += tfd[i] >= 0;
    }
    EXPECT("sixty timerfds at once", ntf == 60);
    for (int i = 0; i < 60; i++)
        close(tfd[i]);

    /* inotify instances: thirty at once. */
    int ino[30], nino = 0;
    for (int i = 0; i < 30; i++) {
        ino[i] = inotify_init1(IN_NONBLOCK);
        nino += ino[i] >= 0;
    }
    EXPECT("thirty inotify instances at once", nino == 30);
    for (int i = 0; i < 30; i++)
        if (ino[i] >= 0)
            close(ino[i]);

    /* named pipes: thirty open at once. */
    int ff[30], nff = 0;
    for (int i = 0; i < 30; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "in_fifo%d", i);
        unlink(nm);
        ff[i] = mkfifo(nm, 0600) == 0 ? open(nm, O_RDONLY | O_NONBLOCK) : -1;
        nff += ff[i] >= 0;
    }
    EXPECT("thirty named pipes open at once", nff == 30);
    for (int i = 0; i < 30; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "in_fifo%d", i);
        if (ff[i] >= 0)
            close(ff[i]);
        unlink(nm);
    }

    unlink("in_late");
    unlink("in_f");
    rmdir("in_d");
    return done();
}
