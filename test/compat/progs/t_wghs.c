/* WireGuard's handshake (RFC-less: the Noise IKpsk2 exchange of the white
 * paper) between devices of this machine, and what a device does with
 * messages that are not a fresh, valid initiation or the answer to one it
 * sent. A plain UDP socket plays the part of a peer that is read from, so a
 * recorded initiation can be played back; the messages are checked for the
 * sizes and fields the protocol fixes. Constants are spelled out: the C
 * library lacks linux/wireguard.h. */
#define _GNU_SOURCE
#include "compat.h"
#include <arpa/inet.h>
#include <netinet/in.h>
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



/* The public key of device name (32 bytes), or zeros. */
static void pubkey(int g, int id, const char *name, uint8_t *out) {
    memset(out, 0, 32);
    if (get(g, id, name) != 0) return;
    struct nla *a = find(20, anslen, DEV_PUBLIC_KEY);
    if (a) memcpy(out, a + 1, 32);
}

/* Seconds of the last handshake of the first peer of name, or -1. */
static long last_hs(int g, int id, const char *name) {
    if (get(g, id, name) != 0) return -1;
    struct nla *peers = find(20, anslen, DEV_PEERS);
    struct nla *el = peers ? (void *)(ans + off(peers) + 4) : NULL;
    struct nla *a = el ? find(off(el) + 4, off(el) + el->len, PEER_LAST_HS) : NULL;
    return a ? (long)*(uint64_t *)(a + 1) : -1;
}

/* A device made with a key and a port; peers are added by peer_to. */
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

/* Gives device name a peer with public key pk, pre-shared key number psk
 * (0: none), an endpoint 127.0.0.1:eport (0: none) and a keep-alive. */
static int peer_to(int g, int id, const char *name, const uint8_t *pk, int psk, int eport, int keep) {
    set_open(id, name);
    struct nla *pl = nest(DEV_PEERS);
    struct nla *el = nest(0);
    put(PEER_PUBLIC_KEY, pk, 32);
    if (psk) {
        uint8_t k[32];
        mkkey(k, psk);
        put(PEER_PRESHARED_KEY, k, 32);
    }
    if (eport) {
        uint8_t sa[16] = {2, 0, eport >> 8, eport & 255, 127, 0, 0, 1};
        put(PEER_ENDPOINT, sa, 16);
    }
    if (keep) {
        uint16_t ka = keep;
        put(PEER_KEEPALIVE, &ka, 2);
    }
    close_nla(el);
    close_nla(pl);
    return talk(g, 0);
}

static int udp_on(int port) {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(0x7f000001)};
    if (u < 0 || bind(u, (void *)&a, sizeof a) != 0) return -1;
    return u;
}

static int send_to(int u, int port, const void *m, int len) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(0x7f000001)};
    return sendto(u, m, len, 0, (void *)&a, sizeof a) == len ? 0 : -1;
}

/* A datagram from u within ms, its length or 0. */
static int get_dgram(int u, void *m, int max, int ms) {
    struct pollfd pf = {u, POLLIN, 0};
    if (poll(&pf, 1, ms) != 1) return 0;
    int n = recv(u, m, max, 0);
    return n > 0 ? n : 0;
}

