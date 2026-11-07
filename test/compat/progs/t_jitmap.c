/* A JIT's double mapping, as the Erlang VM's (BEAM) JIT and others do it:
 * one memfd (or shm file) mapped twice, once writable and once executable,
 * so that code written through the first runs through the second. */
#include "compat.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <string.h>

/* mov eax, 42 ; ret */
static const unsigned char code[] = { 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3 };

static int memfd(void) { return (int)syscall(SYS_memfd_create, "jit", 0); }

static int double_map(int exec_first, size_t SZ) {
    int fd = memfd();
    if (fd < 0) return 0;
    if (ftruncate(fd, SZ)) return 0;
    unsigned char *w, *x;
    if (exec_first) {
        x = mmap(0, SZ, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        w = mmap(0, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    } else {
        w = mmap(0, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        x = mmap(0, SZ, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    }
    if (w == MAP_FAILED || x == MAP_FAILED) return 0;
    size_t at = SZ / 2 + 4096;                                   /* code in the middle, far from the start */
    memcpy(w + at, code, sizeof code);
    int r = ((int (*)(void))(x + at))();
    memcpy(w + at, "\xb8\x07\x00\x00\x00\xc3", 6);           /* patch it: the second view sees it */
    r = r * 100 + ((int (*)(void))(x + at))();
    munmap(w, SZ); munmap(x, SZ); close(fd);
    return r == 4207;
}

int main(void) {
    setup();
    EXPECT("code written through a writable view of a memfd runs through an executable one", double_map(0, 1 << 20));
    EXPECT("the same with the executable view mapped first", double_map(1, 1 << 20));
    EXPECT("the same with 16 MiB views (a JIT code area: the Erlang VM's)", double_map(0, 16 << 20));
    return done();
}
