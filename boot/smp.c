/*
 * Multiprocessor startup. Part of the trusted base.
 *
 * Runs on the bootstrap processor (BSP) before the Bastio kernel: finds the
 * other processors in the ACPI MADT and starts each
 * application processor (AP) with INIT and two startup IPIs (Intel SDM
 * vol. 3, 8.4.4). Each AP runs boot/ap.S, checks in through bst_ap_main and
 * parks with interrupts off. The Bastio kernel sees the result through the
 * `cpu_count` builtin, and hands the parked APs work with `smp_run`
 * (bst_platform_smp_start / bst_platform_smp_join below).
 *
 * Firmware tables are untrusted input: every read is checked against the
 * table's length and against what boot.S maps (the identity-mapped first
 * 1 GiB, and the device window at 2-4 GiB, where firmware often puts them).
 *
 * It also finds the ACPI tables for the Bastio kernel, which reads them a
 * byte at a time through the `boot_acpi` builtin (bst_platform_acpi) and
 * parses them itself, and calibrates the local APIC timer the APs use
 * against the HPET, else the ACPI PM timer, else the PIT.
 *
 * Why this is C and not Bastio yet: it runs before the Bastio kernel does,
 * since each AP programs its timer with the calibrated count as it starts.
 * The kernel's own use of ACPI (power off, reset, the power button, its
 * clock) is Bastio: src/acpi.bastio.
 */
#include <stdint.h>
#include "core.h"                   /* MAX_CPUS (cpu_count promises at most 4096), the block layout */

#define AP_TRAMPOLINE 0x8000        /* must match ap.S */
#define IDENT_LIMIT   (1ull << 30)  /* boot.S identity-maps the first 1 GiB */

extern uint64_t bst_gdt[];
extern const char bst_ap_trampoline[], bst_ap_trampoline_end[];

/* One block per processor (core.h); block 0 is the bootstrap processor's. */
uint8_t bst_cores[MAX_CPUS][CORE_BLOCK] __attribute__((aligned(CORE_BLOCK)));

volatile uint64_t bst_ap_stack;     /* read by ap.S; one AP starts at a time */
static uint64_t cpus_online = 1;    /* the BSP; updated atomically */
static struct __attribute__((packed)) { uint16_t limit; uint64_t base; } ap_idtr;
static volatile uint32_t *lapic;
static uint32_t lapic_ticks_10ms;   /* local APIC timer count for 10 ms, divide by 16 */
static uint64_t syscall_msrs[3];    /* STAR, LSTAR, FMASK as bst_platform_init set them */

static inline uint8_t *this_core(void) {
    uint64_t sp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(sp));
    return (uint8_t *)(sp & ~(uint64_t)(CORE_BLOCK - 1));
}

/* The saved user context of the processor running the caller. */
uint64_t *bst_platform_ctx(void) { return (uint64_t *)(this_core() + CORE_CTX); }

uint64_t bst_platform_cpus(void) { return __atomic_load_n(&cpus_online, __ATOMIC_SEQ_CST); }

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
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

/* A valid RSDP on a 16-byte boundary in [from, to), or 0. */
static uint64_t scan_rsdp(uint64_t from, uint64_t to) {
    for (uint64_t a = (from + 15) & ~15ull; a + 20 <= to; a += 16)
        if (sig_is(a, "RSD PTR ", 8) && sum_ok(a, 20)) return a;
    return 0;
}

extern uint8_t bst_boot_rsdp[36];   /* multiboot.c: the copy Multiboot 2 passed */
extern uint32_t bst_boot_rsdp_len;

/* The RSDP: the copy a Multiboot 2 loader passed (GRUB, from the ISO; under
 * UEFI the only way to it), else where BIOS firmware puts it, on a 16-byte
 * boundary in the first KiB of the EBDA or in 0xE0000-0xFFFFF (ACPI 6.5,
 * 5.2.5.1). */
static uint64_t find_rsdp(void) {
    if (bst_boot_rsdp_len >= 20 && sig_is((uint64_t)bst_boot_rsdp, "RSD PTR ", 8) && sum_ok((uint64_t)bst_boot_rsdp, 20)) {
        /* A revision 2 RSDP is only usable whole: find_madt reads the XSDT
         * address from its last 16 bytes. */
        if (rd8((uint64_t)bst_boot_rsdp + 15) < 2 || bst_boot_rsdp_len >= 36) return (uint64_t)bst_boot_rsdp;
    }
    uint64_t ebda = (uint64_t)rd16(0x40E) << 4;
    uint64_t a = 0;
    if (ebda >= 0x80000 && ebda + 1024 <= 0x100000) a = scan_rsdp(ebda, ebda + 1024);
    if (!a) a = scan_rsdp(0xE0000, 0x100000);
    return a;
}

