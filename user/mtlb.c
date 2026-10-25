/* Stale translations. A processor keeps a process's translations across
 * system calls (PCID, boot/boot.S bst_cpu_enter); after a call that copied
 * from the process's memory it must still drop the ones an unmap, a
 * protection change or a fork made wrong, and must not mix up two processes
 * that map the same address. Each case touches a page (so a translation is
 * cached), makes a system call that reads user memory, changes the page, and
 * checks that the next access sees the change. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROUNDS 400
#define FIXED ((void *)0x5a0000000ull)

static sigjmp_buf jb;
static void onsegv(int s) { (void)s; siglongjmp(jb, 1); }

/* Does the access fault? */
static int faults(volatile char *p, int write) {
  if (sigsetjmp(jb, 1)) return 1;
  if (write) *p = 1; else (void)*p;
  return 0;
}

static int fail(const char *what, int i) {
  printf("mtlb: FAIL %s (round %d)\n", what, i);
  return 1;
}

static int fd_null;
static void touch_syscall(void) {
  static char b[64];
  if (write(fd_null, b, sizeof b) != sizeof b) _exit(2);   /* reads b through the process's table */
}

static int unmapped(void) {
  for (int i = 0; i < ROUNDS; i++) {
    volatile char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return fail("mmap", i);
    *p = 1;
    touch_syscall();
    if (*p != 1) return fail("read back", i);
    munmap((void *)p, 4096);
    if (!faults(p, 0)) return fail("read after munmap", i);
  }
  return 0;
}

static int protect(void) {
  for (int i = 0; i < ROUNDS; i++) {
    volatile char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return fail("mmap", i);
    *p = 1;
    touch_syscall();
    mprotect((void *)p, 4096, PROT_READ);
    if (!faults(p, 1)) return fail("write after mprotect", i);
    mprotect((void *)p, 4096, PROT_NONE);
    if (!faults(p, 0)) return fail("read after PROT_NONE", i);
    munmap((void *)p, 4096);
  }
  return 0;
}

/* The parent writes to a page it had cached as writable after fork: the
 * child must still see the old value. */
static int forked(void) {
  volatile char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return fail("mmap", 0);
  for (int i = 0; i < ROUNDS / 4; i++) {
    int go[2];
    if (pipe(go)) return fail("pipe", i);
    *p = 1;
    touch_syscall();
    pid_t c = fork();
    if (c < 0) return fail("fork", i);
    if (c == 0) {
      char b;
      if (read(go[0], &b, 1) != 1) _exit(2);
      _exit(*p == 1 ? 0 : 1);
    }
    *p = 2;
    if (write(go[1], "x", 1) != 1) return fail("pipe write", i);
    int st;
    waitpid(c, &st, 0);
    close(go[0]);
    close(go[1]);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return fail("the child saw the parent's write", i);
  }
  return 0;
}

/* Two processes with different pages at one address take turns. */
static int pingpong(void) {
  int a[2], b[2];
  if (pipe(a) || pipe(b)) return fail("pipe", 0);
  pid_t c = fork();
  if (c < 0) return fail("fork", 0);
  char me = c == 0 ? 'c' : 'p';
  volatile char *p = mmap(FIXED, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (p == MAP_FAILED) return fail("mmap", 0);
  for (int i = 0; i < ROUNDS; i++) {
    char t;
    p[0] = me;
    if (c == 0) {
      if (read(a[0], &t, 1) != 1) _exit(2);
    } else if (i > 0 && read(b[0], &t, 1) != 1) return fail("pipe read", i);
    touch_syscall();
    if (p[0] != me) {
      printf("mtlb: FAIL %c saw %c at the shared address (round %d)\n", me, p[0], i);
      if (c == 0) _exit(1);
      return 1;
    }
    if (c == 0) {
      if (write(b[1], "y", 1) != 1) _exit(2);
    } else if (write(a[1], "y", 1) != 1) return fail("pipe write", i);
  }
  if (c == 0) _exit(0);
  int st;
  waitpid(c, &st, 0);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

int main(void) {
  struct sigaction sa = {.sa_handler = onsegv, .sa_flags = SA_NODEFER};
  sigaction(SIGSEGV, &sa, 0);
  fd_null = open("/dev/null", O_WRONLY);
  if (unmapped() || protect() || forked() || pingpong()) return 1;
  printf("mtlb: ok, %d rounds each\n", ROUNDS);
  return 0;
}
