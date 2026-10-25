/* A client of bwm (user/bwmproto.h): opens a 200 x 100 window, left half
 * red and right half blue, and says "bhello: window ID" on the console.
 * A key typed into the window turns the left half to a colour by key
 * ('g' green, 'y' yellow, anything else white) and is reported as
 * "bhello: key C"; a click is reported as "bhello: mouse X Y BUTTONS"
 * (inside the window), and the close box ends the program with
 * "bhello: closed". The window has a resize grip at its corner; asked for a
 * size, bhello takes it, paints the window again and says "bhello: resized
 * W H" (the size of the contents). With -s the window's pixels are in memory shared with
 * bwm (BWM_CREATE_SHM) and say "bhello: shared window ID". test/iso_test.sh
 * drives it. */
#include "bwmlib.h"

static unsigned width = 200, height = 100;
static uint32_t *pix;
static int shared;
static uint32_t left_colour = 0x00FF0000;

static int show(int fd, int id)
{
    return shared ? bwm_damage(fd, id, 0, 0, width, height) : bwm_draw(fd, id, pix, width, 0, 0, width, height);
}

static void paint(uint32_t left)
{
    left_colour = left;
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            pix[y * width + x] = x < width / 2 ? left : 0x000000FF;
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
        id = bwm_create_shm(fd, width, height, "hello", &pix);
        if (id < 0) {
            puts("bhello: cannot share a window");
            return 1;
        }
        shared = 1;
    } else {
        pix = malloc((size_t)width * height * 4);
        id = pix ? bwm_create(fd, width, height, "hello") : -1;
    }
    if (id < 0) {
        puts("bhello: cannot open a window");
        return 1;
    }
    bwm_resizable(fd, id);
    paint(left_colour);
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
        } else if (t == BWM_RESIZE) {
            struct bwm_resize *r = (struct bwm_resize *)buf;
            uint32_t *np = pix;
            if (shared) {
                if (bwm_resized_shm(fd, id, r->w, r->h, &np, (size_t)width * height * 4) != 0)
                    continue;
            } else {
                np = malloc((size_t)r->w * r->h * 4);
                if (!np || bwm_resized(fd, id, r->w, r->h) != 0)
                    return 1;
                free(pix);
            }
            pix = np;
            width = r->w;
            height = r->h;
            paint(left_colour);
            show(fd, id);
            printf("bhello: resized %u %u\n", width, height);
            fflush(stdout);
        } else if (t == BWM_CLOSE) {
            puts("bhello: closed");
            return 0;
        } else if (t < 0) {
            puts("bhello: server gone");
            return 1;
        }
    }
}
