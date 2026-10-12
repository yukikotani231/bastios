/* A fixed-address (non-PIE) executable, linked where the usual toolchain
 * links one: text at 0x400000, data and bss after it. Alpine's gcc and its
 * cc1 are built so, and cc1 alone spans 0x400000 to 0x2e70000. The user
 * address space must hold the program where its headers say, whatever the
 * kernel keeps in its own memory, and a null pointer must still fault. */
#include "compat.h"
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>

static char big[48 << 20];                    /* bss from about 0x40c000 to 0x340c000 */
static int counter = 7;
static const char *const names[] = {"alpha", "beta", "gamma"};
static int twice(int x) { return 2 * x; }
static int (*const ops[])(int) = {twice};

int main(void) {
    setup();
    uintptr_t text = (uintptr_t)main, data = (uintptr_t)&counter, bss = (uintptr_t)big;
    EXPECT("the text is where it was linked", text >= 0x400000 && text < 0x500000);
    EXPECT("the data follows it", data >= 0x400000 && data < 0x1000000);
    EXPECT("the bss covers what the kernel image would", bss <= 0x1000000 && bss + sizeof big > 0x2000000);
    EXPECT("absolute pointers in the data work", strcmp(names[1], "beta") == 0 && ops[0](21) == 42);
    if (begin("the whole bss is usable")) {
        for (size_t i = 0; i < sizeof big; i += 4096) big[i] = (char)(i >> 12);
        size_t bad = 0;
        for (size_t i = 0; i < sizeof big; i += 4096) bad += big[i] != (char)(i >> 12);
        if (bad) fail("%zu pages read back wrong", bad); else pass();
    }
    EXPECT("malloc and free work", ({ char *p = malloc(1 << 20); int ok = p != NULL; if (p) { memset(p, 1, 1 << 20); free(p); } ok; }));
    if (begin("mmap lands outside the image")) {
        void *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uintptr_t a = (uintptr_t)p;
        if (p == MAP_FAILED) fail("errno %d", errno);
        else if (a < bss + sizeof big && a + (1 << 20) > 0x400000) fail("%p overlaps the image", p);
        else pass();
    }
    if (begin("a child sees the same addresses (fork)")) {
        pid_t c = fork();
        if (c == 0) _exit((uintptr_t)&counter == data && counter == 7 && big[4096] == 1 ? 0 : 1);
        int st = 0;
        if (c < 0 || waitpid(c, &st, 0) != c) fail("fork or wait: errno %d", errno);
        else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) fail("status %#x", st);
        else pass();
    }
    if (begin("a null pointer faults")) {
        pid_t c = fork();
        if (c == 0) {
            volatile int *np = NULL;
            *np = 1;
            _exit(0);
        }
        int st = 0;
        if (c < 0 || waitpid(c, &st, 0) != c) fail("fork or wait: errno %d", errno);
        else if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV) fail("status %#x", st);
        else pass();
    }
    if (begin("so does an address in the kernel's old identity map")) {
        pid_t c = fork();
        if (c == 0) {
            volatile int *kp = (volatile int *)0x100000;     /* below the image: nothing is mapped there */
            *kp = 1;
            _exit(0);
        }
        int st = 0;
        if (c < 0 || waitpid(c, &st, 0) != c) fail("fork or wait: errno %d", errno);
        else if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV) fail("status %#x", st);
        else pass();
    }
    return done();
}
