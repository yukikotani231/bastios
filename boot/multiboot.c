/*
 * Multiboot 2 boot information, put into the Multiboot 1 form the kernel
 * prelude reads. Part of the trusted base.
 *
 * The kernel carries both headers. QEMU's -kernel loader speaks only
 * Multiboot 1; GRUB loads the ISO's kernel with Multiboot 2, which also
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
        if (info != 0 && info <= IDENT_LIMIT - 52) {
            info_mb1 = info;
            if (rd32(info) & (1u << 2)) keep_cmdline(rd32(info + 16), IDENT_LIMIT);
        }
        return info;
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
