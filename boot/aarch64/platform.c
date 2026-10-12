/*
 * BastiOS on AArch64, QEMU virt machine: the platform in C. Part of the
 * trusted base (with boot.S). It reads the device tree for the memory map,
 * builds the kernel's translation table, drives the GICv3 and the generic
 * timer, and gives the kernel the few answers it asks for. It decides
 * nothing: the kernel's policy is in Bastio.
 *
 * The interrupt controller is the one the device tree describes: a GICv3
 * (distributor and redistributors) or a GICv2 (distributor and CPU
 * interface, as on a GIC-400); QEMU virt's GICv3 addresses are the fallback
 * without a tree.
 */
#include <stdint.h>
#include <stddef.h>

#define TICK_HZ 100
#define TIMER_INTID 30          /* the EL1 physical timer, a private interrupt */
#define V_TIMER 32              /* the boot processor's tick, which counts the kernel's time */
#define V_LTIMER 48             /* any other processor's tick */
#define MAX_CPUS 16
#define CPU_STACK 65536

/* One of these per processor, found through TPIDR_EL1 (boot.S): the saved
 * user context (37 words used), the words the compiler's prelude may keep
 * per processor (bst_platform_mmu_state, unused so far), the interrupt that
 * ended bst_cpu_wait, and the processor's place in the table. The offsets
 * are boot.S's PCPU_* constants. */
struct pcpu {
    uint64_t ctx[40];
    uint64_t mmu[4];            /* bst_platform_mmu_state; word 3 is the user table */
    uint64_t irq;
    uint64_t id;
    uint64_t mpidr;
    uint64_t shadow;            /* the table user code runs on (bst_shadow_root[id]) */
};
_Static_assert(__builtin_offsetof(struct pcpu, mmu[3]) == 344, "boot.S PCPU_MMU3");
_Static_assert(__builtin_offsetof(struct pcpu, irq) == 352, "boot.S PCPU_IRQ");
_Static_assert(__builtin_offsetof(struct pcpu, id) == 360, "boot.S PCPU_ID");
_Static_assert(__builtin_offsetof(struct pcpu, shadow) == 376, "boot.S PCPU_SHADOW");
_Static_assert(sizeof(struct pcpu) == 384, "boot.S PCPU_SIZE");
struct pcpu bst_pcpus[MAX_CPUS];
/* Each processor's own top-level table while it runs user code: slot 0 is
 * the process's window (the level-3 table in slot 1 of the process's table),
 * so the window is seen from address 0 and nothing else is in it. */
uint8_t bst_shadow_root[MAX_CPUS][4096] __attribute__((aligned(4096)));
uint8_t bst_cpu_stacks[MAX_CPUS][CPU_STACK] __attribute__((aligned(16)));
static inline struct pcpu *this_cpu(void) {
    struct pcpu *p;
    __asm__ volatile ("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}

/* The memory map bst_kmain gets: a count, then base and length of each run. */
#define MAX_RUNS 8
uint64_t bst_memmap[1 + 2 * MAX_RUNS];
/* The processors the device tree lists (their MPIDR_EL1 affinities, the
 * boot processor first), filled in by bst_early. */
static uint64_t cpu_mpidr[MAX_CPUS];
static uint64_t ncpu_dt;
/* The interrupt controller: its version (3 or 2) and the two frames the
 * tree lists (GICv3: distributor, redistributors; GICv2: distributor, CPU
 * interface). QEMU virt's GICv3 is the default. */
static unsigned gic_ver = 3;
/* The console: the first PL011 the tree lists (the prelude's bst_uart reads
 * this; QEMU virt's address is the default). */
uint64_t bst_uart_base = 0x09000000ull;
static unsigned psci_smc;                   /* the device tree's psci method: smc (1) or hvc (0, QEMU virt's) */
static uint64_t GICD = 0x08000000ull;
static uint64_t GICR = 0x080A0000ull;       /* GICv2: the CPU interface (GICC) */

/* The device tree itself, copied before the page pool can reuse the RAM it
 * sits in (QEMU puts it in RAM): the kernel reads it byte by byte with
 * boot_info (INFO_DTB) and parses it in Bastio. */
#define DTB_MAX 65536
static uint8_t dtb_copy[DTB_MAX];
static uint64_t dtb_len;

/* Tables of 2 MiB blocks for 1 GiB regions that hold RAM and something else. */
#define MAX_L2 8
static uint64_t kernel_l2[MAX_L2][512] __attribute__((aligned(4096)));

/* The bytes of [lo, hi) that the memory map covers. */
static uint64_t ram_overlap(uint64_t lo, uint64_t hi) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < bst_memmap[0]; i++) {
        uint64_t b = bst_memmap[1 + 2 * i], e = b + bst_memmap[2 + 2 * i];
        if (b < lo) b = lo;
        if (e > hi) e = hi;
        if (e > b) n += e - b;
    }
    return n;
}

