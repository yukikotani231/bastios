/* Threads: pthread_create (clone with CLONE_VM), joining, mutexes and
 * condition variables (futex), thread-local storage. */
#include "compat.h"
#include <fcntl.h>
#include <pthread.h>
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
    EXPECT("thread-local storage", pthread_create(&t[0], 0, tlsf, 0) == 0 && pthread_join(t[0], &r) == 0 && r == (void *)2 && tls == 1);
    EXPECT("pthread_detach", pthread_create(&t[0], 0, tlsf, 0) == 0 && pthread_detach(t[0]) == 0);
    EXPECT("gettid differs in a thread", ({ pthread_t x; long other = 0;
           void *g(void *a) { (void)a; return (void *)(long)gettid(); }
           pthread_create(&x, 0, g, 0) == 0 && pthread_join(x, (void **)&other) == 0 && other != gettid(); }));
    EXPECT("/proc/self/stat and status count the threads", count_threads());
    EXPECT("/proc/self/task lists the threads, and /proc/self/task/TID has their files", task_dir());
    EXPECT("/proc/self/task/TID/fd/N and ns/ are the process's", task_fd());
    EXPECT("threads that ended and began one after another have different ids", tids_differ());
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
    if (begin("exit_group while another thread runs keeps its code")) {
        int r = group_exit_code();
        if (r == 1) pass(); else if (r == 0) fail("fork or waitpid failed");
    }
    return done();
}
