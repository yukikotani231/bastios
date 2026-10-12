// A glibc-linked Linux arm64 program that exercises the system call ABI the
// kernel offers: memory, files, processes, signals, threads, pipes, epoll,
// sockets. Built static with the AArch64 cross compiler (see arch/aarch64/Makefile).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile int got;
static void h(int s) { got = s; }
static void *thr(void *a) { *(int *)a = 7; return a; }

static int part1(void) {
    int bad = 0;
    char *p = malloc(3 << 20);
    memset(p, 1, 3 << 20);
    printf("malloc ok %d\n", p[100]);
    void *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("mmap %s\n", m == MAP_FAILED ? "failed" : "ok");
    int fd = open("/tmp/t1.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "abc", 3);
    lseek(fd, 0, SEEK_SET);
    char b[8] = {0};
    read(fd, b, 3);
    close(fd);
    printf("file %s\n", b);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("time %s\n", ts.tv_nsec >= 0 ? "ok" : "bad");
    pid_t c = fork();
    if (c == 0) { printf("child\n"); _exit(5); }
    int st = 0;
    waitpid(c, &st, 0);
    printf("child exit %d\n", WEXITSTATUS(st));
    signal(SIGUSR1, h);
    raise(SIGUSR1);
    printf("signal %d\n", got);
    pthread_t t;
    int v = 0;
    pthread_create(&t, 0, thr, &v);
    pthread_join(t, 0);
    printf("thread %d\n", v);
    return bad;
}

#define CHECK(name, ok) printf("%s %s\n", (ok) ? "ok  " : "FAIL", name)

static int part2(void) {
    int p[2];
    CHECK("pipe", pipe(p) == 0);
    write(p[1], "xy", 2);
    struct pollfd pf = { p[0], POLLIN, 0 };
    CHECK("poll", poll(&pf, 1, 100) == 1 && (pf.revents & POLLIN));
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0x1122334455667788ull }, out = {0};
    CHECK("epoll_ctl", epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    int n = epoll_wait(ep, &out, 1, 100);
    CHECK("epoll_wait", n == 1 && out.data.u64 == 0x1122334455667788ull);
    int efd = eventfd(3, 0);
    unsigned long long v = 0;
    CHECK("eventfd", read(efd, &v, 8) == 8 && v == 3);
    int d = dup2(p[0], 20);
    CHECK("dup2", d == 20);
    char buf[4] = {0};
    CHECK("read pipe", read(20, buf, 2) == 2 && buf[0] == 'x');
    struct utsname u;
    CHECK("uname", uname(&u) == 0 && strlen(u.sysname) > 0);
    printf("  sysname %s machine %s\n", u.sysname, u.machine);
    DIR *dir = opendir("/");
    int cnt = 0;
    struct dirent *e;
    while (dir && (e = readdir(dir))) cnt++;
    CHECK("readdir", cnt > 2);
    struct stat st;
    CHECK("stat", stat("/etc/hosts", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
    CHECK("mkdir", mkdir("/tmp/dd", 0755) == 0);
    int f = open("/tmp/dd/f", O_CREAT | O_RDWR, 0644);
    CHECK("ftruncate", ftruncate(f, 5000) == 0);
    CHECK("fstat size", fstat(f, &st) == 0 && st.st_size == 5000);
    CHECK("rename", rename("/tmp/dd/f", "/tmp/dd/g") == 0);
    void *mp = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    CHECK("mmap file", mp != MAP_FAILED);
    close(f);
    CHECK("unlink", unlink("/tmp/dd/g") == 0 && rmdir("/tmp/dd") == 0);
    struct timeval tv;
    CHECK("gettimeofday", gettimeofday(&tv, 0) == 0);
    struct rlimit rl;
    CHECK("getrlimit", getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 0);
    char r[8];
    CHECK("getrandom", getrandom(r, 8, 0) == 8);
    struct timespec ts = { 0, 20000000 };
    CHECK("nanosleep", nanosleep(&ts, 0) == 0);
    char cwd[64];
    CHECK("getcwd", getcwd(cwd, sizeof cwd) != 0);
    struct termios t;
    printf("  termios %d errno %d\n", tcgetattr(0, &t), errno);
    int sv[2];
    CHECK("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    write(sv[0], "hi", 2);
    CHECK("socket read", read(sv[1], buf, 2) == 2);
    fd_set rs;
    FD_ZERO(&rs); FD_SET(p[0], &rs);
    struct timeval tmo = {0, 1000};
    printf("  select %d\n", select(p[0] + 1, &rs, 0, 0, &tmo));
    return 0;
}

int main(void) {
    part1();
    part2();
    printf("mtest done\n");
    return 0;
}
