/* The BastiOS installer's disk tool (docs/install.md), against musl. It runs
 * in the installer image (boot option `install`), where the disk is not
 * mounted but open to root as raw sectors through the BastiOS system call
 * bstdisk (src/lxextra.bastio, lx_bstdisk).
 *
 *     binstall info            what the disk is: its size, and whether it is
 *                              blank, has a partition table, or other data
 *     binstall write PAYLOAD   erases the disk and makes the installed
 *                              layout: a GPT with a BIOS boot, an EFI system
 *                              and a BastiOS partition, Limine, and the
 *                              kernel (the payload, tools/mkinstall.py); reads
 *                              all of it back before it says it is done
 *     binstall mount           hands the new BastiOS partition to the file
 *                              system as disk/, formatted, and ends install
 *                              mode: the script then copies the rest in
 *
 * The partition table is written last of the data, and the first sector
 * (which makes the disk look partitioned) after it, so a run that is cut
 * short leaves a disk that no operating system takes for a partitioned one. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_bstdisk 20819
enum { BD_SIZE, BD_READ, BD_WRITE, BD_READ_BLOCK, BD_WRITE_BLOCK, BD_MOUNT };

#define SECTOR 512
#define BLOCK 4096
#define OS_MIN_SECTORS (128u * 2048)     /* a BastiOS partition of at least 128 MiB */
#define GPT_SECTORS 34                   /* a protective MBR, GPT's header, 32 sectors of entries */
#define NENT 128
#define ESIZE 128

static long bd(long op, uint64_t lba, void *buf) {
    long r = syscall(SYS_bstdisk, op, lba, buf);
    if (r == -1 && errno == ENODEV) {
        fprintf(stderr, "binstall: the disk is not open for installing (boot the installer image)\n");
        exit(2);
    }
    return r;
}

static uint64_t disk_sectors(void) {
    long r = bd(BD_SIZE, 0, 0);
    if (r < 0) {
        perror("binstall: bstdisk");
        exit(2);
    }
    return (uint64_t)r;
}

/* ---------------------------------------------------------------- sectors */

static void fail(const char *what, uint64_t lba) {
    fprintf(stderr, "binstall: %s at sector %llu failed\n", what, (unsigned long long)lba);
    exit(1);
}

static void rd_sec(uint64_t lba, void *buf) {
    if (bd(BD_READ, lba, buf) != 0)
        fail("reading", lba);
}

static void wr_sec(uint64_t lba, const void *buf) {
    if (bd(BD_WRITE, lba, (void *)buf) != 0)
        fail("writing", lba);
}

/* n sectors from lba, whole 4 KiB blocks at a time where lba allows. */
static void rd_run(uint64_t lba, void *buf, uint64_t n) {
    char *p = buf;
    while (n) {
        if (lba % 8 == 0 && n >= 8) {
            if (bd(BD_READ_BLOCK, lba, p) != 0)
                fail("reading", lba);
            lba += 8, p += BLOCK, n -= 8;
        } else {
            rd_sec(lba, p);
            lba++, p += SECTOR, n--;
        }
    }
}

static void wr_run(uint64_t lba, const void *buf, uint64_t n) {
    const char *p = buf;
    while (n) {
        if (lba % 8 == 0 && n >= 8) {
            if (bd(BD_WRITE_BLOCK, lba, (void *)p) != 0)
                fail("writing", lba);
            lba += 8, p += BLOCK, n -= 8;
        } else {
            wr_sec(lba, p);
            lba++, p += SECTOR, n--;
        }
    }
}

static void zero_run(uint64_t lba, uint64_t n) {
    static char z[BLOCK];
    while (n) {
        uint64_t c = (lba % 8 == 0 && n >= 8) ? 8 : 1;
        wr_run(lba, z, c);
        lba += c, n -= c;
    }
}

/* ---------------------------------------------------------------- CRC-32 */

