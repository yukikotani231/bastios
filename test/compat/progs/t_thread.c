/* Threads: pthread_create (clone with CLONE_VM), joining, mutexes and
 * condition variables (futex), thread-local storage. */
#include "compat.h"
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

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
    if (begin("exit_group while another thread runs keeps its code")) {
        int r = group_exit_code();
        if (r == 1) pass(); else if (r == 0) fail("fork or waitpid failed");
    }
    return done();
}
