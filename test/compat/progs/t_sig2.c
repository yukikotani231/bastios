/* Signal and memory corners LTP's system call tests found: waits that a
 * handler interrupts even with SA_RESTART, SA_RESETHAND, the alternate
 * signal stack, a stack that grows past its first pages, PROT_NONE
 * memory, the sigset size, and more than 16 processes at once. */
#include "compat.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>

static volatile int got;
static char *volatile where;   /* the pointer is volatile: a handler sets it, and the compiler must not assume otherwise */
static void h(int s) { char here; got = s; where = &here; }

/* Sends SIGUSR1 to the parent after 50 ms, from a child. */
static pid_t poke(void) {
    pid_t parent = getpid(), c = fork();
    if (c == 0) {
        usleep(200000);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    return c;
}

static void reap(pid_t c) { waitpid(c, 0, 0); }

/* Writes a byte to every page of a 512 KiB stack array, deep below the
 * pages a new process starts with. */
static int deep(void) {
    volatile char big[512 * 1024];
    for (size_t i = 0; i < sizeof big; i += 4096) big[i] = 1;
    return big[0] + big[sizeof big - 4096];
}

/* read(2) into a stack buffer the program never touched. */
static int deep_read(int fd) {
    char big[256 * 1024];
    return read(fd, big, sizeof big) == 3 && memcmp(big, "abc", 3) == 0;
}

int main(void) {
    setup();
    struct sigaction sa = {.sa_handler = h, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);
    pid_t c;

    EXPECT("SA_RESTART does not restart poll", ({ got = 0; c = poke(); int r = poll(0, 0, 5000); int e = errno; reap(c);
           r == -1 && e == EINTR && got == SIGUSR1; }));
    EXPECT("SA_RESTART does not restart nanosleep", ({ got = 0; c = poke(); struct timespec t = {5, 0};
           int r = nanosleep(&t, 0); int e = errno; reap(c); r == -1 && e == EINTR; }));
    EXPECT("SA_RESTART does not restart epoll_wait", ({ int ep = epoll_create1(0); got = 0; c = poke(); struct epoll_event ev;
           int r = epoll_wait(ep, &ev, 1, 5000); int e = errno; reap(c); close(ep); r == -1 && e == EINTR; }));
    EXPECT("epoll_pwait blocks its mask while it waits", ({ int ep = epoll_create1(0); int p[2]; pipe(p);
           struct epoll_event ev = {.events = EPOLLIN}; epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
           sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); got = 0;
           pid_t parent = getpid(); c = fork();
           if (c == 0) { usleep(50000); kill(parent, SIGUSR1); usleep(50000); write(p[1], "x", 1); _exit(0); }
           int r = epoll_pwait(ep, &ev, 1, 5000, &m); reap(c); close(ep); close(p[0]); close(p[1]);
           r == 1 && got == SIGUSR1; }));
    EXPECT("SA_RESETHAND runs the handler once", ({ struct sigaction r = {.sa_handler = h, .sa_flags = SA_RESETHAND};
           sigemptyset(&r.sa_mask); sigaction(SIGUSR2, &r, 0); got = 0; raise(SIGUSR2);
           struct sigaction o; sigaction(SIGUSR2, 0, &o); got == SIGUSR2 && o.sa_handler == SIG_DFL; }));
    EXPECT("SA_ONSTACK runs on the alternate stack", ({ static char alt[16384]; stack_t s = {.ss_sp = alt, .ss_size = sizeof alt}, o;
           struct sigaction r = {.sa_handler = h, .sa_flags = SA_ONSTACK}; sigemptyset(&r.sa_mask);
           sigaltstack(&s, 0) == 0 && sigaction(SIGUSR2, &r, 0) == 0 && raise(SIGUSR2) == 0 &&
           where >= alt && where < alt + sizeof alt && sigaltstack(0, &o) == 0 && o.ss_sp == alt && o.ss_flags == 0 &&
           (s.ss_flags = SS_DISABLE, sigaltstack(&s, 0)) == 0; }));
    EXPECT("sigaltstack smaller than MINSIGSTKSZ is ENOMEM", ({ static char a[64]; stack_t s = {.ss_sp = a, .ss_size = sizeof a};
           sigaltstack(&s, 0) == -1 && errno == ENOMEM; }));
    EXPECT("the stack grows to 512 KiB", deep() == 2);
    EXPECT("read into an untouched stack buffer", ({ int p[2]; pipe(p); write(p[1], "abc", 3); close(p[1]);
           int ok = deep_read(p[0]); close(p[0]); ok; }));
    EXPECT("a PROT_NONE page is EFAULT to a system call", ({ void *a = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int r = (int)syscall(SYS_rt_sigprocmask, SIG_BLOCK, a, 0, 8); int e = errno;   /* the call itself: glibc's sigprocmask reads the set first */ r == -1 && e == EFAULT && munmap(a, 4096) == 0; }));
    EXPECT("mprotect makes a PROT_NONE mapping usable", ({ char *a = mmap(0, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           mprotect(a + 4096, 4096, PROT_READ | PROT_WRITE) == 0 && (a[4096] = 7, a[4096] == 7) && munmap(a, 8192) == 0; }));
    EXPECT("mprotect PROT_NONE makes a page fault on read", ({ char *a = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           a[0] = 1; int st; mprotect(a, 4096, PROT_NONE) == 0 && ({ pid_t c = fork(); c == 0 ? (_exit(a[0]), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; }) && munmap(a, 4096) == 0; }));
    EXPECT("read() into a PROT_NONE buffer is EFAULT", ({ char *a = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int p[2]; pipe(p); write(p[1], "abc", 3); mprotect(a, 4096, PROT_NONE);
           int r = read(p[0], a, 3); int e = errno; close(p[0]); close(p[1]);
           r == -1 && e == EFAULT && munmap(a, 4096) == 0; }));
    EXPECT("write() from a PROT_NONE buffer is EFAULT", ({ char *a = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           int p[2]; pipe(p); mprotect(a, 4096, PROT_NONE);
           int r = write(p[1], a, 3); int e = errno; close(p[0]); close(p[1]);
           r == -1 && e == EFAULT && munmap(a, 4096) == 0; }));
    EXPECT("a PROT_NONE file mapping faults on read", ({ int fd = open("/tmp/protnone", O_RDWR | O_CREAT | O_TRUNC, 0644);
           write(fd, "hello", 5); char *a = mmap(0, 4096, PROT_NONE, MAP_SHARED, fd, 0); int st;
           a != MAP_FAILED && ({ pid_t c = fork(); c == 0 ? (_exit(a[0]), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; })
           && mprotect(a, 4096, PROT_READ) == 0 && a[0] == 'h' && munmap(a, 4096) == 0 && close(fd) == 0 && unlink("/tmp/protnone") == 0; }));
    EXPECT("a PROT_NONE MAP_SHARED page is still shared after mprotect", ({ int *a = mmap(0, 4096, PROT_NONE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
           int st; pid_t c = fork(); if (c == 0) { mprotect(a, 4096, PROT_READ | PROT_WRITE); a[0] = 42; _exit(0); }
           waitpid(c, &st, 0) == c && mprotect(a, 4096, PROT_READ) == 0 && a[0] == 42 && munmap(a, 4096) == 0; }));
    EXPECT("a forked child keeps a page PROT_NONE", ({ char *a = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           mprotect(a, 4096, PROT_NONE); int st; pid_t c = fork(); c == 0 ? (_exit(a[0]), 0) :
           c > 0 && waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV && munmap(a, 4096) == 0; }));
    EXPECT("mmap of a file with fd -1 is EBADF", mmap(0, 4096, PROT_WRITE, MAP_SHARED, -1, 0) == MAP_FAILED && errno == EBADF);
    EXPECT("mmap of length 0 with fd -1 is EBADF", syscall(SYS_mmap, 0, 0, PROT_WRITE, MAP_SHARED, -1, 0) == -1 && errno == EBADF);
    EXPECT("rt_sigprocmask with a wrong sigset size is EINVAL", ({ sigset_t m; sigemptyset(&m);
           syscall(SYS_rt_sigprocmask, SIG_BLOCK, &m, 0, 4) == -1 && errno == EINVAL; }));
    EXPECT("24 processes at once", ({ pid_t k[24]; int n = 0;
           for (int i = 0; i < 24; i++) { k[i] = fork(); if (k[i] == 0) { pause(); _exit(0); } if (k[i] > 0) n++; }
           for (int i = 0; i < 24; i++) if (k[i] > 0) { kill(k[i], SIGKILL); waitpid(k[i], 0, 0); }
           n == 24; }));
    EXPECT("sigqueue carries a 64-bit sigval whole", ({ static volatile unsigned long long gv; static volatile int gc;
           struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_flags = SA_SIGINFO;
           sa.sa_sigaction = (void (*)(int, siginfo_t *, void *)) 0;
           void hv(int s, siginfo_t *si, void *u) { gv = (unsigned long long) si->si_value.sival_ptr; gc = si->si_code; }
           sa.sa_sigaction = hv; sigaction(SIGUSR2, &sa, 0);
           union sigval v; v.sival_ptr = (void *) 0x123456789abcdef0ull;
           sigqueue(getpid(), SIGUSR2, v) == 0 && gv == 0x123456789abcdef0ull && gc == SI_QUEUE; }));
    return done();
}
