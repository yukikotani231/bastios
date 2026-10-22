// osbench: operating system micro-benchmarks that run unchanged on BastiOS
// and on Linux (a static musl program), so bench/run.py can boot both in the
// same QEMU configuration and compare them.
//
//     osbench [-q] TEST [ARGS]    run one test
//     osbench -l                  list the tests
//
// Each test prints "@begin NAME" before its timed part and
// "@end NAME OPS GUEST_NS" after it: OPS operations took GUEST_NS by the
// guest's CLOCK_MONOTONIC. bench/run.py also timestamps the two lines on the
// host, which is the number it reports, since a guest clock under emulation
// is only as good as the kernel's calibration. A test that cannot run prints
// "@skip NAME REASON" instead.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const char *self = "/proc/self/exe";
static char dir[256] = "/tmp";

static uint64_t now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t t0;
static const char *cur;

static void begin(const char *name) {
  cur = name;
  printf("@begin %s\n", name);
  fflush(stdout);
  t0 = now();
}

static void end(long ops) {
  uint64_t dt = now() - t0;
  printf("@end %s %ld %llu\n", cur, ops, (unsigned long long)dt);
  fflush(stdout);
}

// write() may take less than it was given; this keeps going.
static int write_all(int fd, const char *b, size_t n) {
  while (n) {
    ssize_t w = write(fd, b, n);
    if (w <= 0) return -1;
    b += w;
    n -= (size_t)w;
  }
  return 0;
}

static int skip(const char *name, const char *why) {
  printf("@skip %s %s (errno %d)\n", name, why, errno);
  fflush(stdout);
  return 0;
}

// A loop with no system calls: how fast the emulated processor runs here.
// Every other number is best read against this one.
static volatile uint64_t sink;
static int t_cpu(long n) {
  begin("cpu");
  uint64_t x = 1;
  for (long i = 0; i < n; i++) x = x * 6364136223846793005u + 1442695040888963407u;
  sink = x;
  end(n);
  return 0;
}

// Memory bandwidth inside one process, on pages already mapped. The two
// buffers stay small: under TCG a working set beyond QEMU's software TLB
// (which grows only when the guest flushes it) costs ten times as much.
static int t_memcpy(long n) {
  size_t sz = 256 << 10;
  char *a = malloc(sz), *b = malloc(sz);
  if (!a || !b) return skip("memcpy", "malloc");
  memset(a, 1, sz);
  memset(b, 2, sz);
  begin("memcpy");
  for (long i = 0; i < n; i++) memcpy(i & 1 ? a : b, i & 1 ? b : a, sz);
  end(n);
  return 0;
}

// The cheapest system call: kernel entry and exit.
static int t_syscall(long n) {
  begin("syscall");
  for (long i = 0; i < n; i++) syscall(SYS_getppid);
  end(n);
  return 0;
}

// clock_gettime as programs call it (the vDSO where there is one).
static int t_clock(long n) {
  struct timespec ts;
  begin("clock");
  for (long i = 0; i < n; i++) clock_gettime(CLOCK_MONOTONIC, &ts);
  end(n);
  return 0;
}

// Opening and closing an existing file: path walk plus descriptor work.
static int t_open(long n) {
  char p[300];
  snprintf(p, sizeof p, "%s/osbench.open", dir);
  int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return skip("open", "create");
  close(fd);
  begin("open");
  for (long i = 0; i < n; i++) {
    fd = open(p, O_RDONLY);
    if (fd < 0) return skip("open", "open");
    close(fd);
  }
  end(n);
  unlink(p);
  return 0;
}

static int t_stat(long n) {
  char p[300];
  struct stat st;
  snprintf(p, sizeof p, "%s/osbench.stat", dir);
  int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return skip("stat", "create");
  close(fd);
  begin("stat");
  for (long i = 0; i < n; i++) stat(p, &st);
  end(n);
  unlink(p);
  return 0;
}

// fork, child exits at once, parent waits.
static int t_fork(long n) {
  begin("fork");
  for (long i = 0; i < n; i++) {
    pid_t p = fork();
    if (p < 0) return skip("fork", "fork");
    if (p == 0) _exit(0);
    waitpid(p, 0, 0);
  }
  end(n);
  return 0;
}