static uint32_t crc32(const void *buf, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    const unsigned char *p = buf;
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------- payload */

struct hdr {
    char magic[8];
    uint64_t bios_lba, bios_n, esp_lba, esp_n, os_lba;
    uint32_t extents, zero;
    unsigned char code[440];
    unsigned char pad[16];
};

struct payload {
    unsigned char *buf;
    size_t size;
    struct hdr h;
};

static void load(struct payload *pl, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < (off_t)sizeof(struct hdr)) {
        fprintf(stderr, "binstall: %s is not an installer payload\n", path);
        exit(1);
    }
    pl->buf = malloc(size);
    if (!pl->buf) {
        fprintf(stderr, "binstall: out of memory\n");
        exit(1);
    }
    size_t got = 0;
    while (got < (size_t)size) {
        ssize_t r = pread(fd, pl->buf + got, size - got, got);
        if (r <= 0) {
            perror(path);
            exit(1);
        }
        got += r;
    }
    close(fd);
    pl->size = size;
    memcpy(&pl->h, pl->buf, sizeof pl->h);
    if (memcmp(pl->h.magic, "BSTIPAY1", 8) != 0 || pl->h.bios_n % 8 || pl->h.esp_n % 8 || pl->h.bios_lba % 8
        || pl->h.esp_lba % 8 || pl->h.os_lba % 8 || pl->h.bios_lba < GPT_SECTORS
        || pl->h.bios_lba + pl->h.bios_n > pl->h.esp_lba || pl->h.esp_lba + pl->h.esp_n > pl->h.os_lba) {
        fprintf(stderr, "binstall: %s is not an installer payload\n", path);
        exit(1);
    }
}

/* Calls f(lba, data, blocks) for each extent; stops at a malformed one. */
static void each_extent(struct payload *pl, void (*f)(uint64_t, const unsigned char *, uint64_t, void *), void *arg) {
    size_t at = sizeof(struct hdr);
    for (uint32_t i = 0; i < pl->h.extents; i++) {
        uint64_t ext[2];
        if (at + 16 > pl->size)
            goto bad;
        memcpy(ext, pl->buf + at, 16);
        at += 16;
        if (ext[1] > (pl->size - at) / BLOCK || ext[0] % 8 || ext[0] < pl->h.bios_lba
            || (ext[0] + ext[1] * 8 > pl->h.bios_lba + pl->h.bios_n && ext[0] < pl->h.esp_lba)
            || ext[0] + ext[1] * 8 > pl->h.esp_lba + pl->h.esp_n)
            goto bad;
        f(ext[0], pl->buf + at, ext[1], arg);
        at += ext[1] * BLOCK;
    }
    return;
bad:
    fprintf(stderr, "binstall: the payload is damaged\n");
    exit(1);
}

static void put_extent(uint64_t lba, const unsigned char *data, uint64_t blocks, void *arg) {
    (void)arg;
    wr_run(lba, data, blocks * 8);
}

static void check_extent(uint64_t lba, const unsigned char *data, uint64_t blocks, void *arg) {
    int *bad = arg;
    static unsigned char b[BLOCK];
    for (uint64_t i = 0; i < blocks; i++) {
        rd_run(lba + i * 8, b, 8);
        if (memcmp(b, data + i * BLOCK, BLOCK) != 0) {
            fprintf(stderr, "binstall: sector %llu does not read back\n", (unsigned long long)(lba + i * 8));
            *bad = 1;
        }
    }
}

/* ---------------------------------------------------------------- GPT */

