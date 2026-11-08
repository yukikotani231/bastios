/* Threads: pthread_create (clone with CLONE_VM), joining, mutexes and
 * condition variables (futex), thread-local storage. */
#include "compat.h"
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>

static int counter;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready;
static __thread int tls = 1;

static void *add(void *arg) {
    for (int i = 0; i < 10000; i++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
    }
    return arg;
}

static void *waiter(void *arg) {
    pthread_mutex_lock(&mu);
    while (!ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return arg;
}

/* Waits up to 5 s for `ready`, on the clock the condition variable uses. */
static pthread_cond_t tcv;
static void *timed_waiter(void *arg) {
    struct timespec ts;
    clock_gettime((long)arg, &ts);
    ts.tv_sec += 5;
    pthread_mutex_lock(&mu);
    int r = 0;
    while (!ready && r == 0) r = pthread_cond_timedwait(&tcv, &mu, &ts);
    pthread_mutex_unlock(&mu);
    return (void *)(long)r;
}

/* A timed wait ends as soon as it is signalled, not at its deadline. */
static int timed_wake(clockid_t clock) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, clock);
    pthread_cond_init(&tcv, &a);
    ready = 0;
    pthread_t t;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (pthread_create(&t, 0, timed_waiter, (void *)(long)clock) != 0) return 0;
    usleep(100000);
    pthread_mutex_lock(&mu);
    ready = 1;
    pthread_cond_signal(&tcv);
    pthread_mutex_unlock(&mu);
    void *r = (void *)1;
    pthread_join(t, &r);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_cond_destroy(&tcv);
    return r == 0 && t1.tv_sec - t0.tv_sec < 3;
}

static void *tlsf(void *arg) { tls = 2; return (void *)(long)tls; }

/* Field 20 of /proc/self/stat and the Threads: line of /proc/self/status
 * (Python reads them to warn about fork with threads running). */
static long stat_threads(void) {
    char b[1024]; long n = -1; FILE *f = fopen("/proc/self/stat", "r");
    if (f && fgets(b, sizeof b, f)) { char *p = strrchr(b, ')'); int i = 2;
        while (p && i < 20) { p = strchr(p + 1, ' '); i++; } if (p) n = atol(p + 1); }
    if (f) fclose(f); return n;
}
static long status_threads(void) {
    char b[256]; long n = -1; FILE *f = fopen("/proc/self/status", "r");
    while (f && fgets(b, sizeof b, f)) if (!strncmp(b, "Threads:", 8)) n = atol(b + 8);
    if (f) fclose(f); return n;
}
static int tp[2];
static void *blocked(void *a) { char c; (void)a; return (void *)(long)read(tp[0], &c, 1); }
static int count_threads(void) {
    pthread_t x;
    if (pipe(tp) || pthread_create(&x, 0, blocked, 0)) return 0;
    usleep(20000);
    long a = stat_threads(), b = status_threads();
    write(tp[1], "x", 1); pthread_join(x, 0); close(tp[0]); close(tp[1]);
    return a >= 2 && b >= 2;
}

/* /proc/self/task lists a number for each thread, and /proc/self/task/TID
 * holds that thread's files. */
static long other_tid;
static void *tid_thread(void *a) { char c; (void)a; other_tid = gettid(); return (void *)(long)read(tp[0], &c, 1); }
static int task_dir(void) {
    pthread_t x;
    other_tid = 0;
    if (pipe(tp) || pthread_create(&x, 0, tid_thread, 0)) return 0;
    usleep(20000);
    int n = 0, me = 0, other = 0, ok = 1;
    DIR *d = opendir("/proc/self/task");
    if (!d) ok = 0;
    for (struct dirent *e; d && (e = readdir(d)); ) {
        long v = atol(e->d_name);
        if (v <= 0) continue;
        n++; if (v == gettid()) me = 1; if (v == other_tid) other = 1;
    }
    if (d) closedir(d);
    char path[64], b[64]; struct stat st;
    snprintf(path, sizeof path, "/proc/self/task/%ld", other_tid);
    int isdir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    snprintf(path, sizeof path, "/proc/self/task/%ld/comm", other_tid);
    FILE *f = fopen(path, "r"); int comm = f && fgets(b, sizeof b, f); if (f) fclose(f);
    snprintf(path, sizeof path, "/proc/self/task/%ld/stat", other_tid);
    f = fopen(path, "r"); int sw = f && fgets(b, sizeof b, f) && atol(b) == other_tid; if (f) fclose(f);
    write(tp[1], "x", 1); pthread_join(x, 0); close(tp[0]); close(tp[1]);
    return ok && n == 2 && me && other && isdir && comm && sw;
}

/* A thread's descriptors and namespaces are its process's: Go reaches them as
 * /proc/self/task/TID/fd/N when there is no /proc/thread-self. */
static int task_fd(void) {
    int fd = open("/", O_RDONLY);
    if (fd < 0) return 0;
    char path[96], b[64];
    snprintf(path, sizeof path, "/proc/self/task/%ld/fd/%d", (long)gettid(), fd);
    ssize_t n = readlink(path, b, sizeof b - 1);
    int ok = n == 1 && b[0] == '/';
    snprintf(path, sizeof path, "/proc/self/task/%ld/ns/mnt", (long)gettid());
    n = readlink(path, b, sizeof b - 1);
    ok = ok && n > 5 && strncmp(b, "mnt:[", 5) == 0;
    snprintf(path, sizeof path, "/proc/%d/task/%ld/fd/%d", getpid(), (long)gettid(), fd);
    n = readlink(path, b, sizeof b - 1);
    close(fd);
    return ok && n == 1;
}

/* /proc/self/fd/N read from another thread after the process's chroot:
 * names inside the new root read from it ("/tt_sub", not "/tt_root/tt_sub"),
 * whichever thread reads them (runc reads its /sys/fs/cgroup descriptor back
 * this way). The descriptors are read through /proc/self/fd opened before
 * the chroot, as the root has no /proc. */
