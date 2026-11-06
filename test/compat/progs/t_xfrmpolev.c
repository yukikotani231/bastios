/* The policy multicast group of NETLINK_XFRM: a socket in it is told with
 * XFRM_MSG_NEWPOLICY when a policy is added, UPDPOLICY when one is replaced,
 * DELPOLICY (the id of the policy) when one is deleted and FLUSHPOLICY when
 * all are; a socket in no group or in the SA group hears nothing; a policy of
 * another network namespace is not reported. */
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
#define XFRMNLGRP_POLICY 5
#define XFRM_MSG_UPDPOLICY 25
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

#define P_IP 0xc0a84d01u
#define C_IP 0xc0a84d02u

struct sockaddr_nl_ { uint16_t family, pad; uint32_t pid, groups; };

static int xsock(unsigned groups) {
    int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
    if (s < 0) return -errno;
    struct sockaddr_nl_ a = {.family = AF_NETLINK, .groups = groups};
    if (bind(s, (void *)&a, sizeof a) != 0) { int e = errno; close(s); return -e; }
    return s;
}

static char pend[8192];
static int pendlen, pendsock = -1;
/* The next message on s within ms: its type, and for NEWPOLICY/UPDPOLICY/DELPOLICY
 * the policy's direction and the destination's first octet. */
static int next_ev(int s, int ms, int *dir, int *d0) {
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
    *d0 = (uint8_t)pend[16];
    if ((t == XFRM_MSG_NEWPOLICY || t == XFRM_MSG_UPDPOLICY) && h->len >= 16 + sizeof(struct userpolicy)) *dir = ((struct userpolicy *)(pend + 16))->dir;
    else if (t == XFRM_MSG_DELPOLICY && h->len == 16 + sizeof(struct policy_id)) *dir = ((struct policy_id *)(pend + 16))->dir;
    else if (t != XFRM_MSG_FLUSHPOLICY) t = -t;
    if (len > pendlen) len = pendlen;
    memmove(pend, pend + len, pendlen - len);
    pendlen -= len;
    return t;
}

/* A policy for 10.0.0.<d>/24 -> 10.1.0.0/24 in direction DIR, blocking. */
static int policy(int x, int type, int dir, int d) {
    struct userpolicy up;
    memset(&up, 0, sizeof up);
    a4(up.sel.daddr, 0x0a000000 + d);
    a4(up.sel.saddr, 0x0a010000);
    up.sel.family = AF_INET;
    up.sel.pd = 24;
    up.sel.ps = 24;
    up.priority = 100 + d;
    up.dir = dir;
    up.action = 1;
    start(type, NLM_F_ACK);
    body(&up, sizeof up);
    return talk(x);
}

static int del_policy(int x, int dir, int d) {
    struct policy_id id;
    memset(&id, 0, sizeof id);
    a4(id.sel.daddr, 0x0a000000 + d);
    a4(id.sel.saddr, 0x0a010000);
    id.sel.family = AF_INET;
    id.sel.pd = 24;
    id.sel.ps = 24;
    id.dir = dir;
    start(XFRM_MSG_DELPOLICY, NLM_F_ACK);
    body(&id, sizeof id);
    return talk(x);
}

int main(void) {
    setup();
    int x = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
    EXPECT("a socket to configure with", x >= 0);
    if (x < 0) return done();
    /* start from an empty table: other programs may have left policies behind */
    start(XFRM_MSG_FLUSHPOLICY, NLM_F_ACK);
    EXPECT("the table is flushed first", talk(x) == 0);
    int sub = xsock(1u << (XFRMNLGRP_POLICY - 1));
    EXPECT("root can join the policy group", sub >= 0);
    int quiet = xsock(0);
    int sa = xsock(1u << (XFRMNLGRP_SA - 1));
    EXPECT("sockets in no group and in the SA group", quiet >= 0 && sa >= 0);
    int dir = -1, d0 = 0;
    EXPECT("a policy is added", policy(x, XFRM_MSG_NEWPOLICY, 1, 2) == 0);
    EXPECT("NEWPOLICY is reported with its direction and selector", next_ev(sub, 1000, &dir, &d0) == XFRM_MSG_NEWPOLICY && dir == 1 && d0 == 10);
    EXPECT("it is replaced", policy(x, XFRM_MSG_UPDPOLICY, 1, 2) == 0);
    EXPECT("UPDPOLICY is reported", next_ev(sub, 1000, &dir, &d0) == XFRM_MSG_UPDPOLICY && dir == 1);
    EXPECT("a failed add (EEXIST) reports nothing", ({ int e = policy(x, XFRM_MSG_NEWPOLICY, 1, 2); e != 0 && !next_ev(sub, 300, &dir, &d0); }));
    EXPECT("another direction is another policy", policy(x, XFRM_MSG_NEWPOLICY, 0, 2) == 0);
    EXPECT("NEWPOLICY for it", next_ev(sub, 1000, &dir, &d0) == XFRM_MSG_NEWPOLICY && dir == 0);
    EXPECT("one is deleted", del_policy(x, 1, 2) == 0);
    EXPECT("DELPOLICY is reported with its direction", next_ev(sub, 1000, &dir, &d0) == XFRM_MSG_DELPOLICY && dir == 1);
    EXPECT("deleting what is not there reports nothing", ({ int e = del_policy(x, 1, 2); e != 0 && !next_ev(sub, 300, &dir, &d0); }));
    start(XFRM_MSG_FLUSHPOLICY, NLM_F_ACK);
    EXPECT("the rest are flushed", talk(x) == 0);
    EXPECT("FLUSHPOLICY is reported", next_ev(sub, 1000, &dir, &d0) == XFRM_MSG_FLUSHPOLICY);
    EXPECT("a socket in no group heard nothing", !next_ev(quiet, 300, &dir, &d0));
    EXPECT("nor the SA group", !next_ev(sa, 300, &dir, &d0));

    pid_t c = fork();
    if (c == 0) {
        if (unshare(CLONE_NEWNET) != 0) _exit(2);
        int y = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
        _exit(policy(y, XFRM_MSG_NEWPOLICY, 1, 3) == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(c, &st, 0);
    EXPECT("the child made one", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    EXPECT("a policy of another namespace is not reported", !next_ev(sub, 500, &dir, &d0));
    return done();
}
