/* /dev/input/event0 against musl: the requests that describe an evdev
 * device (EVIOCGVERSION, EVIOCGID, EVIOCGNAME, EVIOCGBIT, EVIOCGPHYS), and
 * a read shorter than one event, which fails with EINVAL. Says
 * "mkeyev: ok" or the first thing that was wrong (exit status 1). */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EVIOCGVERSION _IOR('E', 0x01, int)
#define EVIOCGID _IOR('E', 0x02, struct input_id)
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGPHYS(len) _IOC(_IOC_READ, 'E', 0x07, len)
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)

struct input_id { uint16_t bustype, vendor, product, version; };

static int fail(const char *what) {
    printf("mkeyev: %s (errno %d)\n", what, errno);
    return 1;
}

int main(void) {
    int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return fail("open");
    int version = 0;
    if (ioctl(fd, EVIOCGVERSION, &version) != 0 || version != 0x010001)
        return fail("EVIOCGVERSION");
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) != 0 || id.bustype != 0x11)
        return fail("EVIOCGID");
    char name[64];
    memset(name, 'x', sizeof name);
    int n = ioctl(fd, EVIOCGNAME(sizeof name), name);
    if (n != (int)strlen("BastiOS keyboard") + 1 || strcmp(name, "BastiOS keyboard") != 0)
        return fail("EVIOCGNAME");
    char shortname[4];
    n = ioctl(fd, EVIOCGNAME(sizeof shortname), shortname);
    if (n != 4)
        return fail("EVIOCGNAME (short)");
    unsigned char types[4];
    if (ioctl(fd, EVIOCGBIT(0, sizeof types), types) != 4 || (types[0] & 3) != 3 || (types[0] & 4))
        return fail("EVIOCGBIT(0)");
    unsigned char keys[96];
    int kn = ioctl(fd, EVIOCGBIT(1, sizeof keys), keys);
    if (kn != 96 || !(keys[30 / 8] & (1 << (30 % 8))) || (keys[0] & 1) || (keys[200 / 8] & (1 << (200 % 8))))
        return fail("EVIOCGBIT(EV_KEY)");
    char phys[16];
    if (ioctl(fd, EVIOCGPHYS(sizeof phys), phys) != -1 || errno != ENOENT)
        return fail("EVIOCGPHYS");
    char buf[8];
    if (read(fd, buf, sizeof buf) != -1 || errno != EINVAL)
        return fail("a read of 8 bytes");
    close(fd);
    printf("mkeyev: ok\n");
    return 0;
}
