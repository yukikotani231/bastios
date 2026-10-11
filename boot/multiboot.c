/*
 * Multiboot 2 boot information, put into the Multiboot 1 form the kernel
 * prelude reads. Part of the trusted base.
 *
 * The kernel carries both headers. QEMU's -kernel loader speaks only
 * Multiboot 1; Limine loads the ISO's kernel with Multiboot 2, which also
 * passes a copy of the ACPI RSDP (UEFI firmware gives its address only to
 * EFI programs, so a Multiboot 1 loader cannot pass it on). Converting the
 * few fields the kernel uses keeps one format for everything after this:
 * the prelude's page pool (memory map) and `boot_fb` (framebuffer), and
 * smp.c's search for the ACPI tables (bst_boot_rsdp).
 *
 * The information is untrusted input: every read is checked against the
 * structure's total size and the identity-mapped first 1 GiB, and the
 * results are copied into the kernel image, where the page pool cannot
 * reach them.
 */
#include <stdint.h>

#define IDENT_LIMIT  (1ull << 30)   /* boot.S identity-maps the first 1 GiB */
#define MB1_MAGIC    0x2BADB002u    /* in %eax from a Multiboot 1 loader */
#define MB2_MAGIC    0x36D76289u    /* ... and from a Multiboot 2 loader */
#define MAX_REGIONS  128

static uint32_t mb1[30];                         /* 120 bytes: up to the colour info */
static uint8_t mmap1[MAX_REGIONS * 24];          /* Multiboot 1 entries: size, base, length, type */
uint8_t bst_boot_rsdp[36];                       /* the RSDP copy Multiboot 2 passed, if any */
uint32_t bst_boot_rsdp_len;
static char cmdline[256];                        /* the boot command line, cut at 255 bytes */
static uint32_t cmdline_len;
static uint64_t info_mb1;                        /* the Multiboot 1 information the kernel gets */

static uint32_t rd8(uint64_t a) { return *(volatile uint8_t *)a; }
static uint32_t rd32(uint64_t a) { return rd8(a) | rd8(a + 1) << 8 | rd8(a + 2) << 16 | rd8(a + 3) << 24; }
static uint64_t rd64(uint64_t a) { return rd32(a) | (uint64_t)rd32(a + 4) << 32; }

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* Copy the NUL-terminated command line at `a` (if it lies below `limit`). */
static void keep_cmdline(uint64_t a, uint64_t limit) {
    cmdline_len = 0;
    while (a < limit && cmdline_len < sizeof cmdline - 1) {
        uint8_t c = (uint8_t)rd8(a++);
        if (!c) break;
        cmdline[cmdline_len++] = (char)c;
    }
}

/* The Multiboot 1 information for the kernel, from what the loader passed
 * in %eax (magic) and %ebx (info); 0 if it is neither form. */