/* Is [a, a + n) mapped by boot.S: the first 1 GiB, or the device window
 * from 2 to 4 GiB (uncached, which only makes the reads slower)? */
static int mapped(uint64_t a, uint64_t n) {
    if (a < IDENT_LIMIT) return n <= IDENT_LIMIT - a;
    return a >= 2 * IDENT_LIMIT && a < 4 * IDENT_LIMIT && n <= 4 * IDENT_LIMIT - a;
}

/* A system description table at `a` with a valid header and checksum, or 0. */
static uint64_t sdt_len(uint64_t a) {
    if (a == 0 || !mapped(a, 36)) return 0;
    uint64_t n = rd32(a + 4);
    if (n < 36 || !mapped(a, n) || !sum_ok(a, n)) return 0;
    return n;
}

/* The table with signature `sig` (its four letters, little-endian) listed
 * in the XSDT or RSDT, or 0; its length in *len. */
static uint64_t find_table(uint32_t sig, uint64_t *len) {
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
        if (tn && rd32(t) == sig) {
            *len = tn;
            return t;
        }
    }
    return 0;
}

#define SIG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static uint64_t find_madt(uint64_t *len) {
    uint64_t t = find_table(SIG('A', 'P', 'I', 'C'), len);
    return *len >= 44 ? t : 0;
}

/* The DSDT, which the FADT points at (X_DSDT if it has one), or 0. */
static uint64_t find_dsdt(uint64_t *len) {
    uint64_t fn = 0, f = find_table(SIG('F', 'A', 'C', 'P'), &fn);
    if (!f || fn < 44) return 0;
    uint64_t d = rd32(f + 40);
    if (fn >= 148 && rd64(f + 140) != 0) d = rd64(f + 140);
    uint64_t dn = sdt_len(d);
    if (!dn || rd32(d) != SIG('D', 'S', 'D', 'T')) return 0;
    *len = dn;
    return d;
}

/* The tables the kernel asked for, so a byte costs no search: the kernel
 * reads its tables only at boot, and only a few of them. */
static struct { uint32_t sig; uint64_t at, len; } acpi_seen[8];
static uint64_t acpi_nseen;

/* The boot_acpi builtin: byte `off` of the table with signature `sig`, or
 * 256 past its end or without one (the promise the prover relies on). */
uint64_t bst_platform_acpi(uint64_t sig, uint64_t off) {
    if (sig >> 32) return 256;
    uint64_t i = 0;
    while (i < acpi_nseen && acpi_seen[i].sig != sig) i++;
    if (i == acpi_nseen) {
        uint64_t len = 0, at;
        if (sig == SIG('D', 'S', 'D', 'T')) at = find_dsdt(&len);
        else at = find_table((uint32_t)sig, &len);
        if (!at) len = 0;
        if (i == sizeof acpi_seen / sizeof acpi_seen[0]) {
            return off < len ? rd8(at + off) : 256;    /* not kept */
        }
        acpi_seen[i].sig = (uint32_t)sig;
        acpi_seen[i].at = at;
        acpi_seen[i].len = len;
        acpi_nseen++;
    }
    return off < acpi_seen[i].len ? rd8(acpi_seen[i].at + off) : 256;
}

/* The local APIC lies in the device window boot.S maps uncached (2-4 GiB). */
static int map_lapic(uint64_t base) {
    if (base < 3 * IDENT_LIMIT || base >= 4 * IDENT_LIMIT) return 0;
    lapic = (volatile uint32_t *)base;
    return 1;
}

/* Called from trap_common for every vector: local APIC interrupts must be
 * acknowledged at the local APIC (the 8259's are acknowledged by the kernel). */
void bst_lapic_ack(uint64_t vector) {
    if (vector == LAPIC_TIMER_VECTOR && lapic) lapic[0xB0 / 4] = 0;
}

/* Processor i's task state segment: user-mode traps land on the trap stack
 * at the end of its block. No I/O permission map. Also points
 * KERNEL_GS_BASE at the block for the `syscall` entry. */
