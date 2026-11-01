/* A stand-in for bwm, to test bx11 (user/bx11.c) without a screen: takes one
 * shared window over BWM_SOCK, waits SECONDS for the X client to draw
 * itself, says whether the window holds pixels, moves the pointer to (10, 10)
 * and types KEYS (a string; \r is Enter).
 *
 *     fakebwm SECONDS KEYS */
#include "../../../user/bwmlib.h"
#include <stdint.h>

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
    for (const char *k = keys; *k; k++) {
        unsigned ch = (unsigned char)*k;
        if (k[0] == '\\' && k[1] == 'r') {
            ch = '\r';
            k++;
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
