/* IPsec ESP with extended (64-bit) sequence numbers, between two network
 * namespaces joined by a veth pair. An association made with XFRM_STATE_ESN
 * and an XFRMA_REPLAY_ESN_VAL carries the sequence number's high half only in
 * the associated data (RFC 4106, section 5), so: the sender goes through the
 * 2^32 wrap (the number is reported by GETSA), a packet whose low half is
 * above or below the wrap is taken under the high half the receiver infers
 * (RFC 4303, appendix A.2.2), a replay is refused on either side of the wrap,
 * and a packet sealed under the wrong high half, or with the 8-byte data of
 * a 32-bit association, fails to open and does not move the window. */
#define _GNU_SOURCE
#include "compat.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define SOL_ALG 279
#define AF_ALG_ 38
#define ALG_SET_KEY 1
#define ALG_SET_IV 2
#define ALG_SET_OP 3
#define ALG_SET_AEAD_ASSOCLEN 4
#define ALG_SET_AEAD_AUTHSIZE 5

#define NETLINK_ROUTE 0
#define NETLINK_XFRM 6
#define NLM_F_REQUEST 1
#define NLM_F_ACK 4
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLMSG_ERROR 2
#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define IFLA_IFNAME 3
#define IFLA_NET_NS_PID 19
#define IFLA_LINKINFO 18
#define IFLA_INFO_KIND 1
#define IFLA_INFO_DATA 2
#define VETH_INFO_PEER 1
#define IFA_LOCAL 2
#define IFA_ADDRESS 1
#define IFF_UP 1
#define XFRM_MSG_NEWSA 16
#define XFRM_MSG_FLUSHSA 28
#define XFRM_MSG_NEWPOLICY 19
#define XFRM_MSG_DELPOLICY 20
#define XFRM_MSG_FLUSHPOLICY 29
#define XFRMA_TMPL 5
#define XFRMA_ALG_AEAD 18
#define XFRMA_REPLAY_ESN_VAL 23
#define XFRM_STATE_ESN 128
#define XFRM_MSG_GETSA 18
#define XFRM_MSG_NEWSA_ 16

