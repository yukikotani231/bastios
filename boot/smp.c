/*
 * Multiprocessor startup. Part of the trusted base.
 *
 * Runs on the bootstrap processor (BSP) before the Bastio kernel: finds the
 * other processors in the ACPI MADT, maps the local APIC, and starts each
 * application processor (AP) with INIT and two startup IPIs (Intel SDM
 * vol. 3, 8.4.4). Each AP runs boot/ap.S, checks in through bst_ap_main and
 * parks with interrupts off. The Bastio kernel sees the result through the
 * `cpu_count` builtin.
 *
 * Firmware tables are untrusted input: every read is checked against the
 * table's length and against the identity-mapped first 1 GiB.
 *
 * Why this is C and not Bastio yet: reading firmware tables and the local
 * APIC needs memory-mapped access, which Bastio does not have until the
 * `Mmio` capability lands (bastiolang ROADMAP v3).
 */
#include <stdint.h>

#define AP_TRAMPOLINE 0x8000        /* must match ap.S */
#define MAX_CPUS      16            /* cpu_count promises at most 4096 */
#define AP_STACK      16384
#define IDENT_LIMIT   (1ull << 30)  /* boot.S identity-maps the first 1 GiB */

extern uint64_t bst_kernel_pml4[];
extern const char bst_ap_trampoline[], bst_ap_trampoline_end[];

volatile uint64_t bst_ap_stack;     /* read by ap.S; one AP starts at a time */
static uint64_t cpus_online = 1;    /* the BSP; updated atomically */
static struct __attribute__((packed)) { uint16_t limit; uint64_t base; } ap_idtr;
static uint8_t ap_stacks[MAX_CPUS - 1][AP_STACK] __attribute__((aligned(16)));
static uint64_t lapic_pd[512] __attribute__((aligned(4096)));
static volatile uint32_t *lapic;

/* The saved user context of the processor running the caller. One for now:
 * only the bootstrap processor runs Bastio code. */
extern uint64_t bst_cpu_ctx[21];
uint64_t *bst_platform_ctx(void) { return bst_cpu_ctx; }

uint64_t bst_platform_cpus(void) { return __atomic_load_n(&cpus_online, __ATOMIC_SEQ_CST); }

/* Called by ap.S on each AP, on its own stack, with interrupts off. */
void bst_ap_main(void) {
    __asm__ volatile ("lidt %0" : : "m"(ap_idtr));
    __atomic_fetch_add(&cpus_online, 1, __ATOMIC_SEQ_CST);
}

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* About a microsecond per iteration: a write to the POST diagnostic port. */
static void udelay(uint64_t us) { while (us--) outb(0x80, 0); }

static uint32_t rd8(uint64_t a) { return *(volatile uint8_t *)a; }
static uint32_t rd16(uint64_t a) { return rd8(a) | rd8(a + 1) << 8; }
static uint32_t rd32(uint64_t a) { return rd16(a) | rd16(a + 2) << 16; }
static uint64_t rd64(uint64_t a) { return rd32(a) | (uint64_t)rd32(a + 4) << 32; }

static int sum_ok(uint64_t a, uint64_t n) {
    uint8_t s = 0;
    for (uint64_t i = 0; i < n; i++) s += rd8(a + i);
    return s == 0;
}

static int sig_is(uint64_t a, const char *sig, uint64_t n) {
    for (uint64_t i = 0; i < n; i++)
        if (rd8(a + i) != (uint8_t)sig[i]) return 0;
    return 1;
}

/* The RSDP lies on a 16-byte boundary in the first KiB of the EBDA or in
 * 0xE0000-0xFFFFF (ACPI 6.5, 5.2.5.1). */
static uint64_t find_rsdp(void) {
    uint64_t ebda = (uint64_t)rd16(0x40E) << 4;
    uint64_t from[2] = { ebda, 0xE0000 }, to[2] = { ebda + 1024, 0x100000 };
    for (int r = 0; r < 2; r++) {
        if (from[r] < 0x80000 || to[r] > 0x100000) continue;
        for (uint64_t a = from[r]; a + 20 <= to[r]; a += 16)
            if (sig_is(a, "RSD PTR ", 8) && sum_ok(a, 20)) return a;
    }
    return 0;
}

