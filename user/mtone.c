/* /dev/dsp, against musl: asks for 48 kHz 16-bit stereo through the OSS
 * ioctls, plays half a second of a 1 kHz square wave (24 frames at +8000,
 * 24 at -8000, both channels), waits for it to play out and prints
 * "mtone: ok". With the argument "none" it expects there to be no sound
 * card: opening /dev/dsp fails with ENODEV. test/shell_test.sh records
 * what QEMU's HD Audio codec played into a WAV file and looks for the wave. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SNDCTL_DSP_SPEED     0xC0045002
#define SNDCTL_DSP_SETFMT    0xC0045005
#define SNDCTL_DSP_CHANNELS  0xC0045006
#define SNDCTL_DSP_GETOSPACE 0x8010500C
#define SNDCTL_DSP_SYNC      0x5001
#define AFMT_S16_LE          0x10

static short wave[48000 / 2 * 2];       /* half a second, two channels */

int main(int argc, char **argv)
{
    int fd = open("/dev/dsp", O_WRONLY);
    if (argc > 1 && strcmp(argv[1], "none") == 0) {
        if (fd < 0 && errno == ENODEV) {
            puts("mtone: no sound card (ENODEV)");
            return 0;
        }
        printf("mtone: open gave %d, errno %d\n", fd, errno);
        return 1;
    }
    if (fd < 0) {
        printf("mtone: open /dev/dsp: errno %d\n", errno);
        return 1;
    }
    int fmt = AFMT_S16_LE, ch = 2, rate = 44100;
    struct { int fragments, fragstotal, fragsize, bytes; } space;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) != 0 || fmt != AFMT_S16_LE
        || ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) != 0 || ch != 2
        || ioctl(fd, SNDCTL_DSP_SPEED, &rate) != 0 || rate != 48000
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
    ioctl(fd, SNDCTL_DSP_SYNC, 0);
    close(fd);
    usleep(300000);                     /* what is queued plays out */
    printf("mtone: ok, %zu bytes\n", done);
    return 0;
}