static int rl_pipe[2], rl_dfd, rl_ffd, rl_pfd;
static void *rl_reader(void *a) {
    (void)a;
    char c, b[64];
    if (read(rl_pipe[0], &c, 1) != 1) return 0;
    char n1[16], n2[16];
    snprintf(n1, sizeof n1, "%d", rl_dfd);
    snprintf(n2, sizeof n2, "%d", rl_ffd);
    ssize_t n = readlinkat(rl_pfd, n1, b, sizeof b - 1);
    int ok = n == 7 && memcmp(b, "/tt_sub", 7) == 0;
    n = readlinkat(rl_pfd, n2, b, sizeof b - 1);
    ok = ok && n == 5 && memcmp(b, "/tt_f", 5) == 0;
    return (void *)(long)ok;
}
static int fd_link_after_chroot(void) {
    pid_t c = fork();
    if (c == 0) {
        mkdir("tt_root", 0755);
        mkdir("tt_root/tt_sub", 0755);
        int f = open("tt_root/tt_f", O_CREAT | O_WRONLY, 0644);
        if (f < 0 || pipe(rl_pipe) != 0) _exit(2);
        close(f);
        rl_pfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
        pthread_t t;
        void *r = 0;
        if (rl_pfd < 0 || pthread_create(&t, 0, rl_reader, 0) != 0) _exit(3);
        usleep(20000);                    /* the reader runs, then waits */
        if (chroot("tt_root") != 0 || chdir("/") != 0) _exit(4);
        rl_dfd = open("/tt_sub", O_RDONLY | O_DIRECTORY);
        rl_ffd = open("/tt_f", O_RDONLY);
        if (rl_dfd < 0 || rl_ffd < 0 || write(rl_pipe[1], "x", 1) != 1) _exit(5);
        if (pthread_join(t, &r) != 0) _exit(6);
        _exit(r ? 0 : 1);
    }
    int st;
    int ok = c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    unlink("tt_root/tt_f");
    rmdir("tt_root/tt_sub");
    rmdir("tt_root");
    return ok;
}

/* A thread with a mount namespace of its own (runc's init thread after
 * unshare) chroots there: /proc/self/fd/N, read by that thread, names the
 * directory from its root, though the process's other threads keep theirs. */
static void *ns_reader(void *a) {
    (void)a;
    char b[64];
    if (unshare(CLONE_NEWNS) != 0 || chroot("tt_root") != 0 || chdir("/") != 0) return 0;
    int d = open("/tt_sub", O_RDONLY | O_DIRECTORY);
    if (d < 0) return 0;
    char n1[16];
    snprintf(n1, sizeof n1, "%d", d);
    ssize_t n = readlinkat(rl_pfd, n1, b, sizeof b - 1);
    return (void *)(long)(n == 7 && memcmp(b, "/tt_sub", 7) == 0);
}
static int fd_link_in_thread_ns(void) {
    pid_t c = fork();
    if (c == 0) {
        mkdir("tt_root", 0755);
        mkdir("tt_root/tt_sub", 0755);
        rl_pfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
        pthread_t t;
        void *r = 0;
        if (rl_pfd < 0 || pthread_create(&t, 0, ns_reader, 0) != 0) _exit(3);
        if (pthread_join(t, &r) != 0) _exit(6);
        _exit(r ? 0 : 1);
    }
    int st;
    int ok = c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    rmdir("tt_root/tt_sub");
    rmdir("tt_root");
    return ok;
}

/* A thread that has ended does not hand its id to the next one at once. */
static void *tid_of(void *a) { (void)a; return (void *)(long)gettid(); }
static int tids_differ(void) {
    long id[8];
    for (int i = 0; i < 8; i++) {
        pthread_t x; void *r;
        if (pthread_create(&x, 0, tid_of, 0) || pthread_join(x, &r)) return 0;
        id[i] = (long)r;
        for (int j = 0; j < i; j++) if (id[j] == id[i]) return 0;
    }
    return 1;
}

/* exit_group from one thread while another is running on a processor of
 * its own: the process must end with the code exit_group was given. */
static void *spin(void *arg) { for (volatile long *x = arg;; ) (*x)++; return 0; }
static int group_exit_code(void) {
    for (int round = 0; round < 10; round++) {
        pid_t c = fork();
        if (c == 0) {
            static volatile long n;
            pthread_t s;
            if (pthread_create(&s, 0, spin, (void *)&n) != 0) _exit(1);
            while (n < 1000) {}               /* the spinner is running */
            syscall(SYS_exit_group, 42);
        }
        int st = 0;
        if (c < 0 || waitpid(c, &st, 0) != c) return 0;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 42) { fail("round %d: status %#x", round, st); return -1; }
    }
    return 1;
}

/* A thread keeps using its descriptors while another thread of the process
 * spawns children (posix_spawn lends the address space to the child with
 * CLONE_VM | CLONE_VFORK): the descriptor must stay open for it. */
extern char **environ;
static volatile int spawning;
static volatile int bad_fd;
static void *fd_user(void *arg) {
    int fd = *(int *)arg;
    while (spawning) {
        if (fcntl(fd, F_GETFD) < 0) bad_fd++;
    }
    return arg;
}

/* clone3 (glibc's pthread_create and posix_spawn use it first). */
struct c3args { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls, set_tid, set_tid_size, cgroup; };
static void c3_handler(int s) { (void)s; }

/* A forked child with CLONE_CLEAR_SIGHAND sees default handlers (an
 * ignored signal stays ignored), the parent keeps its own, and a set_tid
 * request or a structure too short for the first version is refused. */
