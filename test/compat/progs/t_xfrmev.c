/* The SA multicast group of NETLINK_XFRM: a socket in it is told with
 * XFRM_MSG_NEWSA when an association is added or replaced and with
 * XFRM_MSG_DELSA (the id of the association) when one is deleted, flushed or
 * expires; a socket in no group hears nothing; an association of another
 * network namespace is not reported. */
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
#define SOL_NETLINK 270
#define NETLINK_ADD_MEMBERSHIP 1
#define NETLINK_DROP_MEMBERSHIP 2
#define XFRM_MSG_EXPIRE 24
#define XFRMNLGRP_SA 4
#define XFRM_MSG_DELSA 17
#define XFRM_MSG_GETSA_ 18
#define NLM_F_REPLACE 0x100
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
#define XFRM_MSG_GETSA 18
#define XFRM_MSG_FLUSHSA 28
#define XFRM_MSG_NEWPOLICY 19
#define XFRM_MSG_DELPOLICY 20
#define XFRM_MSG_FLUSHPOLICY 29
#define XFRMA_TMPL 5
#define XFRMA_ALG_AEAD 18

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

static uint64_t g_lft[8];
static uint32_t g_spi;                  /* the association g_lft applies to; the others have no limits */
static const uint64_t INF = ~0ull;

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

/* The key material of the association with this SPI: 16 bytes of key and 4 of salt. */
static void keymat(uint8_t *k, uint32_t spi) { for (int i = 0; i < 20; i++) k[i] = (spi >> 4) + i * 3 + 1; }

/* An association to dst (reqid, mode), keyed from its SPI. */
static int add_sa(int x, uint32_t dst, uint32_t src, uint32_t spi, int reqid, int mode) {
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
    sa.replay_window = 32;
    for (int i = 0; i < 8; i++) sa.lft[i] = spi == g_spi ? g_lft[i] : INF;
    strcpy(al.name, "rfc4106(gcm(aes))");
    al.key_len = 160;
    al.icv_len = 128;
    keymat(al.key, spi);
    start(XFRM_MSG_NEWSA, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK);
    body(&sa, sizeof sa);
    attr(XFRMA_ALG_AEAD, &al, 72 + 20);
    return talk(x);
}


#define P_IP 0xc0a84d01u
#define C_IP 0xc0a84d02u

struct sockaddr_nl_ { uint16_t family, pad; uint32_t pid, groups; };

struct usersa_id { uint8_t d[16]; uint32_t spi; uint16_t family; uint8_t proto, pad; };

static int xsock(unsigned groups) {
    int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
    if (s < 0) return -errno;
    struct sockaddr_nl_ a = {.family = AF_NETLINK, .groups = groups};
    if (bind(s, (void *)&a, sizeof a) != 0) { int e = errno; close(s); return -e; }
    return s;
}


static char pend[8192];
static int pendlen, pendsock = -1;
/* The next message on s within ms: its type, and for NEWSA/DELSA the spi (host order). */
static int next_ev(int s, int ms, uint32_t *spi) {
    if (pendsock != s) pendlen = 0;
    if (pendlen == 0) {
        struct pollfd pf = {s, POLLIN, 0};
        if (poll(&pf, 1, ms) != 1) return 0;
        ssize_t n = recv(s, pend, sizeof pend, 0);
        if (n < 16) return 0;
        pendlen = n;
        pendsock = s;
    }
    struct nlh *h = (void *)pend;
    int len = (h->len + 3) & ~3, t = h->type;
    if (t == XFRM_MSG_NEWSA && h->len >= 16 + 232) *spi = ntohl(*(uint32_t *)(pend + 16 + 72));
    else if (t == XFRM_MSG_DELSA && h->len == 16 + 24) *spi = ntohl(*(uint32_t *)(pend + 16 + 16));
    else t = -t;
    if (len > pendlen) len = pendlen;
    memmove(pend, pend + len, pendlen - len);
    pendlen -= len;
    return t;
}

