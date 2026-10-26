/* A small client library for bwm (user/bwmproto.h): connect, open a
 * window, draw into it, read events. Header only. */
#ifndef BWMLIB_H
#define BWMLIB_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "bwmproto.h"

static int bwm_write_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n > 0) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        c += w;
        n -= w;
    }
    return 0;
}

static int bwm_read_all(int fd, void *p, size_t n)
{
    char *c = p;
    while (n > 0) {
        ssize_t r = read(fd, c, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        c += r;
        n -= r;
    }
    return 0;
}

/* The size of the screen bwm runs on, from the BWM_SCREEN variable it sets
 * for its children or else from /dev/fb0: 0 x 0 when neither tells. */
static void bwm_screen(unsigned *w, unsigned *h)
{
    *w = *h = 0;
    const char *e = getenv("BWM_SCREEN");
    if (e) {
        char *end;
        unsigned long a = strtoul(e, &end, 10);
        if (*end == 'x') {
            *w = a;
            *h = strtoul(end + 1, NULL, 10);
            return;
        }
    }
    int fb = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fb >= 0) {
        unsigned v[40];
        if (ioctl(fb, 0x4600, v) == 0) {
            *w = v[0];
            *h = v[1];
        }
        close(fb);
    }
}

/* A connection to the server, or -1. */
static int bwm_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, BWM_SOCK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int bwm_send(int fd, uint32_t type, const void *payload, uint32_t len)
{
    struct bwm_hdr h = { type, len };
    if (bwm_write_all(fd, &h, sizeof h) != 0)
        return -1;
    return len ? bwm_write_all(fd, payload, len) : 0;
}

/* Opens a window with w * h pixels of contents; returns its id, or -1. */
static int bwm_create(int fd, unsigned w, unsigned h, const char *title)
{
    struct bwm_create c;
    memset(&c, 0, sizeof c);
    c.w = w;
    c.h = h;
    snprintf(c.title, sizeof c.title, "%s", title);
    if (bwm_send(fd, BWM_CREATE, &c, sizeof c) != 0)
        return -1;
    struct bwm_hdr h2;
    struct bwm_created r;
    if (bwm_read_all(fd, &h2, sizeof h2) != 0 || h2.type != BWM_CREATED || h2.len != sizeof r || bwm_read_all(fd, &r, sizeof r) != 0)
        return -1;
    return r.id;
}

/* Opens a window whose pixels are in memory shared with bwm: *pix is set
 * to its w * h words, and bwm_damage tells bwm which part changed. Returns
 * the window's id, or -1 (for instance when the server is too old to take
 * a memory file), in which case bwm_create can be tried. */
static int bwm_create_shm(int fd, unsigned w, unsigned h, const char *title, uint32_t **pix)
{
    size_t len = (size_t)w * h * 4;
    int mf = memfd_create("bwm-window", MFD_CLOEXEC);
    if (mf < 0)
        return -1;
    void *m = MAP_FAILED;
    if (ftruncate(mf, len) == 0)
        m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
    if (m == MAP_FAILED) {
        close(mf);
        return -1;
    }
    struct bwm_create c;
    memset(&c, 0, sizeof c);
    c.w = w;
    c.h = h;
    snprintf(c.title, sizeof c.title, "%s", title);
    struct bwm_hdr hd = { BWM_CREATE_SHM, sizeof c };
    char msg[sizeof hd + sizeof c];
    memcpy(msg, &hd, sizeof hd);
    memcpy(msg + sizeof hd, &c, sizeof c);
    struct iovec iov = { msg, sizeof msg };
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } u;
    memset(&u, 0, sizeof u);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = u.b;
    mh.msg_controllen = sizeof u.b;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &mf, sizeof(int));
    ssize_t n = sendmsg(fd, &mh, 0);
    close(mf);
    struct bwm_hdr h2;
    struct bwm_created r;
    if (n != (ssize_t)sizeof msg || bwm_read_all(fd, &h2, sizeof h2) != 0 || h2.type != BWM_CREATED || h2.len != sizeof r
        || bwm_read_all(fd, &r, sizeof r) != 0) {
        munmap(m, len);
        return -1;
    }
    *pix = m;
    return r.id;
}

