/* A stand-in for bwm, to test bx11 (user/bx11.c) without a screen: takes one
 * shared window over BWM_SOCK, waits SECONDS for the X client to draw
 * itself, says whether the window holds pixels, moves the pointer to (10, 10)
 * and types KEYS, then says how many pixels of the window changed.
 *
 *     fakebwm SECONDS KEYS
 *
 * In KEYS: \r is Enter, \e Escape, \t Tab, \cX Ctrl-X (X a letter), \xHH a
 * byte, \mX,Y; moves the pointer to (X, Y) and clicks the left button,
 * \dX,Y; double-clicks, \wN; waits N seconds, \s; prints how many pixels
 * changed since the last \s;, and \\ is a backslash. */
#include "../../../user/bwmlib.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    unsigned wait = argc > 1 ? (unsigned)atoi(argv[1]) : 20;
    const char *keys = argc > 2 ? argv[2] : "";
    unlink(BWM_SOCK);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, BWM_SOCK);
    if (bind(ls, (void *)&a, sizeof a) || listen(ls, 1)) {
        puts("fakebwm: cannot listen");
        return 1;
    }
    puts("fakebwm: listening");
    fflush(stdout);
    int c = accept(ls, 0, 0);
    struct bwm_hdr h;
    struct bwm_create cr;
    struct iovec iov[2] = { { &h, sizeof h }, { &cr, sizeof cr } };
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } u;
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = 2, .msg_control = u.b, .msg_controllen = sizeof u.b };
    if (recvmsg(c, &mh, 0) < (ssize_t)sizeof h || h.type != BWM_CREATE_SHM) {
        puts("fakebwm: no window");
        return 1;
    }
    int mf;
    memcpy(&mf, CMSG_DATA(CMSG_FIRSTHDR(&mh)), sizeof mf);
    struct bwm_created r = { 1, cr.w, cr.h };
    bwm_send(c, BWM_CREATED, &r, sizeof r);
    uint32_t *pix = mmap(0, (size_t)cr.w * cr.h * 4, PROT_READ, MAP_SHARED, mf, 0);
    if (pix == MAP_FAILED) {
        puts("fakebwm: cannot map the window");
        return 1;
    }
    printf("fakebwm: window %ux%u %s\n", cr.w, cr.h, cr.title);
    fflush(stdout);
    unsigned long n = 0;
    for (unsigned t = 0; t < wait && n <= 1000; t++) {     /* until the X client has drawn itself */
        sleep(1);
        n = 0;
        for (size_t i = 0; i < (size_t)cr.w * cr.h; i++)
            n += pix[i] != 0;
    }
    if (n > 1000)
        sleep(3);                                          /* and has started its shell */
    printf("fakebwm: pixels %s\n", n > 1000 ? "drawn" : "missing");
    fflush(stdout);
    struct bwm_mouse m = { 1, 10, 10, 0 };
    bwm_send(c, BWM_MOUSE, &m, sizeof m);
    usleep(300000);
    size_t npix = (size_t)cr.w * cr.h;
    uint32_t *before = malloc(npix * 4);
    memcpy(before, pix, npix * 4);
    for (const char *k = keys; *k; k++) {
        unsigned ch = (unsigned char)*k;
        if (k[0] == '\\' && k[1]) {
            char e = *++k;
            if (e == 'r') ch = '\r';
            else if (e == 'e') ch = 27;
            else if (e == 't') ch = '\t';
            else if (e == '\\') ch = '\\';
            else if (e == 'c' && k[1]) ch = (unsigned)((k[1] | 32) - 'a' + 1), k++;
            else if (e == 'x') {
                ch = (unsigned)strtoul(k + 1, 0, 16);
                for (int i = 0; i < 2 && isxdigit((unsigned char)k[1]); i++) k++;
            } else if (e == 'm' || e == 'd') {
                char *end;
                unsigned x = (unsigned)strtoul(k + 1, &end, 10);
                unsigned y = *end == ',' ? (unsigned)strtoul(end + 1, &end, 10) : 0;
                k = *end == ';' ? end : end - 1;
                for (int rep = 0; rep < (e == 'd' ? 2 : 1); rep++) {
                    struct bwm_mouse mv = { 1, x, y, 0 }, dn = { 1, x, y, 1 };
                    bwm_send(c, BWM_MOUSE, &mv, sizeof mv);
                    usleep(100000);
                    bwm_send(c, BWM_MOUSE, &dn, sizeof dn);
                    usleep(80000);
                    bwm_send(c, BWM_MOUSE, &mv, sizeof mv);
                    usleep(150000);
                }
                continue;
            } else if (e == 's') {
                sleep(2);
                unsigned long d = 0;
                for (size_t i = 0; i < npix; i++)
                    d += pix[i] != before[i];
                memcpy(before, pix, npix * 4);
                printf("fakebwm: shot %s\n", d > 500 ? "changed" : "same");
                fflush(stdout);
                if (k[1] == ';') k++;
                continue;
            } else if (e == 'a') {               /* the window as text, 8 x 16 pixels to a character */
                static const char ramp[] = "@%#*+=-:. ";
                for (unsigned y = 0; y + 16 <= cr.h; y += 16) {
                    char row[256];
                    unsigned n2 = 0;
                    for (unsigned x = 0; x + 8 <= cr.w && n2 < 255; x += 8) {
                        unsigned sum = 0;
                        for (unsigned yy = 0; yy < 16; yy += 4)
                            for (unsigned xx = 0; xx < 8; xx += 2) {
                                uint32_t v = pix[(size_t)(y + yy) * cr.w + x + xx];
                                sum += ((v >> 16 & 255) + (v >> 8 & 255) + (v & 255)) / 3;
                            }
                        row[n2++] = ramp[sum / 16 * 9 / 255];
                    }
                    row[n2] = 0;
                    while (n2 && row[n2 - 1] == ' ') row[--n2] = 0;
                    printf("fakebwm| %s\n", row);
                }
                fflush(stdout);
                if (k[1] == ';') k++;
                continue;
            } else if (e == 'w') {
                char *end;
                sleep((unsigned)strtoul(k + 1, &end, 10));
                k = *end == ';' ? end : end - 1;
                continue;
            } else
                continue;
        }
        struct bwm_key kk = { 1, ch };
        bwm_send(c, BWM_KEY, &kk, sizeof kk);
        usleep(150000);
    }
    puts("fakebwm: typed");
    fflush(stdout);
    sleep(2);
    return 0;
}
