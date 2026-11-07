/* compat-timeout: 240
 * WireGuard cookies between two network namespaces joined by a veth pair. A
 * flood of initiations with a good mac1 (made here with a small BLAKE2s) and
 * a body that is garbage reaches the child's device, which turns busy and
 * answers with cookie replies (type 3, 64 bytes, the receiver index of the
 * initiation) instead of doing the handshake work; an initiation with a bad
 * mac1 gets nothing. While the flood goes on, the parent's own device is
 * given a cookie and sends its initiation again with the mac2, so traffic
 * through the tunnel still gets through. Constants are spelled out: the C
 * library lacks linux/wireguard.h. */
#define _GNU_SOURCE
#include "compat.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <sched.h>
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
    if (keep) { uint16_t ka = keep; put(PEER_KEEPALIVE, &ka, 2); }
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


/* ---- BLAKE2s, to make the mac1 of a forged initiation ---- */
static const uint32_t B2IV[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
static const uint8_t B2S[10][16] = {
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4}, {7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13}, {2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11}, {13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5}, {10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0}};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void b2_compress(uint32_t *hh, const uint8_t *blk, uint64_t t, int last) {
    uint32_t m[16], v[16];
    for (int i = 0; i < 16; i++) m[i] = blk[4*i] | blk[4*i+1] << 8 | blk[4*i+2] << 16 | (uint32_t)blk[4*i+3] << 24;
    for (int i = 0; i < 8; i++) { v[i] = hh[i]; v[i+8] = B2IV[i]; }
    v[12] ^= (uint32_t)t;
    v[13] ^= (uint32_t)(t >> 32);
    if (last) v[14] = ~v[14];
#define G(a, b, c, d, x, y) \
    v[a] += v[b] + (x); v[d] = ROR(v[d] ^ v[a], 16); v[c] += v[d]; v[b] = ROR(v[b] ^ v[c], 12); \
    v[a] += v[b] + (y); v[d] = ROR(v[d] ^ v[a], 8);  v[c] += v[d]; v[b] = ROR(v[b] ^ v[c], 7);
    for (int r = 0; r < 10; r++) {
        const uint8_t *s = B2S[r];
        G(0, 4, 8, 12, m[s[0]], m[s[1]]) G(1, 5, 9, 13, m[s[2]], m[s[3]])
        G(2, 6, 10, 14, m[s[4]], m[s[5]]) G(3, 7, 11, 15, m[s[6]], m[s[7]])
        G(0, 5, 10, 15, m[s[8]], m[s[9]]) G(1, 6, 11, 12, m[s[10]], m[s[11]])
        G(2, 7, 8, 13, m[s[12]], m[s[13]]) G(3, 4, 9, 14, m[s[14]], m[s[15]])
    }
    for (int i = 0; i < 8; i++) hh[i] ^= v[i] ^ v[i+8];
}

/* BLAKE2s of in[0..n) with a key of kl bytes (0 for none), outl bytes out. */
static void blake2s(uint8_t *out, int outl, const uint8_t *key, int kl, const uint8_t *in, int n) {
    uint32_t hh[8];
    uint8_t blk[64];
    for (int i = 0; i < 8; i++) hh[i] = B2IV[i];
    hh[0] ^= 0x01010000 ^ (kl << 8) ^ outl;
    uint64_t t = 0;
    if (kl) {
        memset(blk, 0, 64);
        memcpy(blk, key, kl);
        if (n == 0) { t = 64; b2_compress(hh, blk, t, 1); goto done; }
        t = 64;
        b2_compress(hh, blk, t, 0);
    }
    while (n > 64) {
        t += 64;
        b2_compress(hh, in, t, 0);
        in += 64;
        n -= 64;
    }
    memset(blk, 0, 64);
    memcpy(blk, in, n);
    t += n;
    b2_compress(hh, blk, t, 1);
done:
    for (int i = 0; i < outl; i++) out[i] = hh[i / 4] >> (8 * (i % 4));
}