// fork with 8 MiB of touched heap in the parent: what copying costs.
static int t_forkbig(long n) {
  size_t sz = 8 << 20;
  char *m = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return skip("forkbig", "mmap");
  memset(m, 1, sz);
  begin("forkbig");
  for (long i = 0; i < n; i++) {
    pid_t p = fork();
    if (p < 0) return skip("forkbig", "fork");
    if (p == 0) _exit(0);
    waitpid(p, 0, 0);
  }
  end(n);
  munmap(m, sz);
  return 0;
}

// fork + execve of this program (which exits at once) + wait: how a shell
// starts a command.
static int t_exec(long n) {
  char *argv[] = {"osbench", "-x", 0};
  begin("exec");
  for (long i = 0; i < n; i++) {
    pid_t p = fork();
    if (p < 0) return skip("exec", "fork");
    if (p == 0) {
      execve(self, argv, environ);
      _exit(127);
    }
    int st;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return skip("exec", "child failed");
  }
  end(n);
  return 0;
}

// posix_spawn (vfork-style) + wait.
static int t_spawn(long n) {
  char *argv[] = {"osbench", "-x", 0};
  begin("spawn");
  for (long i = 0; i < n; i++) {
    pid_t p;
    if (posix_spawn(&p, self, 0, 0, argv, environ) != 0) return skip("spawn", "posix_spawn");
    int st;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return skip("spawn", "child failed");
  }
  end(n);
  return 0;
}

// Fresh anonymous memory: map N MiB, touch every page, unmap. OPS counts
// pages, so this is the cost of a page whether a kernel fills it at mmap or
// on the first touch.
static int t_fault(long n) {
  size_t sz = (size_t)n << 20;
  begin("fault");
  char *m = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return skip("fault", "mmap");
  for (size_t j = 0; j < sz; j += 4096) m[j] = 1;
  munmap(m, sz);
  end((long)(sz / 4096));
  return 0;
}

// One byte back and forth between two processes over two pipes: a round
// trip is two context switches and four pipe calls.
static int t_pipelat(long n) {
  int a[2], b[2];
  char c = 0;
  if (pipe(a) || pipe(b)) return skip("pipelat", "pipe");
  pid_t p = fork();
  if (p < 0) return skip("pipelat", "fork");
  if (p == 0) {
    for (long i = 0; i < n; i++) {
      if (read(a[0], &c, 1) != 1) _exit(1);
      if (write(b[1], &c, 1) != 1) _exit(1);
    }
    _exit(0);
  }
  begin("pipelat");
  for (long i = 0; i < n; i++) {
    write(a[1], &c, 1);
    read(b[0], &c, 1);
  }
  end(n);
  waitpid(p, 0, 0);
  close(a[0]); close(a[1]); close(b[0]); close(b[1]);
  return 0;
}

// nanosleep for 1 ms: the time over 1 ms is how late a sleeper wakes.
static int t_sleep(long n) {
  struct timespec ts = {0, 1000000};
  begin("sleep");
  for (long i = 0; i < n; i++) nanosleep(&ts, 0);
  end(n);
  return 0;
}

// epoll_wait for 1 ms on an empty pipe: how late a timed-out wait ends
// when the call has something to look at (LTP's epoll_wait02 does this).
static int t_epwait(long n) {
  int a[2];
  struct epoll_event ev = {.events = EPOLLIN};
  int ep = epoll_create1(0);
  if (ep < 0 || pipe(a)) return skip("epwait", "epoll");
  ev.data.fd = a[0];
  if (epoll_ctl(ep, EPOLL_CTL_ADD, a[0], &ev)) return skip("epwait", "epoll_ctl");
  begin("epwait");
  for (long i = 0; i < n; i++) epoll_wait(ep, &ev, 1, 1);
  end(n);
  close(a[0]);
  close(a[1]);
  close(ep);
  return 0;
}

// One nanosleep of n ms. bench/run.py reports the host CPU time QEMU used
// meanwhile: an idle guest should use almost none.
static int t_idle(long n) {
  struct timespec ts = {n / 1000, n % 1000 * 1000000};
  begin("idle");
  nanosleep(&ts, 0);
  end(1);
  return 0;
}

