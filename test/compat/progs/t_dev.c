/* Block devices: a loop device over a file, set up the way losetup and
 * LTP's tst_device.c do it. musl-gcc has no Linux headers here, so the
 * ioctl numbers and struct loop_info64 are spelled out (linux/loop.h). */
#include "compat.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define LOOP_SET_FD 0x4C00
#define LOOP_CLR_FD 0x4C01
#define LOOP_GET_STATUS 0x4C03
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

/* struct loop_info, x86-64 layout. */
struct loop_info {
    int lo_number;
    unsigned long lo_device, lo_inode, lo_rdevice;
    int lo_offset, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    char lo_name[64];
    unsigned char lo_encrypt_key[32];
    unsigned long lo_init[2];
    char reserved[4];
};

/* Reads /sys/block/loopN/ATTR into buf. */
static int read_sys(int n, const char *attr, char *buf) {
    char path[64];
    snprintf(path, sizeof path, "/sys/block/loop%d/%s", n, attr);
    int f = open(path, O_RDONLY);
    if (f < 0) return 0;
    int r = read(f, buf, 255);
    close(f);
    if (r <= 0) return 0;
    buf[r] = 0;
    return 1;
}

int main(void) {
    setup();
    char dev[32], buf[4096];
    struct stat st;
    struct loop_info64 li;
    struct loop_info old;
    static char big[6000];
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
    EXPECT("/sys/block size in sectors", read_sys(n, "size", buf) && strcmp(buf, "120\n") == 0);
    EXPECT("/sys/block loop/offset", read_sys(n, "loop/offset", buf) && strcmp(buf, "4096\n") == 0);
    EXPECT("/sys/block ro", read_sys(n, "ro", buf) && strcmp(buf, "0\n") == 0);
    EXPECT("/sys/block loop/backing_file", read_sys(n, "loop/backing_file", buf)
           && strstr(buf, "/ld_img\n") != 0 && buf[0] == '/');
    memset(&old, 0, sizeof old);
    EXPECT("LOOP_GET_STATUS (old struct)", ioctl(fd, LOOP_GET_STATUS, &old) == 0 && old.lo_offset == 4096
           && old.lo_number == n && strcmp(old.lo_name, "ld_img") == 0);
    EXPECT("write through the device", write(fd, "hello", 5) == 5);
    EXPECT("lands past the offset", pread(file, buf, 6, 4096) == 6 && memcmp(buf, "hellox", 6) == 0);
    EXPECT("lseek SEEK_END is the device size", lseek(fd, 0, SEEK_END) == 61440);
    EXPECT("write at the end is ENOSPC", write(fd, "z", 1) == -1 && errno == ENOSPC);
    EXPECT("read through the device", lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, 7) == 7
           && memcmp(buf, "helloxx", 7) == 0);
    memset(big, 'y', sizeof big);
    EXPECT("one write of 6000 bytes", lseek(fd, 0, SEEK_SET) == 0 && write(fd, big, sizeof big) == sizeof big
           && pread(fd, buf, 4, 5996) == 4 && memcmp(buf, "yyyy", 4) == 0);
    EXPECT("file stays bound after close", close(file) == 0 && pread(fd, buf, 2, 0) == 2);
    EXPECT("LOOP_CLR_FD", ioctl(fd, LOOP_CLR_FD, 0) == 0);
    EXPECT("cleared twice is ENXIO", ioctl(fd, LOOP_CLR_FD, 0) == -1 && errno == ENXIO);
    EXPECT("unbound reads nothing", read(fd, buf, 1) == 0);
    if (fd >= 0) close(fd);

    /* Thirty-two devices at once: loop8 .. loop31 take two digits. */
    int bound = 0, lfd[32];
    file = open("ld_img", O_RDWR);
    for (int i = 0; i < 32; i++) lfd[i] = -1;
    for (int i = 8; i < 32; i++) {
        snprintf(dev, sizeof dev, "/dev/loop%d", i);
        lfd[i] = open(dev, O_RDWR);
        if (lfd[i] >= 0 && ioctl(lfd[i], LOOP_SET_FD, file) == 0) bound++;
    }
    EXPECT("loop8 .. loop31 all bind", bound == 24);
    EXPECT("loop31 is block device 7:31", stat("/dev/loop31", &st) == 0 && S_ISBLK(st.st_mode)
           && major(st.st_rdev) == 7 && minor(st.st_rdev) == 31);
    EXPECT("/sys/block/loop31/size", read_sys(31, "size", buf) && atoi(buf) == 128);
    EXPECT("/sys/block/loop17 is its own", lfd[17] >= 0 && read_sys(17, "loop/backing_file", buf)
           && strstr(buf, "/ld_img\n") != 0);
    EXPECT("the free ones are loop0 .. loop7", (n = ioctl(ctl, LOOP_CTL_GET_FREE)) >= 0 && n < 8);
    EXPECT("loop32 does not exist", open("/dev/loop32", O_RDWR) < 0 && errno == ENOENT);
    EXPECT("loop08 does not exist", stat("/dev/loop08", &st) < 0 && errno == ENOENT);
    EXPECT("/sys/block/loop32 does not exist", !read_sys(32, "size", buf));
    for (int i = 8; i < 32; i++)
        if (lfd[i] >= 0) { ioctl(lfd[i], LOOP_CLR_FD, 0); close(lfd[i]); }
    if (file >= 0) close(file);
    if (ctl >= 0) close(ctl);
    unlink("ld_img");
    return done();
}
