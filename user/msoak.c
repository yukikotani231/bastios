/* A long-run soak: rounds of a mixed workload (processes, memory, files,
 * pipes, threads, sockets, signals, polling), each round leaving the system as
 * it found it. After every round it prints what the kernel still holds for
 * this process and for the whole machine, so test/soak.py can tell a leak
 * (free memory, descriptors or processes that only ever go down or up) from
 * noise.
 *
 *   msoak [ROUNDS [SEED [MEMPCT]]]       run ROUNDS rounds (default 10)
 *   msoak child                          (internal) the exec'd child
 *
 * Lines: "soak: round N ok ops=OPS free=KB cached=KB fds=N procs=N" for a
 * round that passed, and "soak: FAIL what" for the first thing that did not,
 * after which it exits 1. A round's operations are chosen by a generator
 * seeded with SEED, so a failing round replays (the interleaving of the
 * processors does not). MEMPCT is how much of free memory the memory
 * pressure step may touch at once (default 60).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t rng = 88172645463325252ull;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)((rng >> 11) % n);
}

static long ops;
static char dir[64];
static int failed;

#define CHECK(c, what) do { if (!(c)) { printf("soak: FAIL %s (errno %d)\n", what, errno); fflush(stdout); failed = 1; return; } } while (0)

static long kb_of(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long v = -1;
    if (!f)
        return -1;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, n) && line[n] == ':') {
            v = atol(line + n + 1);
            break;
        }
    fclose(f);
    return v;
}

static int count_dir(const char *path, int numeric_only)
{
    DIR *d = opendir(path);
    int n = 0;
    struct dirent *e;
    if (!d)
        return -1;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && (!numeric_only || (e->d_name[0] >= '0' && e->d_name[0] <= '9')))
            n++;
    closedir(d);
    return n;
}

/* ---- processes ---- */

static void do_fork(void)
{
    for (int i = 0; i < 20; i++) {
        size_t len = (size_t)(1 + rnd(64)) * 4096;
        char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(p != MAP_FAILED, "fork: mmap");
        memset(p, 0x5a, len);
        pid_t c = fork();
        CHECK(c >= 0, "fork");
        if (c == 0) {
            for (size_t o = 0; o < len; o += 4096)
                p[o] ^= 0xff;                 /* copy on write */
            _exit(p[0] == (char)0xa5 ? 7 : 1);
        }
        int st = 0;
        CHECK(waitpid(c, &st, 0) == c, "fork: waitpid");
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7, "fork: child status");
        CHECK(p[0] == 0x5a, "fork: parent page changed by the child");
        munmap(p, len);
        ops++;
    }
}

static void do_exec(void)
{
    for (int i = 0; i < 10; i++) {
        pid_t c = fork();
        CHECK(c >= 0, "exec: fork");
        if (c == 0) {
            char *argv[] = {"/disk/msoak", "child", 0};
            execv(argv[0], argv);
            _exit(99);
        }
        int st = 0;
        CHECK(waitpid(c, &st, 0) == c, "exec: waitpid");
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "exec: child status");
        ops++;
    }
}

/* Many children alive at once, then reaped in a random order. */
static void do_swarm(void)
{
    pid_t k[40];
    int go[2];
    CHECK(pipe(go) == 0, "swarm: pipe");
    for (int i = 0; i < 40; i++) {
        k[i] = fork();
        CHECK(k[i] >= 0, "swarm: fork");
        if (k[i] == 0) {
            char c;
            close(go[1]);
            read(go[0], &c, 1);
            _exit(i & 63);
        }
    }
    close(go[1]);
    close(go[0]);
    for (int i = 39; i > 0; i--) {
        int j = rnd(i + 1);
        pid_t t = k[i]; k[i] = k[j]; k[j] = t;
    }
    for (int i = 0; i < 40; i++) {
        int st = 0;
        CHECK(waitpid(k[i], &st, 0) == k[i], "swarm: waitpid");
        ops++;
    }
}

