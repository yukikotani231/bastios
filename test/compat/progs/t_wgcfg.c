/* Configuring a WireGuard device the way `wg set` and `wg show` do: the
 * generic netlink family "wireguard" (its number found through nlctrl),
 * SET_DEVICE with keys, a listen port, peers and allowed IPs, and GET_DEVICE
 * as a dump. The public key is checked against RFC 7748's X25519 vector.
 * Constants are spelled out: the C library lacks linux/wireguard.h. */
#define _GNU_SOURCE
#include "compat.h"
#include <poll.h>
#include <stdint.h>
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

static const uint8_t rfc_priv[32] = {
    0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
    0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a};
static const uint8_t rfc_pub[32] = {
    0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
    0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a};

static uint8_t key(int k) {
    static uint8_t b[32];
    for (int i = 0; i < 32; i++) b[i] = k * 16 + i;
    return b[0];
}
static void mkkey(uint8_t *b, int k) { for (int i = 0; i < 32; i++) b[i] = k * 16 + i + 1; }

static void aip(uint32_t ip, int plen) {
    struct nla *e = nest(0);
    uint16_t fam = 2;
    uint8_t c = plen;
    put(AIP_FAMILY, &fam, 2);
    put(AIP_IPADDR, &ip, 4);
    put(AIP_CIDR, &c, 1);
    close_nla(e);
}

static uint32_t ip4(int a, int b, int c, int d) { return a | b << 8 | c << 16 | (uint32_t)d << 24; }

/* A SET_DEVICE for name, left open so the caller adds attributes; send it
 * with talk(g, 0). */
static void set_open(int id, const char *name) {
    start(id, NLM_F_ACK, WG_CMD_SET_DEVICE, 1);
    put(DEV_IFNAME, name, strlen(name) + 1);
}

/* One peer {key k, allowed IPs} in a peers list. */
static void peer_one(int k, int flags, int nips, const uint32_t *ips, const int *plens) {
    uint8_t pk[32];
    mkkey(pk, k);
    struct nla *pl = nest(DEV_PEERS);
    struct nla *el = nest(0);
    put(PEER_PUBLIC_KEY, pk, 32);
    if (flags) put(PEER_FLAGS, &flags, 4);
    if (nips) {
        struct nla *al = nest(PEER_ALLOWEDIPS);
        for (int i = 0; i < nips; i++) aip(ips[i], plens[i]);
        close_nla(al);
    }
    close_nla(el);
    close_nla(pl);
}

/* GET as a dump; the device message's attributes start at 20 in ans. */
static int get(int g, int id, const char *name) {
    start(id, NLM_F_DUMP, WG_CMD_GET_DEVICE, 1);
    if (name) put(DEV_IFNAME, name, strlen(name) + 1);
    return talk(g, 1);
}

/* How many elements the nested list a holds. */
static int count(struct nla *a) {
    int n = 0;
    if (!a) return -1;
    for (int at = off(a) + 4; at + 4 <= off(a) + a->len;) {
        struct nla *e = (void *)(ans + at);
        if (e->len < 4) break;
        n++;
        at += (e->len + 3) & ~3;
    }
    return n;
}

