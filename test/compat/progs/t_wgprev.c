/* compat-timeout: 420
 * WireGuard previous session: packets the peer sent under the old keys just
 * before a new handshake finished are still taken, once each, until the old
 * session is 180 s old; after that they are not. A relay between the two
 * namespaces holds two such packets back, and releases one (twice) after the
 * rekey and the other after the old session has expired. Two network
 * namespaces joined by a veth, as in t_wgtimer. This case waits for real time
 * to pass: it takes about four minutes. */
#define _GNU_SOURCE
#include "compat.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <sched.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>

#define NETLINK_ROUTE 0
#define NETLINK_GENERIC 16
#define NLM_F_REQUEST 1
#define NLM_F_ACK 4
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLM_F_DUMP 0x300
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLA_F_NESTED 0x8000
#define RTM_NEWLINK 16
#define RTM_DELLINK 17
#define IFLA_IFNAME 3
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define IFLA_NET_NS_PID 19
#define IFLA_INFO_DATA 2
#define VETH_INFO_PEER 1
#define IFA_LOCAL 2
#define IFA_ADDRESS 1
#define IFF_UP 1
#define IFLA_LINKINFO 18
#define IFLA_INFO_KIND 1
#define GENL_ID_CTRL 16
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2
#define WG_CMD_GET_DEVICE 0
#define WG_CMD_SET_DEVICE 1
#define DEV_IFNAME 2
#define DEV_PRIVATE_KEY 3
#define DEV_PUBLIC_KEY 4
#define DEV_FLAGS 5
#define DEV_LISTEN_PORT 6
#define DEV_FWMARK 7
#define DEV_PEERS 8
#define PEER_PUBLIC_KEY 1
#define PEER_PRESHARED_KEY 2
#define PEER_FLAGS 3
#define PEER_ENDPOINT 4
#define PEER_KEEPALIVE 5
#define PEER_LAST_HS 6
#define PEER_ALLOWEDIPS 9
#define AIP_FAMILY 1
#define AIP_IPADDR 2
#define AIP_CIDR 3
#define F_REPLACE_PEERS 1
#define PF_REMOVE_ME 1
#define PF_REPLACE_ALLOWEDIPS 2