/* A child that dies on a signal or faults must leave nothing behind. */
static void do_die(void)
{
    for (int i = 0; i < 6; i++) {
        pid_t c = fork();
        CHECK(c >= 0, "die: fork");
        if (c == 0) {
            switch (i % 3) {
            case 0: raise(SIGKILL); break;
            case 1: *(volatile int *)0 = 1; break;
            default: abort();
            }
            _exit(0);
        }
        int st = 0;
        CHECK(waitpid(c, &st, 0) == c, "die: waitpid");
        CHECK(WIFSIGNALED(st), "die: child was not killed by a signal");
        ops++;
    }
}

/* ---- memory ---- */

static void do_mmap(void)
{
    for (int i = 0; i < 30; i++) {
        size_t pages = 4 + rnd(60);
        size_t len = pages * 4096;
        unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(p != MAP_FAILED, "mmap");
        for (size_t o = 0; o < len; o += 4096)
            p[o] = (unsigned char)(o >> 12);
        /* drop a hole in the middle, then move what is left */
        CHECK(munmap(p + 2 * 4096, 4096) == 0, "mmap: partial munmap");
        CHECK(mprotect(p, 4096, PROT_READ) == 0, "mmap: mprotect");
        unsigned char *q = mremap(p + 3 * 4096, (pages - 3) * 4096, (pages - 3) * 4096 + 8 * 4096, MREMAP_MAYMOVE);
        CHECK(q != MAP_FAILED, "mmap: mremap");
        CHECK(q[0] == 3, "mmap: mremap lost a page");
        munmap(q, (pages - 3) * 4096 + 8 * 4096);
        munmap(p, 2 * 4096);
        ops++;
    }
}

static void do_filemap(void)
{
    char path[96];
    snprintf(path, sizeof path, "%s/map", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "filemap: open");
    CHECK(ftruncate(fd, 64 * 4096) == 0, "filemap: ftruncate");
    char *p = mmap(0, 64 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "filemap: mmap");
    for (int i = 0; i < 64; i++)
        memset(p + i * 4096, i + 1, 4096);
    CHECK(msync(p, 64 * 4096, MS_SYNC) == 0, "filemap: msync");
    munmap(p, 64 * 4096);
    char buf[4096];
    for (int i = 0; i < 64; i += 7) {
        CHECK(pread(fd, buf, sizeof buf, (off_t)i * 4096) == sizeof buf, "filemap: pread");
        CHECK(buf[0] == (char)(i + 1) && buf[4095] == (char)(i + 1), "filemap: contents");
    }
    close(fd);
    unlink(path);
    ops++;
}

/* Touch much of free memory at once, from several processes, then give it all
 * back: the page allocator is walked from one end to the other. */
