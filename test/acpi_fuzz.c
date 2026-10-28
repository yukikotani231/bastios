/*
 * Host-side fuzzer for the ACPI search in boot/smp.c (bst_platform_acpi:
 * the RSDP check, the XSDT/RSDT walk, table checksums and lengths, and
 * the page-table walk that decides what may be read). It compiles the real
 * smp.c as a non-PIE program, with stand-ins for the symbols assembly
 * provides, and a fake page table that maps only a 64 MiB arena. Tables
 * are built from random fields, some with lying lengths or addresses
 * outside the arena; each table sits at the end of a page run followed by
 * a PROT_NONE page (and behind one), so a read past a table whose length
 * was truthful faults. The rest of the arena is zeroed RAM, as the identity
 * map makes the rest of memory: a stray pointer into it reads harmlessly. The promise the prover relies on is checked on every query: a
 * byte (0..255) inside the table, 256 past its end or without one.
 *
 * usage: acpi_fuzz [SEED [RUNS]]
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <ucontext.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096ull
#define ARENA 0x10000000ull
#define ARENA_SIZE (64ull << 20)

/* what boot.S, ap.S, platform.c and multiboot.c provide */
uint64_t bst_kernel_pml4[512];
uint64_t bst_gdt[64];
const char bst_ap_trampoline[1] = {0}, bst_ap_trampoline_end[1] = {0};
uint64_t bst_cpu_halt(void) { return 0; }
void bst_alias_map_block(uint64_t i, uint8_t *core) { (void)i; (void)core; }
void bst_alias_load_gdt(void) {}
uint64_t bst_boot_take(uint64_t size, uint64_t align) { (void)size; (void)align; return 0; }
uint64_t bst_boot_cmdline(uint64_t i) { (void)i; return 256; }
uint64_t bst_boot_region(uint64_t n, uint64_t f) { (void)n; (void)f; return 0; }
uint64_t bst_boot_framebuffer(uint64_t f) { (void)f; return 0; }
void bst_pat_load(void) {}
uint8_t bst_boot_rsdp[36];
uint32_t bst_boot_rsdp_len;
extern uint64_t bst_platform_acpi(uint64_t sig, uint64_t off);

static uint64_t rng = 1;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t below(uint64_t n) { return n ? rnd() % n : 0; }

static sigjmp_buf jb;
static volatile sig_atomic_t armed;
static void on_segv(int s, siginfo_t *si, void *c) {
    (void)s;
    if (armed) { fprintf(stderr, "fault at %p, pc %#llx\n", si->si_addr, (unsigned long long)((ucontext_t *)c)->uc_mcontext.gregs[REG_RIP]); siglongjmp(jb, 1); }
    _exit(2);
}

static uint64_t pdpt[512] __attribute__((aligned(4096))), pd[512] __attribute__((aligned(4096)));
static void fix(uint8_t *t, size_t n) {            /* make the checksum (byte 9) right */
    t[9] = 0;
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += t[i];
    t[9] = (uint8_t)-s;
}

static uint8_t *arena;
static size_t next_page;
/* A table of `n` bytes at the end of a mapped page run behind a PROT_NONE page. */
static uint8_t *table(size_t n) {
    size_t pages = (n + PAGE - 1) / PAGE;
    uint8_t *lo = arena + (next_page + 1) * PAGE;
    mprotect(lo - PAGE, PAGE, PROT_NONE);
    mprotect(lo + pages * PAGE, PAGE, PROT_NONE);
    next_page += pages + 2;
    uint8_t *p = lo + pages * PAGE - n;
    memset(p, 0, n);
    return p;
}

static const char *sigs[] = {"APIC", "FACP", "HPET", "DSDT", "SSDT", "MCFG"};