static void core_init(uint64_t i) {
    uint8_t *core = bst_cores[i];
    uint64_t top = (uint64_t)(core + CORE_BLOCK);
    *(uint64_t *)(core + CORE_TRAP_TOP) = top;
    uint8_t *tss = core + CORE_TSS;
    for (int b = 0; b < 104; b++) tss[b] = 0;
    *(uint64_t *)(tss + 4) = top;                   /* rsp0 */
    *(uint64_t *)(tss + 36) = (uint64_t)(core + CORE_IST_TOP);   /* ist1 */
    *(uint16_t *)(tss + 102) = 104;                 /* iomap_base: none */
    uint64_t base = (uint64_t)tss, limit = 103;
    bst_gdt[5 + 2 * i] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40)
                       | (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    bst_gdt[6 + 2 * i] = base >> 32;
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)TSS_SEL(i)));
    wrmsr(0xC0000101, 0);                           /* GS_BASE: the user's, zero */
    wrmsr(0xC0000102, (uint64_t)core);              /* KERNEL_GS_BASE */
}

/* Called by ap.S on each AP, on the kernel stack of its block, with
 * interrupts off. Sets the processor up like the bootstrap one, starts its
 * local APIC timer (preemption once it runs processes), checks in, and
 * parks: it sleeps between timer interrupts and runs nothing. */
/* Work for the APs (bst_platform_smp_start): the function, its argument,
 * and a generation number each AP compares with the last one it ran. */
static void (*volatile work_fn)(void *);
static void *volatile work_arg;
static uint64_t work_gen, work_done;

/* Runs fn(arg) on every AP. The caller runs it on this processor itself,
 * then waits in bst_platform_smp_join. One piece of work at a time. */
void bst_platform_smp_start(void (*fn)(void *), void *arg) {
    work_fn = fn;
    work_arg = arg;
    __atomic_store_n(&work_done, 0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&work_gen, 1, __ATOMIC_RELEASE);
}

/* Waits until every AP has returned from the current work. */
void bst_platform_smp_join(void) {
    while (__atomic_load_n(&work_done, __ATOMIC_ACQUIRE) + 1 < cpus_online)
        __asm__ volatile ("pause");
}

void bst_ap_main(void) {
    __asm__ volatile ("lidt %0" : : "m"(ap_idtr));
    for (int m = 0; m < 3; m++) wrmsr(0xC0000081 + m + (m == 2), syscall_msrs[m]);
    core_init((uint64_t)(this_core() - bst_cores[0]) / CORE_BLOCK);
    lapic[0xF0 / 4] = 0x1FF;                        /* enable, spurious vector 0xFF */
    lapic[0x3E0 / 4] = 0x3;                         /* timer: divide by 16 */
    lapic[0x320 / 4] = 0x20000 | LAPIC_TIMER_VECTOR; /* periodic */
    lapic[0x380 / 4] = lapic_ticks_10ms;
    __atomic_fetch_add(&cpus_online, 1, __ATOMIC_SEQ_CST);
    /* Park: wait for work, waking on the local timer to look. Interrupts
     * are on only inside hlt, as in cpu_idle. */
    uint64_t seen = 0;
    for (;;) {
        uint64_t g = __atomic_load_n(&work_gen, __ATOMIC_ACQUIRE);
        if (g != seen) {
            seen = g;
            work_fn(work_arg);
            __atomic_fetch_add(&work_done, 1, __ATOMIC_RELEASE);
            continue;
        }
        __asm__ volatile ("sti; hlt; cli");
    }
}

static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* The local APIC timer's count over 10 ms of the HPET, if the ACPI tables
 * list one in the device window with a sane period (at most 100 ns a tick,
 * as the specification requires); 0 otherwise. */
static uint32_t lapic_vs_hpet(void) {
    uint64_t len = 0, t = find_table(SIG('H', 'P', 'E', 'T'), &len);
    if (!t || len < 52 || rd8(t + 40) != 0) return 0;          /* memory space */
    uint64_t base = rd64(t + 44);
    if (base % 1024 || base < 3 * IDENT_LIMIT || base > 4 * IDENT_LIMIT - 1024) return 0;
    volatile uint64_t *h = (volatile uint64_t *)base;
    uint64_t period = h[0] >> 32;                               /* femtoseconds a tick */
    if (period == 0 || period > 100000000) return 0;
    h[0x10 / 8] |= 1;                                           /* the counter runs */
    uint64_t want = 10000000000000ull / period, c0 = h[0xF0 / 8];
    lapic[0x380 / 4] = 0xFFFFFFFF;
    for (uint64_t i = 0; i < 100000000 && h[0xF0 / 8] - c0 < want; i++) {}
    uint32_t n = 0xFFFFFFFF - lapic[0x390 / 4];
    lapic[0x380 / 4] = 0;
    return h[0xF0 / 8] - c0 >= want ? n : 0;
}