static int mempct = 60;
static void do_pressure(void)
{
    long free_kb = kb_of("MemFree");
    CHECK(free_kb > 0, "pressure: MemFree");
    size_t per = (size_t)free_kb * 1024 / 100 * mempct / 100 / 3;
    per &= ~4095ul;
    pid_t k[3];
    for (int i = 0; i < 3; i++) {
        k[i] = fork();
        CHECK(k[i] >= 0, "pressure: fork");
        if (k[i] == 0) {
            char *p = mmap(0, per, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                _exit(0);
            for (size_t o = 0; o < per; o += 4096)
                p[o] = (char)o;
            for (size_t o = 0; o < per; o += 4096 * 17)
                if (p[o] != (char)o)
                    _exit(2);
            if (i == 1)
                raise(SIGKILL);              /* some die holding it all */
            _exit(0);
        }
    }
    for (int i = 0; i < 3; i++) {
        int st = 0;
        CHECK(waitpid(k[i], &st, 0) == k[i], "pressure: waitpid");
        CHECK(!WIFEXITED(st) || WEXITSTATUS(st) == 0, "pressure: contents wrong");
    }
    ops++;
}

/* ---- files ---- */

static void do_files(void)
{
    char path[96], path2[96], buf[1500];
    for (int i = 0; i < 24; i++) {
        snprintf(path, sizeof path, "%s/f%d", dir, i);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0, "files: open");
        int n = 1 + rnd(20);
        for (int j = 0; j < n; j++) {
            memset(buf, 'a' + (i + j) % 26, sizeof buf);
            CHECK(write(fd, buf, sizeof buf) == sizeof buf, "files: write");
        }
        CHECK(lseek(fd, 100, SEEK_SET) == 100, "files: lseek");
        CHECK(read(fd, buf, 10) == 10 && buf[0] == 'a' + i % 26, "files: read back");
        if (rnd(2))
            CHECK(ftruncate(fd, rnd(5000)) == 0, "files: ftruncate");
        close(fd);
        snprintf(path2, sizeof path2, "%s/g%d", dir, i);
        CHECK(rename(path, path2) == 0, "files: rename");
        ops++;
    }
    for (int i = 0; i < 24; i++) {
        snprintf(path, sizeof path, "%s/g%d", dir, i);
        CHECK(unlink(path) == 0, "files: unlink");
    }
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof path, "%s/d%d", dir, i);
        CHECK(mkdir(path, 0755) == 0, "files: mkdir");
        snprintf(path2, sizeof path2, "%s/d%d/x", dir, i);
        int fd = open(path2, O_WRONLY | O_CREAT, 0644);
        CHECK(fd >= 0, "files: open in a directory");
        close(fd);
    }
    for (int i = 0; i < 8; i++) {
        snprintf(path2, sizeof path2, "%s/d%d/x", dir, i);
        CHECK(unlink(path2) == 0, "files: unlink in a directory");
        snprintf(path, sizeof path, "%s/d%d", dir, i);
        CHECK(rmdir(path) == 0, "files: rmdir");
    }
    /* an unlinked file stays usable while it is open, and goes with its last close */
    snprintf(path, sizeof path, "%s/orphan", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "files: orphan open");
    CHECK(unlink(path) == 0, "files: orphan unlink");
    memset(buf, 7, sizeof buf);
    CHECK(write(fd, buf, sizeof buf) == sizeof buf, "files: orphan write");
    CHECK(pread(fd, buf, 4, 0) == 4 && buf[0] == 7, "files: orphan read");
    close(fd);
    ops++;
}

static void do_pipes(void)
{
    for (int i = 0; i < 20; i++) {
        int p[2];
        CHECK(pipe(p) == 0, "pipe");
        char buf[3000];
        memset(buf, i, sizeof buf);
        pid_t c = fork();
        CHECK(c >= 0, "pipe: fork");
        if (c == 0) {
            close(p[0]);
            for (int j = 0; j < 20; j++)
                if (write(p[1], buf, sizeof buf) != sizeof buf)
                    _exit(1);
            _exit(0);
        }
        close(p[1]);
        long got = 0;
        ssize_t n;
        char in[4096];
        while ((n = read(p[0], in, sizeof in)) > 0) {
            for (ssize_t j = 0; j < n; j++)
                if (in[j] != (char)i) {
                    printf("soak: FAIL pipe data\n");
                    failed = 1;
                    return;
                }
            got += n;
        }
        close(p[0]);
        int st = 0;
        waitpid(c, &st, 0);
        CHECK(got == 20 * 3000, "pipe: byte count");
        ops++;
    }
    /* a reader that goes away: the writer gets SIGPIPE/EPIPE, not a hang */
    for (int i = 0; i < 10; i++) {
        int p[2];
        CHECK(pipe(p) == 0, "pipe: broken");
        close(p[0]);
        signal(SIGPIPE, SIG_IGN);
        CHECK(write(p[1], "x", 1) < 0 && errno == EPIPE, "pipe: EPIPE");
        close(p[1]);
        ops++;
    }
}

/* ---- threads ---- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *worker(void *a)
{
    (void)a;
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
    }
    char *p = malloc(100000);
    if (p) {
        memset(p, 1, 100000);
        free(p);
    }
    return 0;
}

static void do_threads(void)
{
    for (int r = 0; r < 4; r++) {
        pthread_t t[8];
        counter = 0;
        for (int i = 0; i < 8; i++)
            CHECK(pthread_create(&t[i], 0, worker, 0) == 0, "threads: create");
        for (int i = 0; i < 8; i++)
            CHECK(pthread_join(t[i], 0) == 0, "threads: join");
        CHECK(counter == 8 * 2000, "threads: lost an update");
        ops++;
    }
}

/* ---- sockets and polling ---- */