struct nlh { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct ifinfo { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct ifaddr { uint8_t family, prefixlen, flags, scope; uint32_t index; };
struct selector { uint8_t daddr[16], saddr[16]; uint16_t dport, dport_mask, sport, sport_mask, family;
                  uint8_t pd, ps, proto, pad; int32_t ifindex; uint32_t user; };
struct xid { uint8_t daddr[16]; uint32_t spi; uint8_t proto, pad[3]; };
struct usersa {
    struct selector sel;
    struct xid id;
    uint8_t saddr[16];
    uint64_t lft[8], curlft[4];
    uint32_t stats[3];
    uint32_t seq, reqid;
    uint16_t family;
    uint8_t mode, replay_window, flags, pad[7];
};
struct aead { char name[64]; uint32_t key_len, icv_len; uint8_t key[36]; };
struct userpolicy {
    struct selector sel;
    uint64_t lft[8], curlft[4];
    uint32_t priority, index;
    uint8_t dir, action, flags, share;
};
struct policy_id { struct selector sel; uint32_t index; uint8_t dir, pad[3]; };
struct tmpl {
    struct xid id;
    uint16_t family, p;
    uint8_t saddr[16];
    uint32_t reqid;
    uint8_t mode, share, optional, p2;
    uint32_t aalgos, ealgos, calgos;
};

static char buf[2048];
static int blen;
static char ans[8192];
static int anslen;

static void start(int type, int flags) {
    struct nlh *h = (void *)buf;
    memset(buf, 0, sizeof buf);
    h->type = type;
    h->flags = flags | NLM_F_REQUEST;
    h->seq = 9;
    blen = 16;
}
static void body(const void *d, int len) { memcpy(buf + blen, d, len); blen = (blen + len + 3) & ~3; }
static void attr(int type, const void *d, int len) {
    uint16_t *a = (void *)(buf + blen);
    a[0] = 4 + len;
    a[1] = type;
    memcpy(buf + blen + 4, d, len);
    blen = (blen + 4 + len + 3) & ~3;
}
static int nest_open(int type) {
    uint16_t *a = (void *)(buf + blen);
    a[0] = 0;
    a[1] = type | 0x8000;
    blen += 4;
    return blen - 4;
}
static void nest_close(int o) { *(uint16_t *)(buf + o) = blen - o; }

/* Sends buf and waits for the answer: 0 or -errno. */
static int talk(int s) {
    ((struct nlh *)buf)->len = blen;
    if (send(s, buf, blen, 0) != blen) return -errno;
    anslen = 0;
    struct pollfd pf = {s, POLLIN, 0};
    if (poll(&pf, 1, 2000) != 1) return -ETIMEDOUT;
    ssize_t n = recv(s, ans, sizeof ans, 0);
    if (n < 16) return -EBADMSG;
    anslen = n;
    struct nlh *h = (void *)ans;
    if (h->type == NLMSG_ERROR) return *(int *)(ans + 16);
    return 0;
}

static void a4(uint8_t *a, uint32_t ip) { memset(a, 0, 16); a[0] = ip >> 24; a[1] = ip >> 16; a[2] = ip >> 8; a[3] = ip; }

static int veth_pair(int s, pid_t pid) {
    struct ifinfo z = {0};
    start(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK);
    body(&z, sizeof z);
    attr(IFLA_IFNAME, "vta", 4);
    int li = nest_open(IFLA_LINKINFO);
    attr(IFLA_INFO_KIND, "veth", 5);
    int d = nest_open(IFLA_INFO_DATA);
    int p = nest_open(VETH_INFO_PEER);
    body(&z, sizeof z);
    attr(IFLA_IFNAME, "vtb", 4);
    uint32_t v = pid;
    attr(IFLA_NET_NS_PID, &v, 4);
    nest_close(p); nest_close(d); nest_close(li);
    return talk(s);
}

/* Address ip (host order)/24 on link name, and up. */
static int addr_up(int s, const char *name, uint32_t ip) {
    struct ifinfo q = {0};
    start(RTM_GETLINK, 0);
    body(&q, sizeof q);
    attr(IFLA_IFNAME, name, strlen(name) + 1);
    if (talk(s) != 0) return -1;
    int index = ((struct ifinfo *)(ans + 16))->index;
    uint32_t be = htonl(ip);
    struct ifaddr ad = {.family = 2, .prefixlen = 24, .index = index};
    start(RTM_NEWADDR, NLM_F_CREATE | NLM_F_ACK);
    body(&ad, sizeof ad);
    attr(IFA_LOCAL, &be, 4);
    attr(IFA_ADDRESS, &be, 4);
    if (talk(s) != 0) return -2;
    struct ifinfo up = {.index = index, .flags = IFF_UP, .change = IFF_UP};
    start(RTM_NEWLINK, NLM_F_ACK);
    body(&up, sizeof up);
    return talk(s) == 0 ? 0 : -3;
}

#define P_IP 0xc0a84d01u   /* 192.168.77.1: the parent */
#define C_IP 0xc0a84d02u   /* 192.168.77.2: the child */

/* The key material of the association with this SPI: 16 bytes of key and 4 of salt. */
static void keymat(uint8_t *k, uint32_t spi) { for (int i = 0; i < 20; i++) k[i] = (spi >> 4) + i * 3 + 1; }

/* An association to dst (reqid, mode), keyed from its SPI. */
struct esnstate { uint32_t bmp_len, oseq, seq, oseq_hi, seq_hi, replay_window, bmp[1]; };
#define W_OSEQ 0xFFFFFFFDu                    /* the parent's first number to the child, less one */
#define W_RSEQ 0xFFFFFFF0u                    /* where the child's window starts */
static int add_sa(int x, uint32_t dst, uint32_t src, uint32_t spi, int reqid, int mode, int esn, uint32_t oseq, uint32_t seq) {
    struct usersa sa;
    struct aead al;
    memset(&sa, 0, sizeof sa);
    memset(&al, 0, sizeof al);
    a4(sa.id.daddr, dst);
    sa.id.spi = htonl(spi);
    sa.id.proto = IPPROTO_ESP;
    a4(sa.saddr, src);
    sa.family = AF_INET;
    sa.sel.family = AF_INET;
    sa.reqid = reqid;
    sa.mode = mode;
    sa.replay_window = esn ? 0 : 32;
    sa.flags = esn ? XFRM_STATE_ESN : 0;
    strcpy(al.name, "rfc4106(gcm(aes))");
    al.key_len = 160;
    al.icv_len = 128;
    keymat(al.key, spi);
    start(XFRM_MSG_NEWSA, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK);
    body(&sa, sizeof sa);
    attr(XFRMA_ALG_AEAD, &al, 72 + 20);
    if (esn) {
        struct esnstate es = {1, oseq, seq, 0, 0, 32, {0}};
        attr(XFRMA_REPLAY_ESN_VAL, &es, sizeof es);
    }
    return talk(x);
}

/* A UDP policy from SRC to DST in direction DIR with an ESP template
 * (reqid, mode, between SRC and DST) or, with reqid 0, a block. */
static int add_policy(int x, int dir, uint32_t src, uint32_t dst, int prio, int reqid, int mode) {
    struct userpolicy up;
    struct tmpl t;
    memset(&up, 0, sizeof up);
    memset(&t, 0, sizeof t);
    a4(up.sel.daddr, dst); a4(up.sel.saddr, src);
    up.sel.family = AF_INET; up.sel.pd = 32; up.sel.ps = 32; up.sel.proto = IPPROTO_UDP;
    up.priority = prio; up.dir = dir; up.action = reqid ? 0 : 1;
    a4(t.id.daddr, dst); t.id.proto = IPPROTO_ESP; t.family = AF_INET;
    a4(t.saddr, src); t.reqid = reqid; t.mode = mode;
    t.aalgos = t.ealgos = t.calgos = ~0u;
    start(XFRM_MSG_NEWPOLICY, NLM_F_ACK);
    body(&up, sizeof up);
    if (reqid) attr(XFRMA_TMPL, &t, sizeof t);
    return talk(x);
}

static int del_policy(int x, int dir, uint32_t src, uint32_t dst) {
    struct policy_id id;
    memset(&id, 0, sizeof id);
    a4(id.sel.daddr, dst); a4(id.sel.saddr, src);
    id.sel.family = AF_INET; id.sel.pd = 32; id.sel.ps = 32; id.sel.proto = IPPROTO_UDP; id.dir = dir;
    start(XFRM_MSG_DELPOLICY, NLM_F_ACK);
    body(&id, sizeof id);
    return talk(x);
}

static int flush_all(int x) {
    char z = 0;
    start(XFRM_MSG_FLUSHSA, NLM_F_ACK); body(&z, 1);
    int a = talk(x);
    start(XFRM_MSG_FLUSHPOLICY, NLM_F_ACK); body(&z, 1);
    return a ? a : talk(x);
}

/* Mode m ends of a pair of associations (the parent's to the child, SPI
 * 0x1000 + 0x100 m, and back, 0x2000 + 0x100 m) and the policies of one
 * side: side 0 is the parent. Both sides hold both associations. */
static uint32_t spi_pc(int m) { return 0x1000 + 0x100 * m; }
static uint32_t spi_cp(int m) { return 0x2000 + 0x100 * m; }
static int configure(int x, int side, int m) {
    int e = flush_all(x);
    /* to the child: the parent counts from init_oseq[0], the child expects from init_seq[0] */
    e = e ? e : add_sa(x, C_IP, P_IP, spi_pc(m), 1, m, 1, side == 0 ? W_OSEQ : 0, side == 0 ? 0 : W_RSEQ);
    e = e ? e : add_sa(x, P_IP, C_IP, spi_cp(m), 2, m, 1, 0, 0);
    if (side == 0) {
        e = e ? e : add_policy(x, 1, P_IP, C_IP, 100, 1, m);
        e = e ? e : add_policy(x, 0, C_IP, P_IP, 100, 2, m);
    } else {
        e = e ? e : add_policy(x, 1, C_IP, P_IP, 100, 2, m);
        e = e ? e : add_policy(x, 0, P_IP, C_IP, 100, 1, m);
    }
    return e;
}

static int udp_on(uint32_t ip, int port) {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip)};
    if (u < 0 || bind(u, (void *)&a, sizeof a) != 0) return -1;
    return u;
}

