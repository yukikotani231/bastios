/* ALSA playback against musl, with the raw ioctls alsa-lib's "hw" plugin
 * makes: opens /dev/snd/controlC0 and pcmC0D0p, narrows the hardware
 * parameters the way snd_pcm_hw_params_set_*_near does (rate, channels,
 * format, then a buffer time and a period time), applies them, prepares and
 * writes half a second of a 1 kHz square wave as mono 24 kHz S16_LE in
 * periods, drains, and prints "malsa: ok". Without a sound card the devices
 * must fail to open with ENODEV, which it reports (exit status 2).
 * test/shell_test.sh records what QEMU's HD Audio codec played into a WAV
 * file and looks for the wave. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PCM_PVERSION   0x80044100u
#define PCM_INFO       0x81204101u
#define PCM_HW_REFINE  0xC2604110u
#define PCM_HW_PARAMS  0xC2604111u
#define PCM_SW_PARAMS  0xC0884113u
#define PCM_STATUS     0x80984120u
#define PCM_DELAY      0x80084121u
#define PCM_SYNC_PTR   0xC0884123u
#define PCM_HW_FREE    0x4112u
#define PCM_CHANNEL_INFO 0x80184132u
#define PCM_START      0x4142u
#define PCM_PREPARE    0x4140u
#define PCM_DRAIN      0x4144u
#define PCM_DROP       0x4143u
#define PCM_WRITEI     0x40184150u
#define CTL_PVERSION   0x80045500u
#define CTL_CARD_INFO  0x81785501u
#define CTL_SUBSCRIBE  0xC0045516u
#define CTL_ELEM_LIST  0xC0505510u
#define CTL_ELEM_INFO  0xC1105511u
#define CTL_ELEM_READ  0xC4C85512u
#define CTL_ELEM_WRITE 0xC4C85513u
#define CTL_PCM_NEXT   0x80045530u

struct mask { uint32_t bits[8]; };
struct interval { uint32_t min, max, flags; };
struct hw_params {                      /* struct snd_pcm_hw_params, 608 bytes */
    uint32_t flags;
    struct mask masks[3];
    struct mask mres[5];
    struct interval intervals[12];
    struct interval ires[9];
    uint32_t rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char reserved[64];
};
enum { SBITS, FBITS, CH, RATE, PTIME, PSIZE, PBYTES, PERIODS, BTIME, BSIZE, BBYTES, TICK };
struct xferi { long result; void *buf; unsigned long frames; };

static void any(struct hw_params *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 3; i++)
        memset(&p->masks[i], 0xff, sizeof p->masks[i]);
    for (int i = 0; i < 12; i++)
        p->intervals[i] = (struct interval){0, 0xffffffffu, 0};
}

static void fix(struct hw_params *p, int i, uint32_t v)
{
    p->intervals[i].min = p->intervals[i].max = v;
}

static short wave[24000 / 2];           /* half a second of mono 24 kHz */

/* Negotiates 24 kHz mono S16_LE the way aplay does with the given access
 * (1 << 3 is RW_INTERLEAVED, 1 << 0 MMAP_INTERLEAVED), applies the hardware
 * and software parameters and prepares the stream. */