static void do_unix(void)
{
    for (int i = 0; i < 20; i++) {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "unix: socketpair");
        char b[512], r[512];
        memset(b, i, sizeof b);
        CHECK(write(sv[0], b, sizeof b) == sizeof b, "unix: write");
        CHECK(read(sv[1], r, sizeof r) == sizeof r && !memcmp(b, r, sizeof b), "unix: read");
        close(sv[0]);
        CHECK(read(sv[1], r, 1) == 0, "unix: EOF");
        close(sv[1]);
        ops++;
    }
}

static void do_tcp(void)
{
    for (int i = 0; i < 6; i++) {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(ls >= 0, "tcp: socket");
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        CHECK(bind(ls, (struct sockaddr *)&a, sizeof a) == 0, "tcp: bind");
        socklen_t l = sizeof a;
        CHECK(getsockname(ls, (struct sockaddr *)&a, &l) == 0, "tcp: getsockname");
        CHECK(listen(ls, 4) == 0, "tcp: listen");
        pid_t c = fork();
        CHECK(c >= 0, "tcp: fork");
        if (c == 0) {
            close(ls);
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (connect(s, (struct sockaddr *)&a, sizeof a) < 0)
                _exit(1);
            char buf[1024];
            for (int j = 0; j < 40; j++) {
                memset(buf, j, sizeof buf);
                if (write(s, buf, sizeof buf) != sizeof buf)
                    _exit(2);
            }
            shutdown(s, SHUT_WR);
            long n = 0;
            ssize_t r;
            while ((r = read(s, buf, sizeof buf)) > 0)
                n += r;
            _exit(n == 40 * 1024 ? 0 : 3);
        }
        int s = accept(ls, 0, 0);
        CHECK(s >= 0, "tcp: accept");
        long n = 0;
        ssize_t r;
        char buf[1024];
        while ((r = read(s, buf, sizeof buf)) > 0) {
            if (write(s, buf, r) != r) {
                printf("soak: FAIL tcp: echo write\n");
                failed = 1;
                return;
            }
            n += r;
        }
        close(s);
        close(ls);
        int st = 0;
        CHECK(waitpid(c, &st, 0) == c, "tcp: waitpid");
        CHECK(n == 40 * 1024 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "tcp: transfer");
        ops++;
    }
}

static void do_udp(void)
{
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(a >= 0 && b >= 0, "udp: socket");
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(a, (struct sockaddr *)&sa, sizeof sa) == 0, "udp: bind");
    socklen_t l = sizeof sa;
    getsockname(a, (struct sockaddr *)&sa, &l);
    for (int i = 0; i < 50; i++) {
        char m[200], r[200];
        memset(m, i, sizeof m);
        CHECK(sendto(b, m, sizeof m, 0, (struct sockaddr *)&sa, sizeof sa) == sizeof m, "udp: sendto");
        struct pollfd p = {a, POLLIN, 0};
        CHECK(poll(&p, 1, 2000) == 1, "udp: poll");
        CHECK(recv(a, r, sizeof r, 0) == sizeof r && !memcmp(m, r, sizeof m), "udp: recv");
        ops++;
    }
    close(a);
    close(b);
}

static void do_epoll(void)
{
    for (int i = 0; i < 10; i++) {
        int ep = epoll_create1(0), ev = eventfd(0, EFD_NONBLOCK);
        int p[2];
        CHECK(ep >= 0 && ev >= 0 && pipe(p) == 0, "epoll: create");
        struct epoll_event e = {.events = EPOLLIN, .data.fd = ev};
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e) == 0, "epoll: add");
        e.data.fd = p[0];
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &e) == 0, "epoll: add pipe");
        uint64_t one = 1;
        CHECK(write(ev, &one, 8) == 8, "epoll: eventfd write");
        write(p[1], "x", 1);
        struct epoll_event out[4];
        CHECK(epoll_wait(ep, out, 4, 2000) == 2, "epoll: wait");
        /* close with the entries still registered */
        close(p[0]);
        close(p[1]);
        close(ev);
        close(ep);
        ops++;
    }
}