static int clone3_clear_sighand(void) {
    struct c3args a;
    memset(&a, 0, sizeof a);
    signal(SIGUSR1, c3_handler);
    signal(SIGUSR2, SIG_IGN);
    a.flags = 1ULL << 32;                           /* CLONE_CLEAR_SIGHAND */
    a.exit_signal = SIGCHLD;
    long r = syscall(435, &a, sizeof a);
    if (r == 0) {
        struct sigaction o;
        sigaction(SIGUSR1, 0, &o);
        int dfl = o.sa_handler == SIG_DFL;
        sigaction(SIGUSR2, 0, &o);
        _exit(dfl * 2 + (o.sa_handler == SIG_IGN));
    }
    int st = 0;
    if (r < 0 || waitpid(r, &st, 0) != r || !WIFEXITED(st) || WEXITSTATUS(st) != 3) return 0;
    struct sigaction o;
    sigaction(SIGUSR1, 0, &o);
    if (o.sa_handler != c3_handler) return 0;
    a.set_tid_size = 1;
    if (syscall(435, &a, sizeof a) != -1 || errno != E2BIG) return 0;
    a.set_tid_size = 0;
    if (syscall(435, &a, 40) != -1 || errno != EINVAL) return 0;
    a.flags = 0x800;                                /* CLONE_SIGHAND without CLONE_VM */
    if (syscall(435, &a, sizeof a) != -1 || errno != EINVAL) return 0;
    a.flags = 0;
    a.exit_signal = 256;
    return syscall(435, &a, sizeof a) == -1 && errno == EINVAL;
}

/* A clone3 fork child runs on its own copy and its exit status arrives. */
static int clone3_fork(void) {
    struct c3args a;
    memset(&a, 0, sizeof a);
    a.exit_signal = SIGCHLD;
    long r = syscall(435, &a, 64);
    if (r == 0) _exit(5);
    int st = 0;
    return r > 0 && waitpid(r, &st, 0) == r && WIFEXITED(st) && WEXITSTATUS(st) == 5;
}

static int spawn_keeps_fds(void) {
    int p[2];
    if (pipe(p)) return 0;
    spawning = 1;
    bad_fd = 0;
    pthread_t t;
    if (pthread_create(&t, 0, fd_user, &p[0])) return 0;
    for (int i = 0; i < 60; i++) {
        pid_t c;
        char *argv[] = { "/bin/true", 0 };
        if (posix_spawn(&c, "/bin/true", 0, 0, argv, environ)) { spawning = 0; return 0; }
        waitpid(c, 0, 0);
    }
    spawning = 0;
    pthread_join(t, 0);
    return bad_fd == 0;
}

/* fork while another thread writes two pages in turn, the lower one first:
 * the child's copy must be the parent's memory at one moment, as on Linux,
 * so the higher page never holds a newer count than the lower one. */
static volatile long *pair_lo, *pair_hi;
static volatile int pair_go;
static void *pair_writer(void *x) {
    (void)x;
    for (long i = 1; pair_go; i++) { *pair_lo = i; *pair_hi = i; }
    return 0;
}
static int fork_snapshot(void) {
    char *m = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return 0;
    pair_lo = (long *)m;
    pair_hi = (long *)(m + 2 * 4096);
    pair_go = 1;
    pthread_t w;
    if (pthread_create(&w, 0, pair_writer, 0) != 0) return 0;
    while (*pair_lo < 1000) {}            /* the writer is running */
    int bad = 0, rounds = 300;
    for (int i = 0; i < rounds && !bad; i++) {
        pid_t c = fork();
        if (c == 0) _exit(*pair_hi > *pair_lo ? 1 : 0);
        int st = 0;
        if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st)) { bad = -1; break; }
        if (WEXITSTATUS(st) != 0) bad = i + 1;
    }
    pair_go = 0;
    pthread_join(w, 0);
    munmap(m, 3 * 4096);
    if (bad > 0) { fail("fork %d: the child saw the higher page newer than the lower one", bad); return -1; }
    return bad == 0;
}

/* The process's CPU clock, times() and getrusage(RUSAGE_SELF) count every
   thread, those that ended too: a thread spins for 0.3 s of its own CPU
   time and is joined, and the process has used at least that much more
   than the main thread. */
