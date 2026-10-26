/* rtnetlink the way Docker's libnetwork uses it: a veth pair whose peer is
 * made straight in another network namespace (IFLA_NET_NS_PID in
 * VETH_INFO_PEER), and RTM_GETROUTE for one address (programGateway). The
 * constants are spelled out: the C library's headers lack linux/netlink.h. */
#define _GNU_SOURCE
#include "compat.h"
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define NETLINK_ROUTE 0
#define NLM_F_REQUEST 1
#define NLM_F_ACK 4
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLMSG_ERROR 2
#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26
#define IFLA_IFNAME 3
#define IFLA_NET_NS_PID 19
#define IFLA_LINKINFO 18
#define IFLA_INFO_KIND 1
#define IFLA_INFO_DATA 2
#define VETH_INFO_PEER 1
#define IFA_LOCAL 2
#define IFA_ADDRESS 1
#define RTA_DST 1
#define RTA_OIF 4
#define RTA_GATEWAY 5
#define NLA_F_NESTED 0x8000
#define IFF_UP 1

struct nlh { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct nla { uint16_t len, type; };
struct ifinfo { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct ifaddr { uint8_t family, prefixlen, flags, scope; uint32_t index; };
struct rtm { uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags; };

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

/* Sends the request on s; returns the error of the answer (0 for an ack or
 * a RTM_NEWROUTE/RTM_NEWLINK answer, -errno), the answer left in rbuf. */
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

/* The attribute of type t in the answer after its header and a body of
 * blen bytes, or NULL. */
static struct nla *find(int blen, int t) {
    struct nlh *r = (void *)rbuf;
    char *p = rbuf + sizeof *r + blen, *end = rbuf + r->len;
    while (p + sizeof(struct nla) <= end) {
        struct nla *a = (void *)p;
        if (a->len < sizeof *a) return NULL;
        if ((a->type & 0x3fff) == t) return a;
        p += (a->len + 3) & ~3u;
    }
    return NULL;
}

/* A veth pair a/b with b made in the namespace of process pid. */
static int veth_in(int s, const char *a, const char *b, pid_t pid) {
    struct ifinfo i = {0}, pi = {0};
    start(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, &i, sizeof i);
    put(IFLA_IFNAME, a, strlen(a) + 1);
    struct nla *li = put(IFLA_LINKINFO | NLA_F_NESTED, 0, 0);
    put(IFLA_INFO_KIND, "veth", 5);
    struct nla *d = put(IFLA_INFO_DATA | NLA_F_NESTED, 0, 0);
    struct nla *p = put(VETH_INFO_PEER | NLA_F_NESTED, 0, 0);
    memcpy(buf + h->len, &pi, sizeof pi);
    h->len += sizeof pi;
    put(IFLA_IFNAME, b, strlen(b) + 1);
    if (pid) { uint32_t v = pid; put(IFLA_NET_NS_PID, &v, 4); }
    close_nla(p); close_nla(d); close_nla(li);
    return xfer(s);
}

/* In the namespace s is in: address ip/24 on link name, up. */
static int addr_up(int s, const char *name, uint32_t ip) {
    int idx = getlink(s, name);
    if (idx != 0) return -1;
    struct ifinfo *ii = (void *)(rbuf + sizeof(struct nlh));
    int index = ii->index;
    struct ifaddr ad = {.family = 2, .prefixlen = 24, .index = index};
    start(RTM_NEWADDR, NLM_F_CREATE | NLM_F_ACK, &ad, sizeof ad);
    put(IFA_LOCAL, &ip, 4);
    put(IFA_ADDRESS, &ip, 4);
    if (xfer(s) != 0) return -2;
    struct ifinfo up = {.family = 0, .index = index, .flags = IFF_UP, .change = IFF_UP};
    start(RTM_NEWLINK, NLM_F_ACK, &up, sizeof up);
    return xfer(s) == 0 ? 0 : -3;
}

static uint32_t ip4(int a, int b, int c, int d) { return a | b << 8 | c << 16 | (uint32_t)d << 24; }

/* The route to ip: 0 if one comes back (its link in *oif, whether it has a
 * gateway in *gw), else the error. */
static int route_get(int s, uint32_t ip, int *oif, int *gw) {
    struct rtm m = {.family = 2, .dst_len = 32};
    start(RTM_GETROUTE, 0, &m, sizeof m);
    put(RTA_DST, &ip, 4);
    int e = xfer(s);
    if (e != 0) return e;
    struct nlh *r = (void *)rbuf;
    if (r->type != RTM_NEWROUTE) return -1002;
    struct nla *o = find(sizeof m, RTA_OIF);
    *oif = o ? *(int *)(o + 1) : 0;
    *gw = find(sizeof m, RTA_GATEWAY) != NULL;
    return 0;
}

int main(void) {
    setup();
    int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    int in[2], out[2];
    pid_t c = -1;
    char x;

    EXPECT("netlink socket", s >= 0);
    EXPECT("a child in a new network namespace", pipe(in) == 0 && pipe(out) == 0 && (c = fork()) >= 0);
    if (c == 0) {
        /* The child: a namespace of its own, found by the parent by pid,
         * then the peer it was given: address it, ask for routes. */
        int cs = -1, oif = 0, gw = 0, ok = 1;
        close(in[1]); close(out[0]);
        if (unshare(CLONE_NEWNET) != 0) _exit(1);
        cs = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (write(out[1], "r", 1) != 1 || read(in[0], &x, 1) != 1) _exit(2);
        ok &= getlink(cs, "nlvb") == 0;
        ok &= getlink(cs, "nlva") == -19;           /* ENODEV: the other end is not here */
        ok &= addr_up(cs, "nlvb", ip4(10, 77, 0, 2)) == 0;
        ok &= route_get(cs, ip4(10, 77, 0, 9), &oif, &gw) == 0 && oif != 0 && !gw;
        ok &= route_get(cs, ip4(8, 8, 8, 8), &oif, &gw) == -101;   /* ENETUNREACH */
        if (write(out[1], ok ? "y" : "n", 1) != 1) _exit(3);
        _exit(0);
    }
    close(in[0]); close(out[1]);
    EXPECT("the child is ready", read(out[0], &x, 1) == 1);
    EXPECT("a veth pair with its peer in another namespace",
           veth_in(s, "nlva", "nlvb", c) == 0);
    EXPECT("the host end is here", getlink(s, "nlva") == 0);
    EXPECT("the peer is not here", getlink(s, "nlvb") == -19);
    EXPECT("a peer name that is taken is refused", veth_in(s, "nlva", "nlvc", c) == -17);
    EXPECT("a peer in a namespace of no process is refused", veth_in(s, "nlvd", "nlve", 999999) != 0);
    EXPECT("the peer there: addressed, routes asked for",
           write(in[1], "g", 1) == 1 && read(out[0], &x, 1) == 1 && x == 'y');
    {
        int st;
        EXPECT("the child ended well", waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    return done();
}
