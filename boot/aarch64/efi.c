/*
 * UEFI entry for the AArch64 port. The image is a PE/COFF application as well
 * as a Linux arm64 Image (the header in boot.S), so a firmware that boots
 * from a file or from QEMU's -kernel loads it wherever it has room and calls
 * efi_entry with the MMU on. The kernel is linked at KERNEL_BASE, so this
 * file's job is what a boot loader's is: ask for the memory at that address,
 * copy the image there, find the device tree the firmware keeps in its
 * configuration table, leave the firmware's boot services, turn the MMU and
 * the caches off, and enter the image at its first instruction with the
 * device tree's address in x0, as boot.S expects. A firmware that has only
 * ACPI (a server's, or QEMU's with acpi=on) gives no device tree: then a
 * small one is written from the tables ACPI has, so the kernel reads one
 * thing whichever the firmware is (see synth_fdt).
 *
 * It runs before the kernel exists, from wherever the firmware put the image,
 * so it must not use an address that is fixed at link time: no pointers in
 * initialised data, and no global variables. Everything here is trusted (it
 * is listed in test/trusted_base.txt).
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define KERNEL_BASE 0x40080000ull
#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR 0x8000000000000001ull
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ull
#define ALLOCATE_ADDRESS 2
#define EFI_LOADER_DATA 2
#define PAGE 4096ull
/* Where boot.S puts SizeOfCode: the PE header at 0x40, its signature, the
 * COFF header and the optional header's magic and linker version. */
#define PE_SIZE_OF_CODE (0x40 + 4 + 20 + 4)

struct guid {
    u32 a;
    u16 b, c;
    u8 d[8];
};

/* The services used, by index in the table (UEFI 2.x, Table 7-1). */
#define BS_ALLOCATE_PAGES 2
#define BS_GET_MEMORY_MAP 4
#define BS_ALLOCATE_POOL 5
#define BS_EXIT_BOOT_SERVICES 26

struct boot_services {
    u8 header[24];
    u64 fn[27];
};

struct config_entry {
    struct guid guid;
    void *table;
};

struct system_table {
    u8 header[24];
    u64 vendor;
    u32 revision, pad;
    u64 console[6];
    u64 runtime;
    struct boot_services *boot;
    u64 entries;
    struct config_entry *config;
};

typedef u64 (*alloc_pages_fn)(u32 type, u32 memory, u64 pages, u64 *address);
typedef u64 (*memory_map_fn)(u64 *size, void *map, u64 *key, u64 *desc_size, u32 *desc_version);
typedef u64 (*alloc_pool_fn)(u32 memory, u64 size, void **buffer);
typedef u64 (*exit_fn)(void *image, u64 key);

