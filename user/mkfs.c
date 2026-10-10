// mkfs.bstdisk3 [-options] DEVICE [SIZE]: makes an empty BastiOS file
// system (the format of src/diskfs.bastio) on a loop device or a disk
// image file. It writes the superblock (the magic "BSTDISK3" and no
// directory blocks past the fixed ones) and zeroes the four fixed
// directory blocks, which is an empty directory. The data area needs no
// setting up: a mount finds the blocks in use from the directory.
//
// Options are accepted and ignored (test suites pass the ones they give
// every mkfs, such as -F or -b SIZE); so is a size after the device.

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK 4096
#define FIXED_END (5 * BLOCK)          // the superblock and directory blocks 1..4
#define MIN_SIZE (21 * BLOCK)          // what a mount needs: DATA_B0 + 16 blocks

int main(int argc, char **argv) {
    const char *dev = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            // An option; one that takes a value has it in the next word.
            if (argv[i][1] && strchr("bCEIiLlNOoTUc", argv[i][1]) && !argv[i][2] && i + 1 < argc)
                i++;
            continue;
        }
        if (!dev)
            dev = argv[i];
    }
    if (!dev) {
        fprintf(stderr, "usage: mkfs.bstdisk3 DEVICE\n");
        return 1;
    }
    int fd = open(dev, O_WRONLY);
    if (fd < 0) {
        perror(dev);
        return 1;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size >= 0 && size < MIN_SIZE) {
        fprintf(stderr, "mkfs.bstdisk3: %s is too small (%ld bytes, needs %d)\n", dev, (long)size, MIN_SIZE);
        return 1;
    }
    static char zero[BLOCK];
    char sb[BLOCK];
    memset(sb, 0, sizeof sb);
    memcpy(sb, "BSTDISK3", 8);
    // The directory first, then the superblock, so that a failed run never
    // leaves a magic over a directory that was not cleared.
    for (off_t at = BLOCK; at < FIXED_END; at += BLOCK) {
        if (pwrite(fd, zero, BLOCK, at) != BLOCK) {
            perror(dev);
            return 1;
        }
    }
    if (pwrite(fd, sb, BLOCK, 0) != BLOCK) {
        perror(dev);
        return 1;
    }
    close(fd);
    return 0;
}