/* BLAKE2s-256 of a label (8 bytes) and a key (32 bytes): the key of a mac1. */
static void mac1_key(uint8_t *k, const uint8_t *pub) {
    uint8_t b[40];
    memcpy(b, "mac1----", 8);
    memcpy(b + 8, pub, 32);
    blake2s(k, 32, 0, 0, b, 40);
}

/* The initiation with sender index idx and a garbage body; good selects
 * whether the mac1 is right for the device whose public key is pub. */
static void forge(uint8_t *m, uint32_t idx, const uint8_t *pub, int good, unsigned salt) {
    uint8_t k[32];
    memset(m, 0, 148);
    m[0] = 1;
    memcpy(m + 4, &idx, 4);
    for (int i = 8; i < 116; i++) m[i] = (i * 7 + salt * 13 + idx) & 255;
    mac1_key(k, pub);
    blake2s(m + 116, 16, k, 32, m, 116);
    if (!good) m[116] ^= 1;
}

static int flood_ctl = -1, flood_out = -1;
static void flooder(const uint8_t *pub) {
    /* From 192.168.77.1:40000: bursts of 12 initiations with a good mac1
     * every 400 ms. Counts the cookie replies and whether each is 64 bytes,
     * type 3 and carries the index of an initiation of the burst. On Q it
     * sends one with a bad mac1, waits for a reply to it, and reports. */
    int u = udp_on(ip4(192, 168, 77, 1), 40000);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(51872)};
    to.sin_addr.s_addr = ip4(192, 168, 77, 2);
    struct { int sent, replies, wrong, afterbad; } st = {0, 0, 0, 0};
    uint32_t base = 0x70000000;
    int quit = 0;
    while (!quit) {
        for (int k = 0; k < 12; k++) {
            uint8_t m[148];
            forge(m, base + k, pub, 1, st.sent);
            sendto(u, m, 148, 0, (void *)&to, sizeof to);
            st.sent++;
        }
        for (int t = 0; t < 400 && !quit;) {
            struct pollfd pf[2] = {{u, POLLIN, 0}, {flood_ctl, POLLIN, 0}};
            int w = poll(pf, 2, 50);
            t += 50;
            if (w > 0 && (pf[1].revents & POLLIN)) quit = 1;
            if (w > 0 && (pf[0].revents & POLLIN)) {
                uint8_t r[256];
                int n = recv(u, r, sizeof r, 0);
                uint32_t idx;
                if (n > 8) memcpy(&idx, r + 4, 4);
                if (n == 64 && r[0] == 3 && r[1] == 0 && r[2] == 0 && r[3] == 0 && idx >= base && idx < base + 12) st.replies++;
                else st.wrong++;
            }
        }
        base += 16;
    }
    {
        uint8_t m[148], r[256];
        forge(m, 0x71000000, pub, 0, 1);
        sendto(u, m, 148, 0, (void *)&to, sizeof to);
        struct pollfd pf = {u, POLLIN, 0};
        if (poll(&pf, 1, 700) == 1 && recv(u, r, sizeof r, 0) > 0) st.afterbad++;
    }
    write(flood_out, &st, sizeof st);
}

