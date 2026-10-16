/*
 * A memory hog for the out-of-memory tests (test/memory_test.sh).
 *
 *   mhog touch MIB    map MIB MiB of anonymous memory and write every page
 *   mhog hold MIB     the same, then sleep until killed
 *   mhog cache MIB [F]  write a MIB MiB file F (default /tmp/mhog.cache)
 *   mhog mapfile MIB F  map MIB MiB of file F shared and read every page: the
 *                     kernel's page cache holds them afterwards
 *   mhog meminfo      print MemTotal, MemFree and SwapFree from /proc/meminfo
 *
 * touch exits 0 when it got all its memory; the out-of-memory killer ends
 * it with SIGKILL when the machine runs out first.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned long arg_mib(int argc, char **argv) {
    return argc > 2 ? strtoul(argv[2], 0, 10) : 1;
}

static int grab(unsigned long mib, int verify) {
    unsigned long len = mib << 20;
    unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("mhog: mmap of %lu MiB failed\n", mib);
        return 1;
    }
    for (unsigned long i = 0; i < len; i += 4096)
        p[i] = (unsigned char)(i >> 12);
    if (verify) {
        for (unsigned long i = 0; i < len; i += 4096)
            if (p[i] != (unsigned char)(i >> 12)) {
                printf("mhog: page %lu changed\n", i >> 12);
                return 1;
            }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: mhog touch|hold|cache|meminfo ...");
        return 2;
    }
    if (strcmp(argv[1], "touch") == 0) {
        if (grab(arg_mib(argc, argv), 1))
            return 1;
        printf("mhog: touched %lu MiB\n", arg_mib(argc, argv));
        return 0;
    }
    if (strcmp(argv[1], "hold") == 0) {
        if (grab(arg_mib(argc, argv), 0))
            return 1;
        printf("mhog: holding %lu MiB\n", arg_mib(argc, argv));
        fflush(stdout);
        for (;;)
            pause();
    }
    if (strcmp(argv[1], "cache") == 0) {
        unsigned long mib = arg_mib(argc, argv);
        const char *path = argc > 3 ? argv[3] : "/tmp/mhog.cache";
        int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
        if (fd < 0) {
            printf("mhog: cannot create %s\n", path);
            return 1;
        }
        static char block[65536];
        memset(block, 0x5c, sizeof block);
        for (unsigned long i = 0; i < (mib << 20) / sizeof block; i++)
            if (write(fd, block, sizeof block) != (ssize_t)sizeof block) {
                printf("mhog: short write after %lu KiB\n", i * 64);
                return 1;
            }
        close(fd);
        printf("mhog: wrote %lu MiB\n", mib);
        return 0;
    }
    if (strcmp(argv[1], "mapfile") == 0 && argc > 3) {
        unsigned long len = arg_mib(argc, argv) << 20;
        int fd = open(argv[3], O_RDONLY);
        if (fd < 0) {
            printf("mhog: cannot open %s\n", argv[3]);
            return 1;
        }
        unsigned char *p = mmap(0, len, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            printf("mhog: mmap of %s failed\n", argv[3]);
            return 1;
        }
        unsigned long sum = 0;
        for (unsigned long i = 0; i < len; i += 4096)
            sum += p[i];
        munmap(p, len);
        close(fd);
        printf("mhog: read %lu MiB of %s (sum %lu)\n", len >> 20, argv[3], sum);
        return 0;
    }
    if (strcmp(argv[1], "meminfo") == 0) {
        FILE *f = fopen("/proc/meminfo", "r");
        char line[128];
        while (f && fgets(line, sizeof line, f))
            if (!strncmp(line, "MemTotal", 8) || !strncmp(line, "MemFree", 7) || !strncmp(line, "Swap", 4))
                fputs(line, stdout);
        return 0;
    }
    puts("usage: mhog touch|hold|cache|meminfo ...");
    return 2;
}
