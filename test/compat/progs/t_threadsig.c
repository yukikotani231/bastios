/* A signal sent to a thread, the way Go's runtime changes ids in every
 * thread (AllThreadsSyscall) and musl's setuid does (__synccall): tgkill
 * signal 33 at a thread and wait for its handler. The thread is in a state a
 * runtime leaves threads in: waiting on a futex (a condition variable), in
 * epoll_wait, in nanosleep, in a blocking read, or running. Then the
 * sender has dropped root already, as Go's setuid does in the thread that
 * calls it first, and the others are still root. */
#include "compat.h"
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>

/* Go's runtime sends signal 33 (its _SIGRTMIN + 1); musl's SIGRTMIN is 35. */
#define SIG_PER_THREAD 33
#define NT 5
static atomic_int hit[NT + 1];
static pid_t tid[NT + 1];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int stop, ready;
static int pfd[2];
static atomic_int spin;

static void handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    int me = (int)syscall(SYS_gettid);
    for (int i = 0; i <= NT; i++) if (tid[i] == me) atomic_fetch_add(&hit[i], 1);
}

/* The signal return trampoline (rt_sigreturn), which the C library would supply. */
#if defined(__x86_64__)
#define SA_RESTORER_FLAG 0x04000000
__attribute__((naked)) static void restore(void) { __asm__ volatile("mov $15, %eax; syscall"); }
#else                               /* arm64 has no SA_RESTORER: the kernel returns through its own */
#define SA_RESTORER_FLAG 0
static void restore(void) {}
#endif

static void *worker(void *arg) {
    long k = (long)arg;
    tid[k] = (pid_t)syscall(SYS_gettid);
    pthread_mutex_lock(&mu); ready++; pthread_mutex_unlock(&mu);
    if (k == 0) {                                   /* futex wait */
        pthread_mutex_lock(&mu);
        while (!stop) pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
    } else if (k == 1) {                            /* epoll_wait */
        int ep = epoll_create1(0);
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = pfd[0] }, out;
        epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev);
        while (!stop) epoll_wait(ep, &out, 1, 100);
    } else if (k == 2) {                            /* nanosleep */
        while (!stop) { struct timespec t = { 0, 50 * 1000 * 1000 }; nanosleep(&t, 0); }
    } else if (k == 3) {                            /* a blocking read */
        char c;
        while (!stop) if (read(pfd[0], &c, 1) < 0 && errno != EINTR) break;
    } else {                                        /* running */
        while (!stop) atomic_fetch_add(&spin, 1);
    }
    return 0;
}

static int installed;
static int hear(int kind) {      /* kind: -1 the calling thread alone, else one waiting thread */
    if (!installed) {
        /* musl keeps signals 32 to 34 for itself and refuses sigaction on them,
         * as Go's runtime does not: install the handler by the system call. */
        struct { void *handler; unsigned long flags; void *restorer; unsigned long mask; } ksa =
            { (void *)handler, 4 /* SA_SIGINFO */ | SA_RESTORER_FLAG /* SA_RESTORER */ | 0x10000000 /* SA_RESTART */,
              (void *)restore, ~0UL };
        if (syscall(SYS_rt_sigaction, SIG_PER_THREAD, &ksa, 0, 8)) return 0;
        if (pipe(pfd)) return 0;
        tid[NT] = (pid_t)syscall(SYS_gettid);
        installed = 1;
    }
    for (int i = 0; i <= NT; i++) atomic_store(&hit[i], 0);
    stop = 0; ready = 0;
    pthread_t th;
    int target = NT;
    if (kind >= 0) {
        pthread_create(&th, 0, worker, (void *)(long)kind);
        for (;;) { pthread_mutex_lock(&mu); int r = ready; pthread_mutex_unlock(&mu); if (r == 1) break; usleep(1000); }
        usleep(200 * 1000);                         /* let it reach its wait */
        target = kind;
    }
    syscall(SYS_tgkill, getpid(), tid[target], SIG_PER_THREAD);
    int ok = 0;
    for (int t = 0; t < 300 && !ok; t++) {
        ok = atomic_load(&hit[target]) >= 1;
        if (!ok) usleep(10 * 1000);
    }
    if (kind >= 0) {
        pthread_mutex_lock(&mu); stop = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
        char c = 'x'; write(pfd[1], &c, 1);
        pthread_join(th, 0);
        char d[8]; fcntl(pfd[0], F_SETFL, O_NONBLOCK); while (read(pfd[0], d, 8) > 0) {}
        fcntl(pfd[0], F_SETFL, 0);
    }
    return ok;
}

/* A thread that has given up root (the system call changes the calling
 * thread's ids only) signals the thread that started it, still root:
 * a thread may signal its own thread group whatever the ids. */
static pid_t main_tid;
static atomic_int sent_rc = -2;

static void *dropper(void *arg) {
    (void)arg;
    if (syscall(SYS_setuid, 65534) != 0) { sent_rc = -3; return 0; }
    sent_rc = (int)syscall(SYS_tgkill, getpid(), main_tid, SIG_PER_THREAD);
    return 0;
}

static int unprivileged_sibling_signals(void) {
    main_tid = tid[NT];
    atomic_store(&hit[NT], 0);
    pthread_t th;
    pthread_create(&th, 0, dropper, 0);
    pthread_join(th, 0);
    for (int t = 0; t < 300 && atomic_load(&hit[NT]) < 1; t++) usleep(10 * 1000);
    return sent_rc == 0 && atomic_load(&hit[NT]) >= 1 && getuid() == 0;
}

int main(void) {
    setup();
    EXPECT("signal 33 reaches the thread that sends it", hear(-1));
    EXPECT("signal 33 reaches a thread waiting on a futex", hear(0));
    EXPECT("signal 33 reaches a thread in epoll_wait", hear(1));
    EXPECT("signal 33 reaches a thread in nanosleep", hear(2));
    EXPECT("signal 33 reaches a thread blocked in read", hear(3));
    EXPECT("signal 33 reaches a thread that is running", hear(4));
    EXPECT("a thread that has dropped root signals a sibling that has not", unprivileged_sibling_signals());
    return done();
}