/* A system description table at `a` with a valid header and checksum, or 0. */
static uint64_t sdt_len(uint64_t a) {
    if (a == 0 || a >= IDENT_LIMIT - 36) return 0;
    uint64_t n = rd32(a + 4);
    if (n < 36 || n > IDENT_LIMIT - a || !sum_ok(a, n)) return 0;
    return n;
}

static uint64_t find_madt(uint64_t *len) {
    uint64_t rsdp = find_rsdp();
    if (!rsdp) return 0;
    uint64_t root = rd32(rsdp + 16), entry = 4;
    if (rd8(rsdp + 15) >= 2 && sum_ok(rsdp, 36) && rd64(rsdp + 24) != 0) {
        root = rd64(rsdp + 24);    /* XSDT */
        entry = 8;
    }
    uint64_t n = sdt_len(root);
    for (uint64_t i = 36; n && i + entry <= n; i += entry) {
        uint64_t t = entry == 8 ? rd64(root + i) : rd32(root + i);
        uint64_t tn = sdt_len(t);
        if (tn >= 44 && sig_is(t, "APIC", 4)) {
            *len = tn;
            return t;
        }
    }
    return 0;
}

/* Map the local APIC's page uncached, kernel-only, through a new page
 * directory for the fourth GiB in the kernel's (shared) level-3 table. */
static int map_lapic(uint64_t base) {
    if (base < 3 * IDENT_LIMIT || base >= 4 * IDENT_LIMIT) return 0;
    uint64_t *pdpt = (uint64_t *)(bst_kernel_pml4[0] & 0x000FFFFFFFFFF000ull);
    if (pdpt[3] != 0) return 0;
    lapic_pd[(base >> 21) & 511] = (base & ~0x1FFFFFull) | 0x9B;   /* present, writable, PWT, PCD, 2 MiB */
    pdpt[3] = (uint64_t)lapic_pd | 3;
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    lapic = (volatile uint32_t *)base;
    return 1;
}

static void send_ipi(uint32_t apic_id, uint32_t low) {
    while (lapic[0x300 / 4] & (1u << 12)) {}    /* delivery pending */
    lapic[0x310 / 4] = apic_id << 24;
    lapic[0x300 / 4] = low;
    while (lapic[0x300 / 4] & (1u << 12)) {}
}

/* Start one AP and wait up to a second for it to check in. Returns whether
 * it did. */
static int start_ap(uint32_t apic_id, uint64_t n) {
    uint64_t before = bst_platform_cpus();
    bst_ap_stack = (uint64_t)&ap_stacks[n][AP_STACK];
    send_ipi(apic_id, 0x4500);                          /* INIT */
    udelay(10000);
    for (int i = 0; i < 2 && bst_platform_cpus() == before; i++) {
        send_ipi(apic_id, 0x4600 | (AP_TRAMPOLINE >> 12)); /* startup */
        udelay(200);
    }
    for (int i = 0; i < 1000000 && bst_platform_cpus() == before; i++) udelay(1);
    return bst_platform_cpus() != before;
}

void bst_smp_init(void) {
    uint64_t apic_msr = rdmsr(0x1B);
    if (!(apic_msr & (1u << 11))) return;               /* APIC globally disabled */
    uint64_t len = 0, madt = find_madt(&len);
    if (!madt || !map_lapic(apic_msr & 0xFFFFF000ull)) return;
    uint64_t size = (uint64_t)(bst_ap_trampoline_end - bst_ap_trampoline);
    if (size > 4096) return;
    for (uint64_t i = 0; i < size; i++) ((volatile char *)AP_TRAMPOLINE)[i] = bst_ap_trampoline[i];
    __asm__ volatile ("sidt %0" : "=m"(ap_idtr));

    uint32_t self = lapic[0x20 / 4] >> 24;
    uint64_t started = 0;
    /* Processor-local APIC entries: type 0, length 8, flags bit 0 = enabled. */
    for (uint64_t i = 44; i + 2 <= len && started < MAX_CPUS - 1; ) {
        uint64_t type = rd8(madt + i), elen = rd8(madt + i + 1);
        if (elen < 2 || i + elen > len) break;
        if (type == 0 && elen >= 8 && (rd32(madt + i + 4) & 1)) {
            uint32_t id = rd8(madt + i + 3);
            /* An AP that misses its deadline may still wake up later on
             * bst_ap_stack, so start no other AP after it. */
            if (id != self && !start_ap(id, started++)) return;
        }
        i += elen;
    }
}
