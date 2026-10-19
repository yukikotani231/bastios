// SMP wake-up stress: each test runs ROUNDS times and reports a hang via alarm.
#define _GNU_SOURCE
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

static volatile long where;
static const char *test;
#include <dirent.h>
#include <fcntl.h>
static void dump_one(const char *path) {
    char b[1024]; int fd = open(path, O_RDONLY); if (fd < 0) return;
    int n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return; b[n] = 0;
    printf("  %s: %s", path, b); if (b[n - 1] != '\n') printf("\n");
}
static void on_alarm(int s) {
    (void)s;
    printf("HANG %s at %ld (pid %d)\n", test, where, getpid());
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[300];
        snprintf(p, sizeof p, "/proc/%s/stat", e->d_name); dump_one(p);
        snprintf(p, sizeof p, "/proc/%s/wchan", e->d_name); dump_one(p);
        snprintf(p, sizeof p, "/proc/%s/task", e->d_name);
        DIR *t = opendir(p); struct dirent *f;
        while (t && (f = readdir(t))) { if (f->d_name[0] < '0' || f->d_name[0] > '9' || !strcmp(f->d_name, e->d_name)) continue; char q[600]; snprintf(q, sizeof q, "/proc/%s/task/%s/stat", e->d_name, f->d_name); dump_one(q); snprintf(q, sizeof q, "/proc/%s/task/%s/wchan", e->d_name, f->d_name); dump_one(q); }
        if (t) closedir(t);
    }
    fflush(stdout);
    _exit(3);
}
static void arm(const char *t, int secs) { test = t; where = 0; alarm(secs); }

struct shm { sem_t a, b, lock; long count; };

static void t_sem(long rounds) {   // process ping-pong on shared semaphores
    struct shm *m = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    sem_init(&m->a, 1, 0); sem_init(&m->b, 1, 0);
    pid_t c = fork();
    if (c == 0) { alarm(0); for (long i = 0; i < rounds; i++) { sem_wait(&m->a); sem_post(&m->b); } _exit(0); }
    arm("sem", 120);
    for (long i = 0; i < rounds; i++) { where = i; sem_post(&m->a); sem_wait(&m->b); }
    waitpid(c, 0, 0); alarm(0);
    munmap(m, 4096);
}

static void t_lock(long rounds, int procs) {   // processes contend a shared lock
    struct shm *m = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    sem_init(&m->lock, 1, 1); m->count = 0;
    arm("lock", 180);
    pid_t k[8];
    for (int p = 0; p < procs; p++) {
        if ((k[p] = fork()) == 0) {
            for (long i = 0; i < rounds; i++) { sem_wait(&m->lock); long v = m->count; if ((i & 7) == 0) sched_yield(); m->count = v + 1; sem_post(&m->lock); }
            _exit(0);
        }
    }
    for (int p = 0; p < procs; p++) { where = p; waitpid(k[p], 0, 0); }
    alarm(0);
    if (m->count != rounds * procs) printf("LOST lock: %ld of %ld\n", m->count, rounds * procs);
    munmap(m, 4096);
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long turn_, rounds_g;
static void *pong(void *x) { (void)x; for (long i = 0; i < rounds_g; i++) { pthread_mutex_lock(&mu); while (turn_ != 1) pthread_cond_wait(&cv, &mu); turn_ = 0; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu); } return 0; }
static void t_cond(long rounds) {   // threads ping-pong on a condition variable
    rounds_g = rounds; turn_ = 0;
    pthread_t t; pthread_create(&t, 0, pong, 0);
    arm("cond", 120);
    for (long i = 0; i < rounds; i++) { where = i; pthread_mutex_lock(&mu); turn_ = 1; pthread_cond_signal(&cv); while (turn_ != 0) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu); }
    pthread_join(t, 0); alarm(0);
}

static volatile sig_atomic_t chld;
static void on_chld(int s) { (void)s; chld++; }
static void t_pselect(long rounds) {   // make's loop: SIGCHLD blocked, pselect unblocks it
    sigset_t blk, old; sigemptyset(&blk); sigaddset(&blk, SIGCHLD);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_chld; sigaction(SIGCHLD, &sa, 0);
    sigprocmask(SIG_BLOCK, &blk, &old);
    int p[2]; pipe(p);
    arm("pselect", 180);
    for (long i = 0; i < rounds; i++) {
        where = i;
        pid_t c = fork();
        if (c == 0) _exit(0);
        int got = 0;
        while (!got) {
            if (waitpid(c, 0, WNOHANG) == c) { got = 1; break; }
            fd_set r; FD_ZERO(&r); FD_SET(p[0], &r);
            pselect(p[0] + 1, &r, 0, 0, 0, &old);   // no timeout: only SIGCHLD ends it
        }
    }
    alarm(0);
    sigprocmask(SIG_SETMASK, &old, 0);
    close(p[0]); close(p[1]);
    signal(SIGCHLD, SIG_DFL);
}

static void t_pipe(long rounds) {   // process ping-pong over two pipes
    int a[2], b[2]; pipe(a); pipe(b);
    pid_t c = fork();
    char x = 0;
    if (c == 0) { alarm(0); for (long i = 0; i < rounds; i++) { if (read(a[0], &x, 1) != 1) _exit(1); write(b[1], &x, 1); } _exit(0); }
    arm("pipe", 120);
    for (long i = 0; i < rounds; i++) { where = i; write(a[1], &x, 1); if (read(b[0], &x, 1) != 1) break; }
    waitpid(c, 0, 0); alarm(0);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

int main(int argc, char **argv) {
    signal(SIGALRM, on_alarm);
    long n = argc > 2 ? atol(argv[2]) : 2000;
    int iters = argc > 3 ? atoi(argv[3]) : 1;
    const char *which = argc > 1 ? argv[1] : "all";
    if (!strcmp(which, "dump")) { test = "dump"; on_alarm(0); }
    for (int it = 0; it < iters; it++) {
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        if (!strcmp(which, "all") || strstr(which, "sem")) t_sem(n);
        if (!strcmp(which, "all") || strstr(which, "lock")) t_lock(n / 4, 3);
        if (!strcmp(which, "all") || strstr(which, "cond")) t_cond(n);
        if (!strcmp(which, "all") || strstr(which, "psel")) t_pselect(n / 20);
        if (!strcmp(which, "all") || strstr(which, "pipe")) t_pipe(n);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("iter %d ok %.1fs\n", it, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
        fflush(stdout);
    }
    return 0;
}