int main(int argc, char **argv) {
    rng = (argc > 1 ? strtoull(argv[1], 0, 0) : 1) * 0x9E3779B97F4A7C15ull | 1;
    long runs = argc > 2 ? atol(argv[2]) : 5000;
    arena = mmap((void *)ARENA, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (arena != (void *)ARENA) { perror("mmap"); return 2; }
    bst_kernel_pml4[0] = (uint64_t)pdpt | 3;
    pdpt[0] = (uint64_t)pd | 3;
    for (uint64_t i = ARENA >> 21; i < (ARENA + ARENA_SIZE) >> 21; i++) pd[i] = (i << 21) | 0x83;
    struct sigaction sa = {.sa_sigaction = on_segv, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigaction(SIGSEGV, &sa, 0);

    for (long r = 0; r < runs; r++) {
        /* smp.c caches what it found: fresh process state per run via fork */
        pid_t pid = fork();
        if (pid == 0) {
            rng ^= (uint64_t)r * 0x2545F4914F6CDD1Dull; rnd();
            if (sigsetjmp(jb, 1)) { fprintf(stderr, "run %ld: read outside a table\n", r); _exit(1); }
            next_page = 0;
            int ntab = 1 + (int)below(6);
            uint64_t addrs[8]; int na = 0;
            for (int k = 0; k < ntab; k++) {
                size_t n = 36 + below(300);
                uint8_t *t = table(n);
                for (size_t i = 36; i < n; i++) t[i] = (uint8_t)rnd();
                memcpy(t, sigs[below(6)], 4);
                uint32_t len = (uint32_t)n;
                if (below(12) == 0) len = (uint32_t)(ARENA_SIZE + below(1ull << 31));   /* a lying length that runs off the mapped arena */
                memcpy(t + 4, &len, 4);
                fix(t, n);
                if (below(10) == 0) t[9] ^= 1 + (uint8_t)below(255);          /* a bad checksum */
                addrs[na++] = (uint64_t)t;
            }
            int xsdt = below(2);
            size_t es = xsdt ? 8 : 4, nroot = na + (size_t)below(2), rn = 36 + nroot * es;
            uint8_t *root = table(rn);
            memcpy(root, xsdt ? "XSDT" : "RSDT", 4);
            uint32_t rl = (uint32_t)rn;
            memcpy(root + 4, &rl, 4);
            for (size_t k = 0; k < nroot; k++) {
                uint64_t wild = 0x40000000ull + below(0xB0000000ull) + (xsdt ? below(1ull << 20) << 32 : 0);   /* never in the arena */
                uint64_t a = k < (size_t)na ? addrs[k] : (below(2) ? 0 : wild);
                if (below(15) == 0) a = wild;
                if (xsdt) memcpy(root + 36 + 8 * k, &a, 8); else { uint32_t a32 = (uint32_t)a; memcpy(root + 36 + 4 * k, &a32, 4); }
            }
            fix(root, rn);
            memset(bst_boot_rsdp, 0, sizeof bst_boot_rsdp);
            memcpy(bst_boot_rsdp, "RSD PTR ", 8);
            bst_boot_rsdp[15] = xsdt ? 2 : 0;
            uint32_t r32 = (uint32_t)(uint64_t)root;
            memcpy(bst_boot_rsdp + 16, &r32, 4);
            uint64_t r64 = (uint64_t)root;
            memcpy(bst_boot_rsdp + 24, &r64, 8);
            uint8_t s = 0; for (int i = 0; i < 20; i++) s += bst_boot_rsdp[i]; bst_boot_rsdp[8] = (uint8_t)-s;
            if (xsdt) { s = 0; for (int i = 0; i < 36; i++) s += bst_boot_rsdp[i]; bst_boot_rsdp[32] = (uint8_t)-s; }
            bst_boot_rsdp_len = 36;
            armed = 1;
            for (int q = 0; q < 12; q++) {
                uint32_t sg; memcpy(&sg, sigs[below(6)], 4);
                uint64_t off = below(4) ? below(400) : rnd();
                uint64_t v = bst_platform_acpi(sg, off);
                if (v > 256) { fprintf(stderr, "run %ld: answered %llu\n", r, (unsigned long long)v); _exit(1); }
            }
            if (bst_platform_acpi(1ull << 32, 0) != 256) _exit(1);
            _exit(0);
        }
        int st; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) return 1;
    }
    printf("acpi fuzz: %ld runs ok\n", runs);
    return 0;
}