/* Says that the window can be resized: bwm may now send BWM_RESIZE. */
static int bwm_resizable(int fd, int id)
{
    struct bwm_resizable r = { (uint32_t)id };
    return bwm_send(fd, BWM_RESIZABLE, &r, sizeof r);
}

/* Answers a BWM_RESIZE: the window's contents are now w * h pixels. */
static int bwm_resized(int fd, int id, unsigned w, unsigned h)
{
    struct bwm_resized r = { (uint32_t)id, w, h };
    return bwm_send(fd, BWM_RESIZED, &r, sizeof r);
}

/* The same for a shared window: new memory for its w * h pixels goes to
 * bwm, and *pix points to it (the old memory is unmapped, old_len bytes).
 * Returns 0, or -1 with the old pixels still in place. */
static int bwm_resized_shm(int fd, int id, unsigned w, unsigned h, uint32_t **pix, size_t old_len)
{
    size_t len = (size_t)w * h * 4;
    int mf = memfd_create("bwm-window", MFD_CLOEXEC);
    if (mf < 0)
        return -1;
    void *m = MAP_FAILED;
    if (ftruncate(mf, len) == 0)
        m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
    if (m == MAP_FAILED) {
        close(mf);
        return -1;
    }
    struct bwm_resized r = { (uint32_t)id, w, h };
    struct bwm_hdr hd = { BWM_RESIZED, sizeof r };
    char msg[sizeof hd + sizeof r];
    memcpy(msg, &hd, sizeof hd);
    memcpy(msg + sizeof hd, &r, sizeof r);
    struct iovec iov = { msg, sizeof msg };
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } u;
    memset(&u, 0, sizeof u);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = u.b;
    mh.msg_controllen = sizeof u.b;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &mf, sizeof(int));
    ssize_t n = sendmsg(fd, &mh, 0);
    close(mf);
    if (n != (ssize_t)sizeof msg) {
        munmap(m, len);
        return -1;
    }
    munmap(*pix, old_len);
    *pix = m;
    return 0;
}

/* Tells bwm that the rectangle (x, y, w, h) of a shared window changed. */
static int bwm_damage(int fd, int id, unsigned x, unsigned y, unsigned w, unsigned h)
{
    struct bwm_draw d = { (uint32_t)id, x, y, w, h };
    return bwm_send(fd, BWM_DRAW, &d, sizeof d);
}

/* Sends the rectangle (x, y, w, h) of `pix` (a picture `stride` pixels
 * wide), a few rows at a time. */
static int bwm_draw(int fd, int id, const uint32_t *pix, unsigned stride, unsigned x, unsigned y, unsigned w, unsigned h)
{
    unsigned rows = BWM_MAX_PAYLOAD / 4 / (w ? w : 1);
    if (rows > 32)
        rows = 32;
    if (rows == 0)
        return -1;
    for (unsigned r = 0; r < h; r += rows) {
        unsigned n = h - r < rows ? h - r : rows;
        struct bwm_draw d = { (uint32_t)id, x, y + r, w, n };
        struct bwm_hdr hd = { BWM_DRAW, (uint32_t)(sizeof d + (size_t)w * n * 4) };
        if (bwm_write_all(fd, &hd, sizeof hd) != 0 || bwm_write_all(fd, &d, sizeof d) != 0)
            return -1;
        for (unsigned i = 0; i < n; i++)
            if (bwm_write_all(fd, pix + (size_t)(y + r + i) * stride + x, (size_t)w * 4) != 0)
                return -1;
    }
    return 0;
}

/* The next event: its type, and its payload in buf (BWM_MAX_PAYLOAD at most). -1 at the end. */
static int bwm_event(int fd, void *buf)
{
    struct bwm_hdr h;
    if (bwm_read_all(fd, &h, sizeof h) != 0 || h.len > BWM_MAX_PAYLOAD || bwm_read_all(fd, buf, h.len) != 0)
        return -1;
    return h.type;
}

#endif