struct nlh { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct nla { uint16_t len, type; };
struct ifinfo { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };

static char buf[4096];
static struct nlh *h = (void *)buf;
static char ans[16384];
static int anslen;

static void start(int type, int flags, int cmd, int genl) {
    memset(buf, 0, sizeof buf);
    h->type = type;
    h->flags = flags | NLM_F_REQUEST;
    h->seq = 5;
    if (genl) { buf[16] = cmd; buf[17] = 1; h->len = 20; }
    else h->len = 16 + sizeof(struct ifinfo);
}

static struct nla *put(int type, const void *d, int len) {
    struct nla *a = (void *)(buf + ((h->len + 3) & ~3u));
    a->type = type;
    a->len = sizeof *a + len;
    if (len) memcpy((char *)(a + 1), d, len);
    h->len = ((h->len + 3) & ~3u) + ((a->len + 3) & ~3u);
    return a;
}
static struct nla *nest(int type) { return put(type | NLA_F_NESTED, 0, 0); }
static void close_nla(struct nla *a) { a->len = buf + h->len - (char *)a; }

/* Sends buf; collects the answers until an error, done or (no dump) one
 * reply; returns the error number (positive) or 0. */
static int talk(int s, int dump) {
    if (send(s, buf, h->len, 0) != (ssize_t)h->len) return -1;
    anslen = 0;
    for (;;) {
        char rb[8192];
        struct pollfd pf = {s, POLLIN, 0};
        if (poll(&pf, 1, 1500) != 1) return 1000;
        ssize_t n = recv(s, rb, sizeof rb, 0);
        if (n <= 0) return 1000;
        if (anslen + n <= (int)sizeof ans) { memcpy(ans + anslen, rb, n); anslen += n; }
        for (char *p = rb; p < rb + n;) {
            struct nlh *r = (void *)p;
            if (r->len < 16) return 1001;
            if (r->type == NLMSG_ERROR) return -*(int *)(p + 16);
            if (r->type == NLMSG_DONE) return 0;
            if (!dump) return 0;
            p += (r->len + 3) & ~3u;
        }
    }
}

static int add_link(int s, const char *name) {
    start(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, 0, 0);
    put(IFLA_IFNAME, name, strlen(name) + 1);
    struct nla *li = nest(IFLA_LINKINFO);
    put(IFLA_INFO_KIND, "wireguard", 10);
    close_nla(li);
    return talk(s, 0);
}
static int del_link(int s, const char *name) {
    start(RTM_DELLINK, NLM_F_ACK, 0, 0);
    put(IFLA_IFNAME, name, strlen(name) + 1);
    return talk(s, 0);
}

/* The attribute of type t between from and to in ans, or NULL. */
static struct nla *find(int from, int to, int t) {
    for (int at = from; at + 4 <= to;) {
        struct nla *a = (void *)(ans + at);
        if (a->len < 4 || at + a->len > to) return NULL;
        if ((a->type & 0x3fff) == t) return a;
        at += (a->len + 3) & ~3;
    }
    return NULL;
}
static int off(struct nla *a) { return (char *)a - ans; }

static void mkkey(uint8_t *b, int k) { for (int i = 0; i < 32; i++) b[i] = k * 16 + i + 1; }

static uint32_t ip4(int a, int b, int c, int d) { return a | b << 8 | c << 16 | (uint32_t)d << 24; }

/* A SET_DEVICE for name, left open so the caller adds attributes; send it
 * with talk(g, 0). */
static void set_open(int id, const char *name) {
    start(id, NLM_F_ACK, WG_CMD_SET_DEVICE, 1);
    put(DEV_IFNAME, name, strlen(name) + 1);
}

/* GET as a dump; the device message's attributes start at 20 in ans. */
static int get(int g, int id, const char *name) {
    start(id, NLM_F_DUMP, WG_CMD_GET_DEVICE, 1);
    if (name) put(DEV_IFNAME, name, strlen(name) + 1);
    return talk(g, 1);
}




struct ifaddr { uint8_t family, prefixlen, flags, scope; uint32_t index; };

static void start_body(int type, int flags, const void *body, int blen) {
    memset(buf, 0, sizeof buf);
    h->type = type;
    h->flags = flags | NLM_F_REQUEST;
    h->seq = 5;
    h->len = 16 + blen;
    memcpy(buf + 16, body, blen);
}

static int veth_pair(int s, const char *a, const char *b, pid_t pid) {
    start(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, 0, 0);
    put(IFLA_IFNAME, a, strlen(a) + 1);
    struct nla *li = nest(IFLA_LINKINFO);
    put(IFLA_INFO_KIND, "veth", 5);
    struct nla *d = nest(IFLA_INFO_DATA);
    struct nla *p = nest(VETH_INFO_PEER);
    struct ifinfo pi = {0};
    memcpy(buf + h->len, &pi, sizeof pi);
    h->len += sizeof pi;
    put(IFLA_IFNAME, b, strlen(b) + 1);
    uint32_t v = pid;
    put(IFLA_NET_NS_PID, &v, 4);
    close_nla(p); close_nla(d); close_nla(li);
    return talk(s, 0);
}

/* Address ip/plen on link name, and up. */
static int addr_up(int s, const char *name, uint32_t ip, int plen) {
    struct ifinfo q = {0};
    start_body(RTM_GETLINK, 0, &q, sizeof q);
    put(IFLA_IFNAME, name, strlen(name) + 1);
    if (talk(s, 0) != 0) return -1;
    int index = ((struct ifinfo *)(ans + 16))->index;
    struct ifaddr ad = {.family = 2, .prefixlen = plen, .index = index};
    start_body(RTM_NEWADDR, NLM_F_CREATE | NLM_F_ACK, &ad, sizeof ad);
    put(IFA_LOCAL, &ip, 4);
    put(IFA_ADDRESS, &ip, 4);
    if (talk(s, 0) != 0) return -2;
    struct ifinfo up = {.index = index, .flags = IFF_UP, .change = IFF_UP};
    start_body(RTM_NEWLINK, NLM_F_ACK, &up, sizeof up);
    return talk(s, 0) == 0 ? 0 : -3;
}

static void pubkey(int g, int id, const char *name, uint8_t *out) {
    memset(out, 0, 32);
    if (get(g, id, name) != 0) return;
    struct nla *a = find(20, anslen, DEV_PUBLIC_KEY);
    if (a) memcpy(out, a + 1, 32);
}

/* The first peer of name: rx and tx bytes and the last handshake in sec. */
static int peer_stats(int g, int id, const char *name, uint64_t *rx, uint64_t *tx, uint64_t *hs) {
    if (get(g, id, name) != 0) return -1;
    struct nla *peers = find(20, anslen, DEV_PEERS);
    struct nla *el = peers ? (void *)(ans + off(peers) + 4) : NULL;
    if (!el) return -2;
    struct nla *a = find(off(el) + 4, off(el) + el->len, 7);
    *rx = a ? *(uint64_t *)(a + 1) : 0;
    a = find(off(el) + 4, off(el) + el->len, 8);
    *tx = a ? *(uint64_t *)(a + 1) : 0;
    a = find(off(el) + 4, off(el) + el->len, PEER_LAST_HS);
    *hs = a ? *(uint64_t *)(a + 1) : 0;
    return 0;
}

static int make_dev(int r, int g, int id, const char *name, int k, int port) {
    uint8_t priv[32];
    mkkey(priv, k);
    uint16_t p = port;
    if (add_link(r, name) != 0) return -1;
    set_open(id, name);
    put(DEV_PRIVATE_KEY, priv, 32);
    put(DEV_LISTEN_PORT, &p, 2);
    return talk(g, 0);
}

/* Peer pk of device name: endpoint ip:eport, allowed IP aip/32 (replacing
 * what it had). */
static int peer_cfg(int g, int id, const char *name, const uint8_t *pk, uint32_t eip, int eport, uint32_t aip, int keep) {
    set_open(id, name);
    struct nla *pl = nest(DEV_PEERS);
    struct nla *el = nest(0);
    put(PEER_PUBLIC_KEY, pk, 32);
    int fl = PF_REPLACE_ALLOWEDIPS;
    put(PEER_FLAGS, &fl, 4);
    uint8_t sa[16] = {2, 0, eport >> 8, eport & 255};
    memcpy(sa + 4, &eip, 4);
    put(PEER_ENDPOINT, sa, 16);
    if (keep) { uint16_t ka = keep < 0 ? 0 : keep; put(PEER_KEEPALIVE, &ka, 2); }
    struct nla *al = nest(PEER_ALLOWEDIPS);
    {
        struct nla *e = nest(0);
        uint16_t fam = 2;
        uint8_t c = 32;
        put(AIP_FAMILY, &fam, 2);
        put(AIP_IPADDR, &aip, 4);
        put(AIP_CIDR, &c, 1);
        close_nla(e);
    }
    close_nla(al);
    close_nla(el);
    close_nla(pl);
    return talk(g, 0);
}

static int udp_on(uint32_t ip, int port) {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip};
    if (u < 0 || bind(u, (void *)&a, sizeof a) != 0) return -1;
    return u;
}

