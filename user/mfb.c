/* /dev/fb0, against musl: the screen as a Linux framebuffer. It reads the
 * geometry (FBIOGET_VSCREENINFO, FBIOGET_FSCREENINFO), checks the mode is
 * 32 bits a pixel, and draws with lseek and write: the top half of the
 * screen red, the bottom half blue, a 64x64 green square at (100, 100).
 * Then it says "mfb: drawn WxH" and waits for a line on its input, after
 * which it checks that a write at the end of the screen fails with
 * ENOSPC, closes the device (so the console is drawn again) and says
 * "mfb: done". Without a screen, opening the device fails with ENODEV,
 * which it reports (exit status 2). test/iso_test.sh reads the screen
 * back from a QEMU screendump while the program waits and after it ends. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct bitfield { uint32_t offset, length, msb_right; };
struct var_info {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct bitfield red, green, blue, transp;
    uint32_t rest[20];
};
struct fix_info {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

static uint32_t pixel(const struct var_info *v, unsigned r, unsigned g, unsigned b)
{
    return (r << v->red.offset) | (g << v->green.offset) | (b << v->blue.offset);
}

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 && errno == ENODEV) {
        puts("mfb: no screen (ENODEV)");
        return 2;
    }
    if (fd < 0) {
        printf("mfb: open: errno %d\n", errno);
        return 1;
    }
    struct var_info v;
    struct fix_info f;
    if (ioctl(fd, 0x4600, &v) != 0 || ioctl(fd, 0x4602, &f) != 0) {
        printf("mfb: ioctl: errno %d\n", errno);
        return 1;
    }
    if (v.bits_per_pixel != 32 || v.red.length != 8 || f.line_length < v.xres * 4 || strcmp(f.id, "BastiOS") != 0) {
        printf("mfb: unexpected mode: %u bpp, red %u bits, line %u, id %s\n", v.bits_per_pixel, v.red.length, f.line_length, f.id);
        return 1;
    }
    static uint32_t line[8192];
    if (f.line_length > sizeof line) {
        puts("mfb: line too long");
        return 1;
    }
    for (unsigned y = 0; y < v.yres; y++) {
        uint32_t c = y < v.yres / 2 ? pixel(&v, 255, 0, 0) : pixel(&v, 0, 0, 255);
        for (unsigned x = 0; x < v.xres; x++)
            line[x] = c;
        if (y >= 100 && y < 164)
            for (unsigned x = 100; x < 164; x++)
                line[x] = pixel(&v, 0, 255, 0);
        if (lseek(fd, (off_t)y * f.line_length, SEEK_SET) != (off_t)y * f.line_length
            || write(fd, line, f.line_length) != (ssize_t)f.line_length) {
            printf("mfb: write at line %u: errno %d\n", y, errno);
            return 1;
        }
    }
    printf("mfb: drawn %ux%u\n", v.xres, v.yres);
    fflush(stdout);
    char buf[16];
    read(0, buf, sizeof buf);
    off_t end = lseek(fd, 0, SEEK_END);
    if (end != (off_t)f.line_length * v.yres) {
        printf("mfb: SEEK_END gave %ld\n", (long)end);
        return 1;
    }
    if (write(fd, line, 4) != -1 || errno != ENOSPC) {
        printf("mfb: write at the end: errno %d\n", errno);
        return 1;
    }
    close(fd);
    puts("mfb: done");
    return 0;
}
