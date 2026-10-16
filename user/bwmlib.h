/* A small client library for bwm (user/bwmproto.h): connect, open a
 * window, draw into it, read events. Header only. */
#ifndef BWMLIB_H
#define BWMLIB_H
#include <errno.h>
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