static void relay(void) {
    /* Between the parent's device (127.0.0.1:51871) and the child's
     * (192.168.77.2:51872). The control socket (port 51881) takes 'H': hold
     * the next data message from the parent; and '0' or '1': send the first or
     * the second held message on to the child. */
    int u = udp_on(0, 51880);
    int c = udp_on(0, 51881);
    uint8_t held[2][1600];
    int heldlen[2] = {0, 0}, nheld = 0, armed = 0;
    for (;;) {
        struct pollfd pf[2] = {{u, POLLIN, 0}, {c, POLLIN, 0}};
        if (poll(pf, 2, -1) < 1) continue;
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(51872)};
        to.sin_addr.s_addr = ip4(192, 168, 77, 2);
        if (pf[1].revents & POLLIN) {
            char k;
            if (recv(c, &k, 1, 0) == 1) {
                if (k == 'H') armed++;
                if ((k == '0' || k == '1') && heldlen[k - '0']) sendto(u, held[k - '0'], heldlen[k - '0'], 0, (void *)&to, sizeof to);
            }
        }
        if (pf[0].revents & POLLIN) {
            uint8_t m[1600];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(u, m, sizeof m, 0, (void *)&from, &fl);
            if (n <= 0) continue;
            if (from.sin_addr.s_addr == htonl(0x7f000001)) {
                if (m[0] == 4 && n > 32 && armed > 0 && nheld < 2) {
                    memcpy(held[nheld], m, n);
                    heldlen[nheld++] = n;
                    armed--;
                } else {
                    sendto(u, m, n, 0, (void *)&to, sizeof to);
                }
            } else {
                struct sockaddr_in back = {.sin_family = AF_INET, .sin_port = htons(51871)};
                back.sin_addr.s_addr = htonl(0x7f000001);
                sendto(u, m, n, 0, (void *)&back, sizeof back);
            }
        }
    }
}