/* EFI_DTB_TABLE_GUID: b1b621d5-f19c-41a5-830b-d9152c69aae0 */
static const struct guid dtb_guid = {0xb1b621d5, 0xf19c, 0x41a5,
                                     {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};

/* EFI_ACPI_20_TABLE_GUID: 8868e871-e4f1-11d3-bc22-0080c73c8881. The first
 * three fields are stored little-endian in memory, as this struct is. */
static const struct guid acpi_guid = {0x8868e871, 0xe4f1, 0x11d3,
                                      {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};

static int same_guid(const struct guid *x, const struct guid *y) {
    const u64 *p = (const u64 *)x, *q = (const u64 *)y;
    return p[0] == q[0] && p[1] == q[1];
}

static void copy(volatile u64 *to, const volatile u64 *from, u64 n) {
    u64 i;
    for (i = 0; i < n / 8; i++)
        to[i] = from[i];
}

/* ---- A device tree from ACPI ---------------------------------------- */

/* What the kernel's platform code reads from a tree: the RAM runs, the
 * processors (their MPIDR affinities, the boot processor first), the
 * interrupt controller, the PSCI conduit, the first PL011 (the console) and
 * the PCI Express configuration window. Everything else (virtio-mmio
 * slots, the clock) it finds by default or does without. */
#define MAX_CPUS 16
#define MAX_RUNS 8
#define MAX_PRE_RUNS 64
#define MPIDR_AFF 0xFF00FFFFFFull

struct acpi_info {
    u64 gicd, gicr, gicc;
    u32 gic_ver;                 /* 3, 2, or 0: none found */
    u64 mpidr[MAX_CPUS];
    u32 ncpu;
    u64 uart;                    /* the PL011 SPCR names, or 0 */
    u64 ecam;                    /* MCFG's first window (bus 0), or 0 */
    u32 psci;                    /* 0: none, 1: hvc, 2: smc */
};

static u32 rd8(u64 a) { return *(const volatile u8 *)a; }
static u32 rd32(u64 a) { return rd8(a) | rd8(a + 1) << 8 | rd8(a + 2) << 16 | rd8(a + 3) << 24; }
static u64 rd64(u64 a) { return rd32(a) | (u64)rd32(a + 4) << 32; }

#define SIG(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)

/* Reads what is wanted from the tables `rsdp` leads to (the XSDT's entries:
 * MADT "APIC", FADT "FACP", SPCR, MCFG). Every length is checked against
 * the table's own; a table that does not fit is skipped. */
static void clear_acpi(struct acpi_info *in) {
    in->gic_ver = 0;
    in->ncpu = 0;
    in->uart = in->ecam = in->gicd = in->gicr = in->gicc = 0;
    in->psci = 0;
}

static void read_acpi(u64 rsdp, struct acpi_info *in) {
    u64 xsdt, mpidr_boot, i;
    u32 n;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr_boot));
    mpidr_boot &= MPIDR_AFF;
    if (rd32(rsdp) != SIG('R', 'S', 'D', ' ') || rd32(rsdp + 4) != SIG('P', 'T', 'R', ' ') || rd8(rsdp + 15) < 2)
        return;
    xsdt = rd64(rsdp + 24);
    if (!xsdt || rd32(xsdt) != SIG('X', 'S', 'D', 'T') || rd32(xsdt + 4) < 36)
        return;
    n = (rd32(xsdt + 4) - 36) / 8;
    for (i = 0; i < n; i++) {
        u64 t = rd64(xsdt + 36 + 8 * i);
        u32 sig = rd32(t), len = rd32(t + 4);
        u64 e, end;
        if (!t || len < 36)
            continue;
        if (sig == SIG('A', 'P', 'I', 'C') && len >= 44) {
            for (e = t + 44, end = t + len; e + 2 <= end && rd8(e + 1) >= 2 && e + rd8(e + 1) <= end; e += rd8(e + 1)) {
                u32 type = rd8(e), elen = rd8(e + 1);
                if (type == 0xB && elen >= 76 && (rd32(e + 12) & 1) && in->ncpu < MAX_CPUS) {
                    u64 m = rd64(e + 68) & MPIDR_AFF;
                    if (m == mpidr_boot && in->ncpu) {      /* the boot processor goes first */
                        in->mpidr[in->ncpu++] = in->mpidr[0];
                        in->mpidr[0] = m;
                    } else {
                        in->mpidr[in->ncpu++] = m;
                    }
                    if (!in->gicc)
                        in->gicc = rd64(e + 32);
                } else if (type == 0xC && elen >= 24 && !in->gicd) {
                    in->gicd = rd64(e + 8);
                    in->gic_ver = rd8(e + 20);
                } else if (type == 0xE && elen >= 16 && !in->gicr) {
                    in->gicr = rd64(e + 4);
                }
            }
        } else if (sig == SIG('F', 'A', 'C', 'P') && len >= 131) {
            u32 flags = rd8(t + 129) | rd8(t + 130) << 8;     /* ARM boot architecture flags */
            if (flags & 1)
                in->psci = (flags & 2) ? 1 : 2;
        } else if (sig == SIG('S', 'P', 'C', 'R') && len >= 52) {
            if (rd8(t + 36) == 3 && rd8(t + 40) == 0)         /* a PL011 in memory space */
                in->uart = rd64(t + 44);
        } else if (sig == SIG('M', 'C', 'F', 'G') && len >= 60) {
            if (rd8(t + 54) == 0 && !in->ecam)                /* the window starting at bus 0 */
                in->ecam = rd64(t + 44);
        }
    }
    if (in->gic_ver != 2 && in->gic_ver != 3)
        in->gic_ver = in->gicr ? 3 : (in->gicd && in->gicc ? 2 : 0);
}