static volatile sig_atomic_t usr1;
static void on_usr1(int s) { (void)s; usr1++; }

static void do_signals(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, 0);
    for (int i = 0; i < 40; i++) {
        usr1 = 0;
        CHECK(kill(getpid(), SIGUSR1) == 0, "signals: kill");
        CHECK(usr1 == 1, "signals: handler");
        ops++;
    }
    pid_t c = fork();
    CHECK(c >= 0, "signals: fork");
    if (c == 0) {
        for (;;)
            pause();
    }
    for (int i = 0; i < 10; i++)
        kill(c, SIGUSR1);
    kill(c, SIGTERM);
    int st = 0;
    CHECK(waitpid(c, &st, 0) == c && WIFSIGNALED(st), "signals: killed child");
    ops++;
}


/* ---- containers, mounts and the odder objects ---- */

static void note_once(const char *what, int err)
{
    static char seen[16][24];
    for (int i = 0; i < 16; i++) {
        if (!strcmp(seen[i], what))
            return;
        if (!seen[i][0]) {
            strncpy(seen[i], what, 23);
            printf("soak: note %s unsupported (errno %d), skipped\n", what, err);
            return;
        }
    }
}

/* What a container runtime does on every start: new namespaces, a tmpfs and
 * /proc mounted in them, a cgroup, children in a new PID namespace. The
 * namespaces and mounts must go away with the last process in them. */
static void do_container(void)
{
    static int serial;
    for (int i = 0; i < 4; i++) {
        char mnt[96];
        /* on the disk, as a container's root is: a tmpfs over a disk
         * directory belongs to the namespace; one over a tmpfs directory is
         * the machine's and stays until it is unmounted */
        snprintf(mnt, sizeof mnt, "/disk/soak%d-c%d", (int)getpid(), serial++);
        mkdir(mnt, 0755);
        pid_t c = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD, 0, 0, 0, 0);
        if (c < 0) {
            note_once("container clone", errno);
            rmdir(mnt);
            return;
        }
        if (c == 0) {
            if (getpid() != 1)
                _exit(10);
            mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);      /* as runc does */
            if (mount("tmpfs", mnt, "tmpfs", 0, "size=1m") < 0) {
                printf("soak: container mount %s: errno %d\n", mnt, errno);
                _exit(11);
            }
            char f[128], buf[4096];
            memset(buf, 'c', sizeof buf);
            for (int j = 0; j < 8; j++) {
                snprintf(f, sizeof f, "%s/f%d", mnt, j);
                int fd = open(f, O_WRONLY | O_CREAT, 0644);
                if (fd < 0 || write(fd, buf, sizeof buf) != sizeof buf)
                    _exit(12);
                close(fd);
            }
            if (sethostname("soak-box", 8) < 0)
                _exit(13);
            pid_t g = fork();
            if (g == 0) {
                if (getpid() != 2)
                    _exit(14);
                _exit(0);
            }
            int st = 0;
            if (waitpid(g, &st, 0) != g || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
                _exit(15);
            /* leave the tmpfs mounted on purpose: the namespace's death unmounts it */
            _exit(0);
        }
        int st = 0;
        CHECK(waitpid(c, &st, 0) == c, "container: waitpid");
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("soak: FAIL container: the child failed (status 0x%x, errno %d)\n", st, errno);
            failed = 1;
            return;
        }
        /* The tmpfs is marked for everyone until the namespace is collected
         * (src/lxns.bastio), so the directory may stay busy for a while. */
        if (getenv("SOAK_NORMDIR") == 0) rmdir(mnt);
        ops++;
    }
}

