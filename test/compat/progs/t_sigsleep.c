/* A signal sent to a thread that is running on another processor, and that
 * goes to sleep in a futex wait before its next trap, is not lost: the
 * thread wakes, runs the handler, and goes on (as musl's setuid, which signals
 * every thread and waits for all of them, relies on). */
#include "compat.h"
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>

static volatile int word;
static volatile int flag;
static volatile int stop;

static void handler(int sig) { (void)sig; flag = 1; }

static void *worker(void *arg) {
    (void)arg;
    unsigned x = 12345;
    while (!stop) {
        x = x * 1103515245 + 12345;
        for (volatile unsigned i = 0; i < (x >> 16) % 300; i++) {}
        syscall(SYS_futex, &word, 128 /* FUTEX_WAIT_PRIVATE */, 0, NULL, NULL, 0);
    }
    return NULL;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void) {
    setup();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigaction(SIGUSR1, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    struct timespec nap = { 0, 1000000 };
    nanosleep(&nap, NULL);
    int lost = 0, rounds = 0;
    double start = now();
    while (rounds < 4000 && now() - start < 20) {
        flag = 0;
        pthread_kill(t, SIGUSR1);
        double sent = now();
        while (!flag && now() - sent < 3) {
            struct timespec w = { 0, 200000 };
            nanosleep(&w, NULL);
        }
        if (!flag) { lost = 1; break; }
        rounds++;
    }
    EXPECT("a signal sent to a thread about to sleep in a futex wait runs its handler", !lost && rounds > 0);
    if (lost) return done();
    stop = 1;
    pthread_kill(t, SIGUSR1);
    pthread_join(t, NULL);
    return done();
}