extern uint64_t bst_kernel_root[512];
extern uint64_t kernel_l1[512];

/* Tells the compiled kernel (the prelude's BST_DEFERRED) that user code runs
 * on a table of its own, which bst_cpu_enter loads into TTBR0 from word 3 of
 * bst_platform_mmu_state: the kernel's tables stay in TTBR0 in Bastio code. */
const char bst_platform_deferred_ttbr0 = 1;
uint64_t *bst_platform_mmu_state(void) { return this_cpu()->mmu; }

/* TTBR1 maps one page, the trampoline (boot.S), at TRAMP_VA: the exception
 * vectors and the code that switches TTBR0, which must be reachable while
 * TTBR0 holds a user table. One table per level, all index 0. */
extern char tramp_page[];
uint64_t bst_ttbr1_tables[4][512] __attribute__((aligned(4096)));
#define TRAMP_PTE ((uint64_t)tramp_page | 3 | (1ull << 10) | (3ull << 8) | (1ull << 7) | (1ull << 54))

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int starts_with(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

static int same(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

/* Reads `n` big-endian 32-bit words as one number. */
static uint64_t cells(const uint8_t *p, uint32_t n) {
    uint64_t v = 0;
    while (n--) { v = v << 32 | be32(p); p += 4; }
    return v;
}

/* The RAM runs in the device tree's memory nodes, into bst_memmap. Without
 * a tree (or without memory in it) it falls back to QEMU's default: 128 MiB
 * at 1 GiB. */
static void read_memory(const uint8_t *dtb) {
    uint64_t n = 0;
    if (dtb && be32(dtb) == 0xd00dfeed) {
        const uint8_t *st = dtb + be32(dtb + 8);
        const uint8_t *strs = dtb + be32(dtb + 12);
        uint32_t ac = 2, sc = 1, cac = 1;
        int depth = 0, in_mem = 0, in_cpus = 0, in_cpu = 0;
        unsigned node_gic = 0;                       /* this node's compatible: 3, 2 or 0 */
        int node_uart = 0, have_uart = 0;
        int in_psci = 0;
        uint64_t node_reg[2] = { 0, 0 };
        int node_regs = 0;
        for (;;) {
            uint32_t tok = be32(st);
            st += 4;
            if (tok == 1) {
                const char *name = (const char *)st;
                size_t l = 0;
                while (name[l]) l++;
                st += (l + 1 + 3) & ~(size_t)3;
                depth++;
                in_mem = depth == 2 && starts_with(name, "memory");
                if (depth == 2 && same(name, "cpus")) in_cpus = 1;
                in_cpu = in_cpus && depth == 3 && starts_with(name, "cpu@");
                if (depth == 2) { node_gic = 0; node_regs = 0; node_uart = 0; }
                in_psci = depth == 2 && same(name, "psci");
            } else if (tok == 2) {
                if (depth == 2) {
                    in_cpus = 0;
                    if (node_gic && node_regs >= 2) {
                        gic_ver = node_gic;
                        GICD = node_reg[0];
                        GICR = node_reg[1];
                    }
                    if (node_uart && node_regs >= 1 && !have_uart) {
                        bst_uart_base = node_reg[0];
                        have_uart = 1;
                    }
                }
                depth--;
                in_mem = 0;
                in_cpu = 0;
                in_psci = 0;
            } else if (tok == 3) {
                uint32_t len = be32(st), off = be32(st + 4);
                const uint8_t *data = st + 8;
                const char *pname = (const char *)strs + off;
                st += 8 + ((len + 3) & ~3u);
                if (depth == 1 && same(pname, "#address-cells")) ac = be32(data);
                else if (depth == 1 && same(pname, "#size-cells")) sc = be32(data);
                else if (in_psci && same(pname, "method")) psci_smc = same((const char *)data, "smc");
                else if (depth == 2 && same(pname, "compatible")) {
                    for (uint32_t o = 0; o < len; ) {
                        const char *c = (const char *)data + o;
                        if (same(c, "arm,pl011")) node_uart = 1;
                        if (same(c, "arm,gic-v3")) node_gic = 3;
                        else if (same(c, "arm,cortex-a15-gic") || same(c, "arm,gic-400") || same(c, "arm,cortex-a9-gic")) node_gic = 2;
                        while (o < len && data[o]) o++;
                        o++;
                    }
                }
                else if (depth == 2 && !in_mem && same(pname, "reg") && ac <= 2 && sc <= 2 && len >= (ac + sc) * 8) {
                    node_reg[0] = cells(data, ac);
                    node_reg[1] = cells(data + (ac + sc) * 4, ac);
                    node_regs = 2;
                } else if (depth == 2 && !in_mem && same(pname, "reg") && ac <= 2 && len >= ac * 4) {
                    node_reg[0] = cells(data, ac);          /* a node with one frame: the UART */
                    node_regs = 1;
                }
                else if (in_cpus && depth == 2 && same(pname, "#address-cells")) cac = be32(data);
                else if (in_cpu && same(pname, "reg") && cac >= 1 && cac <= 2 && len >= cac * 4 && ncpu_dt < MAX_CPUS)
                    cpu_mpidr[ncpu_dt++] = cells(data, cac);
                else if (in_mem && same(pname, "reg") && ac <= 2 && sc <= 2) {
                    uint32_t stride = (ac + sc) * 4;
                    for (uint32_t o = 0; stride && o + stride <= len && n < MAX_RUNS; o += stride) {
                        uint64_t base = cells(data + o, ac), size = cells(data + o + ac * 4, sc);
                        if (size) {
                            bst_memmap[1 + 2 * n] = base;
                            bst_memmap[2 + 2 * n] = size;
                            n++;
                        }
                    }
                }
            } else if (tok == 4) {
            } else {
                break;
            }
        }
    }
    if (n == 0) {
        bst_memmap[1] = 0x40000000ull;
        bst_memmap[2] = 128ull << 20;
        n = 1;
    }
    bst_memmap[0] = n;
}

#define BLOCK_DEVICE (1ull | (1ull << 2) | (1ull << 10) | (1ull << 53) | (1ull << 54))
#define BLOCK_NORMAL (1ull | (3ull << 8) | (1ull << 10) | (1ull << 54))

/* Called by boot.S with the MMU off (so, compiled with -mstrict-align): the
 * memory map, and the kernel's map in top-level slot 0 (every user table
 * copies it): 1 GiB blocks (2 MiB blocks where RAM shares a GiB with devices), EL1 only, never executable at EL0. RAM is normal memory (MAIR index 0); the first 4 GiB without RAM are devices (MAIR index 1, not executable). Block 0
 * holds the devices (MAIR index 1, not executable); the blocks that hold
 * RAM are normal memory (MAIR index 0). */
void bst_early(const uint8_t *dtb) {
    read_memory(dtb);
    /* QEMU pads the tree to 1 MiB (totalsize says so) though the blocks it
     * holds take a few KiB: what matters is that both blocks lie inside the
     * part kept, and the copy's totalsize says how much that is. */
    if (dtb && be32(dtb) == 0xd00dfeed) {
        uint64_t total = be32(dtb + 4);
        uint64_t st = (uint64_t)be32(dtb + 8) + be32(dtb + 36);      /* structure block's end */
        uint64_t strs = (uint64_t)be32(dtb + 12) + be32(dtb + 32);   /* strings block's end */
        if (total > DTB_MAX && st <= DTB_MAX && strs <= DTB_MAX) total = DTB_MAX;
        if (total <= DTB_MAX) {
            dtb_len = total;
            for (uint64_t i = 0; i < dtb_len; i++) dtb_copy[i] = dtb[i];
            for (int i = 0; i < 4; i++) dtb_copy[4 + i] = (uint8_t)(total >> (24 - 8 * i));
        }
    }
    uint64_t top = 0;
    for (uint64_t i = 0; i < bst_memmap[0]; i++) {
        uint64_t end = bst_memmap[1 + 2 * i] + bst_memmap[2 + 2 * i];
        if (end > top) top = end;
    }
    if (top > (512ull << 30)) top = 512ull << 30;
    for (int i = 0; i < 512; i++) kernel_l1[i] = 0;
    /* Every 1 GiB of the address space the kernel can reach is one of: all
     * RAM (a normal-memory block), no RAM and in the first 4 GiB (a device
     * block: where the peripherals of every board lie), or mixed (a table of
     * 2 MiB blocks, each normal if RAM touches it, device otherwise: a
     * board with RAM from address 0 and peripherals near 4 GiB). */
    unsigned l2used = 0;
    for (uint64_t g = 0; g < 512 && ((g << 30) < top || g < 4); g++) {
        uint64_t lo = g << 30, hi = lo + (1ull << 30);
        uint64_t cov = ram_overlap(lo, hi);
        if (cov == 0) {
            if (g < 4) kernel_l1[g] = BLOCK_DEVICE | lo;
        } else if (cov == (1ull << 30) || l2used == MAX_L2) {
            kernel_l1[g] = BLOCK_NORMAL | lo;
        } else {
            uint64_t *l2 = kernel_l2[l2used++];
            for (uint64_t i = 0; i < 512; i++) {
                uint64_t a = lo + (i << 21);
                l2[i] = (ram_overlap(a, a + (2ull << 20)) ? BLOCK_NORMAL : BLOCK_DEVICE) | a;
            }
            kernel_l1[g] = (uint64_t)l2 | 3;
        }
    }
    bst_kernel_root[0] = (uint64_t)kernel_l1 | 3;
    bst_ttbr1_tables[0][0] = (uint64_t)bst_ttbr1_tables[1] | 3;
    bst_ttbr1_tables[1][0] = (uint64_t)bst_ttbr1_tables[2] | 3;
    bst_ttbr1_tables[2][0] = (uint64_t)bst_ttbr1_tables[3] | 3;
    bst_ttbr1_tables[3][0] = TRAMP_PTE;       /* EL1 read-only, never executable at EL0 */
}

static inline void w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t r32(uint64_t a) { return *(volatile uint32_t *)a; }
static inline void w8(uint64_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

static uint64_t tick_cycles;

static inline uint64_t r64(uint64_t a) { return *(volatile uint64_t *)a; }

/* The redistributor frame of this processor: GICR_TYPER carries the
 * affinity (bits 32-63) the frame belongs to; frames follow one another
 * until one is marked last. */
static uint64_t redistributor(uint64_t mpidr) {
    uint64_t aff = (mpidr & 0xFFFFFFull) | ((mpidr >> 8) & 0xFF000000ull);
    uint64_t rd = GICR;
    for (int i = 0; i < MAX_CPUS; i++, rd += 0x20000) {
        uint64_t typer = r64(rd + 0x0008);
        if ((typer >> 32) == aff) return rd;
        if (typer & 0x10) break;
    }
    return GICR;
}

/* The interrupt controller for the calling processor: GICv3 with its
 * redistributor awake, or GICv2 with its CPU interface on; the timer's
 * interrupt enabled and every priority let through. */
static void gic_cpu_init(void) {
    if (gic_ver == 2) {
        w32(GICD + 0x0080, 0);                              /* GICD_IGROUPR0: group 0, signalled as IRQ while GICC_CTLR.FIQEn is 0 */
        w8(GICD + 0x0400 + TIMER_INTID, 0x80);               /* priority (a byte register: banked per processor) */
        w32(GICD + 0x0100, 1u << TIMER_INTID);               /* GICD_ISENABLER0 */
        w32(GICR + 0x0004, 0xFF);                            /* GICC_PMR */
        w32(GICR + 0x0000, 1);                               /* GICC_CTLR: enable */
        return;
    }
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint64_t rd = redistributor(mpidr);
    w32(rd + 0x0014, r32(rd + 0x0014) & ~2u);                /* GICR_WAKER.ProcessorSleep = 0 */
    while (r32(rd + 0x0014) & 4u) { }                        /* ChildrenAsleep */
    uint64_t sgi = rd + 0x10000;
    w32(sgi + 0x0080, 0xFFFFFFFFu);                          /* group 1 */
    w8(sgi + 0x0400 + TIMER_INTID, 0x80);                    /* priority */
    w32(sgi + 0x0100, 1u << TIMER_INTID);                    /* enable */
    uint64_t sre;
    __asm__ volatile ("mrs %0, icc_sre_el1" : "=r"(sre));
    __asm__ volatile ("msr icc_sre_el1, %0; isb" : : "r"(sre | 1));
    __asm__ volatile ("msr icc_pmr_el1, %0" : : "r"(0xFFull));
    __asm__ volatile ("msr icc_igrpen1_el1, %0; isb" : : "r"(1ull));
}

/* The calling processor's timer, ticking TICK_HZ times a second, or off. */
static void timer_start(void) {
    uint64_t f;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    tick_cycles = f / TICK_HZ;
    __asm__ volatile ("msr cntp_tval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(tick_cycles), "r"(1ull));
}
static void timer_stop(void) {
    __asm__ volatile ("msr cntp_ctl_el0, %0; isb" : : "r"(0ull));
}

/* The boot processor: the distributor on, then its own interface and
 * timer. */
void bst_gic_init(void) {
    w32(GICD + 0x0000, gic_ver == 2 ? 1u : (1u << 4) | (1u << 1) | 1u);   /* v3: ARE, group 1 non-secure; v2: enable */
    gic_cpu_init();
    timer_start();
}

/* An interrupt was taken: acknowledge it, rearm the timer if it was that,
 * and return the kernel's vector for it: 32 for the boot processor's tick
 * (it counts the kernel's time), 48 for another processor's, 0 if it was
 * spurious. */
uint64_t bst_irq_ack(void) {
    uint64_t id;
    if (gic_ver == 2) id = r32(GICR + 0x000C);               /* GICC_IAR */
    else __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r"(id));
    id &= 0xFFFFFF;
    if (id >= 1020) return 0;
    uint64_t v = 6;
    if (id == TIMER_INTID) {
        __asm__ volatile ("msr cntp_tval_el0, %0; isb" : : "r"(tick_cycles));
        v = this_cpu()->id == 0 ? V_TIMER : V_LTIMER;
    }
    if (gic_ver == 2) w32(GICR + 0x0010, (uint32_t)id);      /* GICC_EOIR */
    else __asm__ volatile ("msr icc_eoir1_el1, %0; isb" : : "r"(id));
    return v;
}

uint64_t bst_cpu_wait(void) {
    struct pcpu *c = this_cpu();
    c->irq = 0;
    __asm__ volatile ("msr daifclr, #2; wfi; isb; msr daifset, #2" : : : "memory");
    return c->irq;
}

/* ---- more than one processor: PSCI CPU_ON, then a table of work. A
 * processor started here parks (wfe, interrupts masked, its timer off)
 * until bst_platform_smp_start gives it a function to run; it runs that
 * with its timer on, then parks again. */
extern void bst_secondary_entry(void);

static volatile uint64_t cpus_online;       /* processors parked or working, the boot one not counted */
static void (*volatile work_fn)(void *);
static void *volatile work_arg;
static volatile uint64_t work_gen, work_done;

/* A PSCI call through the conduit the device tree names (hvc on QEMU virt
 * booted at EL1, smc when the machine has EL2 or EL3 firmware). */
static int64_t psci_call(uint64_t fn, uint64_t a, uint64_t b, uint64_t c) {
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a;
    register uint64_t x2 __asm__("x2") = b;
    register uint64_t x3 __asm__("x3") = c;
    if (psci_smc) __asm__ volatile ("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else __asm__ volatile ("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}
static int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx) {
    return psci_call(0xC4000003ull, mpidr, entry, ctx);          /* CPU_ON, 64-bit */
}
void bst_psci_system_off(void) {
    psci_call(0x84000008ull, 0, 0, 0);                           /* SYSTEM_OFF */
}

/* Called by boot.S on the boot processor once the MMU and the interrupt
 * controller are up: starts every other processor the device tree lists,
 * one at a time (each checks in before the next is started). */
void bst_smp_boot(void) {
    uint64_t me;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(me));
    bst_pcpus[0].id = 0;
    bst_pcpus[0].mpidr = me;
    uint64_t n = 1;
    for (uint64_t i = 0; i < ncpu_dt && n < MAX_CPUS; i++) {
        uint64_t want = cpu_mpidr[i];
        if (((want ^ me) & 0xFF00FFFFFFull) == 0) continue;   /* this one */
        bst_pcpus[n].id = n;
        bst_pcpus[n].mpidr = want;
        if (psci_cpu_on(want, (uint64_t)bst_secondary_entry, n) != 0) continue;
        for (uint64_t spin = 0; spin < 100000000ull && cpus_online < n; spin++) { }
        if (cpus_online < n) break;                            /* it never came: no more */
        n++;
    }
}

/* Called by bst_secondary_entry on a new processor, with the MMU on. */
void bst_secondary_main(void) {
    gic_cpu_init();
    __atomic_fetch_add(&cpus_online, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile ("sev");
    uint64_t seen = 0;
    for (;;) {
        uint64_t g = __atomic_load_n(&work_gen, __ATOMIC_ACQUIRE);
        if (g != seen) {
            seen = g;
            timer_start();
            work_fn(work_arg);
            timer_stop();
            __atomic_fetch_add(&work_done, 1, __ATOMIC_RELEASE);
            __asm__ volatile ("dsb ish; sev");
            continue;
        }
        __asm__ volatile ("wfe");
    }
}

uint64_t bst_platform_cpus(void) { return 1 + __atomic_load_n(&cpus_online, __ATOMIC_ACQUIRE); }
void bst_platform_smp_start(void (*fn)(void *), void *arg) {
    work_fn = fn;
    work_arg = arg;
    work_done = 0;
    __atomic_fetch_add(&work_gen, 1, __ATOMIC_RELEASE);
    __asm__ volatile ("dsb ish; sev");
}
void bst_platform_smp_join(void) {
    while (__atomic_load_n(&work_done, __ATOMIC_ACQUIRE) < __atomic_load_n(&cpus_online, __ATOMIC_ACQUIRE))
        __asm__ volatile ("wfe");
}
uint64_t bst_platform_acpi(uint64_t sig, uint64_t off) { (void)sig; (void)off; return 256; }
/* boot_info: query 10 is the device tree, byte i of it, 256 past its end
 * (or when there is none); query 11 is the generic timer's rate in Hz
 * (CNTFRQ_EL0); every other query is 0. */
uint64_t bst_platform_info(uint64_t what, uint64_t i) {
    if (what == 11) {
        uint64_t f;
        __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
        return f;
    }
    if (what != 10) return 0;
    return i < dtb_len ? dtb_copy[i] : 256;
}
