/* rtnetlink multicast groups: a NETLINK_ROUTE socket that joined RTNLGRP_LINK
 * hears RTM_NEWLINK / RTM_DELLINK when a link is made, changed or removed, and
 * one in RTNLGRP_IPV4_IFADDR hears RTM_NEWADDR / RTM_DELADDR when an address
 * is added or removed. A socket in no group, or in the other group, hears
 * nothing of it, and neither does one in another network namespace. */
#define _GNU_SOURCE
#include "compat.h"
#include <poll.h>
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
#define RTM_DELLINK 17
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define RTM_DELADDR 21
#define IFLA_IFNAME 3
#define IFLA_LINKINFO 18
#define IFLA_INFO_KIND 1
#define IFLA_INFO_DATA 2
#define VETH_INFO_PEER 1
#define IFA_LOCAL 2
#define IFA_ADDRESS 1
#define NLA_F_NESTED 0x8000
#define IFF_UP 1
#define GRP_LINK 1u
#define GRP_ADDR 16u

struct nlh { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct nla { uint16_t len, type; };
struct ifinfo { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct ifaddr { uint8_t family, prefixlen, flags, scope; uint32_t index; };
struct sockaddr_nl_ { uint16_t family, pad; uint32_t pid, groups; };

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
    struct pollfd pf = {s, POLLIN, 0};
    if (poll(&pf, 1, 2000) != 1) return -1002;
    ssize_t n = recv(s, rbuf, sizeof rbuf, 0);
    if (n < (ssize_t)sizeof(struct nlh)) return -1001;
    struct nlh *r = (void *)rbuf;
    if (r->type == NLMSG_ERROR) return ((int *)(rbuf + sizeof *r))[0];
    return 0;
}

static int veth(int s, const char *a, const char *b) {
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
    close_nla(p); close_nla(d); close_nla(li);
    return xfer(s);
}

static int index_of(int s, const char *name) {
    struct ifinfo i = {0};
    start(RTM_GETLINK, 0, &i, sizeof i);
    put(IFLA_IFNAME, name, strlen(name) + 1);
    if (xfer(s) != 0) return -1;
    return ((struct ifinfo *)(rbuf + sizeof(struct nlh)))->index;
}

static uint32_t ip4(int a, int b, int c, int d) { return a | b << 8 | c << 16 | (uint32_t)d << 24; }

static int addr(int s, int type, int index, uint32_t ip) {
    struct ifaddr ad = {.family = 2, .prefixlen = 24, .index = index};
    start(type, (type == RTM_NEWADDR ? NLM_F_CREATE : 0) | NLM_F_ACK, &ad, sizeof ad);
    put(IFA_LOCAL, &ip, 4);
    put(IFA_ADDRESS, &ip, 4);
    return xfer(s);
}
static int set_up(int s, int index) {
    struct ifinfo up = {.index = index, .flags = IFF_UP, .change = IFF_UP};
    start(RTM_NEWLINK, NLM_F_ACK, &up, sizeof up);
    return xfer(s);
}
static int del_link(int s, int index) {
    struct ifinfo i = {.index = index};
    start(RTM_DELLINK, NLM_F_ACK, &i, sizeof i);
    return xfer(s);
}

static int sock(unsigned groups) {
    int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (s < 0) return -errno;
    struct sockaddr_nl_ a = {.family = AF_NETLINK, .groups = groups};
    if (bind(s, (void *)&a, sizeof a) != 0) { int e = errno; close(s); return -e; }
    return s;
}

/* The next event on s within ms: its type, with the interface name (links)
 * or the address (addresses) in *name / *ip; 0 if none came. A read can
 * return several messages, so the rest is kept for the next call. */
static char pend[8192];
static int pendlen, pendsock = -1;
static int next_ev(int s, int ms, char *name, uint32_t *ip) {
    if (pendsock != s) pendlen = 0;
    if (pendlen == 0) {
        struct pollfd pf = {s, POLLIN, 0};
        if (poll(&pf, 1, ms) != 1) return 0;
        ssize_t n = recv(s, pend, sizeof pend, 0);
        if (n < 16) return 0;
        pendlen = n;
        pendsock = s;
    }
    struct nlh *m = (void *)pend;
    int len = (m->len + 3) & ~3, type = m->type;
    int body = (type == RTM_NEWLINK || type == RTM_DELLINK) ? 16 : 8;
    name[0] = 0;
    *ip = 0;
    for (char *p = pend + 16 + body; p + 4 <= pend + m->len;) {
        struct nla *a = (void *)p;
        if (a->len < 4) break;
        if (type == RTM_NEWLINK || type == RTM_DELLINK) {
            if (a->type == IFLA_IFNAME) snprintf(name, 16, "%s", p + 4);
        } else if (a->type == IFA_ADDRESS || a->type == IFA_LOCAL) {
            memcpy(ip, p + 4, 4);
        }
        p += (a->len + 3) & ~3;
    }
    if (len > pendlen) len = pendlen;
    memmove(pend, pend + len, pendlen - len);
    pendlen -= len;
    return type;
}

int main(void) {
    setup();
    int x = sock(0);
    int links = sock(GRP_LINK), addrs = sock(GRP_ADDR), both = sock(GRP_LINK | GRP_ADDR), none = sock(0);
    EXPECT("sockets in the link group, the address group, both and none", x >= 0 && links >= 0 && addrs >= 0 && both >= 0 && none >= 0);
    if (x < 0 || links < 0 || addrs < 0 || both < 0) return done();
    char nm[32];
    uint32_t ip;
    EXPECT("a veth pair is made", veth(x, "evA", "evB") == 0);
    EXPECT("the link group hears both ends", next_ev(links, 1000, nm, &ip) == RTM_NEWLINK && !strcmp(nm, "evA")
        && next_ev(links, 1000, nm, &ip) == RTM_NEWLINK && !strcmp(nm, "evB"));
    EXPECT("a socket in both groups hears them too", next_ev(both, 1000, nm, &ip) == RTM_NEWLINK && next_ev(both, 1000, nm, &ip) == RTM_NEWLINK);
    EXPECT("the address group and no group hear nothing of it", !next_ev(addrs, 300, nm, &ip) && !next_ev(none, 300, nm, &ip));
    int ia = index_of(x, "evA");
    EXPECT("the link has an index", ia > 0);
    EXPECT("an address is added", addr(x, RTM_NEWADDR, ia, ip4(10, 77, 1, 1)) == 0);
    EXPECT("the address group hears RTM_NEWADDR with it", next_ev(addrs, 1000, nm, &ip) == RTM_NEWADDR && ip == ip4(10, 77, 1, 1));
    EXPECT("the link group does not", !next_ev(links, 300, nm, &ip));
    EXPECT("the link is set up", set_up(x, ia) == 0);
    EXPECT("the link group hears RTM_NEWLINK for it", next_ev(links, 1000, nm, &ip) == RTM_NEWLINK && !strcmp(nm, "evA"));
    EXPECT("the address group does not", !next_ev(addrs, 300, nm, &ip));
    EXPECT("the address is removed", addr(x, RTM_DELADDR, ia, ip4(10, 77, 1, 1)) == 0);
    EXPECT("the address group hears RTM_DELADDR", next_ev(addrs, 1000, nm, &ip) == RTM_DELADDR && ip == ip4(10, 77, 1, 1));
    EXPECT("the link is removed", del_link(x, ia) == 0);
    EXPECT("the link group hears RTM_DELLINK for both ends", next_ev(links, 1000, nm, &ip) == RTM_DELLINK && !strcmp(nm, "evA")
        && next_ev(links, 1000, nm, &ip) == RTM_DELLINK && !strcmp(nm, "evB"));
    EXPECT("the socket in both groups heard the rest", ({ int a = next_ev(both, 300, nm, &ip), c = 0; while (a) { c++; a = next_ev(both, 100, nm, &ip); } c == 5; }));
    EXPECT("a socket in no group heard nothing at all", !next_ev(none, 300, nm, &ip));

    pid_t c = fork();
    if (c == 0) {
        if (unshare(CLONE_NEWNET) != 0) _exit(2);
        int y = sock(0);
        _exit(veth(y, "evC", "evD") == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(c, &st, 0);
    EXPECT("another namespace makes a pair", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    EXPECT("it is not reported here", !next_ev(links, 500, nm, &ip));
    return done();
}