/* A mount and umount in this namespace, repeatedly, with files on it. */
static void do_mounts(void)
{
    char mnt[96], f[128];
    snprintf(mnt, sizeof mnt, "%s/mnt", dir);
    CHECK(mkdir(mnt, 0755) == 0, "mounts: mkdir");
    for (int i = 0; i < 12; i++) {
        if (mount("tmpfs", mnt, "tmpfs", 0, "size=2m") < 0) {
            note_once("tmpfs mount", errno);
            break;
        }
        snprintf(f, sizeof f, "%s/x", mnt);
        int fd = open(f, O_WRONLY | O_CREAT, 0644);
        CHECK(fd >= 0, "mounts: create");
        char buf[8192];
        memset(buf, i, sizeof buf);
        CHECK(write(fd, buf, sizeof buf) == sizeof buf, "mounts: write");
        close(fd);
        CHECK(umount(mnt) == 0, "mounts: umount");
        CHECK(access(f, F_OK) < 0, "mounts: file visible after umount");
        ops++;
    }
    rmdir(mnt);
}

static void do_cgroup(void)
{
    char p[128];
    for (int i = 0; i < 8; i++) {
        snprintf(p, sizeof p, "/sys/fs/cgroup/soak%d", (int)getpid());
        if (mkdir(p, 0755) < 0) {
            note_once("cgroup mkdir", errno);
            return;
        }
        char f[160];
        snprintf(f, sizeof f, "%s/pids.max", p);
        int fd = open(f, O_WRONLY);
        if (fd >= 0) {
            write(fd, "50", 2);
            close(fd);
        }
        pid_t c = fork();
        CHECK(c >= 0, "cgroup: fork");
        if (c == 0) {
            snprintf(f, sizeof f, "%s/cgroup.procs", p);
            int fd2 = open(f, O_WRONLY);
            char num[16];
            if (fd2 >= 0) {
                snprintf(num, sizeof num, "%d", (int)getpid());
                write(fd2, num, strlen(num));
                close(fd2);
            }
            _exit(0);
        }
        CHECK(waitpid(c, 0, 0) == c, "cgroup: waitpid");
        CHECK(rmdir(p) == 0, "cgroup: rmdir");
        ops++;
    }
}

static void do_ipc(void)
{
    for (int i = 0; i < 10; i++) {
        int shm = shmget(IPC_PRIVATE, 64 * 1024, IPC_CREAT | 0600);
        if (shm < 0) {
            note_once("sysv shm", errno);
            return;
        }
        char *p = shmat(shm, 0, 0);
        CHECK(p != (char *)-1, "ipc: shmat");
        memset(p, i, 64 * 1024);
        CHECK(shmdt(p) == 0, "ipc: shmdt");
        CHECK(shmctl(shm, IPC_RMID, 0) == 0, "ipc: shmctl");
        int q = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
        if (q >= 0) {
            struct { long t; char d[16]; } m = {1, "hello"}, r;
            CHECK(msgsnd(q, &m, sizeof m.d, 0) == 0, "ipc: msgsnd");
            CHECK(msgrcv(q, &r, sizeof r.d, 0, 0) == sizeof r.d, "ipc: msgrcv");
            msgctl(q, IPC_RMID, 0);
        }
        int sem = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
        if (sem >= 0)
            semctl(sem, 0, IPC_RMID);
        ops++;
    }
}

static void do_notify(void)
{
    char path[96];
    snprintf(path, sizeof path, "%s/ino", dir);
    mkdir(path, 0755);
    for (int i = 0; i < 10; i++) {
        int in = inotify_init1(0);
        if (in < 0) {
            note_once("inotify", errno);
            break;
        }
        int wd = inotify_add_watch(in, path, IN_CREATE | IN_DELETE);
        CHECK(wd >= 0, "notify: add_watch");
        char f[128];
        snprintf(f, sizeof f, "%s/n", path);
        int fd = open(f, O_WRONLY | O_CREAT, 0644);
        close(fd);
        unlink(f);
        char buf[512];
        struct pollfd p = {in, POLLIN, 0};
        CHECK(poll(&p, 1, 2000) == 1, "notify: no event");
        CHECK(read(in, buf, sizeof buf) > 0, "notify: read");
        close(in);                       /* with the watch still on */
        ops++;
    }
    for (int i = 0; i < 10; i++) {
        int t = timerfd_create(CLOCK_MONOTONIC, 0);
        if (t < 0) {
            note_once("timerfd", errno);
            break;
        }
        struct itimerspec its = {{0, 0}, {0, 2000000}};
        timerfd_settime(t, 0, &its, 0);
        uint64_t n;
        CHECK(read(t, &n, 8) == 8 && n >= 1, "notify: timerfd");
        close(t);
        ops++;
    }
    rmdir(path);
}