static int child(int in, int out) {
    char x;
    if (unshare(CLONE_NEWNET) != 0) return 1;
    int r = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    int xf = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
    if (write(out, "r", 1) != 1 || read(in, &x, 1) != 1) return 2;
    int ok = addr_up(r, "vtb", C_IP) == 0;
    int u = udp_on(C_IP, 7777);
    ok &= u >= 0 && xf >= 0;
    if (write(out, ok ? "y" : "n", 1) != 1) return 3;
    int count = 0;
    for (;;) {
        struct pollfd pf[2] = {{u, POLLIN, 0}, {in, POLLIN, 0}};
        if (poll(pf, 2, 150000) < 1) return 4;
        if (pf[1].revents & POLLIN) {
            if (read(in, &x, 1) != 1) return 5;
            if (x == 'S' || x == 'T') {
                int e = configure(xf, 1, x == 'T');
                count = 0;
                if (write(out, e == 0 ? "y" : "n", 1) != 1) return 6;
            } else {
                if (write(out, &count, sizeof count) != sizeof count) return 7;
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

/* Sends tag number seq (padded to len) and counts the replies carrying it
 * within ms. */
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

/* ---- sealing packets by hand */

/* gcm(aes) over IN (the 8 bytes of associated data, then the text) with the
 * 12-byte nonce; OUT gets the data, the ciphertext and the tag. */
static int seal(const uint8_t *key, const uint8_t *nonce, const uint8_t *in, int inlen, int assoc, uint8_t *out) {
    struct sockaddr_alg { uint16_t family; uint8_t type[14]; uint32_t feat, mask; uint8_t name[64]; } sa = {AF_ALG_};
    int s = socket(AF_ALG_, SOCK_SEQPACKET, 0);
    if (s < 0) return -1;
    strcpy((char *)sa.type, "aead");
    strcpy((char *)sa.name, "gcm(aes)");
    if (bind(s, (void *)&sa, sizeof sa) != 0 || setsockopt(s, SOL_ALG, ALG_SET_KEY, key, 16) != 0) { close(s); return -2; }
    int op = accept(s, NULL, NULL);
    close(s);
    if (op < 0) return -3;
    union { char b[128]; struct cmsghdr a; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    struct iovec v = { (void *)in, inlen };
    m.msg_iov = &v;
    m.msg_iovlen = 1;
    m.msg_control = ctl.b;
    struct cmsghdr *c = (struct cmsghdr *)ctl.b;
    size_t used = 0;
    c->cmsg_level = SOL_ALG; c->cmsg_type = ALG_SET_OP; c->cmsg_len = CMSG_LEN(4);
    *(uint32_t *)CMSG_DATA(c) = 1;
    used += CMSG_SPACE(4);
    c = (struct cmsghdr *)(ctl.b + used);
    c->cmsg_level = SOL_ALG; c->cmsg_type = ALG_SET_IV; c->cmsg_len = CMSG_LEN(4 + 12);
    *(uint32_t *)CMSG_DATA(c) = 12;
    memcpy(CMSG_DATA(c) + 4, nonce, 12);
    used += CMSG_SPACE(4 + 12);
    c = (struct cmsghdr *)(ctl.b + used);
    c->cmsg_level = SOL_ALG; c->cmsg_type = ALG_SET_AEAD_ASSOCLEN; c->cmsg_len = CMSG_LEN(4);
    *(uint32_t *)CMSG_DATA(c) = assoc;
    used += CMSG_SPACE(4);
    m.msg_controllen = used;
    int r = -4;
    if (sendmsg(op, &m, 0) >= 0) r = read(op, out, inlen + 16);
    close(op);
    return r;
}

/* The inner packet to seal: transport mode, a UDP datagram from the parent's
 * port 5555 to the child's echo (data DATA), or in tunnel mode (m == 1) that
 * datagram inside an IPv4 header from SRC; padded as RFC 4303 says, with
 * the PAD style 0 (right) or 1 (wrong first byte). */
static int inner(uint8_t *p, int m, uint32_t src, const char *data, int pad_style) {
    int dl = strlen(data), n = 0;
    if (m == 1) {
        int tot = 20 + 8 + dl;
        memset(p, 0, 20);
        p[0] = 0x45; p[2] = tot >> 8; p[3] = tot; p[8] = 64; p[9] = IPPROTO_UDP;
        *(uint32_t *)(p + 12) = htonl(src);
        *(uint32_t *)(p + 16) = htonl(C_IP);
        uint32_t sum = 0;
        for (int i = 0; i < 20; i += 2) sum += (p[i] << 8) | p[i + 1];
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        sum = ~sum & 0xffff;
        p[10] = sum >> 8; p[11] = sum;
        n = 20;
    }
    p[n] = 5555 >> 8; p[n + 1] = 5555 & 255; p[n + 2] = 7777 >> 8; p[n + 3] = 7777 & 255;
    p[n + 4] = (8 + dl) >> 8; p[n + 5] = 8 + dl; p[n + 6] = p[n + 7] = 0;
    memcpy(p + n + 8, data, dl);
    n += 8 + dl;
    int pad = (4 - (n + 2) % 4) % 4;
    for (int i = 0; i < pad; i++) p[n + i] = i + 1;
    if (pad && pad_style == 1) p[n] = 99;
    p[n + pad] = pad;
    p[n + pad + 1] = m == 1 ? 4 : IPPROTO_UDP;
    return n + pad + 2;
}

/* A sealed ESP payload (SPI spi, 64-bit number seq of which the header holds
 * the low half) for the child, transport mode. With old8 the data are the
 * 8 bytes of a 32-bit association. Returns its length. */
static int esp_packet(uint8_t *out, uint32_t spi, uint64_t seq, const char *data, int old8) {
    uint8_t plain[256], in[12 + 256 + 16], res[12 + 256 + 16 + 16], key[20], nonce[12];
    int pl = inner(plain, 0, P_IP, data, 0);
    keymat(key, spi);
    memcpy(nonce, key + 16, 4);
    uint32_t hi = seq >> 32, lo = seq;
    nonce[4] = hi >> 24; nonce[5] = hi >> 16; nonce[6] = hi >> 8; nonce[7] = hi;
    nonce[8] = lo >> 24; nonce[9] = lo >> 16; nonce[10] = lo >> 8; nonce[11] = lo;
    uint32_t bs = htonl(spi), bh = htonl(hi), bl = htonl(lo);
    int al = old8 ? 8 : 12;
    memcpy(in, &bs, 4);
    if (old8) memcpy(in + 4, &bl, 4);
    else { memcpy(in + 4, &bh, 4); memcpy(in + 8, &bl, 4); }
    memcpy(in + al, plain, pl);
    int r = seal(key, nonce, in, al + pl, al, res);
    if (r != al + pl + 16) return -1;
    memcpy(out, &bs, 4);
    memcpy(out + 4, &bl, 4);
    memcpy(out + 8, nonce + 4, 8);
    memcpy(out + 16, res + al, pl + 16);
    return 16 + pl + 16;
}

static int raw_send(int rs, const uint8_t *p, int n) {
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(C_IP)};
    return sendto(rs, p, n, 0, (void *)&to, sizeof to);
}

/* Waits ms for a reply to a crafted datagram: how many came with this data. */
static int echoes(int u, const char *data, int ms) {
    int got = 0;
    for (int t = 0; t < ms;) {
        char r[256];
        struct pollfd pf = {u, POLLIN, 0};
        int w = poll(&pf, 1, 100);
        t += 100;
        if (w == 1) {
            int n = recv(u, r, sizeof r, 0);
            if (n == (int)strlen(data) && memcmp(r, data, n) == 0) got++;
        }
    }
    return got;
}

/* The association to dst with this SPI, as the kernel reports it: its
 * XFRMA_REPLAY_ESN_VAL (0 if there is none). */
static int getsa(int x, uint32_t dst, uint32_t spi, struct esnstate *es) {
    struct { uint8_t daddr[16]; uint32_t spi; uint16_t family; uint8_t proto, pad; } id;
    memset(&id, 0, sizeof id);
    a4(id.daddr, dst);
    id.spi = htonl(spi);
    id.family = AF_INET;
    id.proto = IPPROTO_ESP;
    start(XFRM_MSG_GETSA, 0);
    body(&id, sizeof id);
    int e = talk(x);
    if (e) return e;
    if (((struct nlh *)ans)->type != XFRM_MSG_NEWSA_ || anslen < 16 + (int)sizeof(struct usersa)) return -EBADMSG;
    for (int o = 16 + sizeof(struct usersa); o + 4 <= anslen;) {
        uint16_t l = *(uint16_t *)(ans + o), t = *(uint16_t *)(ans + o + 2);
        if (l < 4 || o + l > anslen) break;
        if ((t & 0x3fff) == XFRMA_REPLAY_ESN_VAL && l >= 4 + 24) {
            memset(es, 0, sizeof *es);
            memcpy(es, ans + o + 4, 24);
            return 0;
        }
        o += (l + 3) & ~3;
    }
    return -ENOENT;
}

int main(void) {
    setup();
    int r = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    int xf = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
    EXPECT("netlink sockets", r >= 0 && xf >= 0);
    if (r < 0 || xf < 0) return done();
    int in[2], out[2];
    pid_t c = -1;
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
    EXPECT("a veth pair, one end in the child's namespace", veth_pair(r, c) == 0);
    EXPECT("the parent's end has an address", addr_up(r, "vta", P_IP) == 0);
    EXPECT("go", write(in[1], "g", 1) == 1 && read(out[0], &x, 1) == 1 && x == 'y');
    int u = udp_on(P_IP, 5555);
    struct sockaddr_in srv = {.sin_family = AF_INET, .sin_port = htons(7777), .sin_addr.s_addr = htonl(C_IP)};
    EXPECT("a client socket", u >= 0 && connect(u, (void *)&srv, sizeof srv) == 0);
    int seq = 0, delivered = 0, up = 0;
    for (int t = 0; t < 20 && !up; t++) up = ask(u, seq++, 64, 1500) >= 1;
    delivered += up;
    EXPECT("without IPsec the datagram is echoed", up);

    EXPECT("the parent has ESN associations", configure(xf, 0, 0) == 0);
    EXPECT("the child has them too", write(in[1], "S", 1) == 1 && read(out[0], &x, 1) == 1 && x == 'y');
    delivered = 0;
    struct esnstate es;
    EXPECT("GETSA reports the ESN state of the parent's sending association", getsa(xf, C_IP, spi_pc(0), &es) == 0
           && es.oseq == W_OSEQ && es.oseq_hi == 0 && es.replay_window == 32);
    {
        /* numbers FFFFFFFE, FFFFFFFF, then 1:0 ... : the sender crosses 2^32 */
        unsigned got = 0;
        for (int i = 0; i < 6; i++) got |= (ask(u, seq++, 64, 1500) == 1) << i;
        delivered += __builtin_popcount(got);
        EXPECT("six datagrams are echoed through the wrap of the sequence number", got == 63);
    }
    EXPECT("GETSA says the sender is at 1:3", getsa(xf, C_IP, spi_pc(0), &es) == 0 && es.oseq_hi == 1 && es.oseq == 3);
    EXPECT("and the answers took the parent's other association's numbers from 1", getsa(xf, P_IP, spi_cp(0), &es) == 0 && es.seq_hi == 0 && es.seq >= 6);

    int rs = socket(AF_INET, SOCK_RAW, IPPROTO_ESP);
    EXPECT("a raw ESP socket opens", rs >= 0);
    uint8_t pk[512];
    int n;
    uint32_t spi = spi_pc(0);
    /* the child's window is now at 1:3 (the packets it took from the parent) */
    n = esp_packet(pk, spi, 0x100000010ull, "n10", 0);
    EXPECT("a sealed packet numbered 1:0x10 is echoed", n > 0 && raw_send(rs, pk, n) == n && echoes(u, "n10", 1500) == 1);
    delivered++;
    EXPECT("played again it is not", raw_send(rs, pk, n) == n && echoes(u, "n10", 1200) == 0);
    n = esp_packet(pk, spi, 0xFFFFFFF5ull, "nold", 0);
    EXPECT("a packet numbered 0xFFFFFFF5, below the wrap but inside the window, is echoed", n > 0 && raw_send(rs, pk, n) == n && echoes(u, "nold", 1500) == 1);
    delivered++;
    EXPECT("and played again it is not", raw_send(rs, pk, n) == n && echoes(u, "nold", 1200) == 0);
    n = esp_packet(pk, spi, 0xFFFFFFE0ull, "ngone", 0);
    EXPECT("a number below the wrap and outside the window is not", n > 0 && raw_send(rs, pk, n) == n && echoes(u, "ngone", 1200) == 0);
    n = esp_packet(pk, spi, 0x00000010ull, "nhi0", 0);
    EXPECT("a packet sealed under the high half 0 for the low half 0x10, which the receiver reads as 1:0x10, is not", n > 0 && raw_send(rs, pk, n) == n && echoes(u, "nhi0", 1200) == 0);
    n = esp_packet(pk, spi, 0x200000011ull, "nhi2", 0);
    EXPECT("a packet sealed under the high half 2 for the low half 0x11 is not", n > 0 && raw_send(rs, pk, n) == n && echoes(u, "nhi2", 1200) == 0);
    n = esp_packet(pk, spi, 0x100000012ull, "nold8", 1);
    EXPECT("a packet sealed with the 8 bytes of a 32-bit association is not", n > 0 && raw_send(rs, pk, n) == n && echoes(u, "nold8", 1200) == 0);
    n = esp_packet(pk, spi, 0x100000013ull, "nflip", 0);
    pk[8] ^= 1;
    EXPECT("a packet with a bit of its IV changed is not", raw_send(rs, pk, n) == n && echoes(u, "nflip", 1200) == 0);
    n = esp_packet(pk, spi, 0x100000012ull, "n12", 0);
    EXPECT("after those, the next real packet 1:0x12 is echoed: no forgery moved the window", raw_send(rs, pk, n) == n && echoes(u, "n12", 1500) == 1);
    delivered++;
    n = esp_packet(pk, spi, 0x300000000ull + 5, "nfar", 0);
    EXPECT("a number whose high half is far ahead is not taken (the window cannot jump on an inferred half)", n > 0 && raw_send(rs, pk, n) == n && echoes(u, "nfar", 1200) == 0);
    n = esp_packet(pk, spi, 0x100000014ull, "n14", 0);
    EXPECT("and the window is where it was: 1:0x14 is echoed", raw_send(rs, pk, n) == n && echoes(u, "n14", 1500) == 1);
    delivered++;
    EXPECT("the sender still counts on", ask(u, seq++, 64, 1500) == 1);
    delivered++;
    EXPECT("the child stops", write(in[1], "Q", 1) == 1);
    int count = -1;
    EXPECT("the child reports", read(out[0], &count, sizeof count) == sizeof count);
    EXPECT("the child took in exactly the datagrams that were whole, fresh and ours", count == delivered);
    waitpid(c, 0, 0);
    flush_all(xf);
    close(u);
    close(r);
    close(xf);
    return done();
}