static int child(int in, int out) {
    /* The other namespace: its end of the veth, a device, an echo server. */
    char x;
    if (unshare(CLONE_NEWNET) != 0) return 1;
    int r = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    int g = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    start(GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY, 1);
    put(CTRL_ATTR_FAMILY_NAME, "wireguard", 10);
    talk(g, 0);
    struct nla *fid = find(20, anslen, CTRL_ATTR_FAMILY_ID);
    int id = fid ? *(uint16_t *)(fid + 1) : 0;
    if (write(out, "r", 1) != 1 || read(in, &x, 1) != 1) return 2;
    int ok = id != 0;
    ok &= addr_up(r, "vtb", ip4(192, 168, 77, 2), 24) == 0;
    ok &= make_dev(r, g, id, "wgt0", 2, 51872) == 0;
    ok &= addr_up(r, "wgt0", ip4(10, 9, 0, 2), 24) == 0;
    uint8_t mine[32], theirs[32];
    pubkey(g, id, "wgt0", mine);
    if (write(out, mine, 32) != 32 || read(in, theirs, 32) != 32) return 3;
    ok &= peer_cfg(g, id, "wgt0", theirs, ip4(192, 168, 77, 1), 51880, ip4(10, 9, 0, 1), 0) == 0;
    int u = udp_on(ip4(10, 9, 0, 2), 7777);
    ok &= u >= 0;
    if (write(out, ok ? "y" : "n", 1) != 1) return 4;
    int count = 0;
    for (;;) {
        struct pollfd pf[2] = {{u, POLLIN, 0}, {in, POLLIN, 0}};
        if (poll(pf, 2, 450000) < 1) return 5;
        if (pf[1].revents & POLLIN) {
            if (read(in, &x, 1) != 1) return 6;
            if (x == 'A') {
                /* the parent may now only use 10.9.0.5 */
                int e = peer_cfg(g, id, "wgt0", theirs, ip4(192, 168, 77, 1), 51880, ip4(10, 9, 0, 5), 0);
                if (write(out, e == 0 ? "y" : "n", 1) != 1) return 7;
            } else {
                struct { int count, pad; uint64_t hs, rx; } st = {count, 0, 0, 0};
               
                uint64_t tx;
                if (peer_stats(g, id, "wgt0", &st.rx, &tx, &st.hs) != 0) return 8;
                if (write(out, &st, sizeof st) != sizeof st) return 9;
                return 0;
            }
        }
        if (pf[0].revents & POLLIN) {
            char m[2048];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(u, m, sizeof m, 0, (void *)&from, &fl);
            if (n > 0) {
                count++;
                sendto(u, m, n, 0, (void *)&from, fl);
            }
        }
    }
}

/* Sends tag number seq (padded to len) and waits ms for a reply; returns
 * the number of replies carrying that tag (0, 1 or more). */
static int ask(int u, int seq, int len, int ms) {
    char m[2048], r[2048];
    memset(m, 'z', sizeof m);
    snprintf(m, 16, "m%d", seq);
    if (send(u, m, len, 0) != len) return -1;
    int got = 0;
    for (int t = 0; t < ms;) {
        struct pollfd pf = {u, POLLIN, 0};
        int w = poll(&pf, 1, 250);
        t += 250;
        if (w == 1) {
            int n = recv(u, r, sizeof r, 0);
            if (n == len && memcmp(r, m, len) == 0) got++;
        }
    }
    return got;
}