uint32_t bst_multiboot_info(uint32_t magic, uint32_t info) {
    if (magic == MB1_MAGIC) {
        /* Copied as well, memory map included, so that bst_boot_take can
         * take memory out of the map before the kernel reads it. */
        if (info == 0 || info > IDENT_LIMIT - sizeof mb1) return 0;
        uint8_t *out = (uint8_t *)mb1;
        for (uint32_t i = 0; i < sizeof mb1; i++) out[i] = (uint8_t)rd8(info + i);
        uint32_t flags = rd32(info);
        if (flags & (1u << 2)) keep_cmdline(rd32(info + 16), IDENT_LIMIT);
        if (flags & (1u << 6)) {
            uint64_t at = rd32(info + 48), end = at + rd32(info + 44), n = 0;
            if (end > IDENT_LIMIT) end = at;
            for (uint64_t e = at; e + 24 <= end && n < MAX_REGIONS && rd32(e) >= 20; e += rd32(e) + 4, n++) {
                uint8_t *m = mmap1 + 24 * n;
                put32(m, 20);
                put64(m + 4, rd64(e + 4));
                put64(m + 12, rd64(e + 12));
                put32(m + 20, rd32(e + 20));
            }
            put32(out + 44, 24 * n);
            put32(out + 48, (uint32_t)(uint64_t)mmap1);
        }
        info_mb1 = (uint64_t)mb1;
        return (uint32_t)(uint64_t)mb1;
    }
    if (magic != MB2_MAGIC || info == 0 || info % 8 != 0 || info > IDENT_LIMIT - 16) return 0;
    uint64_t total = rd32(info);
    if (total < 16 || total > IDENT_LIMIT - info) return 0;
    uint8_t *out = (uint8_t *)mb1;
    uint32_t flags = 0;
    uint64_t end = info + total;
    for (uint64_t t = info + 8; t + 8 <= end; ) {
        uint32_t type = rd32(t), size = rd32(t + 4);
        if (type == 0 || size < 8 || size > end - t) break;
        if (type == 1 && size > 8) {                   /* the command line */
            keep_cmdline(t + 8, t + size);
        } else if (type == 4 && size >= 16) {                 /* basic memory information */
            put32(out + 4, rd32(t + 8));
            put32(out + 8, rd32(t + 12));
            flags |= 1u << 0;
        } else if (type == 6 && size >= 16) {          /* memory map */
            uint32_t esize = rd32(t + 8), n = 0;
            if (esize >= 24) {
                for (uint64_t e = t + 16; e + 24 <= t + size && n < MAX_REGIONS; e += esize, n++) {
                    uint8_t *m = mmap1 + 24 * n;
                    put32(m, 20);
                    put64(m + 4, rd64(e));
                    put64(m + 12, rd64(e + 8));
                    put32(m + 20, rd32(e + 16));
                }
                put32(out + 44, 24 * n);
                put32(out + 48, (uint32_t)(uint64_t)mmap1);
                flags |= 1u << 6;
            }
        } else if (type == 8 && size >= 38) {          /* framebuffer: RGB colour info follows */
            for (int i = 0; i < 22; i++) out[88 + i] = (uint8_t)rd8(t + 8 + i);    /* address .. type */
            for (int i = 0; i < 6; i++) out[110 + i] = (uint8_t)rd8(t + 32 + i);   /* field positions and sizes */
            flags |= 1u << 12;
        } else if ((type == 15 || (type == 14 && bst_boot_rsdp_len == 0)) && size > 8) {
            uint32_t n = size - 8 < sizeof bst_boot_rsdp ? size - 8 : sizeof bst_boot_rsdp;
            for (uint32_t i = 0; i < n; i++) bst_boot_rsdp[i] = (uint8_t)rd8(t + 8 + i);
            bst_boot_rsdp_len = n;                     /* ACPI 2.0 and later (15) wins over 1.0 (14) */
        }
        t += (size + 7) & ~7u;
    }
    put32(out, flags);
    info_mb1 = (uint64_t)mb1;
    return (uint32_t)(uint64_t)mb1;
}

/* Byte `i` of the boot command line, or 256 past its end. */
uint64_t bst_boot_cmdline(uint64_t i) {
    return i < cmdline_len ? (uint8_t)cmdline[i] : 256;
}

/* Field `f` of the boot loader's framebuffer if it is 32-bit RGB, wherever
 * it is: 0 its address, 1 bytes per line, 2 width, 3 height, 4 the bit
 * positions of red, green and blue (a byte each); 0 without one. The
 * `boot_fb` builtin says the same but only for one below 4 GiB, and a
 * GPU's framebuffer is often above (Intel graphics at 0x4000000000). */
uint64_t bst_boot_framebuffer(uint64_t f) {
    uint64_t mb = info_mb1;
    if (!mb || !(rd32(mb) & (1u << 12)) || rd8(mb + 109) != 1 || rd8(mb + 108) != 32) return 0;
    uint64_t addr = rd64(mb + 88), pitch = rd32(mb + 96), w = rd32(mb + 100), h = rd32(mb + 104);
    if (addr == 0 || pitch < w * 4) return 0;
    return f == 0 ? addr : f == 1 ? pitch : f == 2 ? w : f == 3 ? h
         : f == 4 ? rd8(mb + 110) | (uint64_t)rd8(mb + 112) << 8 | (uint64_t)rd8(mb + 114) << 16 : 0;
}

/* Field `f` (0 base, 1 length, 2 type) of region `n` of the memory map
 * the kernel was given, or 0 past its end. */
uint64_t bst_boot_region(uint64_t n, uint64_t f) {
    uint64_t mb = info_mb1;
    if (!mb || !(rd32(mb) & (1u << 6)) || f > 2) return 0;
    uint64_t at = rd32(mb + 48), end = at + rd32(mb + 44);
    if (end > IDENT_LIMIT) return 0;
    for (uint64_t e = at; e + 24 <= end; e += rd32(e) + 4, n--) {
        if (rd32(e) < 20) return 0;
        if (n == 0) return f == 0 ? rd64(e + 4) : f == 1 ? rd64(e + 12) : rd32(e + 20);
    }
    return 0;
}

/* Takes `size` bytes, aligned to `align` (a power of two), out of the
 * memory map the kernel gets: from the start of the first available region
 * with room for them above the kernel image and below IDENT_LIMIT. The
 * region then starts after them, so the page pool never sees them. Returns
 * their address, or 0 if no region has room. */
