// Multi-core scalability workloads, driven by bench/scale.py.
//
//   scale TEST WORKERS ITERS
//
// Starts WORKERS processes that each run ITERS rounds of TEST at the same
// time; the work per worker is fixed, so on a kernel that scales perfectly
// the time stays flat while WORKERS <= processors. Prints "@begin NAME" once
// every worker is ready to start, and "@end NAME OPS" after the last one
// finished; OPS is the number of operations all workers did together.
//
// Tests: spin (user-mode only, the control), syscall (a system call that
// takes the kernel path), fork (fork + exit + wait), pipe (a pair of
// processes ping-pong one byte), usock (the same over a socketpair), fault
// (touch fresh anonymous pages), file (open/read/close of one file).
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGES 256

static long run_spin(long iters, int id) {
  volatile unsigned long x = id;
  for (long i = 0; i < iters * 20000; i++) x = x * 6364136223846793005UL + 1;
  return iters;
}

static long run_syscall(long iters, int id) {
  int fd = open("/dev/zero", O_RDONLY);
  char c;
  for (long i = 0; i < iters * 200; i++) read(fd, &c, 1);
  close(fd);
  return iters * 200;
}

static long run_fork(long iters, int id) {
  for (long i = 0; i < iters; i++) {
    pid_t p = fork();
    if (p == 0) _exit(0);
    waitpid(p, 0, 0);
  }
  return iters;
}

static long run_fault(long iters, int id) {
  size_t len = PAGES * 4096;
  for (long i = 0; i < iters; i++) {
    char *m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    for (size_t o = 0; o < len; o += 4096) m[o] = 1;
    munmap(m, len);
  }
  return iters * PAGES;
}

static long run_file(long iters, int id) {
  char c[64];
  for (long i = 0; i < iters * 20; i++) {
    int fd = open("/bin/true", O_RDONLY);
    if (fd < 0) fd = open("/dev/zero", O_RDONLY);
    read(fd, c, sizeof c);
    close(fd);
  }
  return iters * 20;
}

// A pair: this process and a child ping-pong over a pipe or socketpair.
static long run_pair(long iters, int sock) {
  int a[2], b[2];
  if (sock) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, a) || socketpair(AF_UNIX, SOCK_STREAM, 0, b)) { perror("socketpair"); exit(1); }
  } else if (pipe(a) || pipe(b)) { perror("pipe"); exit(1); }
  pid_t p = fork();
  char c = 'x';
  long n = iters * 50;
  if (p == 0) {
    for (long i = 0; i < n; i++) { if (read(a[0], &c, 1) != 1) _exit(1); write(b[1], &c, 1); }
    _exit(0);
  }
  for (long i = 0; i < n; i++) { write(a[1], &c, 1); if (read(b[0], &c, 1) != 1) exit(1); }
  waitpid(p, 0, 0);
  close(a[0]); close(a[1]); close(b[0]); close(b[1]);
  return n * 2;
}

static long run_pipe(long iters, int id) { return run_pair(iters, 0); }
static long run_usock(long iters, int id) { return run_pair(iters, 1); }

static const struct { const char *name; long (*fn)(long, int); } TESTS[] = {
  {"spin", run_spin}, {"syscall", run_syscall}, {"fork", run_fork}, {"pipe", run_pipe},
  {"usock", run_usock}, {"fault", run_fault}, {"file", run_file},
};

int main(int argc, char **argv) {
  if (argc != 4) { fprintf(stderr, "usage: scale TEST WORKERS ITERS\n"); return 2; }
  int w = atoi(argv[2]);
  long iters = atol(argv[3]);
  long (*fn)(long, int) = 0;
  for (unsigned i = 0; i < sizeof TESTS / sizeof *TESTS; i++)
    if (!strcmp(argv[1], TESTS[i].name)) fn = TESTS[i].fn;
  if (!fn) { fprintf(stderr, "scale: no test %s\n", argv[1]); return 2; }
  // Workers report their op counts through a pipe and wait for a go byte.
  int res[2], go[2];
  if (pipe(res) || pipe(go)) return 1;
  for (int i = 0; i < w; i++) {
    if (fork() == 0) {
      char c;
      long ready = 1;
      write(res[1], &ready, sizeof ready);
      read(go[0], &c, 1);
      long ops = fn(iters, i);
      write(res[1], &ops, sizeof ops);
      _exit(0);
    }
  }
  long v, total = 0;
  for (int i = 0; i < w; i++) read(res[0], &v, sizeof v);
  printf("@begin %s\n", argv[1]);
  fflush(stdout);
  char g[64];
  memset(g, 1, sizeof g);
  for (int i = 0; i < w; i++) write(go[1], g, 1);
  for (int i = 0; i < w; i++) { read(res[0], &v, sizeof v); total += v; }
  while (wait(0) > 0) ;
  printf("@end %s %ld\n@done\n", argv[1], total);
  fflush(stdout);
  return 0;
}
