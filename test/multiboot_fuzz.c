/*
 * Host-side fuzzer for boot/multiboot.c, the code that reads the boot
 * loader's information before anything is proved. It compiles the real
 * file (not a copy) as a non-PIE program, so its static buffers sit below
 * 4 GiB like the kernel's, and gives it a fake identity-mapped memory:
 * one mapping below IDENT_LIMIT, with a PROT_NONE page on each side of the
 * boot information. A read outside the structure the loader described
 * (whose size field is truthful in most runs) faults, and the fault is the
 * failure. After each parse it also checks what the kernel relies on:
 * bounded region counts, and that bst_boot_take hands out memory that lies
 * in an available region, above _end and below IDENT_LIMIT, once.
 *
 * usage: multiboot_fuzz [SEED [RUNS]]
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define IDENT_LIMIT (1ull << 30)
#define PAGE 4096ull
#define ARENA 0x10000000ull        /* where the fake low memory starts */
#define ARENA_SIZE (64ull << 20)

char _end[1];
uint64_t bst_kernel_pml4[512];
extern uint8_t bst_boot_rsdp[36];
extern uint32_t bst_boot_rsdp_len;
extern uint32_t bst_multiboot_info(uint32_t magic, uint32_t info);
extern uint64_t bst_boot_cmdline(uint64_t i);
extern uint64_t bst_boot_region(uint64_t n, uint64_t f);
extern uint64_t bst_boot_framebuffer(uint64_t f);
extern uint64_t bst_boot_take(uint64_t size, uint64_t align);

static uint64_t rng = 1;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t below(uint64_t n) { return n ? rnd() % n : 0; }