static int child(int in, int out) {
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
    ok &= peer_cfg(g, id, "wgt0", theirs, ip4(192, 168, 77, 1), 51871, ip4(10, 9, 0, 1), 0) == 0;
    int u = udp_on(ip4(10, 9, 0, 2), 7777);
    ok &= u >= 0;
    if (write(out, ok ? "y" : "n", 1) != 1) return 4;
    int count = 0;
    for (;;) {
        struct pollfd pf[2] = {{u, POLLIN, 0}, {in, POLLIN, 0}};
        if (poll(pf, 2, 150000) < 1) return 5;
        if (pf[1].revents & POLLIN) {
            if (read(in, &x, 1) != 1) return 6;
            struct { int count, pad; uint64_t hs, rx; } st = {count, 0, 0, 0};
            uint64_t tx;
            if (peer_stats(g, id, "wgt0", &st.rx, &tx, &st.hs) != 0) return 8;
            if (write(out, &st, sizeof st) != sizeof st) return 9;
            return 0;
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
    {
        /* RFC 7693 appendix E: BLAKE2s-256("abc") */
        uint8_t d[32], want[4] = {0x50, 0x8c, 0x5e, 0x8c};
        blake2s(d, 32, 0, 0, (const uint8_t *)"abc", 3);
        EXPECT("the BLAKE2s used here gives the known digest of abc", memcmp(d, want, 4) == 0 && d[31] == 0x82);
    }
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
    pid_t c = -1, fl = -1;
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
    EXPECT("the peer is set up, the child's endpoint direct",
           peer_cfg(g, id, "wgt0", theirs, ip4(192, 168, 77, 2), 51872, ip4(10, 9, 0, 2), 0) == 0);

    int cp[2], fo[2];
    if (pipe(cp) != 0 || pipe(fo) != 0) return 1;
    if ((fl = fork()) == 0) {
        flood_ctl = cp[0];
        flood_out = fo[1];
        flooder(theirs);
        _exit(0);
    }
    usleep(1500000);   /* let the child see more than a second of flood */

    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in srv = {.sin_family = AF_INET, .sin_port = htons(7777), .sin_addr.s_addr = ip4(10, 9, 0, 2)};
    EXPECT("a client socket", u >= 0 && connect(u, (void *)&srv, sizeof srv) == 0);
    int seq = 0, up = 0;
    for (int t = 0; t < 120 && !up; t++) up = ask(u, seq++, 64, 2000) >= 1;
    EXPECT("the handshake gets through the flood, by cookie", up);
    int all = 1, twice = 0;
    for (int i = 0; i < 5; i++) {
        /* a packet the busy device is slow with is sent again, under a new
         * tag so a late echo of the first is not mistaken for the second */
        int got = 0;
        for (int t = 0; t < 10 && !got; t++) {
            int n = ask(u, seq++, 100 + i * 200, 4000);
            if (n > 1) twice = 1;
            got = n == 1;
        }
        if (!got) all = 0;
    }
    EXPECT("five more are echoed while it goes on", all);
    EXPECT("and none of them twice", !twice);
    {
        uint64_t rx, tx, hs;
        int e = peer_stats(g, id, "wgt0", &rx, &tx, &hs);
        EXPECT("the peer shows traffic both ways and a handshake", e == 0 && rx > 0 && tx > 0 && hs > 0);
    }
    EXPECT("the flooder stops", write(cp[1], "Q", 1) == 1);
    struct { int sent, replies, wrong, afterbad; } fs;
    EXPECT("the flooder reports", read(fo[0], &fs, sizeof fs) == sizeof fs);
    EXPECT("the flood was sent", fs.sent >= 36);
    EXPECT("the busy device answered with cookie replies", fs.replies >= 12);
    EXPECT("each reply is 64 bytes, type 3 and has the index of an initiation", fs.wrong == 0);
    EXPECT("fewer replies than initiations: the first ones were taken before it was busy", fs.replies < fs.sent);
    EXPECT("an initiation with a bad mac1 gets no reply", fs.afterbad == 0);
    EXPECT("the child stops", write(in[1], "Q", 1) == 1);
    struct { int count, pad; uint64_t hs, rx; } st;
    EXPECT("the child reports", read(out[0], &st, sizeof st) == sizeof st);
    EXPECT("the child took at least six packets and no more than were sent", st.count >= 6 && st.count <= seq);
    EXPECT("its last handshake time is set", st.hs > 0 && st.rx > 0);

    waitpid(fl, 0, 0);
    waitpid(c, 0, 0);
    del_link(r, "wgt0");
    close(u);
    close(r);
    close(g);
    return done();
}
