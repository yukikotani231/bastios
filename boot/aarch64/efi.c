/*
 * UEFI entry for the AArch64 port. The image is a PE/COFF application as well
 * as a Linux arm64 Image (the header in boot.S), so a firmware that boots
 * from a file or from QEMU's -kernel loads it wherever it has room and calls
 * efi_entry with the MMU on. The kernel is linked at KERNEL_BASE, so this
 * file's job is what a boot loader's is: ask for the memory at that address,
 * copy the image there, find the device tree the firmware keeps in its
 * configuration table, leave the firmware's boot services, turn the MMU and
 * the caches off, and enter the image at its first instruction with the
 * device tree's address in x0, as boot.S expects.
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

static int same_guid(const struct guid *x, const struct guid *y) {
    const u64 *p = (const u64 *)x, *q = (const u64 *)y;
    return p[0] == q[0] && p[1] == q[1];
}

static void copy(volatile u64 *to, const volatile u64 *from, u64 n) {
    u64 i;
    for (i = 0; i < n / 8; i++)
        to[i] = from[i];
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
    u64 file_size, dtb = 0, dtb_size = 0, i;
    u64 map_size = 0, key, desc_size;
    u32 desc_version;
    void *map = 0;

    /* The device tree. */
    for (i = 0; i < st->entries; i++)
        if (same_guid(&st->config[i].guid, &dtb_guid)) {
            dtb = (u64)st->config[i].table;
            dtb_size = ((u64)((const u8 *)dtb)[4] << 24) | ((u64)((const u8 *)dtb)[5] << 16) |
                       ((u64)((const u8 *)dtb)[6] << 8) | (u64)((const u8 *)dtb)[7];
        }
    if (!dtb)
        return EFI_LOAD_ERROR;

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