static sigjmp_buf jb;
static volatile sig_atomic_t in_parse;
static void on_segv(int s, siginfo_t *si, void *c) {
    (void)s; (void)c;
    if (in_parse) { fprintf(stderr, "fault at %p\n", si->si_addr); siglongjmp(jb, 1); }
    _exit(2);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* The boot information goes at the end of a page run with a PROT_NONE page
 * after it and one before: `len` bytes, returned as its address. */
static uint8_t *place(size_t len) {
    static uint8_t *area;
    if (!area) {
        area = mmap((void *)ARENA, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (area != (void *)ARENA) { perror("mmap"); exit(2); }
    }
    uint8_t *lo = area + 8 * PAGE;
    size_t pages = (len + PAGE - 1) / PAGE + 1;
    mprotect(lo - PAGE, PAGE, PROT_NONE);
    mprotect(lo, pages * PAGE, PROT_READ | PROT_WRITE);
    mprotect(lo + pages * PAGE, PAGE, PROT_NONE);
    uint8_t *p = lo + pages * PAGE - len;
    return (uint8_t *)((uintptr_t)p & ~7ull) < lo ? lo : p;
}

static size_t gen_mb2(uint8_t *b, size_t cap) {
    size_t n = 8;
    int tags = 1 + (int)below(6);
    for (int k = 0; k < tags && n + 64 < cap; k++) {
        uint32_t type = below(4) ? (uint32_t[]){1, 4, 6, 8, 14, 15}[below(6)] : (uint32_t)below(20);
        size_t size = 8;
        switch (type) {
        case 1: size = 8 + below(300); for (size_t i = 8; i < size; i++) b[n + i] = 'a' + below(26); if (below(2)) b[n + size - 1] = 0; break;
        case 6: { uint32_t es = below(8) ? 24 : (uint32_t)below(64); int cnt = (int)below(150); size = 16 + (size_t)cnt * (es ? es : 24);
                  if (n + size + 8 > cap) size = 16 + 24 * 4;
                  put32(b + n + 8, es); put32(b + n + 12, 0);
                  for (size_t i = 16; i + 8 <= size; i += 8) put64(b + n + i, below(3) ? below(1ull << 32) : rnd());
                  break; }
        default: size = 8 + below(48); for (size_t i = 8; i < size; i++) b[n + i] = (uint8_t)rnd();
        }
        if (below(8) == 0) size = below(40);
        put32(b + n, type); put32(b + n + 4, (uint32_t)size);
        n += (size + 7) & ~7ull;
    }
    put32(b + n, 0); put32(b + n + 4, 8); n += 8;
    put32(b, (uint32_t)n);
    if (below(10) == 0) put32(b, (uint32_t)below(n + 64));      /* a lying total */
    return n;
}

static size_t gen_mb1(uint8_t *b, size_t cap) {
    (void)cap;
    put32(b, (uint32_t)(below(2) ? rnd() : 0x45));
    put32(b + 44, (uint32_t)below(24 * 40));              /* mmap_addr and cmdline are patched once placed */
    uint8_t *m = b + 96;
    for (int i = 0; i < 40; i++, m += 24) { put32(m, below(8) ? 20 : (uint32_t)below(64)); put64(m + 4, below(1ull << 31)); put64(m + 12, below(1ull << 29)); put32(m + 20, 1 + (uint32_t)below(3)); }
    return 96 + 24 * 40;
}

static void check(const char *what) {
    uint64_t n;
    for (n = 0; bst_boot_region(n, 2) != 0 && n < 4096; n++) {}
    if (n > 128) { fprintf(stderr, "%s: %llu regions\n", what, (unsigned long long)n); exit(1); }
    for (n = 0; n < 300 && bst_boot_cmdline(n) != 256; n++) {}
    if (n > 255) { fprintf(stderr, "%s: command line of %llu\n", what, (unsigned long long)n); exit(1); }
    bst_boot_framebuffer(0); bst_boot_framebuffer(4);
    uint64_t taken = bst_boot_take(1 << 17, 1 << 17);
    if (taken && (taken < (uint64_t)_end || taken + (1 << 17) > IDENT_LIMIT || taken % (1 << 17))) {
        fprintf(stderr, "%s: bst_boot_take returned %#llx\n", what, (unsigned long long)taken); exit(1);
    }
}

int main(int argc, char **argv) {
    rng = (argc > 1 ? strtoull(argv[1], 0, 0) : 1) * 0x9E3779B97F4A7C15ull | 1;
    long runs = argc > 2 ? atol(argv[2]) : 20000;
    struct sigaction sa = {.sa_sigaction = on_segv, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigaction(SIGSEGV, &sa, 0);
    for (long r = 0; r < runs; r++) {
        uint8_t tmp[16384];
        memset(tmp, 0, sizeof tmp);
        int form = (int)below(3);
        size_t len = form == 0 ? gen_mb2(tmp, sizeof tmp) : form == 1 ? gen_mb1(tmp, sizeof tmp) : 56 + 24 * below(150);
        if (form == 2) {                                /* PVH */
            for (size_t i = 0; i < len; i++) tmp[i] = (uint8_t)rnd();
            put32(tmp, 0x336EC578); put32(tmp + 4, below(2)); put64(tmp + 24, 0); put64(tmp + 32, 0);
            if (below(2)) { put64(tmp + 40, ARENA + 64 * PAGE); put32(tmp + 48, (uint32_t)below(200)); }
        }
        uint8_t *at = place(len);
        memcpy(at, tmp, len);
        if (form == 1) {                                /* the map follows the 96 bytes; the command line is its first (zero) byte */
            put32(at + 48, (uint32_t)(uintptr_t)(at + 96));
            put32(at + 16, (uint32_t)(uintptr_t)(at + 95));
            at[95] = 0;
        }
        bst_boot_rsdp_len = 0;
        uint32_t magic = form == 0 ? 0x36D76289u : form == 1 ? 0x2BADB002u : 0x31485650u;
        if (!sigsetjmp(jb, 1)) {
            in_parse = 1;
            uint32_t addr = (uint32_t)(uintptr_t)at;
            bst_multiboot_info(magic, addr);
            check(form == 0 ? "multiboot2" : form == 1 ? "multiboot1" : "pvh");
            in_parse = 0;
        } else {
            fprintf(stderr, "run %ld (form %d, %zu bytes): read outside the information\n", r, form, len);
            return 1;
        }
    }
    printf("multiboot fuzz: %ld runs ok\n", runs);
    return 0;
}