static void do_pty(void)
{
    for (int i = 0; i < 6; i++) {
        int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
        if (m < 0) {
            note_once("pty", errno);
            return;
        }
        int n = 0, zero = 0;
        if (ioctl(m, TIOCSPTLCK, &zero) < 0 || ioctl(m, TIOCGPTN, &n) < 0) {
            close(m);
            note_once("pty ioctl", errno);
            return;
        }
        char sl[48];
        snprintf(sl, sizeof sl, "/dev/pts/%d", n);
        int s = open(sl, O_RDWR | O_NOCTTY);
        if (s < 0) {
            close(m);
            note_once("pty slave", errno);
            return;
        }
        CHECK(write(s, "hi\n", 3) == 3, "pty: write");
        char buf[16];
        struct pollfd p = {m, POLLIN, 0};
        CHECK(poll(&p, 1, 2000) == 1, "pty: poll");
        CHECK(read(m, buf, sizeof buf) > 0, "pty: read");
        close(s);
        close(m);
        ops++;
    }
}

/* ---- the round ---- */

typedef void (*step)(void);
static const struct { const char *name; step fn; } steps[] = {
    {"fork", do_fork}, {"exec", do_exec}, {"swarm", do_swarm}, {"die", do_die},
    {"mmap", do_mmap}, {"filemap", do_filemap}, {"pressure", do_pressure},
    {"files", do_files}, {"pipes", do_pipes}, {"threads", do_threads},
    {"unix", do_unix}, {"tcp", do_tcp}, {"udp", do_udp}, {"epoll", do_epoll},
    {"signals", do_signals}, {"container", do_container}, {"mounts", do_mounts},
    {"cgroup", do_cgroup}, {"ipc", do_ipc}, {"notify", do_notify}, {"pty", do_pty},
};
#define NSTEPS (int)(sizeof steps / sizeof steps[0])

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child"))
        return 0;
    int rounds = argc > 1 ? atoi(argv[1]) : 10;
    if (argc > 2)
        rng ^= (uint64_t)atoi(argv[2]) * 0x9e3779b97f4a7c15ull;
    if (argc > 3)
        mempct = atoi(argv[3]);
    snprintf(dir, sizeof dir, "/tmp/soak%d", (int)getpid());
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        printf("soak: FAIL mkdir %s (errno %d)\n", dir, errno);
        return 1;
    }
    for (int r = 1; r <= rounds && !failed; r++) {
        /* every step once, in a different order each round */
        int order[NSTEPS];
        for (int i = 0; i < NSTEPS; i++)
            order[i] = i;
        for (int i = NSTEPS - 1; i > 0; i--) {
            int j = rnd(i + 1), t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        for (int i = 0; i < NSTEPS && !failed; i++) {
            const char *only = getenv("SOAK_ONLY");     /* e.g. SOAK_ONLY=container,mounts */
            if (only && !strstr(only, steps[order[i]].name))
                continue;
            if (getenv("SOAK_TRACE"))
                printf("soak: round %d step %s\n", r, steps[order[i]].name);
            steps[order[i]].fn();
            fflush(stdout);
        }
        if (failed)
            break;
        printf("soak: round %d ok ops=%ld free=%ld cached=%ld fds=%d procs=%d\n", r, ops,
               kb_of("MemFree"), kb_of("Cached"), count_dir("/proc/self/fd", 0), count_dir("/proc", 1));
        fflush(stdout);
    }
    rmdir(dir);
    return failed;
}