/* The same over 10 ms of the ACPI PM timer (3.579545 MHz, 24 or 32 bits)
 * the FADT names, if it has one in I/O space; 0 otherwise. */
static uint32_t lapic_vs_pmtimer(void) {
    uint64_t len = 0, f = find_table(SIG('F', 'A', 'C', 'P'), &len);
    if (!f || len < 116) return 0;
    uint64_t port = rd32(f + 76);
    if (port == 0 || port > 0xFFFC || rd8(f + 91) < 4) return 0;
    uint32_t mask = (rd32(f + 112) & (1u << 8)) ? 0xFFFFFFFFu : 0xFFFFFFu;
    uint32_t t0 = inl((uint16_t)port) & mask, d = 0;
    lapic[0x380 / 4] = 0xFFFFFFFF;
    for (uint64_t i = 0; i < 100000000 && d < 35795; i++) d = ((inl((uint16_t)port) & mask) - t0) & mask;
    uint32_t n = 0xFFFFFFFF - lapic[0x390 / 4];
    lapic[0x380 / 4] = 0;
    return d >= 35795 ? n : 0;
}

/* Count the local APIC timer against 10 ms of the HPET, else of the ACPI PM
 * timer, else of PIT channel 2 (the speaker channel, which the kernel never
 * uses; newer PCs may gate it off), on the bootstrap processor with its
 * timer masked. The kernel picks its clock in the same order. */
static void calibrate_lapic_timer(void) {
    lapic[0x3E0 / 4] = 0x3;
    lapic[0x320 / 4] = 0x10000;                     /* masked, one-shot */
    uint32_t n = lapic_vs_hpet();
    if (!n) n = lapic_vs_pmtimer();
    if (n) {
        lapic_ticks_10ms = n;
        return;
    }
    outb(0x61, (inb(0x61) & ~0x02) & ~0x01);        /* speaker off, gate low */
    outb(0x43, 0xB0);                               /* channel 2, lo/hi, mode 0 */
    outb(0x42, 11932 & 0xFF);                       /* 1193182 Hz / 100 */
    outb(0x42, 11932 >> 8);
    lapic[0x380 / 4] = 0xFFFFFFFF;
    outb(0x61, inb(0x61) | 0x01);                   /* gate high: count */
    for (uint64_t i = 0; i < 100000000 && !(inb(0x61) & 0x20); i++) {}
    n = 0xFFFFFFFF - lapic[0x390 / 4];
    outb(0x61, inb(0x61) & ~0x01);
    lapic[0x380 / 4] = 0;
    lapic_ticks_10ms = n ? n : 100000;
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
    bst_ap_stack = (uint64_t)(bst_cores[n] + CORE_KSTACK);
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
    core_init(0);
    uint64_t apic_msr = rdmsr(0x1B);
    if (!(apic_msr & (1u << 11))) return;               /* APIC globally disabled */
    uint64_t len = 0, madt = find_madt(&len);
    if (!madt || !map_lapic(apic_msr & 0xFFFFF000ull)) return;
    uint64_t size = (uint64_t)(bst_ap_trampoline_end - bst_ap_trampoline);
    if (size > 4096) return;
    for (uint64_t i = 0; i < size; i++) ((volatile char *)AP_TRAMPOLINE)[i] = bst_ap_trampoline[i];
    __asm__ volatile ("sidt %0" : "=m"(ap_idtr));
    for (int m = 0; m < 3; m++) syscall_msrs[m] = rdmsr(0xC0000081 + m + (m == 2));
    calibrate_lapic_timer();

    uint32_t self = lapic[0x20 / 4] >> 24;
    uint64_t started = 0;
    /* Processor-local APIC entries: type 0, length 8, flags bit 0 = enabled. */
    for (uint64_t i = 44; i + 2 <= len && started + 1 < MAX_CPUS; ) {
        uint64_t type = rd8(madt + i), elen = rd8(madt + i + 1);
        if (elen < 2 || i + elen > len) break;
        if (type == 0 && elen >= 8 && (rd32(madt + i + 4) & 1)) {
            uint32_t id = rd8(madt + i + 3);
            /* An AP that misses its deadline may still wake up later on
             * bst_ap_stack, so start no other AP after it. */
            if (id != self && !start_ap(id, ++started)) return;
        }
        i += elen;
    }
}
