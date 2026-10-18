/* No-execute memory: code runs only from pages that are executable.
 *
 * A child process puts a tiny function (mov eax, 42; ret) in a page and calls
 * it. From the stack, the heap, a data array or a plain read-write mapping
 * that must kill the child with SIGSEGV; from a mapping that is executable,
 * or that mprotect made executable after the code was written, it returns 42.
 * x86-64 (mov eax, 42; ret) and AArch64 (mov w0, #42; ret). */
#include "compat.h"
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#if defined(__x86_64__) || defined(__aarch64__)

#ifdef __x86_64__
static const unsigned char CODE[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
#else
static const unsigned char CODE[] = {0x40, 0x05, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};
#endif

typedef int (*fn_t)(void);

/* Runs `body` in a child: what it returned (exit status), or -signal. */
static int in_child(int (*body)(void)) {
    fflush(stdout);
    pid_t c = fork();
    if (c < 0) return -1000;
    if (c == 0) _exit(body());
    int st = 0;
    if (waitpid(c, &st, 0) != c) return -1001;
    if (WIFSIGNALED(st)) return -WTERMSIG(st);
    return WEXITSTATUS(st);
}

static unsigned char data_page[4096] __attribute__((aligned(4096)));

static int call(void *p) {
    __builtin___clear_cache((char *)p, (char *)p + 64);   /* no-op on x86-64 */
    return ((fn_t)p)() == 42 ? 0 : 1;
}

static int from_stack(void) {
    unsigned char buf[64] __attribute__((aligned(16)));
    memcpy(buf, CODE, sizeof CODE);
    return call(buf);
}

static int from_heap(void) {
    unsigned char *p = malloc(4096);
    memcpy(p, CODE, sizeof CODE);
    return call(p);
}

static int from_data(void) {
    memcpy(data_page, CODE, sizeof CODE);
    return call(data_page);
}

static int from_rw_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(p, CODE, sizeof CODE);
    return call(p);
}

static int from_rwx_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 2;
    memcpy(p, CODE, sizeof CODE);
    return call(p);
}

static int flip_to_x(void) {
    void *p = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 2;
    memcpy((char *)p + 4096, CODE, sizeof CODE);
    if (mprotect((char *)p + 4096, 4096, PROT_READ | PROT_EXEC) != 0) return 3;
    return call((char *)p + 4096);
}

static int flip_to_rwx(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 2;
    memcpy(p, CODE, sizeof CODE);
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 3;
    return call(p);
}

/* Executable, then writable again: the write works and the page stops running. */
static int x_then_w(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 2;
    memcpy(p, CODE, sizeof CODE);
    if (call(p) != 0) return 3;
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0) return 4;
    ((unsigned char *)p)[8] = 1;
    return call(p);
}

static int rx_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(p, CODE, sizeof CODE);
    void *q = mremap(p, 4096, 2 * 4096, MREMAP_MAYMOVE);
    if (q == MAP_FAILED) return 2;
    if (mprotect(q, 2 * 4096, PROT_READ | PROT_EXEC) != 0) return 3;
    return call(q);
}

/* How V8 gets code memory: a PROT_NONE reservation, made read-write-execute,
 * touched, and then emptied with MADV_DONTNEED. The page still takes the code
 * and runs it. */
static int jit_reserve(void) {
    char *p = mmap(NULL, 2 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return 2;
    if (madvise(p, 2 * 4096, MADV_DONTFORK) != 0) return 3;
    if (mprotect(p, 2 * 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 4;
    p[4096] = 1;
    if (madvise(p, 2 * 4096, MADV_DONTNEED) != 0) return 5;
    memcpy(p + 4096, CODE, sizeof CODE);
    return call(p + 4096);
}

/* A page the parent wrote and the fork shared copy-on-write still cannot run. */
static int forked_stack(void) {
    static unsigned char buf[64] __attribute__((aligned(16)));
    unsigned char local[64] __attribute__((aligned(16)));
    memcpy(local, CODE, sizeof CODE);
    memcpy(buf, CODE, sizeof CODE);
    pid_t c = fork();
    if (c < 0) return 2;
    if (c == 0) _exit(call(local));
    int st = 0;
    waitpid(c, &st, 0);
    if (WIFSIGNALED(st)) return 0;
    return 1;
}

static int shm_not_x(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 2;
    memcpy(p, CODE, sizeof CODE);
    return call(p);
}

static int shm_flip(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 2;
    memcpy(p, CODE, sizeof CODE);
    if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0) return 3;
    return call(p);
}

#define FAULTS(name, fn) \
    if (begin(name)) { int r = in_child(fn); if (r == -SIGSEGV) pass(); else fail("expected SIGSEGV, got %d", r); }
#define RUNS(name, fn) \
    if (begin(name)) { int r = in_child(fn); if (r == 0) pass(); else fail("expected to run, got %d", r); }

int main(void) {
    setup();
    RUNS("a mapping moved by mremap and made executable runs", rx_map)
    RUNS("code runs from a read-write-execute mapping", from_rwx_map)
    FAULTS("code on the stack does not run", from_stack)
    FAULTS("code on the heap does not run", from_heap)
    FAULTS("code in a data array does not run", from_data)
    FAULTS("code in a read-write mapping does not run", from_rw_map)
    FAULTS("code in a shared read-write mapping does not run", shm_not_x)
    RUNS("mprotect(PROT_EXEC) after writing makes the code run", flip_to_x)
    RUNS("mprotect(PROT_EXEC) on a shared mapping makes the code run", shm_flip)
    RUNS("mprotect to read-write-execute keeps the code running", flip_to_rwx)
    RUNS("a read-write-execute page emptied with MADV_DONTNEED still runs code", jit_reserve)
    FAULTS("mprotect back to read-write stops the code", x_then_w)
    RUNS("a fork child's copy-on-write stack stays no-execute", forked_stack)
    return done();
}

#else

int main(void) {
    setup();
    return done();
}

#endif
