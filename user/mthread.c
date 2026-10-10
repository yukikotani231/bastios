/* Linux threads against musl's pthreads: clone(CLONE_THREAD | CLONE_VM
 * | CLONE_SETTLS | CLONE_CHILD_CLEARTID ...), futex waits and wakes behind
 * mutexes, condition variables and joins, set_tid_address, and exit_group
 * from a thread while others still wait. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int counter;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready;
static __thread int tls = 1;

static void *add(void *arg)
{
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&mu);
        int c = counter;
        sched_yield();                /* another thread may take its turn here */
        counter = c + 1;
        pthread_mutex_unlock(&mu);
    }
    return arg;
}

static void *waiter(void *arg)
{
    pthread_mutex_lock(&mu);
    while (!ready)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return arg;
}

static void *ids(void *arg)
{
    (void)arg;
    tls = 2;
    return (void *)(long)(gettid() * 1000 + (getpid() == *(int *)arg) + 10 * tls);
}

static void *sleeper(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    for (;;)
        pthread_cond_wait(&cv, &mu);  /* never signalled */
}

static void *ender(void *arg)
{
    (void)arg;
    usleep(20000);
    _exit(7);                         /* exit_group: every thread ends */
}

/* `mthread par`: four threads of pure computation, timed. How much faster
 * four processors run them than one (test/threadbench.sh) shows whether
 * the threads of a process really run at the same time. */
static void *spin(void *arg)
{
    unsigned long x = (unsigned long)arg | 1;
    /* Memory mapped while the other threads run (malloc asks the kernel
     * for pages), written and read back. */
    unsigned long *m = malloc(256 * 1024);
    if (!m)
        return 0;
    for (long i = 0; i < 30000000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        m[i & 32767] = x;
    }
    for (long i = 0; i < 32768; i++)
        x += m[i];
    free(m);
    return (void *)x;
}

static int par(void)
{
    pthread_t t[4];
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], 0, spin, (void *)(long)(i + 1));
    unsigned long sum = 0;
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (unsigned long)r;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    printf("mthread: par 4 threads %ld ms (%lx)\n", ms, sum & 0xfff);
    return 0;
}

/* `mthread exec`: execve while two other threads are running (on other
 * processors, with SMP): the kernel ends them first. */
static void *forever(void *arg)
{
    volatile unsigned long *n = arg;
    for (;;)
        (*n)++;
    return 0;
}

static int exec_threads(void)
{
    static volatile unsigned long n[2];
    pthread_t t;
    for (int i = 0; i < 2; i++)
        pthread_create(&t, 0, forever, (void *)&n[i]);
    while (n[0] == 0 || n[1] == 0)
        ;
    char *av[] = { "mthread", "done", 0 };
    execv("/bin/mthread", av);
    printf("mthread: execv failed\n");
    return 1;
}

/* `mthread unmap`: munmap, mprotect and mremap while two other threads
 * run (on other processors, with SMP): the kernel waits for them to leave
 * the processors before it changes the shared page tables, and they run
 * on afterwards. */
static int unmap_threads(void)
{
    static volatile unsigned long n[2];
    pthread_t t;
    for (int i = 0; i < 2; i++)
        pthread_create(&t, 0, forever, (void *)&n[i]);
    while (n[0] == 0 || n[1] == 0)
        ;
    for (int i = 0; i < 20; i++) {
        char *p = mmap(0, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("mthread: mmap failed\n");
            return 1;
        }
        p[0] = 1;
        p[3 * 4096] = 2;
        if (mprotect(p, 4 * 4096, PROT_READ) || p[0] != 1 || mprotect(p, 4 * 4096, PROT_READ | PROT_WRITE)) {
            printf("mthread: mprotect failed\n");
            return 1;
        }
        p[4096] = 3;
        char *q = mremap(p, 4 * 4096, 8 * 4096, MREMAP_MAYMOVE);
        if (q == MAP_FAILED || q[0] != 1 || q[4096] != 3 || q[3 * 4096] != 2) {
            printf("mthread: mremap failed\n");
            return 1;
        }
        if (munmap(q, 8 * 4096)) {
            printf("mthread: munmap failed\n");
            return 1;
        }
    }
    unsigned long a = n[0], b = n[1];
    while (n[0] == a || n[1] == b)
        ;
    printf("mthread: unmap ok, threads still running\n");
    return 0;
}

#define FAIL(...) do { printf("mthread: " __VA_ARGS__); printf("\n"); return 1; } while (0)

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == 'p')
        return par();
    if (argc > 1 && argv[1][0] == 'e')
        return exec_threads();
    if (argc > 1 && argv[1][0] == 'u')
        return unmap_threads();
    if (argc > 1 && argv[1][0] == 'd') {
        printf("mthread: exec ok, threads gone\n");
        return 0;
    }
    pthread_t t[4];
    void *r = 0;

    if (pthread_create(&t[0], 0, add, (void *)5) || pthread_join(t[0], &r) || r != (void *)5)
        FAIL("create + join");
    counter = 0;
    for (int i = 0; i < 4; i++)
        if (pthread_create(&t[i], 0, add, 0))
            FAIL("create %d", i);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], 0);
    if (counter != 8000)
        FAIL("mutex: counter %d", counter);

    if (pthread_create(&t[0], 0, waiter, 0))
        FAIL("create waiter");
    usleep(20000);
    pthread_mutex_lock(&mu);
    ready = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    if (pthread_join(t[0], 0))
        FAIL("condition variable");

    int me = getpid();
    if (pthread_create(&t[0], 0, ids, &me) || pthread_join(t[0], &r))
        FAIL("ids");
    long v = (long)r;
    if (v / 1000 == gettid() || v % 10 != 1 || v / 10 % 10 != 2 || tls != 1)
        FAIL("ids: %ld (pid %d tid %d tls %d)", v, me, gettid(), tls);

    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 30000000;
    if (dl.tv_nsec >= 1000000000) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&mu);
    int e = pthread_cond_timedwait(&cv, &mu, &dl);
    pthread_mutex_unlock(&mu);
    if (e != ETIMEDOUT)
        FAIL("timed wait gave %d", e);

    if (pthread_create(&t[0], 0, ids, &me) || pthread_detach(t[0]))
        FAIL("detach");

    /* A child whose thread calls _exit while the others wait. */
    pid_t c = fork();
    if (c == 0) {
        pthread_create(&t[1], 0, sleeper, 0);
        pthread_create(&t[2], 0, ender, 0);
        pthread_join(t[1], 0);
        return 1;
    }
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 7)
        FAIL("exit_group from a thread: status %x", st);
    printf("mthread: ok, %d increments\n", counter);
    return 0;
}
