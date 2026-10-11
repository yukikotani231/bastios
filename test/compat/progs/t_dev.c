/* Block devices: a loop device over a file, set up the way losetup and
 * LTP's tst_device.c do it. musl-gcc has no Linux headers here, so the
 * ioctl numbers and struct loop_info64 are spelled out (linux/loop.h). */
#include "compat.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define LOOP_SET_FD 0x4C00
#define LOOP_CLR_FD 0x4C01
#define LOOP_SET_STATUS64 0x4C04
#define LOOP_GET_STATUS64 0x4C05
#define LOOP_CTL_GET_FREE 0x4C82
#define BLKGETSIZE64 0x80081272
#define BLKSSZGET 0x1268

struct loop_info64 {
    uint64_t lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    uint32_t lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    uint8_t lo_file_name[64], lo_crypt_name[64], lo_encrypt_key[32];
    uint64_t lo_init[2];
};

int main(void) {
    setup();
    char dev[32], buf[4096];
    struct stat st;
    struct loop_info64 li;
    uint64_t size = 0;
    int n = -1, ctl = -1, fd = -1, file = -1, bs = 0;

    /* A 64 KiB image whose first 4 KiB the device skips. */
    file = open("ld_img", O_RDWR | O_CREAT | O_TRUNC, 0600);
    memset(buf, 'x', sizeof buf);
    for (int i = 0; i < 16; i++) write(file, buf, sizeof buf);

    EXPECT("/dev/loop-control", (ctl = open("/dev/loop-control", O_RDWR)) >= 0);
    EXPECT("LOOP_CTL_GET_FREE", (n = ioctl(ctl, LOOP_CTL_GET_FREE)) >= 0);
    snprintf(dev, sizeof dev, "/dev/loop%d", n < 0 ? 0 : n);
    EXPECT("loop device is a block device", stat(dev, &st) == 0 && S_ISBLK(st.st_mode));
    EXPECT("open loop device", (fd = open(dev, O_RDWR)) >= 0);
    EXPECT("unbound: LOOP_GET_STATUS64 is ENXIO", ioctl(fd, LOOP_GET_STATUS64, &li) == -1 && errno == ENXIO);
    EXPECT("LOOP_SET_FD", ioctl(fd, LOOP_SET_FD, file) == 0);
    EXPECT("bound twice is EBUSY", ioctl(fd, LOOP_SET_FD, file) == -1 && errno == EBUSY);
    EXPECT("LOOP_CTL_GET_FREE skips it", ioctl(ctl, LOOP_CTL_GET_FREE) != n);
    memset(&li, 0, sizeof li);
    li.lo_offset = 4096;
    strcpy((char *)li.lo_file_name, "ld_img");
    EXPECT("LOOP_SET_STATUS64", ioctl(fd, LOOP_SET_STATUS64, &li) == 0);
    EXPECT("BLKGETSIZE64", ioctl(fd, BLKGETSIZE64, &size) == 0 && size == 61440);
    EXPECT("BLKSSZGET", ioctl(fd, BLKSSZGET, &bs) == 0 && bs == 512);
    memset(&li, 0, sizeof li);
    EXPECT("LOOP_GET_STATUS64", ioctl(fd, LOOP_GET_STATUS64, &li) == 0 && li.lo_offset == 4096
           && strcmp((char *)li.lo_file_name, "ld_img") == 0 && li.lo_number == (uint32_t)n);
    EXPECT("write through the device", write(fd, "hello", 5) == 5);
    EXPECT("lands past the offset", pread(file, buf, 6, 4096) == 6 && memcmp(buf, "hellox", 6) == 0);
    EXPECT("lseek SEEK_END is the device size", lseek(fd, 0, SEEK_END) == 61440);
    EXPECT("write at the end is ENOSPC", write(fd, "z", 1) == -1 && errno == ENOSPC);
    EXPECT("read through the device", lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, 7) == 7
           && memcmp(buf, "helloxx", 7) == 0);
    EXPECT("file stays bound after close", close(file) == 0 && pread(fd, buf, 2, 0) == 2);
    EXPECT("LOOP_CLR_FD", ioctl(fd, LOOP_CLR_FD, 0) == 0);
    EXPECT("cleared twice is ENXIO", ioctl(fd, LOOP_CLR_FD, 0) == -1 && errno == ENXIO);
    EXPECT("unbound reads nothing", read(fd, buf, 1) == 0);
    if (fd >= 0) close(fd);
    if (ctl >= 0) close(ctl);
    unlink("ld_img");
    return done();
}