static const unsigned char T_BIOS[16] = {0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e, 0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49};
static const unsigned char T_ESP[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
/* e0c9ea2a-d5f1-45b3-a799-0a7bec83d701: BASTIOS_PART in src/xhci.bastio */
static const unsigned char T_BASTIOS[16] = {0x2a, 0xea, 0xc9, 0xe0, 0xf1, 0xd5, 0xb3, 0x45, 0xa7, 0x99, 0x0a, 0x7b, 0xec, 0x83, 0xd7, 0x01};

static void random_guid(unsigned char *g) {
    if (getrandom(g, 16, 0) != 16) {
        fprintf(stderr, "binstall: no random numbers\n");
        exit(1);
    }
    g[7] = (g[7] & 0x0F) | 0x40;         /* version 4 (the fields are little-endian here) */
    g[8] = (g[8] & 0x3F) | 0x80;
}

static void entry(unsigned char *e, const unsigned char *type, uint64_t first, uint64_t last, const char *name) {
    memcpy(e, type, 16);
    random_guid(e + 16);
    memcpy(e + 32, &first, 8);
    memcpy(e + 40, &last, 8);
    for (int i = 0; name[i] && i < 36; i++)
        e[56 + 2 * i] = name[i];
}

static void header(unsigned char *s, uint64_t me, uint64_t other, uint64_t last_use, const unsigned char *guid, uint64_t elba,
                   uint32_t ecrc) {
    memset(s, 0, SECTOR);
    uint32_t rev = 0x10000, hs = 92, n = NENT, es = ESIZE, first_use = GPT_SECTORS;
    uint64_t fu = first_use;
    memcpy(s, "EFI PART", 8);
    memcpy(s + 8, &rev, 4);
    memcpy(s + 12, &hs, 4);
    memcpy(s + 24, &me, 8);
    memcpy(s + 32, &other, 8);
    memcpy(s + 40, &fu, 8);
    memcpy(s + 48, &last_use, 8);
    memcpy(s + 56, guid, 16);
    memcpy(s + 72, &elba, 8);
    memcpy(s + 80, &n, 4);
    memcpy(s + 84, &es, 4);
    memcpy(s + 88, &ecrc, 4);
    uint32_t c = crc32(s, 92);
    memcpy(s + 16, &c, 4);
}

/* ---------------------------------------------------------------- commands */

static const char *describe(void) {
    static unsigned char s0[SECTOR], s1[SECTOR];
    uint64_t n = disk_sectors();
    int any = 0;
    static unsigned char s[SECTOR];
    for (uint64_t i = 0; i < 2 * GPT_SECTORS && n >= 2 * GPT_SECTORS; i++) {
        rd_sec(i < GPT_SECTORS ? i : n - 2 * GPT_SECTORS + i, s);
        for (int k = 0; k < SECTOR; k++)
            if (s[k])
                any = 1;
    }
    if (!any)
        return "blank";
    rd_sec(0, s0);
    rd_sec(1, s1);
    if (memcmp(s1, "EFI PART", 8) == 0)
        return "has a GPT partition table";
    if (s0[510] == 0x55 && s0[511] == 0xAA)
        return "has an MBR partition table";
    return "holds other data";
}

static int cmd_info(void) {
    uint64_t n = disk_sectors();
    printf("%llu sectors, %llu MiB, %s\n", (unsigned long long)n, (unsigned long long)(n / 2048), describe());
    return 0;
}

static int cmd_write(const char *path) {
    struct payload pl;
    load(&pl, path);
    uint64_t total = disk_sectors();
    uint64_t os = pl.h.os_lba;
    if (total < os + OS_MIN_SECTORS + GPT_SECTORS) {
        fprintf(stderr, "binstall: the disk is too small: %llu MiB, and the least is %llu MiB\n", (unsigned long long)(total / 2048),
                (unsigned long long)((os + OS_MIN_SECTORS + GPT_SECTORS) / 2048 + 1));
        return 1;
    }
    uint64_t last_use = total - GPT_SECTORS;       /* the last sector the partitions may use */
    uint64_t os_last = last_use;

    /* The new table, in memory. */
    static unsigned char ents[NENT * ESIZE];
    unsigned char guid[16];
    random_guid(guid);
    entry(ents + 0 * ESIZE, T_BIOS, pl.h.bios_lba, pl.h.bios_lba + pl.h.bios_n - 1, "BIOS boot");
    entry(ents + 1 * ESIZE, T_ESP, pl.h.esp_lba, pl.h.esp_lba + pl.h.esp_n - 1, "EFI system");
    entry(ents + 2 * ESIZE, T_BASTIOS, os, os_last, "BastiOS");
    uint32_t ecrc = crc32(ents, sizeof ents);
    static unsigned char prim[SECTOR], back[SECTOR], mbr[SECTOR];
    header(prim, 1, total - 1, last_use, guid, 2, ecrc);
    header(back, total - 1, 1, last_use, guid, total - 33, ecrc);
    memcpy(mbr, pl.h.code, 440);
    mbr[446 + 1] = 0;
    mbr[446 + 2] = 2;
    mbr[446 + 4] = 0xEE;                           /* a protective MBR: one partition for the whole GPT disk */
    mbr[446 + 5] = mbr[446 + 6] = mbr[446 + 7] = 0xFF;
    uint32_t one = 1, span = total - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(total - 1);
    memcpy(mbr + 446 + 8, &one, 4);
    memcpy(mbr + 446 + 12, &span, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    printf("erasing the old partition tables\n");
    zero_run(0, GPT_SECTORS);
    zero_run(total - GPT_SECTORS + 1, GPT_SECTORS - 1);
    printf("BIOS boot partition (%llu KiB)\n", (unsigned long long)(pl.h.bios_n / 2));
    zero_run(pl.h.bios_lba, pl.h.bios_n);
    printf("EFI system partition (%llu MiB)\n", (unsigned long long)(pl.h.esp_n / 2048));
    zero_run(pl.h.esp_lba, pl.h.esp_n);
    each_extent(&pl, put_extent, 0);
    /* The BastiOS partition starts blank, so that BastiOS formats it. */
    zero_run(os, GPT_SECTORS);
    zero_run(os_last - GPT_SECTORS + 1, GPT_SECTORS);

    printf("partition table\n");
    wr_run(total - 33, ents, 32);
    wr_sec(total - 1, back);
    wr_run(2, ents, 32);
    wr_sec(1, prim);
    wr_sec(0, mbr);

    printf("checking what was written\n");
    int bad = 0;
    static unsigned char rb[NENT * ESIZE];
    rd_run(2, rb, 32);
    if (memcmp(rb, ents, sizeof ents) != 0)
        bad = 1;
    rd_run(total - 33, rb, 32);
    if (memcmp(rb, ents, sizeof ents) != 0)
        bad = 1;
    static unsigned char s[SECTOR];
    rd_sec(1, s);
    if (memcmp(s, prim, SECTOR) != 0)
        bad = 1;
    rd_sec(total - 1, s);
    if (memcmp(s, back, SECTOR) != 0)
        bad = 1;
    rd_sec(0, s);
    if (memcmp(s, mbr, SECTOR) != 0)
        bad = 1;
    each_extent(&pl, check_extent, &bad);
    if (bad) {
        fprintf(stderr, "binstall: the disk does not read back what was written; it is not bootable\n");
        return 1;
    }
    printf("the disk is partitioned and the boot loader is in place\n");
    return 0;
}

static int cmd_mount(void) {
    long blocks = bd(BD_MOUNT, 0, 0);
    if (blocks < 0) {
        perror("binstall: mount");
        return 1;
    }
    if (blocks == 0) {
        fprintf(stderr, "binstall: BastiOS could not mount the new partition\n");
        return 1;
    }
    printf("disk/ is the new BastiOS partition: %ld blocks of 4 KiB\n", blocks);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "info") == 0)
        return cmd_info();
    if (argc == 3 && strcmp(argv[1], "write") == 0)
        return cmd_write(argv[2]);
    if (argc == 2 && strcmp(argv[1], "mount") == 0)
        return cmd_mount();
    fprintf(stderr, "usage: binstall info | write PAYLOAD | mount\n");
    return 2;
}
