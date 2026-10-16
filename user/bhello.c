/* A client of bwm (user/bwmproto.h): opens a 200 x 100 window, left half
 * red and right half blue, and says "bhello: window ID" on the console.
 * A key typed into the window turns the left half to a colour by key
 * ('g' green, 'y' yellow, anything else white) and is reported as
 * "bhello: key C"; a click is reported as "bhello: mouse X Y BUTTONS"
 * (inside the window), and the close box ends the program with
 * "bhello: closed". With -s the window's pixels are in memory shared with
 * bwm (BWM_CREATE_SHM) and say "bhello: shared window ID". test/iso_test.sh
 * drives it. */
#include "bwmlib.h"

#define WIDTH 200
#define HEIGHT 100

static uint32_t buf0[WIDTH * HEIGHT];
static uint32_t *pix = buf0;
static int shared;

static int show(int fd, int id)
{
    return shared ? bwm_damage(fd, id, 0, 0, WIDTH, HEIGHT) : bwm_draw(fd, id, pix, WIDTH, 0, 0, WIDTH, HEIGHT);
}

static void paint(uint32_t left)
{
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++)
            pix[y * WIDTH + x] = x < WIDTH / 2 ? left : 0x000000FF;
}

int main(int argc, char **argv)
{
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bhello: no window server");
        return 2;
    }
    int id = -1;
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        id = bwm_create_shm(fd, WIDTH, HEIGHT, "hello", &pix);
        if (id < 0) {
            puts("bhello: cannot share a window");
            return 1;
        }
        shared = 1;
    } else {
        id = bwm_create(fd, WIDTH, HEIGHT, "hello");
    }
    if (id < 0) {
        puts("bhello: cannot open a window");
        return 1;
    }
    paint(0x00FF0000);
    if (show(fd, id) != 0) {
        puts("bhello: draw failed");
        return 1;
    }
    printf("bhello: %s %d\n", shared ? "shared window" : "window", id);
    fflush(stdout);
    static unsigned char buf[BWM_MAX_PAYLOAD];
    for (;;) {
        int t = bwm_event(fd, buf);
        if (t == BWM_KEY) {
            struct bwm_key *k = (struct bwm_key *)buf;
            paint(k->ch == 'g' ? 0x0000FF00 : k->ch == 'y' ? 0x00FFFF00 : 0x00FFFFFF);
            show(fd, id);
            printf("bhello: key %c\n", (int)k->ch);
            fflush(stdout);
        } else if (t == BWM_MOUSE) {
            struct bwm_mouse *m = (struct bwm_mouse *)buf;
            if (m->buttons) {
                printf("bhello: mouse %u %u %u\n", m->x, m->y, m->buttons);
                fflush(stdout);
            }
        } else if (t == BWM_CLOSE) {
            puts("bhello: closed");
            return 0;
        } else if (t < 0) {
            puts("bhello: server gone");
            return 1;
        }
    }
}