int main(void) {
    setup();
    int r = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    int g = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    EXPECT("sockets", r >= 0 && g >= 0);
    if (r < 0 || g < 0) return done();
    start(GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY, 1);
    put(CTRL_ATTR_FAMILY_NAME, "wireguard", 10);
    talk(g, 0);
    struct nla *fid = find(20, anslen, CTRL_ATTR_FAMILY_ID);
    int id = fid ? *(uint16_t *)(fid + 1) : 0;
    if (id == 0) return done();

    int in[2], out[2];
    pid_t c = -1, rl = -1;
    char x;
    EXPECT("a child in a new network namespace", pipe(in) == 0 && pipe(out) == 0 && (c = fork()) >= 0);
    if (c == 0) {
        close(in[1]);
        close(out[0]);
        _exit(child(in[0], out[1]));
    }
    close(in[0]);
    close(out[1]);
    EXPECT("the child is ready", read(out[0], &x, 1) == 1 && x == 'r');
    EXPECT("a veth pair, one end in the child's namespace", veth_pair(r, "vta", "vtb", c) == 0);
    EXPECT("the parent's end has an address", addr_up(r, "vta", ip4(192, 168, 77, 1), 24) == 0);
    EXPECT("a device with an address", make_dev(r, g, id, "wgt0", 1, 51871) == 0 && addr_up(r, "wgt0", ip4(10, 9, 0, 1), 24) == 0);
    EXPECT("go", write(in[1], "g", 1) == 1);
    uint8_t mine[32], theirs[32];
    pubkey(g, id, "wgt0", mine);
    EXPECT("keys are traded", read(out[0], theirs, 32) == 32 && write(in[1], mine, 32) == 32);
    EXPECT("the child is set up", read(out[0], &x, 1) == 1 && x == 'y');
    if ((rl = fork()) == 0) {
        relay();
        _exit(0);
    }
    EXPECT("the peer is set up with the relay as its endpoint",
           peer_cfg(g, id, "wgt0", theirs, ip4(127, 0, 0, 1), 51880, ip4(10, 9, 0, 2), 0) == 0);

    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in srv = {.sin_family = AF_INET, .sin_port = htons(7777), .sin_addr.s_addr = ip4(10, 9, 0, 2)};
    EXPECT("a client socket", u >= 0 && connect(u, (void *)&srv, sizeof srv) == 0);
    int seq = 0, up = 0;
    for (int t = 0; t < 30 && !up; t++) up = ask(u, seq++, 64, 2000) >= 1;
    EXPECT("the tunnel comes up", up);
    uint64_t rx, tx, hs0 = 0, hs1 = 0;
    EXPECT("a last handshake time", peer_stats(g, id, "wgt0", &rx, &tx, &hs0) == 0 && hs0 > 0);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ctl = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in cto = {.sin_family = AF_INET, .sin_port = htons(51881), .sin_addr.s_addr = htonl(0x7f000001)};
    EXPECT("the relay's control socket", ctl >= 0 && connect(ctl, (void *)&cto, sizeof cto) == 0);

    /* 125 s on: two datagrams under the old keys, which the relay holds back; the first
     * one is past the rekey time, so the handshake starts after it */
    for (;;) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= 125) break;
        poll(0, 0, 1000);
    }
    char m[2][64], rb[64];
    memset(m, 'z', sizeof m);
    snprintf(m[0], 16, "old-a");
    snprintf(m[1], 16, "old-b");
    EXPECT("two messages are held", send(ctl, "H", 1, 0) == 1 && send(ctl, "H", 1, 0) == 1 && poll(0, 0, 300) == 0
           && send(u, m[0], 64, 0) == 64 && send(u, m[1], 64, 0) == 64);
    int renewed = 0;
    for (int w = 0; w < 90 && !renewed; w++) {
        peer_stats(g, id, "wgt0", &rx, &tx, &hs1);
        renewed = hs1 > hs0;
        if (!renewed) poll(0, 0, 1000);
    }
    EXPECT("the session is renewed", renewed);
    int echoed[2] = {0, 0};
    EXPECT("nothing came back while they were held", ({ struct pollfd pf = {u, POLLIN, 0}; poll(&pf, 1, 500) == 0; }));
    /* the first is released twice: once is taken, the second time is a replay */
    EXPECT("the first old packet is released twice", send(ctl, "0", 1, 0) == 1 && poll(0, 0, 200) == 0 && send(ctl, "0", 1, 0) == 1);
    for (int t = 0; t < 12000;) {
        struct pollfd pf = {u, POLLIN, 0};
        int w = poll(&pf, 1, 250);
        t += 250;
        if (w == 1) {
            int n = recv(u, rb, sizeof rb, 0);
            if (n == 64 && memcmp(rb, m[0], 64) == 0) echoed[0]++;
        }
        if (t >= 6000 && echoed[0]) break;
    }
    EXPECT("a packet sent under the old keys is still taken after the rekey, and echoed once", echoed[0] == 1);
    EXPECT("the tunnel carries new traffic", ask(u, seq++, 64, 3000) >= 1);

    /* the old session is 180 s old: its last packet is too late */
    for (;;) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= 192) break;
        poll(0, 0, 1000);
    }
    EXPECT("the second old packet is released late", send(ctl, "1", 1, 0) == 1);
    for (int t = 0; t < 4000;) {
        struct pollfd pf = {u, POLLIN, 0};
        int w = poll(&pf, 1, 250);
        t += 250;
        if (w == 1) {
            int n = recv(u, rb, sizeof rb, 0);
            if (n == 64 && memcmp(rb, m[1], 64) == 0) echoed[1]++;
        }
    }
    EXPECT("it is not taken once the old session is 180 s old", echoed[1] == 0);
    EXPECT("the tunnel still works", ask(u, seq++, 64, 3000) >= 1);

    EXPECT("the child stops", write(in[1], "Q", 1) == 1);
    kill(rl, 9);
    waitpid(rl, 0, 0);
    waitpid(c, 0, 0);
    del_link(r, "wgt0");
    close(u);
    close(r);
    close(g);
    return done();
}
