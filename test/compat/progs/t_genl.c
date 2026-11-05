/* Generic netlink: NETLINK_GENERIC sockets and the controller family
 * "nlctrl", through which a program finds the id of every other family
 * (WireGuard, nl80211, ...). Constants are spelled out: the C library's
 * headers lack linux/genetlink. */
#define _GNU_SOURCE
#include "compat.h"
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>

#define NETLINK_GENERIC 16
#define NLM_F_REQUEST 1
#define NLM_F_MULTI 2
#define NLM_F_ACK 4
#define NLM_F_DUMP 0x300
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define GENL_ID_CTRL 16
#define CTRL_CMD_NEWFAMILY 1
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2
#define CTRL_ATTR_VERSION 3
#define CTRL_ATTR_MAXATTR 5

struct nlh { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };

static char buf[512];
static int blen;
static char ans[8192];
static int anslen;

static void start(int type, int flags, int cmd) {
    struct nlh *h = (void *)buf;
    memset(buf, 0, sizeof buf);
    h->type = type;
    h->flags = flags;
    h->seq = 7;
    buf[16] = cmd;
    buf[17] = 1;
    blen = 20;
}

static void attr(int type, const void *d, int len) {
    uint16_t *a = (void *)(buf + blen);
    a[0] = 4 + len;
    a[1] = type;
    memcpy(buf + blen + 4, d, len);
    blen = (blen + 4 + len + 3) & ~3;
}

static void fin(void) { ((struct nlh *)buf)->len = blen; }

/* Sends buf and gathers every answer until an error, a done, or a single
 * reply (unless a dump is asked for); returns the error (an errno, 0 for
 * none). */
static int talk(int s, int dump) {
    fin();
    if (send(s, buf, blen, 0) != blen) return -errno;
    anslen = 0;
    for (;;) {
        char rb[4096];
        struct pollfd pf = {s, POLLIN, 0};
        if (poll(&pf, 1, 1500) != 1) return -ETIMEDOUT;
        ssize_t n = recv(s, rb, sizeof rb, 0);
        if (n <= 0) return -ETIMEDOUT;
        if (anslen + n <= (int)sizeof ans) {
            memcpy(ans + anslen, rb, n);
            anslen += n;
        }
        for (char *p = rb; p < rb + n;) {
            struct nlh *h = (void *)p;
            if (h->len < 16) return -EBADMSG;
            if (h->type == NLMSG_ERROR) return *(int *)(p + 16);
            if (h->type == NLMSG_DONE) return 0;
            if (!dump) return 0;
            p += (h->len + 3) & ~3u;
        }
    }
}

/* The value of attribute type in the first reply (after the 20 bytes of
 * headers), or -1. */
static long find(int type, int from) {
    for (int at = from + 20; at + 4 <= anslen;) {
        uint16_t *a = (void *)(ans + at);
        if (a[0] < 4) return -1;
        if (a[1] == type) {
            if (type == CTRL_ATTR_FAMILY_ID) return a[2];
            if (type == CTRL_ATTR_FAMILY_NAME) return at + 4;
            return *(uint32_t *)(ans + at + 4);
        }
        at += (a[0] + 3) & ~3;
    }
    return -1;
}

int main(void) {
    setup();
    int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    EXPECT("a NETLINK_GENERIC socket opens", s >= 0);
    if (s < 0) return done();

    start(GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY);
    attr(CTRL_ATTR_FAMILY_NAME, "nlctrl", 7);
    int e = talk(s, 0);
    EXPECT("GETFAMILY by name finds nlctrl", e == 0 && find(CTRL_ATTR_FAMILY_ID, 0) == GENL_ID_CTRL);
    EXPECT("the reply carries the name back",
           e == 0 && find(CTRL_ATTR_FAMILY_NAME, 0) > 0 &&
               strcmp(ans + find(CTRL_ATTR_FAMILY_NAME, 0), "nlctrl") == 0);
    EXPECT("the reply is a NEWFAMILY of the controller",
           anslen >= 20 && ((struct nlh *)ans)->type == GENL_ID_CTRL && ans[16] == CTRL_CMD_NEWFAMILY);
    EXPECT("the reply names the version and the largest attribute",
           find(CTRL_ATTR_VERSION, 0) > 0 && find(CTRL_ATTR_MAXATTR, 0) > 0);

    start(GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY);
    uint16_t id = GENL_ID_CTRL;
    attr(CTRL_ATTR_FAMILY_ID, &id, 2);
    e = talk(s, 0);
    EXPECT("GETFAMILY by id finds nlctrl", e == 0 && find(CTRL_ATTR_FAMILY_ID, 0) == GENL_ID_CTRL);

    start(GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY);
    attr(CTRL_ATTR_FAMILY_NAME, "nosuchfamily", 13);
    EXPECT("an unknown family name is ENOENT", talk(s, 0) == -ENOENT);

    start(GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY);
    id = 4000;
    attr(CTRL_ATTR_FAMILY_ID, &id, 2);
    EXPECT("an unknown family id is ENOENT", talk(s, 0) == -ENOENT);

    start(GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY);
    EXPECT("GETFAMILY without an attribute is EINVAL", talk(s, 0) == -EINVAL);

    start(GENL_ID_CTRL, NLM_F_REQUEST, 99);
    EXPECT("an unknown controller command is EOPNOTSUPP", talk(s, 0) == -EOPNOTSUPP);

    start(40, NLM_F_REQUEST, 1);
    EXPECT("a message to a family that is not there is ENOENT", talk(s, 0) == -ENOENT);

    start(GENL_ID_CTRL, NLM_F_REQUEST | NLM_F_DUMP, CTRL_CMD_GETFAMILY);
    e = talk(s, 1);
    EXPECT("a dump lists nlctrl and ends", e == 0 && anslen > 20 && find(CTRL_ATTR_FAMILY_ID, 0) == GENL_ID_CTRL);

    /* A message cut short is refused rather than read past its end. */
    start(GENL_ID_CTRL, NLM_F_REQUEST | NLM_F_ACK, CTRL_CMD_GETFAMILY);
    blen = 17;
    EXPECT("a message shorter than a generic header is EINVAL", talk(s, 0) == -EINVAL);

    /* An attribute that claims more bytes than the message holds. */
    start(GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY);
    attr(CTRL_ATTR_FAMILY_NAME, "nlctrl", 7);
    ((uint16_t *)(buf + 20))[0] = 400;
    EXPECT("an attribute longer than its message is refused", talk(s, 0) != 0);

    /* The socket still answers after the bad ones. */
    start(GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY);
    attr(CTRL_ATTR_FAMILY_NAME, "nlctrl", 7);
    EXPECT("the socket still answers after bad requests", talk(s, 0) == 0 && find(CTRL_ATTR_FAMILY_ID, 0) == GENL_ID_CTRL);

    close(s);
    return done();
}