/* Waits up to ms for the first peer of name to show a handshake. */
static int hs_within(int g, int id, const char *name, int ms) {
    for (int t = 0; t < ms; t += 100) {
        if (last_hs(g, id, name) > 0) return 1;
        poll(NULL, 0, 100);
    }
    return 0;
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

    /* Part 1: wga initiates towards a plain socket that records the message. */
    uint8_t pka[32], pkb[32];
    EXPECT("devices are made", make_dev(r, g, id, "wga", 1, 51831) == 0 && make_dev(r, g, id, "wgb", 2, 51832) == 0);
    pubkey(g, id, "wga", pka);
    pubkey(g, id, "wgb", pkb);
    int cap = udp_on(51840);
    EXPECT("a capture socket", cap >= 0);
    EXPECT("wgb knows wga", peer_to(g, id, "wgb", pka, 9, 0, 0) == 0);
    EXPECT("a peer without a keep-alive starts nothing", get_dgram(cap, ans, 64, 300) == 0);
    EXPECT("wga's peer points at the capture socket", peer_to(g, id, "wga", pkb, 9, 51840, 25) == 0);
    uint8_t init[256];
    int il = get_dgram(cap, init, sizeof init, 8000);
    EXPECT("an initiation is sent: 148 bytes, type 1", il == 148 && init[0] == 1 && init[1] == 0 && init[2] == 0 && init[3] == 0);
    int zero2 = 1, zero1 = 1;
    for (int i = 0; i < 16; i++) { if (init[132 + i]) zero2 = 0; if (init[116 + i]) zero1 = 0; }
    EXPECT("mac1 is set and mac2 is zero (no cookie yet)", il == 148 && !zero1 && zero2);
    EXPECT("wga has no handshake yet", last_hs(g, id, "wga") == 0);

    uint8_t reply[256];
    uint8_t bad[256];
    send_to(cap, 51832, init, 148);
    int rl = get_dgram(cap, reply, sizeof reply, 8000);
    EXPECT("the real initiation is answered: 92 bytes, type 2", rl == 92 && reply[0] == 2 && reply[1] == 0 && reply[2] == 0 && reply[3] == 0);
    EXPECT("the answer is addressed to the initiator's index", rl == 92 && memcmp(reply + 8, init + 4, 4) == 0);
    memcpy(bad, reply, 92);
    bad[60] ^= 1;
    send_to(cap, 51831, bad, 92);
    EXPECT("a response with a bad mac1 is dropped", !hs_within(g, id, "wga", 500));
    memcpy(bad, reply, 92);
    bad[8] ^= 1;
    send_to(cap, 51831, bad, 92);
    EXPECT("a response for another index is dropped", !hs_within(g, id, "wga", 500));
    send_to(cap, 51831, reply, 92);
    EXPECT("the response completes the handshake", hs_within(g, id, "wga", 12000));
    long first = last_hs(g, id, "wga");
    send_to(cap, 51831, reply, 92);
    poll(NULL, 0, 500);
    EXPECT("the same response again changes nothing", last_hs(g, id, "wga") == first);
    EXPECT("the responder shows no handshake before a packet arrives", last_hs(g, id, "wgb") == 0);
    {
        get(g, id, "wgb");
        struct nla *peers = find(20, anslen, DEV_PEERS);
        struct nla *el = peers ? (void *)(ans + off(peers) + 4) : NULL;
        struct nla *ep = el ? find(off(el) + 4, off(el) + el->len, PEER_ENDPOINT) : NULL;
        uint8_t sa[8] = {2, 0, 51840 >> 8, 51840 & 255, 127, 0, 0, 1};
        EXPECT("the responder learned the initiator's address", ep && memcmp(ep + 1, sa, 8) == 0);
    }

    memcpy(bad, init, 148);
    bad[140 - 20] ^= 1;                       /* a byte of mac1 */
    send_to(cap, 51832, bad, 148);
    EXPECT("an initiation with a bad mac1 gets no answer", get_dgram(cap, reply, sizeof reply, 250) == 0);
    memcpy(bad, init, 148);
    bad[0] = 3;
    send_to(cap, 51832, bad, 148);
    EXPECT("a message of an unknown type gets none", get_dgram(cap, reply, sizeof reply, 250) == 0);
    send_to(cap, 51832, init, 147);
    EXPECT("a short initiation gets none", get_dgram(cap, reply, sizeof reply, 250) == 0);
    send_to(cap, 51832, init, 148);
    EXPECT("the same initiation again is not answered (replay)", get_dgram(cap, ans, 128, 400) == 0);


    /* Part 2: two devices talk to each other; a third does not belong. */
    EXPECT("more devices", make_dev(r, g, id, "wgc", 3, 51833) == 0 && make_dev(r, g, id, "wgd", 4, 51834) == 0 &&
                               make_dev(r, g, id, "wge", 5, 51835) == 0);
    uint8_t pkc[32], pkd[32], pke[32];
    pubkey(g, id, "wgc", pkc);
    pubkey(g, id, "wgd", pkd);
    pubkey(g, id, "wge", pke);
    EXPECT("wgd knows wgc", peer_to(g, id, "wgd", pkc, 0, 0, 0) == 0);
    EXPECT("wgc starts a handshake with wgd", peer_to(g, id, "wgc", pkd, 0, 51834, 10) == 0);
    EXPECT("it completes", hs_within(g, id, "wgc", 12000));
    EXPECT("wge, unknown to wgd, gets nowhere", peer_to(g, id, "wge", pkd, 0, 51834, 10) == 0 && !hs_within(g, id, "wge", 1500));
    {
        long before = last_hs(g, id, "wgc");
        poll(NULL, 0, 2100);
        peer_to(g, id, "wgc", pkd, 3, 51834, 11);       /* a pre-shared key wgd does not have */
        poll(NULL, 0, 1500);
        EXPECT("a pre-shared key that differs breaks the handshake", last_hs(g, id, "wgc") == before);
    }

    const char *names[] = {"wga", "wgb", "wgc", "wgd", "wge"};
    for (int i = 0; i < 5; i++) del_link(r, names[i]);
    close(cap);
    close(r);
    close(g);
    return done();
}