int main(void) {
    setup();
    int r = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    int g = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    EXPECT("sockets", r >= 0 && g >= 0);
    if (r < 0 || g < 0) return done();
    (void)key;

    start(GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY, 1);
    put(CTRL_ATTR_FAMILY_NAME, "wireguard", 10);
    int e = talk(g, 0);
    struct nla *fid = find(20, anslen, CTRL_ATTR_FAMILY_ID);
    int id = fid ? *(uint16_t *)(fid + 1) : 0;
    EXPECT("nlctrl knows the wireguard family", e == 0 && id >= 17);
    if (id == 0) return done();

    EXPECT("a device is made", add_link(r, "wgc0") == 0);
    set_open(id, "wgc0");
    put(DEV_PRIVATE_KEY, rfc_priv, 32);
    uint16_t port = 51820;
    uint32_t mark = 7;
    put(DEV_LISTEN_PORT, &port, 2);
    put(DEV_FWMARK, &mark, 4);
    {
        uint8_t pk[32], psk[32];
        mkkey(pk, 1);
        mkkey(psk, 9);
        struct nla *pl = nest(DEV_PEERS);
        struct nla *el = nest(0);
        put(PEER_PUBLIC_KEY, pk, 32);
        put(PEER_PRESHARED_KEY, psk, 32);
        uint8_t sa[16] = {2, 0, 0xca, 0x6d, 192, 0, 2, 1};  /* 192.0.2.1:51821 */
        put(PEER_ENDPOINT, sa, 16);
        uint16_t ka = 25;
        put(PEER_KEEPALIVE, &ka, 2);
        struct nla *al = nest(PEER_ALLOWEDIPS);
        aip(ip4(10, 0, 0, 0), 24);
        aip(ip4(10, 0, 1, 5), 32);
        close_nla(al);
        close_nla(el);
        close_nla(pl);
    }
    EXPECT("SET_DEVICE with a key, a port and a peer", talk(g, 0) == 0);

    e = get(g, id, "wgc0");
    struct nla *pub = e == 0 ? find(20, anslen, DEV_PUBLIC_KEY) : NULL;
    struct nla *prv = e == 0 ? find(20, anslen, DEV_PRIVATE_KEY) : NULL;
    EXPECT("the dump shows the device", e == 0 && anslen > 20 && find(20, anslen, DEV_IFNAME) != NULL);
    EXPECT("the public key is X25519 of the private key (RFC 7748)", pub && memcmp(pub + 1, rfc_pub, 32) == 0);
    EXPECT("the private key is shown clamped",
           prv && ((uint8_t *)(prv + 1))[0] == (0x77 & 248) && ((uint8_t *)(prv + 1))[31] == ((0x2a & 127) | 64) &&
               memcmp((uint8_t *)(prv + 1) + 1, rfc_priv + 1, 30) == 0);
    {
        struct nla *a = find(20, anslen, DEV_LISTEN_PORT);
        EXPECT("the listen port", a && *(uint16_t *)(a + 1) == 51820);
        a = find(20, anslen, DEV_FWMARK);
        EXPECT("the firewall mark", a && *(uint32_t *)(a + 1) == 7);
    }
    struct nla *peers = find(20, anslen, DEV_PEERS);
    EXPECT("one peer", count(peers) == 1);
    struct nla *el = peers ? (void *)(ans + off(peers) + 4) : NULL;
    int ef = el ? off(el) + 4 : 0, et = el ? off(el) + el->len : 0;
    {
        uint8_t pk[32];
        mkkey(pk, 1);
        struct nla *a = el ? find(ef, et, PEER_PUBLIC_KEY) : NULL;
        EXPECT("the peer's public key", a && memcmp(a + 1, pk, 32) == 0);
        mkkey(pk, 9);
        a = el ? find(ef, et, PEER_PRESHARED_KEY) : NULL;
        EXPECT("the peer's pre-shared key", a && memcmp(a + 1, pk, 32) == 0);
        a = el ? find(ef, et, PEER_ENDPOINT) : NULL;
        uint8_t sa[16] = {2, 0, 0xca, 0x6d, 192, 0, 2, 1};
        EXPECT("the peer's endpoint", a && memcmp(a + 1, sa, 8) == 0);
        a = el ? find(ef, et, PEER_KEEPALIVE) : NULL;
        EXPECT("the keep-alive interval", a && *(uint16_t *)(a + 1) == 25);
        a = el ? find(ef, et, PEER_ALLOWEDIPS) : NULL;
        EXPECT("two allowed IPs", count(a) == 2);
        struct nla *first = a ? (void *)(ans + off(a) + 4) : NULL;
        struct nla *ipa = first ? find(off(first) + 4, off(first) + first->len, AIP_IPADDR) : NULL;
        struct nla *cid = first ? find(off(first) + 4, off(first) + first->len, AIP_CIDR) : NULL;
        EXPECT("the first is 10.0.0.0/24", ipa && cid && *(uint32_t *)(ipa + 1) == ip4(10, 0, 0, 0) && *(uint8_t *)(cid + 1) == 24);
    }

    /* A second peer that claims 10.0.0.0/24 takes it from the first. */
    set_open(id, "wgc0");
    {
        uint32_t ips[] = {ip4(10, 0, 0, 77), ip4(10, 9, 0, 0)};
        int pl[] = {24, 16};
        peer_one(2, 0, 2, ips, pl);
    }
    EXPECT("a second peer is added", talk(g, 0) == 0);
    e = get(g, id, "wgc0");
    peers = find(20, anslen, DEV_PEERS);
    EXPECT("two peers", e == 0 && count(peers) == 2);
    {
        struct nla *e1 = peers ? (void *)(ans + off(peers) + 4) : NULL;
        struct nla *al = e1 ? find(off(e1) + 4, off(e1) + e1->len, PEER_ALLOWEDIPS) : NULL;
        EXPECT("the first peer lost the network it shared (the address is masked)", count(al) == 1);
    }

    /* A key that is not 32 bytes, a bad prefix, another family. */
    set_open(id, "wgc0");
    put(DEV_PRIVATE_KEY, rfc_priv, 31);
    EXPECT("a short private key is EINVAL", talk(g, 0) == 22);
    set_open(id, "wgc0");
    {
        uint32_t ips[] = {ip4(1, 2, 3, 4)};
        int pl[] = {33};
        peer_one(3, 0, 1, ips, pl);
    }
    EXPECT("a /33 allowed IP is EINVAL", talk(g, 0) == 22);
    set_open(id, "wgc0");
    {
        uint8_t pk[32];
        mkkey(pk, 4);
        struct nla *pl = nest(DEV_PEERS);
        struct nla *el2 = nest(0);
        put(PEER_PUBLIC_KEY, pk, 32);
        struct nla *al = nest(PEER_ALLOWEDIPS);
        struct nla *a1 = nest(0);
        uint16_t fam = 10;
        uint8_t v6[16] = {0xfd}, c = 64;
        put(AIP_FAMILY, &fam, 2);
        put(AIP_IPADDR, v6, 16);
        put(AIP_CIDR, &c, 1);
        close_nla(a1);
        close_nla(al);
        close_nla(el2);
        close_nla(pl);
    }
    EXPECT("an IPv6 allowed IP is EAFNOSUPPORT", talk(g, 0) == 97);
    set_open(id, "wgc0");
    {
        uint32_t ips[10];
        int pl[10];
        for (int i = 0; i < 10; i++) { ips[i] = ip4(10, 20, i, 0); pl[i] = 24; }
        peer_one(5, 0, 10, ips, pl);
    }
    EXPECT("more allowed IPs than a peer holds is ENOSPC", talk(g, 0) == 28);

    /* Removing a peer, replacing them all. */
    set_open(id, "wgc0");
    peer_one(2, PF_REMOVE_ME, 0, NULL, NULL);
    EXPECT("a peer is removed", talk(g, 0) == 0);
    e = get(g, id, "wgc0");
    EXPECT("the others stay", e == 0 && count(find(20, anslen, DEV_PEERS)) >= 1);
    set_open(id, "wgc0");
    {
        uint32_t f = F_REPLACE_PEERS;
        put(DEV_FLAGS, &f, 4);
    }
    EXPECT("REPLACE_PEERS", talk(g, 0) == 0);
    e = get(g, id, "wgc0");
    EXPECT("leaves none", e == 0 && count(find(20, anslen, DEV_PEERS)) == 0);

    /* Who and what can be asked. */
    set_open(id, "nosuchwg0");
    EXPECT("an unknown device is ENODEV", talk(g, 0) == 19);
    EXPECT("a dump without a device is EBADR", get(g, id, NULL) == 53);
    start(id, 0, WG_CMD_GET_DEVICE, 1);
    put(DEV_IFNAME, "wgc0", 5);
    EXPECT("GET without a dump is EOPNOTSUPP", talk(g, 0) == 95);
    start(id, NLM_F_ACK, 77, 1);
    EXPECT("an unknown command is EOPNOTSUPP", talk(g, 0) == 95);
    EXPECT("an ordinary link is not a wireguard device", get(g, id, "lo") == 19);

    /* A device made again starts empty. */
    EXPECT("the device is deleted", del_link(r, "wgc0") == 0);
    EXPECT("a new one is made", add_link(r, "wgc0") == 0);
    e = get(g, id, "wgc0");
    EXPECT("it holds no key and no peers",
           e == 0 && find(20, anslen, DEV_PRIVATE_KEY) == NULL && count(find(20, anslen, DEV_PEERS)) == 0);
    del_link(r, "wgc0");
    close(r);
    close(g);
    return done();
}
