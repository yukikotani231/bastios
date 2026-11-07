/* /dev/dsp, against musl: asks for 48 kHz 16-bit stereo through the OSS
 * ioctls, plays half a second of a 1 kHz square wave (24 frames at +8000,
 * 24 at -8000, both channels), waits for it to play out and prints
 * "mtone: ok". Without a sound card, opening /dev/dsp must fail with
 * ENODEV, which it reports (exit status 2). test/shell_test.sh records
 * what QEMU's HD Audio codec played into a WAV file and looks for the wave. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SNDCTL_DSP_SPEED     0xC0045002
#define SNDCTL_DSP_SETFMT    0xC0045005
#define SNDCTL_DSP_CHANNELS  0xC0045006
#define SNDCTL_DSP_GETOSPACE 0x8010500C
#define SNDCTL_DSP_SYNC      0x5001
#define AFMT_U8              0x8
#define AFMT_S16_LE          0x10

static short wave[48000 / 2 * 2];       /* half a second, two channels */

int main(void)
{
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0 && errno == ENODEV) {
        puts("mtone: no sound card (ENODEV)");
        return 2;
    }
    if (fd < 0) {
        printf("mtone: open /dev/dsp: errno %d\n", errno);
        return 1;
    }
    int fmt = AFMT_S16_LE, ch = 2, rate = 44100;
    static unsigned char small[6000];    /* 0.25 s of mono U8 at 24 kHz */
    struct { int fragments, fragstotal, fragsize, bytes; } space;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) != 0 || fmt != AFMT_S16_LE
        || ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) != 0 || ch != 2
        || ioctl(fd, SNDCTL_DSP_SPEED, &rate) != 0 || rate != 44100
        || (rate = 48000, ioctl(fd, SNDCTL_DSP_SPEED, &rate)) != 0 || rate != 48000
        || ioctl(fd, SNDCTL_DSP_GETOSPACE, &space) != 0 || space.fragsize != 4096 || space.bytes <= 0) {
        printf("mtone: ioctl: fmt %#x ch %d rate %d\n", fmt, ch, rate);
        return 1;
    }
    for (int i = 0; i < 24000; i++) {
        short v = (i / 24) % 2 ? -8000 : 8000;
        wave[2 * i] = wave[2 * i + 1] = v;
    }
    size_t done = 0;
    while (done < sizeof wave) {
        ssize_t n = write(fd, (char *)wave + done, sizeof wave - done);
        if (n <= 0) {
            printf("mtone: write: %zd, errno %d\n", n, errno);
            return 1;
        }
        done += n;
    }
    /* Straight after it, the same pitch in another format: unsigned 8-bit
     * mono at 24 kHz, which the kernel widens to the card's. 160 and 96 are
     * +-32 around the middle, +-8192 once widened. */
    fmt = AFMT_U8; ch = 1; rate = 24000;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) != 0 || fmt != AFMT_U8
        || ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) != 0 || ch != 1
        || ioctl(fd, SNDCTL_DSP_SPEED, &rate) != 0 || rate != 24000) {
        printf("mtone: convert ioctl: fmt %#x ch %d rate %d\n", fmt, ch, rate);
        return 1;
    }
    for (int i = 0; i < 6000; i++)
        small[i] = (i / 12) % 2 ? 96 : 160;
    done = 0;
    while (done < sizeof small) {
        ssize_t n = write(fd, small + done, sizeof small - done);
        if (n <= 0) {
            printf("mtone: write 8-bit: %zd, errno %d\n", n, errno);
            return 1;
        }
        done += n;
    }
    puts("mtone: converted ok");
    ioctl(fd, SNDCTL_DSP_SYNC, 0);
    close(fd);
    usleep(300000);                     /* what is queued plays out */
    printf("mtone: ok, %zu bytes\n", sizeof wave);
    return 0;
}