extern char _end[];
uint64_t bst_boot_take(uint64_t size, uint64_t align) {
    uint64_t mb = info_mb1;
    if (!mb || !(rd32(mb) & (1u << 6)) || (uint64_t)rd32(mb + 48) != (uint64_t)mmap1) return 0;
    for (uint64_t n = 0; n < MAX_REGIONS && 24 * n < rd32(mb + 44); n++) {
        uint8_t *m = mmap1 + 24 * n;
        uint64_t a = rd64((uint64_t)m + 4), end = a + rd64((uint64_t)m + 12);
        if (rd32((uint64_t)m + 20) != 1 || end < a) continue;
        uint64_t at = a > (uint64_t)_end ? a : (uint64_t)_end;
        at = (at + align - 1) & ~(align - 1);
        if (at < a || at + size > end || at + size > IDENT_LIMIT) continue;
        put64(m + 4, at + size);
        put64(m + 12, end - (at + size));
        return at;
    }
    return 0;
}

/*
 * RAM above 2 GiB, mapped write-back. boot.S maps the first 2 GiB
 * write-back and the device window above them (2 GiB up to 2^46) uncached;
 * the page pool spans every run of RAM the memory map lists (the kernel
 * prelude's bst_pool_init), so the parts of the window that are RAM are
 * remapped write-back here, before the kernel runs. bst_ram_limit tells the
 * prelude how far that reaches: 4 GiB on a processor without 1 GiB pages
 * (boot.S maps nothing above then), else 512 GiB, the end of the kernel's
 * first top-level slot (a user window starts at the second).
 *
 * A 1 GiB page that is only partly RAM is split into 2 MiB pages, from
 * SPARE_PDS spare tables; a 2 MiB page that is only partly RAM stays
 * uncached. Either is correct, only slower (the device window can never
 * be claimed over RAM: the prelude's mmio_claim stops the machine).
 */
#define PTE_P_W_HUGE 0x83ull
#define PTE_UC       0x18ull                    /* PWT | PCD */
#define PTE_ADDR     0x000FFFFFFFFFF000ull
#define SPARE_PDS    16

extern uint64_t bst_kernel_pml4[512];
uint64_t bst_ram_limit = IDENT_LIMIT;
static uint64_t spare_pd[SPARE_PDS][512] __attribute__((aligned(4096)));

/* Does some available (type 1) region of the memory map overlap [lo, hi)?
 * And do they cover all of it, joined end to end? */
static int ram_overlaps(uint64_t lo, uint64_t hi) {
    for (uint64_t n = 0; n < MAX_REGIONS; n++) {
        uint64_t type = bst_boot_region(n, 2), a = bst_boot_region(n, 0), len = bst_boot_region(n, 1);
        if (type == 0) break;
        if (type == 1 && a < hi && lo < a + len && a + len > a) return 1;
    }
    return 0;
}
static int ram_covers(uint64_t lo, uint64_t hi) {
    for (int grew = 1; grew && lo < hi; ) {
        grew = 0;
        for (uint64_t n = 0; n < MAX_REGIONS; n++) {
            uint64_t type = bst_boot_region(n, 2), a = bst_boot_region(n, 0), len = bst_boot_region(n, 1);
            if (type == 0) break;
            if (type == 1 && a <= lo && lo < a + len) { lo = a + len; grew = 1; }
        }
    }
    return lo >= hi;
}

void bst_map_ram(void) {
    uint64_t *pdpt = (uint64_t *)(bst_kernel_pml4[0] & PTE_ADDR);
    bst_ram_limit = 4ull << 30;
    for (uint64_t g = 2; g < 4; g++) {             /* 2-4 GiB: 2 MiB pages (devpd) */
        uint64_t *pd = (uint64_t *)(pdpt[g] & PTE_ADDR);
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t a = (g << 30) + (i << 21);
            if (ram_covers(a, a + (1ull << 21))) pd[i] &= ~PTE_UC;
        }
    }
    if (pdpt[4] & 1) {                               /* 4-512 GiB: 1 GiB pages */
        int spare = 0;
        bst_ram_limit = 512ull << 30;
        for (uint64_t g = 4; g < 512; g++) {
            uint64_t a = g << 30;
            if (!ram_overlaps(a, a + (1ull << 30))) continue;
            if (ram_covers(a, a + (1ull << 30))) { pdpt[g] &= ~PTE_UC; continue; }
            if (spare == SPARE_PDS) continue;
            uint64_t *pd = spare_pd[spare++];
            for (uint64_t i = 0; i < 512; i++) {
                uint64_t b = a + (i << 21);
                pd[i] = b | PTE_P_W_HUGE | (ram_covers(b, b + (1ull << 21)) ? 0 : PTE_UC);
            }
            pdpt[g] = (uint64_t)pd | 3;
        }
    }
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}
