/* Linux threads against musl's pthreads: clone(CLONE_THREAD | CLONE_VM
 * | CLONE_SETTLS | CLONE_CHILD_CLEARTID ...), futex waits and wakes behind
 * mutexes, condition variables and joins, set_tid_address, and exit_group
 * from a thread while others still wait. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
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

#define FAIL(...) do { printf("mthread: " __VA_ARGS__); printf("\n"); return 1; } while (0)

int main(void)
{
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