/* The tree is built into one buffer: the 40-byte header, an empty memory
 * reservation block, the structure block (at most FDT_STRUCT_CAP bytes),
 * then the strings. */
#define FDT_SIZE 8192
#define FDT_STRUCT_AT 56
#define FDT_STRUCT_CAP 5120
#define FDT_STRINGS_AT (FDT_STRUCT_AT + FDT_STRUCT_CAP)

struct fdt {
    u8 *buf;
    u32 st, sl;                  /* bytes used in the structure and strings blocks */
    int err;                     /* something did not fit */
};

static void put32(u8 *p, u32 v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void tok(struct fdt *f, u32 v) {
    if (f->st + 4 > FDT_STRUCT_CAP) {
        f->err = 1;
        return;
    }
    put32(f->buf + FDT_STRUCT_AT + f->st, v);
    f->st += 4;
}

static void bytes(struct fdt *f, const u8 *p, u32 n) {
    u32 i;
    if (f->st + ((n + 3) & ~3u) > FDT_STRUCT_CAP) {
        f->err = 1;
        return;
    }
    for (i = 0; i < ((n + 3) & ~3u); i++)
        f->buf[FDT_STRUCT_AT + f->st + i] = i < n ? p[i] : 0;
    f->st += (n + 3) & ~3u;
}

static u32 slen(const char *s) {
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

static void node(struct fdt *f, const char *name) {
    tok(f, 1);
    bytes(f, (const u8 *)name, slen(name) + 1);
}

static void end_node(struct fdt *f) { tok(f, 2); }

/* The offset of `name` in the strings block, added if it is not there. */
static u32 str_off(struct fdt *f, const char *name) {
    u32 n = slen(name) + 1, o, i;
    u8 *sb = f->buf + FDT_STRINGS_AT;
    for (o = 0; o + n <= f->sl; o++) {
        for (i = 0; i < n && sb[o + i] == (u8)name[i]; i++)
            ;
        if (i == n)
            return o;
    }
    if (FDT_STRINGS_AT + f->sl + n > FDT_SIZE) {
        f->err = 1;
        return 0;
    }
    for (i = 0; i < n; i++)
        sb[f->sl + i] = name[i];
    f->sl += n;
    return f->sl - n;
}

static void prop(struct fdt *f, const char *name, const u8 *data, u32 n) {
    u32 off = str_off(f, name);
    tok(f, 3);
    tok(f, n);
    tok(f, off);
    bytes(f, data, n);
}

static void prop_str(struct fdt *f, const char *name, const char *v) { prop(f, name, (const u8 *)v, slen(v) + 1); }

static void prop_u32(struct fdt *f, const char *name, u32 v) {
    u8 b[4];
    put32(b, v);
    prop(f, name, b, 4);
}

/* A `reg` of `n` 64-bit values (addresses and sizes in turn), two cells each. */
static void prop_reg(struct fdt *f, const char *name, const u64 *v, u32 n) {
    u8 b[64];
    u32 i;
    for (i = 0; i < n && i < 8; i++) {
        put32(b + 8 * i, v[i] >> 32);
        put32(b + 8 * i + 4, v[i]);
    }
    prop(f, name, b, 8 * (n < 8 ? n : 8));
}

static void hex(char *out, u64 v) {
    int i;
    for (i = 0; i < 16; i++)
        out[i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    out[16] = 0;
}

/* The RAM runs in the firmware's memory map: loader and boot-services
 * memory (free once the services are left) and conventional memory, with
 * neighbours merged. The platform keeps MAX_RUNS runs, so the largest are
 * written, in address order. */
static u32 ram_runs(const u8 *map, u64 len, u64 desc_size, u64 *base, u64 *size) {
    u64 b[MAX_PRE_RUNS], z[MAX_PRE_RUNS], off;
    u32 n = 0, i, k, out = 0;
    int used[MAX_PRE_RUNS];
    for (off = 0; off + desc_size <= len; off += desc_size) {
        u32 type = rd32((u64)map + off);
        u64 start = rd64((u64)map + off + 8), bytes_ = rd64((u64)map + off + 24) * PAGE;
        if (!(type == 1 || type == 2 || type == 3 || type == 4 || type == 7) || !bytes_)
            continue;
        if (n && b[n - 1] + z[n - 1] == start) {
            z[n - 1] += bytes_;
        } else if (n < MAX_PRE_RUNS) {
            b[n] = start;
            z[n] = bytes_;
            n++;
        }
    }
    for (i = 0; i < n; i++)
        used[i] = 0;
    for (k = 0; k < MAX_RUNS; k++) {      /* pick the largest unused, k times */
        int best = -1;
        for (i = 0; i < n; i++)
            if (!used[i] && (best < 0 || z[i] > z[best]))
                best = i;
        if (best < 0)
            break;
        used[best] = 1;
    }
    for (i = 0; i < n; i++)
        if (used[i]) {
            base[out] = b[i];
            size[out] = z[i];
            out++;
        }
    return out;
}

/* Writes the tree into `buf` (FDT_SIZE bytes); returns its size, or 0 if it
 * did not fit or there is no RAM. */
static u64 synth_fdt(u8 *buf, const struct acpi_info *in, const u8 *map, u64 map_len, u64 desc_size) {
    struct fdt f;
    u64 base[MAX_RUNS], size[MAX_RUNS], reg[8];
    u32 nruns, i, total;
    char name[40];
    f.buf = buf;
    f.st = f.sl = 0;
    f.err = 0;
    nruns = ram_runs(map, map_len, desc_size, base, size);
    if (!nruns)
        return 0;
    node(&f, "");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    for (i = 0; i < nruns; i++) {
        name[0] = 'm'; name[1] = 'e'; name[2] = 'm'; name[3] = 'o'; name[4] = 'r'; name[5] = 'y'; name[6] = '@';
        hex(name + 7, base[i]);
        node(&f, name);
        prop_str(&f, "device_type", "memory");
        reg[0] = base[i];
        reg[1] = size[i];
        prop_reg(&f, "reg", reg, 2);
        end_node(&f);
    }
    if (in->ncpu) {
        node(&f, "cpus");
        prop_u32(&f, "#address-cells", 2);
        prop_u32(&f, "#size-cells", 0);
        for (i = 0; i < in->ncpu; i++) {
            u8 b[8];
            name[0] = 'c'; name[1] = 'p'; name[2] = 'u'; name[3] = '@';
            hex(name + 4, in->mpidr[i]);
            node(&f, name);
            prop_str(&f, "device_type", "cpu");
            put32(b, in->mpidr[i] >> 32);
            put32(b + 4, in->mpidr[i]);
            prop(&f, "reg", b, 8);
            end_node(&f);
        }
        end_node(&f);
    }
    if (in->psci) {
        node(&f, "psci");
        prop_str(&f, "compatible", "arm,psci-1.0");
        prop_str(&f, "method", in->psci == 2 ? "smc" : "hvc");
        end_node(&f);
    }
    if (in->gic_ver == 3 && in->gicd && in->gicr) {
        node(&f, "intc");
        prop_str(&f, "compatible", "arm,gic-v3");
        reg[0] = in->gicd;
        reg[1] = 0x10000;
        reg[2] = in->gicr;
        reg[3] = 0x20000ull * (in->ncpu ? in->ncpu : 1);
        prop_reg(&f, "reg", reg, 4);
        end_node(&f);
    } else if (in->gic_ver == 2 && in->gicd && in->gicc) {
        node(&f, "intc");
        prop_str(&f, "compatible", "arm,cortex-a15-gic");
        reg[0] = in->gicd;
        reg[1] = 0x1000;
        reg[2] = in->gicc;
        reg[3] = 0x2000;
        prop_reg(&f, "reg", reg, 4);
        end_node(&f);
    }
    if (in->uart) {
        node(&f, "serial");
        prop_str(&f, "compatible", "arm,pl011");
        reg[0] = in->uart;
        reg[1] = 0x1000;
        prop_reg(&f, "reg", reg, 2);
        end_node(&f);
    }
    if (in->ecam) {
        node(&f, "pcie");
        prop_str(&f, "compatible", "pci-host-ecam-generic");
        reg[0] = in->ecam;
        reg[1] = 16ull << 20;
        prop_reg(&f, "reg", reg, 2);
        end_node(&f);
    }
    end_node(&f);
    tok(&f, 9);
    if (f.err)
        return 0;
    total = FDT_STRINGS_AT + f.sl;
    for (i = 0; i < 40; i++)
        buf[i] = 0;
    for (i = 40; i < FDT_STRUCT_AT; i++)
        buf[i] = 0;                           /* the memory reservation block: one empty entry */
    put32(buf, 0xd00dfeed);
    put32(buf + 4, total);
    put32(buf + 8, FDT_STRUCT_AT);
    put32(buf + 12, FDT_STRINGS_AT);
    put32(buf + 16, 40);
    put32(buf + 20, 17);
    put32(buf + 24, 16);
    put32(buf + 32, f.sl);
    put32(buf + 36, f.st);
    return total;
}

/* Defined in efi_enter below: clean the new image and the device tree to the
 * point of coherency, turn the MMU and caches off at the exception level
 * the firmware left us at, and enter the kernel. */
void bst_efi_enter(u64 entry, u64 dtb, u64 image_size, u64 dtb_size) __attribute__((noreturn));

u64 efi_entry(void *image, struct system_table *st) {
    struct boot_services *bs = st->boot;
    extern char _start[];
    u64 here = (u64)_start;
    u64 size = *(const u64 *)(here + 16);  /* image_size, in the Image header */
    u64 file_size, dtb = 0, dtb_size = 0, i, rsdp = 0;
    struct acpi_info acpi;
    void *synth = 0;
    u64 map_size = 0, key, desc_size;
    u32 desc_version;
    void *map = 0;
    clear_acpi(&acpi);

    /* The device tree. */
    for (i = 0; i < st->entries; i++)
        if (same_guid(&st->config[i].guid, &dtb_guid)) {
            dtb = (u64)st->config[i].table;
            dtb_size = ((u64)((const u8 *)dtb)[4] << 24) | ((u64)((const u8 *)dtb)[5] << 16) |
                       ((u64)((const u8 *)dtb)[6] << 8) | (u64)((const u8 *)dtb)[7];
        }
    if (!dtb) {
        /* No device tree: write one from ACPI, once the memory map is final. */
        for (i = 0; i < st->entries; i++)
            if (same_guid(&st->config[i].guid, &acpi_guid))
                rsdp = (u64)st->config[i].table;
        if (!rsdp)
            return EFI_LOAD_ERROR;
        clear_acpi(&acpi);
        read_acpi(rsdp, &acpi);
        if (((alloc_pool_fn)bs->fn[BS_ALLOCATE_POOL])(EFI_LOADER_DATA, FDT_SIZE, &synth) != EFI_SUCCESS)
            return EFI_LOAD_ERROR;
        dtb = (u64)synth;
    }

    /* The memory the kernel is linked at. */
    if (here != KERNEL_BASE) {
        u64 address = KERNEL_BASE;
        if (((alloc_pages_fn)bs->fn[BS_ALLOCATE_PAGES])(ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                                         (size + PAGE - 1) / PAGE, &address) != EFI_SUCCESS)
            return EFI_LOAD_ERROR;
    }

    /* The memory map, and with its key out of the boot services. The first
     * call only says how big the map is; allocating the buffer can grow it. */
    if (((memory_map_fn)bs->fn[BS_GET_MEMORY_MAP])(&map_size, 0, &key, &desc_size, &desc_version) !=
        EFI_BUFFER_TOO_SMALL)
        return EFI_LOAD_ERROR;
    map_size += 8 * desc_size;
    if (((alloc_pool_fn)bs->fn[BS_ALLOCATE_POOL])(EFI_LOADER_DATA, map_size, &map) != EFI_SUCCESS)
        return EFI_LOAD_ERROR;

    /* The part of the image that is in the file (the rest is zeros that
     * boot.S clears): the PE header's SizeOfCode, which is the file's length
     * less the header's 0x200 bytes. */
    file_size = (u64)*(const u32 *)(here + PE_SIZE_OF_CODE) + 0x200;

    if (here != KERNEL_BASE)
        copy((volatile u64 *)KERNEL_BASE, (const volatile u64 *)here, file_size);

    /* Any map taken after the last allocation will do; if the firmware says
     * the key is stale, take it again. */
    for (;;) {
        u64 len = map_size;
        if (((memory_map_fn)bs->fn[BS_GET_MEMORY_MAP])(&len, map, &key, &desc_size, &desc_version) !=
            EFI_SUCCESS)
            return EFI_LOAD_ERROR;
        if (synth) {
            dtb_size = synth_fdt((u8 *)synth, &acpi, map, len, desc_size);
            if (!dtb_size)
                return EFI_LOAD_ERROR;
        }
        if (((exit_fn)bs->fn[BS_EXIT_BOOT_SERVICES])(image, key) == EFI_SUCCESS)
            break;
    }
    bst_efi_enter(KERNEL_BASE, dtb, size, dtb_size);
}

/*
 * x0 = entry, x1 = device tree, x2 = image size, x3 = device tree size.
 * dc civac over both ranges (the image was written with the caches on and
 * the device tree may be dirty in them), instruction cache invalidated, then
 * SCTLR_ELx.{M,C,I} cleared at the current level. Identity-mapped code keeps
 * running when the MMU goes off.
 */
__asm__(
    ".text\n"
    ".global bst_efi_enter\n"
    "bst_efi_enter:\n"
    "    mov x9, x0\n"           /* entry */
    "    mov x10, x1\n"          /* device tree */
    "    mov x4, x0\n"
    "    add x5, x0, x2\n"
    "1:  dc civac, x4\n"
    "    add x4, x4, #64\n"
    "    cmp x4, x5\n"
    "    b.lo 1b\n"
    "    mov x4, x1\n"
    "    add x5, x1, x3\n"
    "2:  dc civac, x4\n"
    "    add x4, x4, #64\n"
    "    cmp x4, x5\n"
    "    b.lo 2b\n"
    "    dsb sy\n"
    "    ic iallu\n"
    "    dsb sy\n"
    "    isb\n"
    "    mrs x4, CurrentEL\n"
    "    cmp x4, #(2 << 2)\n"
    "    b.ne 3f\n"
    "    mrs x4, sctlr_el2\n"
    "    bic x4, x4, #(1 << 0)\n"
    "    bic x4, x4, #(1 << 2)\n"
    "    bic x4, x4, #(1 << 12)\n"
    "    msr sctlr_el2, x4\n"
    "    isb\n"
    "    b 4f\n"
    "3:  mrs x4, sctlr_el1\n"
    "    bic x4, x4, #(1 << 0)\n"
    "    bic x4, x4, #(1 << 2)\n"
    "    bic x4, x4, #(1 << 12)\n"
    "    msr sctlr_el1, x4\n"
    "    isb\n"
    "4:  mov x0, x10\n"
    "    br x9\n");