static int setup(int fd, uint32_t access)
{
    int ver = 0;
    (void)ver;
    struct hw_params hw;
    any(&hw);
    if (ioctl(fd, PCM_HW_REFINE, &hw) != 0) {
        printf("malsa: refine of everything: errno %d\n", errno);
        return -1;
    }
    if (hw.intervals[RATE].min != 4000 || hw.intervals[RATE].max != 96000 || hw.intervals[CH].max != 2
        || !(hw.masks[1].bits[0] & (1u << 2))) {
        printf("malsa: refine: rate %u..%u channels ..%u\n", hw.intervals[RATE].min, hw.intervals[RATE].max, hw.intervals[CH].max);
        return -1;
    }
    /* what aplay does: access, format, channels, rate, buffer time, period time */
    memset(&hw.masks[0], 0, sizeof hw.masks[0]);
    hw.masks[0].bits[0] = access;
    memset(&hw.masks[1], 0, sizeof hw.masks[1]);
    hw.masks[1].bits[0] = 1u << 2;                        /* S16_LE */
    fix(&hw, CH, 1);
    fix(&hw, RATE, 24000);
    if (ioctl(fd, PCM_HW_REFINE, &hw) != 0) {
        printf("malsa: refine to 24 kHz mono: errno %d\n", errno);
        return -1;
    }
    fix(&hw, BTIME, 100000);
    if (ioctl(fd, PCM_HW_REFINE, &hw) != 0 || hw.intervals[BSIZE].min != 2400 || hw.intervals[BSIZE].max != 2400) {
        printf("malsa: buffer time: errno %d, size %u..%u\n", errno, hw.intervals[BSIZE].min, hw.intervals[BSIZE].max);
        return -1;
    }
    fix(&hw, PTIME, 25000);
    if (ioctl(fd, PCM_HW_REFINE, &hw) != 0 || hw.intervals[PSIZE].min != 600 || hw.intervals[PERIODS].min != 4
        || hw.intervals[PBYTES].min != 1200 || hw.intervals[FBITS].min != 16) {
        printf("malsa: period time: errno %d, size %u..%u periods %u\n", errno, hw.intervals[PSIZE].min,
               hw.intervals[PSIZE].max, hw.intervals[PERIODS].min);
        return -1;
    }
    if (ioctl(fd, PCM_HW_PARAMS, &hw) != 0) {
        printf("malsa: hw_params: errno %d\n", errno);
        return -1;
    }
    unsigned long sw[17];                                 /* struct snd_pcm_sw_params, 136 bytes */
    memset(sw, 0, sizeof sw);
    sw[2] = 600;                                          /* avail_min at 16 */
    sw[4] = 1;                                            /* start_threshold at 32 */
    if (ioctl(fd, PCM_SW_PARAMS, sw) != 0 || sw[8] < 2400) {   /* boundary at 64 */
        printf("malsa: sw_params: errno %d, boundary %lu\n", errno, sw[8]);
        return -1;
    }
    if (ioctl(fd, PCM_PREPARE, 0) != 0) {
        printf("malsa: prepare: errno %d\n", errno);
        return -1;
    }
    return 0;
}


#define PCM_READI      0x80184151u

/* Records 4800 frames (a tenth of a second) of 48 kHz stereo S16_LE through
 * /dev/snd/pcmC0D0c the way arecord -D hw does: negotiates, prepares,
 * starts and reads. Returns 0 (also without an input: ENODEV), or 1. */