static int del_sa(int x, uint32_t dst, uint32_t spi) {
    struct usersa_id id;
    memset(&id, 0, sizeof id);
    a4(id.d, dst);
    id.spi = htonl(spi);
    id.family = AF_INET;
    id.proto = IPPROTO_ESP;
    start(XFRM_MSG_DELSA, NLM_F_ACK);
    body(&id, sizeof id);
    return talk(x);
}

int main(void) {
    setup();
    for (int i = 0; i < 8; i++) g_lft[i] = INF;
    int x = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
    EXPECT("a socket to configure with", x >= 0);
    if (x < 0) return done();
    /* start from an empty table: other programs may have left associations behind */
    start(XFRM_MSG_FLUSHSA, NLM_F_ACK);
    uint8_t proto0[4] = {IPPROTO_ESP, 0, 0, 0};
    body(proto0, 4);
    EXPECT("the table is flushed first", talk(x) == 0);
    int sub = xsock(1u << (XFRMNLGRP_SA - 1));
    EXPECT("root can join the SA group", sub >= 0);
    int quiet = xsock(0);
    int exp = xsock(1u << 1);
    EXPECT("sockets in no group and in the expire group", quiet >= 0 && exp >= 0);
    uint32_t spi = 0;
    EXPECT("an association is added", add_sa(x, C_IP, P_IP, 0x100, 1, 1) == 0);
    EXPECT("NEWSA is reported with its SPI", next_ev(sub, 1000, &spi) == XFRM_MSG_NEWSA && spi == 0x100);
    EXPECT("an association is deleted", del_sa(x, C_IP, 0x100) == 0);
    EXPECT("DELSA is reported with its SPI", next_ev(sub, 1000, &spi) == XFRM_MSG_DELSA && spi == 0x100);
    EXPECT("two associations are added", add_sa(x, C_IP, P_IP, 0x101, 1, 1) == 0 && add_sa(x, C_IP, P_IP, 0x102, 1, 1) == 0);
    EXPECT("both NEWSA are reported", next_ev(sub, 1000, &spi) == XFRM_MSG_NEWSA && spi == 0x101 && next_ev(sub, 1000, &spi) == XFRM_MSG_NEWSA && spi == 0x102);
    start(XFRM_MSG_FLUSHSA, NLM_F_ACK);
    uint8_t proto[4] = {IPPROTO_ESP, 0, 0, 0};
    body(proto, 4);
    EXPECT("they are flushed", talk(x) == 0);
    EXPECT("a DELSA for each", ({ int a = next_ev(sub, 1000, &spi), b = next_ev(sub, 1000, &spi); a == XFRM_MSG_DELSA && b == XFRM_MSG_DELSA; }));
    EXPECT("a failed add reports nothing", ({ add_sa(x, C_IP, P_IP, 0x103, 1, 1); add_sa(x, C_IP, P_IP, 0x103, 1, 1) != 0; }));
    EXPECT("only the first add is reported", next_ev(sub, 1000, &spi) == XFRM_MSG_NEWSA && !next_ev(sub, 300, &spi));
    del_sa(x, C_IP, 0x103);
    next_ev(sub, 1000, &spi);

    g_spi = 0x200;
    g_lft[5] = 2;
    EXPECT("an association with a hard limit", add_sa(x, C_IP, P_IP, 0x200, 1, 1) == 0);
    EXPECT("NEWSA", next_ev(sub, 1000, &spi) == XFRM_MSG_NEWSA && spi == 0x200);
    EXPECT("its expiry is a DELSA too", next_ev(sub, 5000, &spi) == XFRM_MSG_DELSA && spi == 0x200);
    EXPECT("a socket in no group heard nothing", !next_ev(quiet, 300, &spi));
    EXPECT("nor the expire group, which hears its own message", ({ int t = next_ev(exp, 300, &spi); t != XFRM_MSG_NEWSA && t != XFRM_MSG_DELSA; }));

    pid_t c = fork();
    if (c == 0) {
        if (unshare(CLONE_NEWNET) != 0) _exit(2);
        int y = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
        g_spi = 0;
        int e = add_sa(y, C_IP, P_IP, 0x300, 1, 1);
        _exit(e == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(c, &st, 0);
    EXPECT("the child made one", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    EXPECT("an association of another namespace is not reported", !next_ev(sub, 500, &spi));
    return done();
}