// Bulk data through one pipe in 64 KiB writes. OPS counts KiB.
static int t_pipebw(long n) {
  int a[2];
  size_t chunk = 64 << 10;
  char *buf = malloc(chunk);
  if (!buf || pipe(a)) return skip("pipebw", "pipe");
  memset(buf, 7, chunk);
  pid_t p = fork();
  if (p < 0) return skip("pipebw", "fork");
  if (p == 0) {
    close(a[1]);
    while (read(a[0], buf, chunk) > 0) {}
    _exit(0);
  }
  close(a[0]);
  begin("pipebw");
  for (long i = 0; i < n; i++) {
    if (write_all(a[1], buf, chunk)) return skip("pipebw", "write");
  }
  close(a[1]);
  waitpid(p, 0, 0);
  end(n * 64);
  return 0;
}

// Write N MiB to a new file in 64 KiB writes, then read it back; OPS counts
// KiB. The file stays small enough for any cache, so these measure the file
// system code, not the disk.
static int t_filewrite(long n) {
  char p[300];
  size_t chunk = 64 << 10;
  char *buf = malloc(chunk);
  snprintf(p, sizeof p, "%s/osbench.data", dir);
  memset(buf, 3, chunk);
  int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return skip("filewrite", "create");
  begin("filewrite");
  for (long i = 0; i < n * 16; i++)
    if (write_all(fd, buf, chunk)) return skip("filewrite", "write");
  close(fd);
  end(n * 1024);
  return 0;
}

static int t_fileread(long n) {
  char p[300];
  size_t chunk = 64 << 10;
  char *buf = malloc(chunk);
  snprintf(p, sizeof p, "%s/osbench.data", dir);
  sync();                 // filewrite's data is on its way to the disk, not part of the reads
  begin("fileread");
  long kib = 0;
  for (long i = 0; i < n; i++) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return skip("fileread", "open (run filewrite first)");
    ssize_t r;
    while ((r = read(fd, buf, chunk)) > 0) kib += r >> 10;
    close(fd);
  }
  end(kib);
  unlink(p);
  return 0;
}

// Pages of a file mapped read-only and touched once each: the cost of
// resolving a file-backed fault (the page is already in the file system's
// cache, so this is the mapping code, not the disk). OPS counts pages.
static int t_filemap(long n) {
  char p[300];
  size_t sz = 4 << 20, chunk = 64 << 10;
  char *buf = malloc(chunk);
  snprintf(p, sizeof p, "%s/osbench.map", dir);
  memset(buf, 4, chunk);
  int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return skip("filemap", "create");
  for (size_t i = 0; i < sz / chunk; i++)
    if (write_all(fd, buf, chunk)) return skip("filemap", "write");
  close(fd);
  fd = open(p, O_RDONLY);
  if (fd < 0) return skip("filemap", "open");
  begin("filemap");
  volatile char sink = 0;
  for (long i = 0; i < n; i++) {
    char *m = mmap(0, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) return skip("filemap", "mmap");
    for (size_t j = 0; j < sz; j += 4096) sink += m[j];
    munmap(m, sz);
  }
  end(n * (long)(sz / 4096));
  close(fd);
  unlink(p);
  return 0;
}

// Create, write 1 KiB to, close and remove a file N times.
static int t_create(long n) {
  char p[300], buf[1024];
  memset(buf, 5, sizeof buf);
  begin("create");
  for (long i = 0; i < n; i++) {
    snprintf(p, sizeof p, "%s/osbench.c%ld", dir, i & 63);
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return skip("create", "create");
    write(fd, buf, sizeof buf);
    close(fd);
    unlink(p);
  }
  end(n);
  return 0;
}

// TCP over the loopback address: a one-byte round trip, then bulk.
static int tcp_pair(int *srv, int *cli) {
  int l = socket(AF_INET, SOCK_STREAM, 0);
  if (l < 0) return -1;
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)(7300 + getpid() % 1000)), .sin_addr.s_addr = htonl(0x7f000001)};
  int one = 1;
  setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(l, (void *)&a, sizeof a) || listen(l, 1)) return -1;
  pid_t p = fork();
  if (p < 0) return -1;
  if (p == 0) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0 || connect(c, (void *)&a, sizeof a)) _exit(1);
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    *cli = c;
    close(l);
    return 0;
  }
  int s = accept(l, 0, 0);
  close(l);
  if (s < 0) return -1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  *srv = s;
  return p;
}

