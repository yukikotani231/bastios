/* Threads: pthread_create (clone with CLONE_VM), joining, mutexes and
 * condition variables (futex), thread-local storage. */
#include "compat.h"
#include <pthread.h>

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

static void *tlsf(void *arg) { tls = 2; return (void *)(long)tls; }

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
    EXPECT("thread-local storage", pthread_create(&t[0], 0, tlsf, 0) == 0 && pthread_join(t[0], &r) == 0 && r == (void *)2 && tls == 1);
    EXPECT("pthread_detach", pthread_create(&t[0], 0, tlsf, 0) == 0 && pthread_detach(t[0]) == 0);
    EXPECT("gettid differs in a thread", ({ pthread_t x; long other = 0;
           void *g(void *a) { (void)a; return (void *)(long)gettid(); }
           pthread_create(&x, 0, g, 0) == 0 && pthread_join(x, (void **)&other) == 0 && other != gettid(); }));
    return done();
}
