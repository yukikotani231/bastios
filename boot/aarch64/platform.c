/*
 * BastiOS on AArch64, QEMU virt machine: the platform in C. Part of the
 * trusted base (with boot.S). It reads the device tree for the memory map,
 * builds the kernel's translation table, drives the GICv3 and the generic
 * timer, and gives the kernel the few answers it asks for. It decides
 * nothing: the kernel's policy is in Bastio.
 *
 * The addresses of the interrupt controller are QEMU virt's own
 * (-M virt,gic-version=3); a device-tree walk for them comes with real
 * boards.
 */
#include <stdint.h>
#include <stddef.h>

#define GICD 0x08000000ull
#define GICR 0x080A0000ull
#define TICK_HZ 100
#define TIMER_INTID 30          /* the EL1 physical timer, a private interrupt */

/* The memory map bst_kmain gets: a count, then base and length of each run. */
#define MAX_RUNS 8
uint64_t bst_memmap[1 + 2 * MAX_RUNS];
uint64_t bst_el1_irq;           /* the interrupt bst_cpu_wait waits for, see boot.S */

/* The device tree itself, copied before the page pool can reuse the RAM it
 * sits in (QEMU puts it in RAM): the kernel reads it byte by byte with
 * boot_info (INFO_DTB) and parses it in Bastio. */
#define DTB_MAX 65536
static uint8_t dtb_copy[DTB_MAX];
static uint64_t dtb_len;

extern uint64_t bst_kernel_root[512];
extern uint64_t kernel_l1[512];

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
        uint32_t ac = 2, sc = 1;
        int depth = 0, in_mem = 0;
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
            } else if (tok == 2) {
                depth--;
                in_mem = 0;
            } else if (tok == 3) {
                uint32_t len = be32(st), off = be32(st + 4);
                const uint8_t *data = st + 8;
                const char *pname = (const char *)strs + off;
                st += 8 + ((len + 3) & ~3u);
                if (depth == 1 && same(pname, "#address-cells")) ac = be32(data);
                else if (depth == 1 && same(pname, "#size-cells")) sc = be32(data);
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
 * copies it): 1 GiB blocks, EL1 only, never executable at EL0. Block 0
 * holds the devices (MAIR index 1, not executable); the blocks that hold
 * RAM are normal memory (MAIR index 0). */
void bst_early(const uint8_t *dtb) {
    read_memory(dtb);
    if (dtb && be32(dtb) == 0xd00dfeed && be32(dtb + 4) <= DTB_MAX) {
        dtb_len = be32(dtb + 4);
        for (uint64_t i = 0; i < dtb_len; i++) dtb_copy[i] = dtb[i];
    }
    uint64_t top = 0;
    for (uint64_t i = 0; i < bst_memmap[0]; i++) {
        uint64_t end = bst_memmap[1 + 2 * i] + bst_memmap[2 + 2 * i];
        if (end > top) top = end;
    }
    if (top > (512ull << 30)) top = 512ull << 30;
    for (int i = 0; i < 512; i++) kernel_l1[i] = 0;
    kernel_l1[0] = BLOCK_DEVICE;
    for (uint64_t g = 1; g < ((top + (1ull << 30) - 1) >> 30); g++)
        kernel_l1[g] = BLOCK_NORMAL | (g << 30);
    bst_kernel_root[0] = (uint64_t)kernel_l1 | 3;
}

static inline void w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t r32(uint64_t a) { return *(volatile uint32_t *)a; }

static uint64_t tick_cycles;

/* GICv3 for one processor: the distributor on, this processor's
 * redistributor awake, the timer's interrupt enabled, and the CPU interface
 * opened to every priority. Then the timer, ticking TICK_HZ times a second. */
void bst_gic_init(void) {
    w32(GICD + 0x0000, (1u << 4) | (1u << 1) | 1u);          /* ARE, group 1 non-secure */
    uint64_t rd = GICR;
    w32(rd + 0x0014, r32(rd + 0x0014) & ~2u);                /* GICR_WAKER.ProcessorSleep = 0 */
    while (r32(rd + 0x0014) & 4u) { }                        /* ChildrenAsleep */
    uint64_t sgi = rd + 0x10000;
    w32(sgi + 0x0080, 0xFFFFFFFFu);                          /* group 1 */
    w32(sgi + 0x0400 + TIMER_INTID, 0x80);                   /* priority */
    w32(sgi + 0x0100, 1u << TIMER_INTID);                    /* enable */
    uint64_t sre;
    __asm__ volatile ("mrs %0, icc_sre_el1" : "=r"(sre));
    __asm__ volatile ("msr icc_sre_el1, %0; isb" : : "r"(sre | 1));
    __asm__ volatile ("msr icc_pmr_el1, %0" : : "r"(0xFFull));
    __asm__ volatile ("msr icc_igrpen1_el1, %0; isb" : : "r"(1ull));
    uint64_t f;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    tick_cycles = f / TICK_HZ;
    __asm__ volatile ("msr cntp_tval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(tick_cycles), "r"(1ull));
}

/* An interrupt was taken: acknowledge it, rearm the timer if it was that,
 * and return the kernel's vector for it: 32 for the tick, 0 if it was
 * spurious. */
uint64_t bst_irq_ack(void) {
    uint64_t id;
    __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r"(id));
    id &= 0xFFFFFF;
    if (id >= 1020) return 0;
    uint64_t v = 6;
    if (id == TIMER_INTID) {
        __asm__ volatile ("msr cntp_tval_el0, %0; isb" : : "r"(tick_cycles));
        v = 32;
    }
    __asm__ volatile ("msr icc_eoir1_el1, %0; isb" : : "r"(id));
    return v;
}

uint64_t bst_cpu_wait(void) {
    bst_el1_irq = 0;
    __asm__ volatile ("msr daifclr, #2; wfi; isb; msr daifset, #2" : : : "memory");
    return bst_el1_irq;
}

uint64_t bst_platform_cpus(void) { return 1; }
void bst_platform_smp_start(void (*fn)(void *), void *arg) { (void)fn; (void)arg; }
void bst_platform_smp_join(void) { }
uint64_t bst_platform_acpi(uint64_t sig, uint64_t off) { (void)sig; (void)off; return 256; }
/* boot_info: query 10 is the device tree, byte i of it, 256 past its end
 * (or when there is none); every other query is 0. */
uint64_t bst_platform_info(uint64_t what, uint64_t i) {
    if (what != 10) return 0;
    return i < dtb_len ? dtb_copy[i] : 256;
}