static double cpu_clock(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *spin_cpu(void *a) {
    (void)a;
    while (cpu_clock(CLOCK_THREAD_CPUTIME_ID) < 0.3) {}
    return 0;
}
static int process_cpu(void) {
    pthread_t x;
    if (pthread_create(&x, 0, spin_cpu, 0) != 0 || pthread_join(x, 0) != 0) return 0;
    double proc = cpu_clock(CLOCK_PROCESS_CPUTIME_ID), self = cpu_clock(CLOCK_THREAD_CPUTIME_ID);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double ru_user = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
    if (proc - self < 0.25) { fail("CLOCK_PROCESS_CPUTIME_ID %.3f s, the main thread %.3f s", proc, self); return -1; }
    if (ru_user < 0.25) { fail("getrusage user time %.3f s", ru_user); return -1; }
    return 1;
}

/* A thread reads its process's memory files through the leader's id: the
 * leader's row does not hold the group's address space while this thread
 * runs, and the files must describe the group's memory all the same. */
static int count_lines(const char *path) {
    char b[4096];
    int fd = open(path, O_RDONLY), n = 0;
    ssize_t r;
    if (fd < 0) return -1;
    while ((r = read(fd, b, sizeof b)) > 0)
        for (ssize_t i = 0; i < r; i++) n += b[i] == '\n';
    close(fd);
    return n;
}
static long file_size(const char *path) {
    char b[4096];
    int fd = open(path, O_RDONLY);
    long n = 0;
    ssize_t r;
    if (fd < 0) return -1;
    while ((r = read(fd, b, sizeof b)) > 0) n += r;
    close(fd);
    return n;
}
static void *memfiles(void *arg) {
    (void)arg;
    char p[64];
    int ok = 1;
    snprintf(p, sizeof p, "/proc/%d/maps", getpid());
    ok = ok && count_lines(p) >= 4;               // the program, libc or the stack, the vDSO and more
    snprintf(p, sizeof p, "/proc/%d/cmdline", getpid());
    ok = ok && file_size(p) > 0;
    snprintf(p, sizeof p, "/proc/%d/auxv", getpid());
    ok = ok && file_size(p) > 0;
    snprintf(p, sizeof p, "/proc/%d/statm", getpid());
    FILE *f = fopen(p, "r");
    long size = 0;
    ok = ok && f && fscanf(f, "%ld", &size) == 1 && size > 0;
    if (f) fclose(f);
    return (void *)(long)ok;
}

/* Why the last share check failed, for its FAIL line. */
static char spin_note[160];
#define EXPECT_SHARE(name, f) do { \
    if (begin(name)) { \
        spin_note[0] = 0; \
        if (f()) pass(); else fail("%s: %s", #f, spin_note); \
    } \
} while (0)

/* oom_score of a process whose memory a thread touched, read by another
   process while that thread waits and then while it runs on a processor.
   (The group's address space is kept with the thread whose call was
   handled last, not with the leader.) */
static int oom_up[2], oom_down[2], oom_note[2];
static volatile int oom_stop;
static size_t oom_len;
static long read_score(pid_t p) {
    char path[64], b[32];
    snprintf(path, sizeof path, "/proc/%d/oom_score", (int)p);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    return strtol(b, 0, 10);
}
/* The state letter of thread tid of process pid (/proc/PID/task/TID/stat),
 * or 0. */
static char thread_state(pid_t pid, pid_t tid) {
    char path[64], b[512];
    snprintf(path, sizeof path, "/proc/%d/task/%d/stat", (int)pid, (int)tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    long n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    char *p = strrchr(b, ')');
    return p && p[1] == ' ' ? p[2] : 0;
}
static void *oom_toucher(void *a) {
    char c = 'a';
    pid_t tid = (pid_t)syscall(SYS_gettid);
    char *m = mmap(0, oom_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return 0;
    for (size_t i = 0; i < oom_len; i += 4096) m[i] = 1;
    if (write(oom_up[1], &tid, sizeof tid) != sizeof tid || read(oom_down[0], &c, 1) != 1 || write(oom_up[1], &c, 1) != 1) return 0;
    while (!oom_stop) ;
    munmap(m, oom_len);
    return (void *)1;
}
static int oom_score_of_threads(void) {
    long kb = 0;
    char b[4096];
    int fd = open("/proc/meminfo", O_RDONLY);
    long n = fd < 0 ? -1 : read(fd, b, sizeof b - 1);
    if (fd >= 0) close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    if (sscanf(b, "MemTotal: %ld", &kb) != 1) return 0;
    oom_len = (size_t)kb * 1024 / 16;               /* a sixteenth of the memory, 16 .. 512 MiB */
    if (oom_len < (16u << 20)) oom_len = 16u << 20;
    if (oom_len > (512u << 20)) oom_len = 512u << 20;
    if (pipe(oom_up) || pipe(oom_down) || pipe(oom_note)) return 0;
    fcntl(oom_note[0], F_SETFL, O_NONBLOCK);
    pid_t me = getpid();
    pid_t c = fork();
    if (c < 0) return 0;
    if (c == 0) {
        char x = 'a';
        pid_t tid;
        if (read(oom_up[0], &tid, sizeof tid) != sizeof tid) _exit(2);
        /* "Waits" means blocked, it in its read and the main thread in
         * waitpid: right after its write either may still be on a
         * processor (a busy host can hold it there), and while a thread of
         * the group runs its count is the last one taken. */
        for (int i = 0; i < 5000 && (thread_state(me, tid) != 'S' || thread_state(me, me) != 'S'); i++) usleep(1000);
        long waiting = read_score(me), own = read_score(getpid());
        if (write(oom_down[1], &x, 1) != 1 || read(oom_up[0], &x, 1) != 1) _exit(3);
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, 0);
        long running = read_score(me);
        char note[96];
        int len = snprintf(note, sizeof note, "waiting %ld, running %ld, the reader's own %ld", waiting, running, own);
        if (write(oom_note[1], note, len) != len) _exit(4);
        _exit(waiting > own && running > own ? 0 : 1);
    }
    pthread_t t;
    void *r = 0;
    int st = -1;
    if (pthread_create(&t, 0, oom_toucher, 0) != 0) { kill(c, SIGKILL); waitpid(c, 0, 0); return 0; }
    pid_t w = waitpid(c, &st, 0);
    oom_stop = 1;
    pthread_join(t, &r);
    long nn = read(oom_note[0], spin_note, sizeof spin_note - 1);
    spin_note[nn > 0 ? nn : 0] = 0;
    close(oom_up[0]); close(oom_up[1]); close(oom_down[0]); close(oom_down[1]); close(oom_note[0]); close(oom_note[1]);
    return w == c && WIFEXITED(st) && WEXITSTATUS(st) == 0 && r == (void *)1;
}


/* The checks below weigh each thread's time against the time the
 * processor ran them in all (a[0] + a[1]), not against the clock: a
 * virtual processor gets less than all of the clock from a busy host, and
 * its timer ticks, which CPU time is counted in, come late or merge. They
 * only ask that the processor ran a fifth of the clock. */

/* Nice values and processor masks: threads that spin on a processor they
 * are pinned to, with the nice value each one sets for itself. */
static volatile int spin_stop, spin_ready;
struct spin { int cpu, nice, away; pthread_t t; int pol, prio; };

static void *spinner(void *a) {
    struct spin *s = a;
    cpu_set_t c;
    CPU_ZERO(&c);
    CPU_SET(s->cpu, &c);
    if (setpriority(PRIO_PROCESS, 0, s->nice) != 0) s->away = -1;
    if (s->pol) {
        struct sched_param sp = { .sched_priority = s->prio };
        if (pthread_setschedparam(pthread_self(), s->pol, &sp) != 0) s->away = -1;
    }
    /* Ready before moving to the processor: there a thread of a higher
     * priority may never let this one run. (No barrier either: its last
     * step could fall to such a thread.) */
    __atomic_add_fetch(&spin_ready, 1, __ATOMIC_SEQ_CST);
    if (sched_setaffinity(0, sizeof c, &c) != 0) s->away = -1;
    while (!spin_stop)
        if (sched_getcpu() != s->cpu && s->away >= 0) s->away++;
    return 0;
}

static double thread_secs(pthread_t t) {
    clockid_t c;
    struct timespec ts;
    if (pthread_getcpuclockid(t, &c) != 0 || clock_gettime(c, &ts) != 0) return -1;
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Runs two spinners for `secs` seconds; their CPU times in a[0], a[1], and
 * the seconds that passed. 0 if a thread could not start or pin itself, or
 * ran on a processor its mask leaves out. */
static int spin_two(struct spin *s, double secs, double *a, double *wall) {
    spin_stop = 0;
    spin_ready = 0;
    for (int i = 0; i < 2; i++) {
        s[i].away = 0;
        if (pthread_create(&s[i].t, 0, spinner, &s[i]) != 0) return 0;
    }
    while (__atomic_load_n(&spin_ready, __ATOMIC_SEQ_CST) < 2)
        usleep(1000);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double b0 = thread_secs(s[0].t), b1 = thread_secs(s[1].t);
    usleep((useconds_t)(secs * 1e6));
    a[0] = thread_secs(s[0].t) - b0;
    a[1] = thread_secs(s[1].t) - b1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    spin_stop = 1;
    pthread_join(s[0].t, 0);
    pthread_join(s[1].t, 0);
    *wall = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (s[0].away || s[1].away)
        snprintf(spin_note, sizeof spin_note, "off the processor: %d and %d", s[0].away, s[1].away);
    return s[0].away == 0 && s[1].away == 0;
}

/* The last processor the mask allows, the way LTP's nice05 picks one. */
static int last_cpu(void) {
    cpu_set_t c;
    int n = -1;
    if (sched_getaffinity(0, sizeof c, &c) != 0) return -1;
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &c)) n = i;
    return n;
}

/* Two threads pinned to one processor share it: their CPU times add up to
 * no more than the time that passed, and each gets about half. */
static int pinned_share(void) {
    int c = last_cpu();
    struct spin s[2] = { { c, 0, 0, 0 }, { c, 0, 0, 0 } };
    double a[2], w;
    if (c < 0 || !spin_two(s, 1.5, a, &w)) return 0;
    double u = a[0] + a[1];
    if (u > w * 1.15 || u < w * 0.2 || a[0] < u * 0.25 || a[1] < u * 0.25) {
        snprintf(spin_note, sizeof spin_note, "cpu %d: %.3f s and %.3f s of %.3f s", c, a[0], a[1], w);
        return 0;
    }
    return 1;
}

/* Two threads pinned to one processor, at nice 0 and nice 5: the nice-0
 * one weighs 1024 to the other's 335 (Linux's weights), so it gets about
 * three times the time, and the other still runs. */
static int nice_share(void) {
    int c = last_cpu();
    struct spin s[2] = { { c, 0, 0, 0 }, { c, 5, 0, 0 } };
    double a[2], w;
    if (c < 0 || !spin_two(s, 2.0, a, &w)) return 0;
    if (a[0] + a[1] < w * 0.2 || a[0] < a[1] * 2 || a[1] < a[0] * 0.15) {
        snprintf(spin_note, sizeof spin_note, "nice 0: %.3f s, nice 5: %.3f s of %.3f s", a[0], a[1], w);
        return 0;
    }
    return 1;
}

/* Two SCHED_FIFO threads on one processor: while the one of priority 20
 * spins, the one of priority 10 does not run at all, even when the
 * real-time share is used up (only threads that are not real-time get the
 * rest). */
static int fifo_order(void) {
    int c = last_cpu();
    struct spin s[2] = { { c, 0, 0, 0, SCHED_FIFO, 20 }, { c, 0, 0, 0, SCHED_FIFO, 10 } };
    double a[2], w;
    if (c < 0 || !spin_two(s, 1.5, a, &w)) return 0;
    double u = a[0] + a[1];
    if (u < w * 0.2 || a[1] > u * 0.02) {
        snprintf(spin_note, sizeof spin_note, "priority 20: %.3f s, priority 10: %.3f s of %.3f s", a[0], a[1], w);
        return 0;
    }
    return 1;
}

/* Two SCHED_RR threads of one priority on one processor take turns. */
static int rr_share(void) {
    int c = last_cpu();
    struct spin s[2] = { { c, 0, 0, 0, SCHED_RR, 10 }, { c, 0, 0, 0, SCHED_RR, 10 } };
    double a[2], w;
    if (c < 0 || !spin_two(s, 1.5, a, &w)) return 0;
    double u = a[0] + a[1];
    if (u < w * 0.2 || a[0] < u * 0.25 || a[1] < u * 0.25) {
        snprintf(spin_note, sizeof spin_note, "%.3f s and %.3f s of %.3f s", a[0], a[1], w);
        return 0;
    }
    return 1;
}

/* A SCHED_FIFO thread that spins leaves a thread that is not real-time
 * on its processor about 5% of the time (sched_rt_runtime_us), not none. */
static int rt_share(void) {
    int c = last_cpu();
    struct spin s[2] = { { c, 0, 0, 0, SCHED_FIFO, 10 }, { c, 0, 0, 0, 0, 0 } };
    double a[2], w;
    if (c < 0 || !spin_two(s, 2.5, a, &w)) return 0;
    double u = a[0] + a[1];
    if (u < w * 0.2 || a[1] < u * 0.01 || a[1] > u * 0.15) {
        snprintf(spin_note, sizeof spin_note, "SCHED_FIFO: %.3f s, SCHED_OTHER: %.3f s of %.3f s", a[0], a[1], w);
        return 0;
    }
    return 1;
}

/* ITIMER_PROF is the process's: set by a thread that then waits, it runs
 * down with another thread's time and signals while that one spins. */
static volatile sig_atomic_t prof_fired;
static void on_prof(int s) { (void)s; prof_fired = 1; }
static void *prof_spin(void *a) {
    (void)a;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do clock_gettime(CLOCK_MONOTONIC, &t);
    while (!prof_fired && t.tv_sec - t0.tv_sec < 5);
    return 0;
}
static void *prof_setter(void *a) {
    (void)a;
    struct itimerval it = { { 0, 0 }, { 0, 100000 } };
    pthread_t sp;
    if (setitimer(ITIMER_PROF, &it, 0) != 0 || pthread_create(&sp, 0, prof_spin, 0) != 0) return 0;
    pthread_join(sp, 0);
    return (void *)1;
}
static int prof_other_thread(void) {
    pthread_t t;
    void *r = 0;
    prof_fired = 0;
    signal(SIGPROF, on_prof);
    if (pthread_create(&t, 0, prof_setter, 0) != 0 || pthread_join(t, &r) != 0 || r != (void *)1) return 0;
    signal(SIGPROF, SIG_DFL);
    return prof_fired;
}

/* FUTEX_WAKE of one wakes the waiter of the highest real-time priority,
 * not the one that waited first: a SCHED_FIFO 10 thread waits, then a
 * SCHED_FIFO 20 one, and the 20 one must come out first. */
#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129
#endif
static int fw_word, fw_ready, fw_n, fw_order[2];
static void *fw_waiter(void *a) {
    struct sched_param sp = { .sched_priority = (int)(long)a };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) return 0;
    __atomic_add_fetch(&fw_ready, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &fw_word, FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
    int i = __atomic_fetch_add(&fw_n, 1, __ATOMIC_SEQ_CST);
    if (i < 2) fw_order[i] = sp.sched_priority;
    return (void *)1;
}
static int futex_wake_prio(void) {
    pthread_t t[2];
    void *r0 = 0, *r1 = 0;
    fw_word = 0; fw_ready = 0; fw_n = 0; fw_order[0] = fw_order[1] = 0;
    struct timespec gap = { 0, 100000000 };
    for (int i = 0; i < 2; i++) {
        int want = i + 1;
        if (pthread_create(&t[i], 0, fw_waiter, (void *)(long)(i ? 20 : 10)) != 0) return 0;
        while (__atomic_load_n(&fw_ready, __ATOMIC_SEQ_CST) < want) usleep(1000);
        nanosleep(&gap, 0);                 /* now in FUTEX_WAIT */
    }
    long w1 = syscall(SYS_futex, &fw_word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    for (int i = 0; i < 1000 && __atomic_load_n(&fw_n, __ATOMIC_SEQ_CST) < 1; i++) usleep(1000);
    int first = fw_order[0];
    while (__atomic_load_n(&fw_n, __ATOMIC_SEQ_CST) < 2) {
        syscall(SYS_futex, &fw_word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
        usleep(1000);
    }
    pthread_join(t[0], &r0);
    pthread_join(t[1], &r1);
    return w1 == 1 && first == 20 && r0 == (void *)1 && r1 == (void *)1;
}

/* A SCHED_FIFO thread woken by a futex on another processor, where a
 * thread that is not real-time spins, runs at once: the waker interrupts
 * that processor instead of leaving the woken thread for its next tick.
 * Measured in that processor's own ticks, not the clock (a busy host
 * delays both): CPU time is counted per tick, so the spinner's CPU time
 * is the same just before the wake and when the woken thread runs only if
 * no tick came in between. Without the interrupt, every wake-up waits for
 * a tick, which charges the spinner, so none of 21 does. At least 3
 * must here: a busy host can hold back the interrupt past the tick now
 * and then (7 of 21 at worst, with the host's cores all busy). */
static int kw_word, kw_ack, kw_stop, kw_sleep;   /* kw_sleep: after the futex, sleep to kw_until and count from there */
static struct timespec kw_until;
static clockid_t kw_clk;
static long long kw_seen;
static long long clk_ns(clockid_t c) {
    struct timespec ts;
    if (clock_gettime(c, &ts) != 0) return -1;
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static void *kw_waiter(void *a) {
    int c = (int)(long)a;
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(c, &m);
    struct sched_param sp = { .sched_priority = 30 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0 || sched_setaffinity(0, sizeof m, &m) != 0) {
        __atomic_store_n(&kw_ack, -1, __ATOMIC_SEQ_CST);
        return 0;
    }
    int seen = 0;
    __atomic_store_n(&kw_ack, 0, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&kw_stop, __ATOMIC_SEQ_CST)) {
        syscall(SYS_futex, &kw_word, FUTEX_WAIT_PRIVATE, seen, 0, 0, 0);
        int now = __atomic_load_n(&kw_word, __ATOMIC_SEQ_CST);
        if (now == seen) continue;
        if (kw_sleep && !__atomic_load_n(&kw_stop, __ATOMIC_SEQ_CST))
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &kw_until, 0) != 0) {}
        kw_seen = clk_ns(kw_clk);
        seen = now;
        __atomic_store_n(&kw_ack, now, __ATOMIC_SEQ_CST);
    }
    return 0;
}
/* Spins without a system call (sched_getcpu would be one), so that only
 * an interrupt takes its processor from it. */
static void *kw_spin(void *a) {
    int c = (int)(long)a;
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(c, &m);
    if (sched_setaffinity(0, sizeof m, &m) != 0) return 0;
    __atomic_add_fetch(&spin_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&kw_stop, __ATOMIC_RELAXED)) {}
    return (void *)1;
}
static int rt_wake_how(int by_sleep) {
    int c = last_cpu();
    cpu_set_t all, other;
    if (c < 1 || sched_getaffinity(0, sizeof all, &all) != 0) return 0;
    CPU_ZERO(&other);
    CPU_SET(0, &other);
    if (sched_setaffinity(0, sizeof other, &other) != 0) return 0;
    spin_ready = 0;
    kw_word = 0; kw_stop = 0; kw_ack = -2; kw_sleep = by_sleep;
    pthread_t w, sp;
    int ok = 0, early = 0, done = 0;
    if (pthread_create(&sp, 0, kw_spin, (void *)(long)c) != 0) goto out;
    while (__atomic_load_n(&spin_ready, __ATOMIC_SEQ_CST) < 1) usleep(1000);
    if (pthread_getcpuclockid(sp, &kw_clk) != 0 || pthread_create(&w, 0, kw_waiter, (void *)(long)c) != 0) {
        kw_stop = 1;
        pthread_join(sp, 0);
        goto out;
    }
    while (__atomic_load_n(&kw_ack, __ATOMIC_SEQ_CST) == -2) usleep(1000);
    if (kw_ack == 0) {
        for (done = 0; done < 21; done++) {
            struct timespec gap = { 0, 30000000 };
            nanosleep(&gap, 0);             /* the waiter is back in FUTEX_WAIT */
            long long before = clk_ns(kw_clk);
            if (by_sleep) {
                /* It sleeps to 30 ms from now; once that has passed, a
                 * system call of this thread finds the sleep over, and
                 * the waiter must run before its processor's next tick. */
                long long until = clk_ns(CLOCK_MONOTONIC) + 30000000;
                kw_until.tv_sec = until / 1000000000;
                kw_until.tv_nsec = until % 1000000000;
            }
            __atomic_store_n(&kw_word, done + 1, __ATOMIC_SEQ_CST);
            syscall(SYS_futex, &kw_word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
            if (by_sleep) {
                long long until = kw_until.tv_sec * 1000000000LL + kw_until.tv_nsec;
                while (clk_ns(CLOCK_MONOTONIC) < until + 500000) {}
                before = clk_ns(kw_clk);
                sched_yield();
            }
            int n = 0;
            while (__atomic_load_n(&kw_ack, __ATOMIC_SEQ_CST) != done + 1 && n++ < 50000) usleep(100);
            if (kw_ack != done + 1) break;
            if (before >= 0 && kw_seen == before) early++;
        }
    }
    __atomic_store_n(&kw_stop, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&kw_word, 1000, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &kw_word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    pthread_join(w, 0);
    pthread_join(sp, 0);
    ok = done == 21 && early >= 3;
    snprintf(spin_note, sizeof spin_note, "%d of %d wake-ups came before the processor's next tick", early, done);
out:
    sched_setaffinity(0, sizeof all, &all);
    return ok;
}
static int rt_wake_now(void) { return rt_wake_how(0); }
static int rt_wake_sleep(void) { return rt_wake_how(1); }

/* Threads of one SCHED_FIFO priority go in the order they became ready,
 * and one that a thread of a higher priority takes the processor from
 * goes on first afterwards (the head of its queue), as on Linux. B spins
 * on the processor; A, woken after it, waits behind it; a higher thread
 * wakes, runs and blocks again: B must go on, and A must not run. */
static int fh_go, fh_hi, fh_stop, fh_ran_a, fh_hi_done;
static void fh_pin(int c, int prio) {
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(c, &m);
    struct sched_param sp = { .sched_priority = prio };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    sched_setaffinity(0, sizeof m, &m);
}
static void *fh_a(void *a) {
    fh_pin((int)(long)a, 10);
    while (!__atomic_load_n(&fh_go, __ATOMIC_SEQ_CST))
        syscall(SYS_futex, &fh_go, FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
    __atomic_store_n(&fh_ran_a, 1, __ATOMIC_SEQ_CST);
    return 0;
}
static void *fh_b(void *a) {
    fh_pin((int)(long)a, 10);
    __atomic_add_fetch(&spin_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&fh_stop, __ATOMIC_RELAXED)) {}
    return 0;
}
static void *fh_high(void *a) {
    fh_pin((int)(long)a, 20);
    while (!__atomic_load_n(&fh_hi, __ATOMIC_SEQ_CST))
        syscall(SYS_futex, &fh_hi, FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
    __atomic_store_n(&fh_hi_done, 1, __ATOMIC_SEQ_CST);
    return 0;                               /* it ends: the processor goes back to the queue of 10 */
}
static int fifo_head(void) {
    int c = last_cpu();
    cpu_set_t all, other;
    if (c < 1 || sched_getaffinity(0, sizeof all, &all) != 0) return 0;
    CPU_ZERO(&other);
    CPU_SET(0, &other);
    if (sched_setaffinity(0, sizeof other, &other) != 0) return 0;
    fh_go = fh_hi = fh_stop = fh_ran_a = fh_hi_done = 0;
    spin_ready = 0;
    pthread_t a, b, h;
    struct timespec gap = { 0, 50000000 };
    int ok = 0;
    /* The high one first and A before B, so that a round robin from the
     * high one's slot, as the scheduler did before, would find A first. */
    if (pthread_create(&h, 0, fh_high, (void *)(long)c) != 0) goto out;
    nanosleep(&gap, 0);                     /* the high one waits on fh_hi */
    if (pthread_create(&a, 0, fh_a, (void *)(long)c) != 0) goto out_h;
    nanosleep(&gap, 0);                     /* A waits on fh_go */
    if (pthread_create(&b, 0, fh_b, (void *)(long)c) != 0) goto out_a;
    while (__atomic_load_n(&spin_ready, __ATOMIC_SEQ_CST) < 1) usleep(1000);
    nanosleep(&gap, 0);                     /* B spins on c */
    __atomic_store_n(&fh_go, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &fh_go, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    nanosleep(&gap, 0);                     /* A is ready, behind B */
    int a_early = fh_ran_a;
    __atomic_store_n(&fh_hi, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &fh_hi, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    for (int i = 0; i < 100 && !fh_hi_done; i++) nanosleep(&gap, 0);
    struct timespec more = { 0, 200000000 };
    nanosleep(&more, 0);                    /* B should still have the processor */
    int a_after = fh_ran_a;
    ok = fh_hi_done && !a_early && !a_after;
    snprintf(spin_note, sizeof spin_note, "high ran %d; A ran before the high one %d, after it %d", fh_hi_done, a_early, a_after);
    __atomic_store_n(&fh_stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(b, 0);
out_a:
    __atomic_store_n(&fh_go, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &fh_go, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    pthread_join(a, 0);
out_h:
    __atomic_store_n(&fh_hi, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &fh_hi, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    pthread_join(h, 0);
out:
    sched_setaffinity(0, sizeof all, &all);
    return ok;
}

int main(void) {
    setup();
    pthread_t t[4];
    void *r = 0;

    EXPECT("pthread_create + join", pthread_create(&t[0], 0, add, (void *)5) == 0 && pthread_join(t[0], &r) == 0 && r == (void *)5);
    if (begin("4 threads, mutex")) {
        int ok = 1;
        counter = 0;
        for (int i = 0; i < 4; i++) ok &= pthread_create(&t[i], 0, add, 0) == 0;
        for (int i = 0; i < 4 && ok; i++) ok &= pthread_join(t[i], 0) == 0;
        if (ok && counter == 40000) pass(); else fail("counter %d", counter);
    }
    EXPECT("condition variable", pthread_create(&t[0], 0, waiter, 0) == 0 &&
           (usleep(50000), pthread_mutex_lock(&mu), ready = 1, pthread_cond_signal(&cv), pthread_mutex_unlock(&mu), 1) &&
           pthread_join(t[0], 0) == 0);
    EXPECT("timed condition wait, signalled early (realtime)", timed_wake(CLOCK_REALTIME));
    EXPECT("timed condition wait, signalled early (monotonic)", timed_wake(CLOCK_MONOTONIC));
    EXPECT("a thread reads the leader's maps, cmdline, auxv and statm and finds the group's memory",
           pthread_create(&t[0], 0, memfiles, 0) == 0 && pthread_join(t[0], &r) == 0 && r == (void *)1);
    EXPECT_SHARE("oom_score counts the memory a thread touched, while it waits and while it runs", oom_score_of_threads);
    EXPECT("thread-local storage", pthread_create(&t[0], 0, tlsf, 0) == 0 && pthread_join(t[0], &r) == 0 && r == (void *)2 && tls == 1);
    EXPECT("pthread_detach", pthread_create(&t[0], 0, tlsf, 0) == 0 && pthread_detach(t[0]) == 0);
    EXPECT("gettid differs in a thread", ({ pthread_t x; long other = 0;
           void *g(void *a) { (void)a; return (void *)(long)gettid(); }
           pthread_create(&x, 0, g, 0) == 0 && pthread_join(x, (void **)&other) == 0 && other != gettid(); }));
    EXPECT("/proc/self/stat and status count the threads", count_threads());
    EXPECT("/proc/self/task lists the threads, and /proc/self/task/TID has their files", task_dir());
    EXPECT("/proc/self/task/TID/fd/N and ns/ are the process's", task_fd());
    EXPECT("threads that ended and began one after another have different ids", tids_differ());
    EXPECT("/proc/self/fd/N read by another thread after a chroot names the file from the new root", fd_link_after_chroot());
    EXPECT("/proc/self/fd/N read by a thread in a mount namespace of its own names the file from its root", fd_link_in_thread_ns());
    EXPECT("a thread keeps its descriptors while another spawns a child", spawn_keeps_fds());
    EXPECT("clone3: CLONE_CLEAR_SIGHAND, and what it refuses", clone3_clear_sighand());
    EXPECT("clone3: a fork child", clone3_fork());
    if (begin("the process's CPU time counts its threads, those that ended too")) {
        int r = process_cpu();
        if (r == 1) pass(); else if (r == 0) fail("pthread_create or pthread_join failed");
    }
    if (begin("fork while another thread writes copies one moment of the parent")) {
        int r = fork_snapshot();
        if (r == 1) pass(); else if (r == 0) fail("mmap, pthread_create, fork or waitpid failed");
    }
    EXPECT_SHARE("two threads pinned to one processor run only there and share it", pinned_share);
    EXPECT_SHARE("a nice-5 thread gets about a third of the time of a nice-0 one on the same processor", nice_share);
    EXPECT_SHARE("a SCHED_FIFO thread of priority 20 keeps one of priority 10 on its processor from running", fifo_order);
    EXPECT_SHARE("two SCHED_RR threads of one priority take turns on one processor", rr_share);
    EXPECT_SHARE("a spinning SCHED_FIFO thread leaves a SCHED_OTHER thread on its processor a few percent", rt_share);
    EXPECT("ITIMER_PROF set by one thread runs down with another thread's time", prof_other_thread());
    EXPECT("FUTEX_WAKE wakes the waiter of the highest real-time priority first", futex_wake_prio());
    EXPECT_SHARE("a SCHED_FIFO thread woken on another processor runs there at once, not at its next tick", rt_wake_now);
    EXPECT_SHARE("a SCHED_FIFO thread whose sleep ends runs on its processor at once, not at its next tick", rt_wake_sleep);
    EXPECT_SHARE("SCHED_FIFO threads of one priority keep their order, and a preempted one goes on first", fifo_head);
    if (begin("exit_group while another thread runs keeps its code")) {
        int r = group_exit_code();
        if (r == 1) pass(); else if (r == 0) fail("fork or waitpid failed");
    }
    return done();
}