static int capture(void)
{
    int fd = open("/dev/snd/pcmC0D0c", O_RDWR | O_NONBLOCK);
    if (fd < 0 && errno == ENODEV)
        return 0;
    if (fd < 0) {
        printf("malsa: open pcmC0D0c: errno %d\n", errno);
        return 1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    char pinfo[288];
    memset(pinfo, 0, sizeof pinfo);
    if (ioctl(fd, PCM_INFO, pinfo) != 0 || *(uint32_t *)(pinfo + 8) != 1) {
        printf("malsa: capture info: errno %d\n", errno);
        return 1;
    }
    struct hw_params hw;
    any(&hw);
    if (ioctl(fd, PCM_HW_REFINE, &hw) != 0 || hw.intervals[RATE].min != 48000 || hw.intervals[CH].min != 2) {
        printf("malsa: capture refine: errno %d, rate %u..%u\n", errno, hw.intervals[RATE].min, hw.intervals[RATE].max);
        return 1;
    }
    memset(&hw.masks[0], 0, sizeof hw.masks[0]);
    hw.masks[0].bits[0] = 1u << 3;
    memset(&hw.masks[1], 0, sizeof hw.masks[1]);
    hw.masks[1].bits[0] = 1u << 2;
    fix(&hw, CH, 2);
    fix(&hw, RATE, 48000);
    if (ioctl(fd, PCM_HW_REFINE, &hw) != 0) {
        printf("malsa: capture refine to 48 kHz stereo: errno %d\n", errno);
        return 1;
    }
    fix(&hw, PTIME, 21312);
    if (ioctl(fd, PCM_HW_REFINE, &hw) != 0) {
        printf("malsa: capture period time: errno %d\n", errno);
        return 1;
    }
    fix(&hw, BTIME, 85250);
    if (ioctl(fd, PCM_HW_REFINE, &hw) != 0) {
        printf("malsa: capture buffer time: errno %d\n", errno);
        return 1;
    }
    if (ioctl(fd, PCM_HW_PARAMS, &hw) != 0) {
        printf("malsa: capture hw_params: errno %d\n", errno);
        return 1;
    }
    unsigned long sw[17];
    memset(sw, 0, sizeof sw);
    sw[2] = 1200;
    sw[4] = 1;
    if (ioctl(fd, PCM_SW_PARAMS, sw) != 0 || ioctl(fd, PCM_PREPARE, 0) != 0 || ioctl(fd, PCM_START, 0) != 0) {
        printf("malsa: capture start: errno %d\n", errno);
        return 1;
    }
    static short buf[4800 * 2];
    size_t got = 0;
    while (got < 4800) {
        struct xferi x = {0, buf + got * 2, 4800 - got};
        if (ioctl(fd, PCM_READI, &x) != 0 || x.result <= 0) {
            printf("malsa: readi: errno %d, result %ld\n", errno, x.result);
            return 1;
        }
        got += x.result;
    }
    if (ioctl(fd, PCM_DROP, 0) != 0) {
        printf("malsa: capture drop: errno %d\n", errno);
        return 1;
    }
    close(fd);
    printf("malsa: captured %zu frames\n", got);
    return 0;
}

int main(int argc, char **argv)
{
    int ctl = open("/dev/snd/controlC0", O_RDWR);
    if (ctl < 0 && errno == ENODEV) {
        puts("malsa: no sound card (ENODEV)");
        return 2;
    }
    if (ctl < 0) {
        printf("malsa: open controlC0: errno %d\n", errno);
        return 1;
    }
    int ver = 0, dev = -1, sub = 1;
    char info[376];
    memset(info, 0, sizeof info);
    if (ioctl(ctl, CTL_SUBSCRIBE, &sub) != 0 || ioctl(ctl, CTL_PVERSION, &ver) != 0 || (ver >> 16) != 2 || ioctl(ctl, CTL_CARD_INFO, info) != 0
        || !strstr(info + 40, "BastiOS") || ioctl(ctl, CTL_PCM_NEXT, &dev) != 0 || dev != 0
        || ioctl(ctl, CTL_PCM_NEXT, &dev) != 0 || dev != -1) {
        printf("malsa: control: version %#x, name \"%s\", next device %d\n", ver, info + 40, dev);
        return 1;
    }
    /* one mixer control, "PCM Playback Volume": listed, described, read, written */
    {
        unsigned char list[80], ids[64], el[1224];
        memset(list, 0, sizeof list);
        if (ioctl(ctl, CTL_ELEM_LIST, list) != 0 || *(uint32_t *)(list + 12) != 1) {
            printf("malsa: elem list: errno %d, count %u\n", errno, *(uint32_t *)(list + 12));
            return 1;
        }
        *(uint32_t *)(list + 4) = 1;
        *(uint64_t *)(list + 16) = (uint64_t)(uintptr_t)ids;
        if (ioctl(ctl, CTL_ELEM_LIST, list) != 0 || *(uint32_t *)(list + 8) != 1 || strcmp((char *)ids + 16, "PCM Playback Volume")) {
            printf("malsa: elem list ids: errno %d\n", errno);
            return 1;
        }
        memset(el, 0, sizeof el);
        strcpy((char *)el + 16, "PCM Playback Volume");
        if (ioctl(ctl, CTL_ELEM_INFO, el) != 0 || *(uint32_t *)(el + 64) != 2 || *(uint32_t *)(el + 72) != 2
            || *(uint64_t *)(el + 88) != 255) {
            printf("malsa: elem info: errno %d\n", errno);
            return 1;
        }
        memset(el, 0, sizeof el);
        *(uint32_t *)el = 1;
        *(uint64_t *)(el + 72) = 200;
        *(uint64_t *)(el + 80) = 100;
        if (ioctl(ctl, CTL_ELEM_WRITE, el) != 1) {
            printf("malsa: elem write: errno %d\n", errno);
            return 1;
        }
        memset(el, 0, sizeof el);
        *(uint32_t *)el = 1;
        if (ioctl(ctl, CTL_ELEM_READ, el) != 0 || *(uint64_t *)(el + 72) != 200 || *(uint64_t *)(el + 80) != 100) {
            printf("malsa: elem read: errno %d\n", errno);
            return 1;
        }
    }
    int fd = open("/dev/snd/pcmC0D0p", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        printf("malsa: open pcmC0D0p: errno %d\n", errno);
        return 1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);   /* alsa-lib opens non-blocking, then clears it */
    char pinfo[288];
    memset(pinfo, 0, sizeof pinfo);
    if (ioctl(fd, PCM_PVERSION, &ver) != 0 || ioctl(fd, PCM_INFO, pinfo) != 0 || strncmp(pinfo + 16, "HDA", 3)) {
        printf("malsa: pcm info: errno %d\n", errno);
        return 1;
    }

    if (setup(fd, 1u << 3) != 0)
        return 1;
    for (int i = 0; i < 12000; i++)
        wave[i] = (i / 12) % 2 ? -8000 : 8000;            /* 1 kHz at 24 kHz: 12 up, 12 down */
    size_t frames = 0;
    while (frames < 12000) {
        struct xferi x = {0, wave + frames, 600};
        if (ioctl(fd, PCM_WRITEI, &x) != 0 || x.result != 600) {
            printf("malsa: writei: errno %d, result %ld\n", errno, x.result);
            return 1;
        }
        frames += x.result;
    }
    long delay = -1;
    uint64_t status[19];
    if (ioctl(fd, PCM_DELAY, &delay) != 0 || delay < 0 || delay > 2400 || ioctl(fd, PCM_STATUS, status) != 0
        || (uint32_t)status[0] != 3 || status[5] != 12000) {      /* RUNNING, appl_ptr 12000 */
        printf("malsa: status: errno %d, delay %ld, state %u appl %lu\n", errno, delay, (unsigned)status[0], (unsigned long)status[5]);
        return 1;
    }
    unsigned char sync[136];
    memset(sync, 0, sizeof sync);
    if (ioctl(fd, PCM_SYNC_PTR, sync) != 0 || *(uint64_t *)(sync + 72) != 12000) {
        printf("malsa: sync_ptr: errno %d\n", errno);
        return 1;
    }
    if (ioctl(fd, PCM_DRAIN, 0) != 0) {
        printf("malsa: drain: errno %d\n", errno);
        return 1;
    }
    if (ioctl(fd, PCM_STATUS, status) != 0 || (uint32_t)status[0] != 1) {   /* SETUP */
        printf("malsa: state after drain %u\n", (unsigned)status[0]);
        return 1;
    }
    (void)argv;
    if (argc > 1)
        goto done;                                        /* "malsa rw": the write path alone */
    usleep(1200000);                                      /* let the card stop (src/hda.bastio) */
    /* The same wave through mmap: the data pages at offset 0 (the status and
     * control pages cannot be mapped, so alsa-lib uses SYNC_PTR), committed
     * with SYNC_PTR as mmap writers do, and started once data is there. */
    unsigned char ci[24];
    memset(ci, 0, sizeof ci);
    if (ioctl(fd, PCM_HW_FREE, 0) != 0 || setup(fd, 1u << 0) != 0) {
        printf("malsa: mmap parameters: errno %d\n", errno);
        return 1;
    }
    if (ioctl(fd, PCM_CHANNEL_INFO, ci) != 0 || *(uint32_t *)(ci + 20) != 16) {
        printf("malsa: channel info: errno %d\n", errno);
        return 1;
    }
    short *map = mmap(NULL, 2400 * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED || mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x80000000L) != MAP_FAILED) {
        printf("malsa: mmap of the data pages: errno %d\n", errno);
        return 1;
    }
    uint64_t appl = 0;
    int spins = 0;
    while (appl < 12000 && spins < 4000) {
        memset(sync, 0, sizeof sync);
        if (ioctl(fd, PCM_SYNC_PTR, sync) != 0) {
            printf("malsa: sync_ptr: errno %d\n", errno);
            return 1;
        }
        uint64_t hw = *(uint64_t *)(sync + 16);
        if (2400 - (appl - hw) < 600) {
            usleep(5000);
            spins++;
            continue;
        }
        for (int i = 0; i < 600; i++)
            map[(appl + i) % 2400] = wave[appl + i];
        appl += 600;
        memset(sync, 0, sizeof sync);
        *(uint32_t *)sync = 2;                            /* SNDRV_PCM_SYNC_PTR_APPL */
        *(uint64_t *)(sync + 72) = appl;
        if (ioctl(fd, PCM_SYNC_PTR, sync) != 0) {
            printf("malsa: sync_ptr commit: errno %d\n", errno);
            return 1;
        }
        if (*(uint32_t *)(sync + 8) == 2 && ioctl(fd, PCM_START, 0) != 0) {   /* PREPARED */
            printf("malsa: start: errno %d\n", errno);
            return 1;
        }
    }
    for (spins = 0; spins < 4000; spins++) {                 /* until the card has taken it all */
        memset(sync, 0, sizeof sync);
        if (ioctl(fd, PCM_SYNC_PTR, sync) != 0 || *(uint64_t *)(sync + 16) == 12000)
            break;
        usleep(5000);
    }
    if (appl != 12000 || ioctl(fd, PCM_DRAIN, 0) != 0 || ioctl(fd, PCM_STATUS, status) != 0 || (uint32_t)status[0] != 1) {
        printf("malsa: mmap drain: committed %lu, errno %d\n", (unsigned long)appl, errno);
        return 1;
    }
    frames += appl;
done:
    close(fd);
    close(ctl);
    if (capture() != 0)
        return 1;
    usleep(300000);
    printf("malsa: ok, %zu frames\n", frames);
    return 0;
}
