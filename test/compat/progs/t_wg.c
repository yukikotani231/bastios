/* WireGuard devices through rtnetlink: `ip link add wg0 type wireguard`
 * makes a layer-3 device (no link-layer address, MTU 1420) that can be
 * addressed, brought up and deleted. The constants are spelled out: the C
 * library's headers lack linux/netlink.h. */
#define _GNU_SOURCE
#include "compat.h"
#include <stdint.h>
#include <sys/socket.h>

#define NETLINK_ROUTE 0
#define NLM_F_REQUEST 1
#define NLM_F_ACK 4
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLMSG_ERROR 2
#define RTM_NEWLINK 16
#define RTM_DELLINK 17
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define IFLA_ADDRESS 1
#define IFLA_IFNAME 3
#define IFLA_MTU 4
#define IFLA_LINKINFO 18
#define IFLA_INFO_KIND 1
#define IFA_LOCAL 2
#define IFA_ADDRESS 1
#define NLA_F_NESTED 0x8000
#define IFF_UP 1
#define ARPHRD_NONE 65534

struct nlh { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct nla { uint16_t len, type; };
struct ifinfo { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct ifaddr { uint8_t family, prefixlen, flags, scope; uint32_t index; };

static char buf[2048];
static struct nlh *h = (void *)buf;

static void start(uint16_t type, uint16_t flags, const void *body, int blen) {
    memset(buf, 0, sizeof buf);
    h->len = sizeof *h + blen;
    h->type = type;
    h->flags = flags | NLM_F_REQUEST;
    h->seq = 1;
    memcpy(buf + sizeof *h, body, blen);
}

static struct nla *put(int type, const void *d, int len) {
    struct nla *a = (void *)(buf + ((h->len + 3) & ~3u));
    a->type = type;
    a->len = sizeof *a + len;
    if (len) memcpy((char *)(a + 1), d, len);
    h->len = ((h->len + 3) & ~3u) + ((a->len + 3) & ~3u);
    return a;
}

static void close_nla(struct nla *a) { a->len = buf + h->len - (char *)a; }

static char rbuf[4096];
static int xfer(int s) {
    if (send(s, buf, h->len, 0) != (ssize_t)h->len) return -1000;
    ssize_t n = recv(s, rbuf, sizeof rbuf, 0);
    if (n < (ssize_t)sizeof(struct nlh)) return -1001;
    struct nlh *r = (void *)rbuf;
    if (r->type == NLMSG_ERROR) return ((int *)(rbuf + sizeof *r))[0];
    return 0;
}

static int getlink(int s, const char *name) {
    struct ifinfo i = {0};
    start(RTM_GETLINK, 0, &i, sizeof i);
    put(IFLA_IFNAME, name, strlen(name) + 1);
    return xfer(s);
}

/* The attribute of type t in the answer, past its header and ifinfomsg. */
static struct nla *find(int t) {
    struct nlh *r = (void *)rbuf;
    char *p = rbuf + sizeof *r + sizeof(struct ifinfo), *end = rbuf + r->len;
    while (p + sizeof(struct nla) <= end) {
        struct nla *a = (void *)p;
        if (a->len < sizeof *a) return NULL;
        if ((a->type & 0x3fff) == t) return a;
        p += (a->len + 3) & ~3u;
    }
    return NULL;
}

/* A link of the given kind; name may be NULL. */
static int add(int s, const char *name, const char *kind) {
    struct ifinfo i = {0};
    start(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, &i, sizeof i);
    if (name) put(IFLA_IFNAME, name, strlen(name) + 1);
    struct nla *li = put(IFLA_LINKINFO | NLA_F_NESTED, 0, 0);
    put(IFLA_INFO_KIND, kind, strlen(kind) + 1);
    close_nla(li);
    return xfer(s);
}

static int del(int s, const char *name) {
    struct ifinfo i = {0};
    start(RTM_DELLINK, NLM_F_ACK, &i, sizeof i);
    put(IFLA_IFNAME, name, strlen(name) + 1);
    return xfer(s);
}

int main(void) {
    setup();
    int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    EXPECT("netlink socket", s >= 0);
    if (s < 0) return done();

    EXPECT("a wireguard device is made", add(s, "wgt0", "wireguard") == 0);
    EXPECT("it is listed", getlink(s, "wgt0") == 0);
    struct ifinfo *ii = (void *)(rbuf + sizeof(struct nlh));
    EXPECT("it has no link-layer type", ii->type == ARPHRD_NONE);
    EXPECT("it has no link-layer address", find(IFLA_ADDRESS) == NULL);
    EXPECT("its MTU is 1420", find(IFLA_MTU) && *(uint32_t *)((char *)find(IFLA_MTU) + 4) == 1420);
    {
        struct nla *li = find(IFLA_LINKINFO);
        struct nla *k = li ? (void *)((char *)li + 4) : NULL;
        EXPECT("its kind is wireguard",
               k && (k->type & 0x3fff) == IFLA_INFO_KIND && strcmp((char *)(k + 1), "wireguard") == 0);
    }
    EXPECT("a name that is taken is EEXIST", add(s, "wgt0", "wireguard") == -17);
    EXPECT("a device with no name is EINVAL", add(s, NULL, "wireguard") == -22);
    EXPECT("a kind that only starts like it is refused", add(s, "wgt1", "wireguarx") == -95);
    EXPECT("a kind that continues past it is refused", add(s, "wgt1", "wireguard2") == -95);
    EXPECT("the refused devices were not made", getlink(s, "wgt1") == -19);

    {
        getlink(s, "wgt0");
        int index = ((struct ifinfo *)(rbuf + sizeof(struct nlh)))->index;
        uint32_t ip = 10 | 8 << 8 | 0 << 16 | (uint32_t)1 << 24;
        struct ifaddr ad = {.family = 2, .prefixlen = 24, .index = index};
        start(RTM_NEWADDR, NLM_F_CREATE | NLM_F_ACK, &ad, sizeof ad);
        put(IFA_LOCAL, &ip, 4);
        put(IFA_ADDRESS, &ip, 4);
        EXPECT("it takes an address", xfer(s) == 0);
        struct ifinfo up = {.index = index, .flags = IFF_UP, .change = IFF_UP};
        start(RTM_NEWLINK, NLM_F_ACK, &up, sizeof up);
        EXPECT("it goes up", xfer(s) == 0);
        getlink(s, "wgt0");
        ii = (void *)(rbuf + sizeof(struct nlh));
        EXPECT("it shows up", ii->flags & IFF_UP);
    }

    EXPECT("it is deleted", del(s, "wgt0") == 0);
    EXPECT("it is gone", getlink(s, "wgt0") == -19);
    EXPECT("deleting it again is ENODEV", del(s, "wgt0") == -19);
    close(s);
    return done();
}