static int t_tcplat(long n) {
  int s = -1, c = -1;
  char b = 0;
  int p = tcp_pair(&s, &c);
  if (p < 0) return skip("tcplat", "loopback connect");
  if (p == 0) {
    for (long i = 0; i < n; i++)
      if (read(c, &b, 1) != 1 || write(c, &b, 1) != 1) _exit(1);
    _exit(0);
  }
  begin("tcplat");
  for (long i = 0; i < n; i++) {
    if (write(s, &b, 1) != 1 || read(s, &b, 1) != 1) return skip("tcplat", "round trip");
  }
  end(n);
  waitpid(p, 0, 0);
  close(s);
  return 0;
}

static int t_tcpbw(long n) {
  int s = -1, c = -1;
  size_t chunk = 64 << 10;
  char *buf = malloc(chunk);
  memset(buf, 9, chunk);
  int p = tcp_pair(&s, &c);
  if (p < 0) return skip("tcpbw", "loopback connect");
  if (p == 0) {
    while (read(c, buf, chunk) > 0) {}
    _exit(0);
  }
  begin("tcpbw");
  for (long i = 0; i < n; i++) {
    if (write_all(s, buf, chunk)) return skip("tcpbw", "write");
  }
  close(s);
  waitpid(p, 0, 0);
  end(n * 64);
  return 0;
}

struct test {
  const char *name;
  int (*fn)(long);
  long n;
};

// Iteration counts sized for about a second each under QEMU TCG.
static const struct test tests[] = {
    {"cpu", t_cpu, 50000000},   {"memcpy", t_memcpy, 1600},   {"syscall", t_syscall, 200000},
    {"clock", t_clock, 200000}, {"open", t_open, 5000},      {"stat", t_stat, 10000},
    {"fork", t_fork, 300},      {"forkbig", t_forkbig, 50},  {"exec", t_exec, 200},
    {"spawn", t_spawn, 200},    {"fault", t_fault, 32},      {"pipelat", t_pipelat, 5000},
    {"pipebw", t_pipebw, 2000}, {"filewrite", t_filewrite, 8}, {"fileread", t_fileread, 8},
    {"create", t_create, 2000}, {"filemap", t_filemap, 4}, {"tcplat", t_tcplat, 2000},  {"tcpbw", t_tcpbw, 500},
    {"sleep", t_sleep, 500},    {"epwait", t_epwait, 500},
    {"idle", t_idle, 3000},
};

int main(int argc, char **argv) {
  int i = 1;
  if (argc > 1 && !strcmp(argv[1], "-x")) return 0;  // the exec/spawn child
  if (argc > 1 && !strcmp(argv[1], "-l")) {
    for (size_t t = 0; t < sizeof tests / sizeof *tests; t++) printf("%s\n", tests[t].name);
    return 0;
  }
  if (argc > 2 && !strcmp(argv[1], "-d")) {
    snprintf(dir, sizeof dir, "%s", argv[2]);
    i = 3;
  }
  // -r: run the tests again and again, until killed (bench/insnprof.py steps
  // through a loop that would otherwise end before the debugger is attached).
  int again = i < argc && !strcmp(argv[i], "-r");
  if (again) i++;
  int first = i;
  if (access(self, X_OK) != 0) self = argv[0];
  do {
    for (i = first; i < argc; i++) {
      char *eq = strchr(argv[i], '=');
      size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
      size_t t;
      for (t = 0; t < sizeof tests / sizeof *tests; t++)
        if (strlen(tests[t].name) == len && !strncmp(tests[t].name, argv[i], len)) break;
      if (t == sizeof tests / sizeof *tests) {
        fprintf(stderr, "osbench: no test %s\n", argv[i]);
        return 2;
      }
      tests[t].fn(eq ? atol(eq + 1) : tests[t].n);
    }
  } while (again);
  printf("@done\n");
  return 0;
}
