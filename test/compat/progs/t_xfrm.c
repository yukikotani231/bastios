/* NETLINK_XFRM: the security associations of ESP, as `ip xfrm state` and the
 * key daemons drive them. The constants and structures are spelled out: the C
 * library's headers lack linux/xfrm.h. */
#define _GNU_SOURCE
#include "compat.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define NETLINK_XFRM 6
#define NLM_F_REQUEST 1
#define NLM_F_MULTI 2
#define NLM_F_ACK 4
#define NLM_F_REPLACE 0x100
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLM_F_DUMP 0x300
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define XFRM_MSG_NEWSA 16
#define XFRM_MSG_DELSA 17
#define XFRM_MSG_GETSA 18
#define XFRM_MSG_FLUSHSA 28
#define XFRM_MSG_NEWPOLICY 19
#define XFRM_MSG_DELPOLICY 20
#define XFRM_MSG_GETPOLICY 21
#define XFRM_MSG_UPDPOLICY 25
#define XFRM_MSG_FLUSHPOLICY 29
#define XFRMA_TMPL 5
#define XFRMA_ALG_CRYPT 2
#define XFRMA_ALG_AEAD 18
#define IPPROTO_ESP_ 50

struct nlh { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
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
struct usersa_id { uint8_t daddr[16]; uint32_t spi; uint16_t family; uint8_t proto, pad; };
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

static char buf[1024];
static int blen;
static char ans[16384];
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

static int talk(int s, int dump) {
    ((struct nlh *)buf)->len = blen;
    if (send(s, buf, blen, 0) != blen) return -errno;
    anslen = 0;
    for (;;) {
        char rb[8192];
        struct pollfd pf = {s, POLLIN, 0};
        if (poll(&pf, 1, 1500) != 1) return dump ? 0 : -ETIMEDOUT;
        ssize_t n = recv(s, rb, sizeof rb, 0);
        if (n <= 0) return -ETIMEDOUT;
        if (anslen + n <= (int)sizeof ans) { memcpy(ans + anslen, rb, n); anslen += n; }
        for (char *p = rb; p < rb + n;) {
            struct nlh *h = (void *)p;
            if (h->len < 16) return -EBADMSG;
            if (h->type == NLMSG_ERROR) return *(int *)(p + 16);
            if (h->type == NLMSG_DONE) return 0;
            if (!dump) return 0;
            p += (h->len + 3) & ~3u;
        }
    }
}

static void ip4(uint8_t *a, uint32_t ip) { memset(a, 0, 16); a[0] = ip >> 24; a[1] = ip >> 16; a[2] = ip >> 8; a[3] = ip; }

/* A NEWSA for destination DST, SPI, with a key of KLEN bytes (and the salt). */
static int add(int s, int flags, uint32_t dst, uint32_t spi, int reqid, int mode, int klen) {
    struct usersa sa;
    struct aead al;
    memset(&sa, 0, sizeof sa);
    memset(&al, 0, sizeof al);
    ip4(sa.id.daddr, dst);
    sa.id.spi = htonl(spi);
    sa.id.proto = IPPROTO_ESP_;
    ip4(sa.saddr, 0x0a000001);
    sa.family = AF_INET;
    sa.sel.family = AF_INET;
    sa.reqid = reqid;
    sa.mode = mode;
    sa.replay_window = 32;
    strcpy(al.name, "rfc4106(gcm(aes))");
    al.key_len = (klen + 4) * 8;
    al.icv_len = 128;
    for (int i = 0; i < klen + 4; i++) al.key[i] = 0x10 + i;
    start(XFRM_MSG_NEWSA, flags | NLM_F_ACK);
    body(&sa, sizeof sa);
    attr(XFRMA_ALG_AEAD, &al, 72 + klen + 4);
    return talk(s, 0);
}

static int idreq(int s, int type, uint32_t dst, uint32_t spi, int flags) {
    struct usersa_id id;
    memset(&id, 0, sizeof id);
    ip4(id.daddr, dst);
    id.spi = htonl(spi);
    id.family = AF_INET;
    id.proto = IPPROTO_ESP_;
    start(type, flags | NLM_F_ACK);
    body(&id, sizeof id);
    return talk(s, 0);
}

static int dump(int s) {
    start(XFRM_MSG_GETSA, NLM_F_DUMP);
    return talk(s, 1);
}

static int count(void) {
    int n = 0;
    for (char *p = ans; p < ans + anslen;) {
        struct nlh *h = (void *)p;
        if (h->len < 16) break;
        if (h->type == XFRM_MSG_NEWSA) n++;
        p += (h->len + 3) & ~3u;
    }
    return n;
}

/* A policy for traffic from SRC/SP to DST/DP in direction DIR; REQID > 0 adds
 * an ESP template (tunnel mode, to TDST) and ACT 1 makes it a block. */
static int policy(int s, int type, int dir, uint32_t dst, int dp, uint32_t src, int sp, int proto, int prio,
                  int reqid, uint32_t tdst, int act, int ntmpl, int tproto) {
    struct userpolicy up;
    struct tmpl t[2];
    memset(&up, 0, sizeof up);
    memset(t, 0, sizeof t);
    ip4(up.sel.daddr, dst); ip4(up.sel.saddr, src);
    up.sel.family = AF_INET; up.sel.pd = dp; up.sel.ps = sp; up.sel.proto = proto;
    up.priority = prio; up.dir = dir; up.action = act;
    for (int i = 0; i < 2; i++) {
        ip4(t[i].id.daddr, tdst); t[i].id.proto = tproto; t[i].family = AF_INET;
        ip4(t[i].saddr, 0x0a000001); t[i].reqid = reqid; t[i].mode = 1;
        t[i].aalgos = t[i].ealgos = t[i].calgos = ~0u;
    }
    start(type, NLM_F_ACK);
    body(&up, sizeof up);
    if (ntmpl) attr(XFRMA_TMPL, t, ntmpl * sizeof t[0]);
    return talk(s, 0);
}

static int selproto;

static int pol_get_sel(int s, int type, int dir, uint32_t dst, int dp, uint32_t src, int sp, int flags) {
    struct policy_id id;
    memset(&id, 0, sizeof id);
    ip4(id.sel.daddr, dst); ip4(id.sel.saddr, src);
    id.sel.family = AF_INET; id.sel.pd = dp; id.sel.ps = sp; id.sel.proto = selproto; id.dir = dir;
    start(type, flags | NLM_F_ACK);
    body(&id, sizeof id);
    return talk(s, 0);
}

static int pol_count(int s) {
    start(XFRM_MSG_GETPOLICY, NLM_F_DUMP);
    if (talk(s, 1) != 0) return -1;
    int n = 0;
    for (char *p = ans; p < ans + anslen;) {
        struct nlh *h = (void *)p;
        if (h->len < 16) break;
        if (h->type == XFRM_MSG_NEWPOLICY) n++;
        p += (h->len + 3) & ~3u;
    }
    return n;
}

#define FL (NLM_F_CREATE | NLM_F_EXCL)

int main(void) {
    setup();
    int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
    EXPECT("a NETLINK_XFRM socket opens", s >= 0);
    if (s < 0) return done();

    EXPECT("an association with an AES-128 key is added", add(s, FL, 0x0a000002, 0x1001, 7, 1, 16) == 0);
    EXPECT("the same one again is EEXIST", add(s, FL, 0x0a000002, 0x1001, 7, 1, 16) == -EEXIST);
    EXPECT("an AES-256 one with another SPI is added", add(s, FL, 0x0a000002, 0x1002, 8, 0, 32) == 0);
    EXPECT("a dump lists both", ({ int e = dump(s); e == 0 && count() == 2; }));
    EXPECT("reading one by its id gives its SPI, mode, reqid and algorithm", ({
        int e = idreq(s, XFRM_MSG_GETSA, 0x0a000002, 0x1001, 0);
        struct usersa *sa = (void *)(ans + 16);
        e == 0 && anslen >= 16 + (int)sizeof *sa && ntohl(sa->id.spi) == 0x1001 && sa->mode == 1 && sa->reqid == 7
            && sa->id.proto == IPPROTO_ESP_ && sa->id.daddr[3] == 2 && sa->family == AF_INET
            && memmem(ans, anslen, "rfc4106(gcm(aes))", 17) != NULL;
    }));
    EXPECT("the key comes back as it was given", ({
        int e = idreq(s, XFRM_MSG_GETSA, 0x0a000002, 0x1002, 0);
        char *a = memmem(ans, anslen, "rfc4106(gcm(aes))", 17);
        e == 0 && a && *(uint32_t *)(a + 64) == 36 * 8 && a[72] == 0x10 && a[72 + 35] == 0x10 + 35;
    }));
    EXPECT("an association can be replaced", ({
        int e = add(s, NLM_F_REPLACE, 0x0a000002, 0x1001, 99, 0, 16);
        int g = idreq(s, XFRM_MSG_GETSA, 0x0a000002, 0x1001, 0);
        struct usersa *sa = (void *)(ans + 16);
        e == 0 && g == 0 && sa->reqid == 99 && sa->mode == 0;
    }));
    EXPECT("replacing one that is not there is ESRCH", add(s, NLM_F_REPLACE, 0x0a000009, 0x77, 1, 0, 16) == -ESRCH);
    EXPECT("reading one that is not there is ESRCH", idreq(s, XFRM_MSG_GETSA, 0x0a000002, 0x9999, 0) == -ESRCH);
    EXPECT("the same SPI to another destination is another association", ({
        int e = add(s, FL, 0x0a000003, 0x1001, 1, 0, 16);
        e == 0 && idreq(s, XFRM_MSG_GETSA, 0x0a000003, 0x1001, 0) == 0;
    }));
    EXPECT("an association is deleted, and then it is ESRCH", ({
        int d = idreq(s, XFRM_MSG_DELSA, 0x0a000003, 0x1001, 0);
        d == 0 && idreq(s, XFRM_MSG_DELSA, 0x0a000003, 0x1001, 0) == -ESRCH
            && idreq(s, XFRM_MSG_GETSA, 0x0a000003, 0x1001, 0) == -ESRCH;
    }));
    EXPECT("a key length that is neither AES-128 nor AES-256 is EINVAL", ({
        struct usersa sa; struct aead al;
        memset(&sa, 0, sizeof sa); memset(&al, 0, sizeof al);
        ip4(sa.id.daddr, 0x0a000005); sa.id.spi = htonl(5); sa.id.proto = IPPROTO_ESP_; sa.family = AF_INET;
        strcpy(al.name, "rfc4106(gcm(aes))"); al.key_len = 28 * 8; al.icv_len = 128;
        start(XFRM_MSG_NEWSA, FL | NLM_F_ACK); body(&sa, sizeof sa); attr(XFRMA_ALG_AEAD, &al, 72 + 28);
        talk(s, 0) == -EINVAL;
    }));
    EXPECT("a 96-bit tag is EINVAL", ({
        struct usersa sa; struct aead al;
        memset(&sa, 0, sizeof sa); memset(&al, 0, sizeof al);
        ip4(sa.id.daddr, 0x0a000005); sa.id.spi = htonl(5); sa.id.proto = IPPROTO_ESP_; sa.family = AF_INET;
        strcpy(al.name, "rfc4106(gcm(aes))"); al.key_len = 20 * 8; al.icv_len = 96;
        start(XFRM_MSG_NEWSA, FL | NLM_F_ACK); body(&sa, sizeof sa); attr(XFRMA_ALG_AEAD, &al, 72 + 20);
        talk(s, 0) == -EINVAL;
    }));
    EXPECT("an algorithm the kernel does not have is ENOENT", ({
        struct usersa sa; struct aead al;
        memset(&sa, 0, sizeof sa); memset(&al, 0, sizeof al);
        ip4(sa.id.daddr, 0x0a000005); sa.id.spi = htonl(5); sa.id.proto = IPPROTO_ESP_; sa.family = AF_INET;
        strcpy(al.name, "rfc4106(ccm(aes))"); al.key_len = 20 * 8; al.icv_len = 128;
        start(XFRM_MSG_NEWSA, FL | NLM_F_ACK); body(&sa, sizeof sa); attr(XFRMA_ALG_AEAD, &al, 72 + 20);
        talk(s, 0) == -ENOENT;
    }));
    EXPECT("an unauthenticated cipher is refused", ({
        struct usersa sa;
        char alg[64 + 4 + 16];
        memset(&sa, 0, sizeof sa); memset(alg, 0, sizeof alg);
        ip4(sa.id.daddr, 0x0a000005); sa.id.spi = htonl(5); sa.id.proto = IPPROTO_ESP_; sa.family = AF_INET;
        strcpy(alg, "cbc(aes)"); *(uint32_t *)(alg + 64) = 128;
        start(XFRM_MSG_NEWSA, FL | NLM_F_ACK); body(&sa, sizeof sa); attr(XFRMA_ALG_CRYPT, alg, sizeof alg);
        talk(s, 0) == -EOPNOTSUPP;
    }));
    EXPECT("a message with no algorithm is EINVAL", ({
        struct usersa sa;
        memset(&sa, 0, sizeof sa);
        ip4(sa.id.daddr, 0x0a000005); sa.id.spi = htonl(5); sa.id.proto = IPPROTO_ESP_; sa.family = AF_INET;
        start(XFRM_MSG_NEWSA, FL | NLM_F_ACK); body(&sa, sizeof sa);
        talk(s, 0) == -EINVAL;
    }));
    EXPECT("AH is refused", ({
        struct usersa sa; struct aead al;
        memset(&sa, 0, sizeof sa); memset(&al, 0, sizeof al);
        ip4(sa.id.daddr, 0x0a000005); sa.id.spi = htonl(5); sa.id.proto = 51; sa.family = AF_INET;
        strcpy(al.name, "rfc4106(gcm(aes))"); al.key_len = 20 * 8; al.icv_len = 128;
        start(XFRM_MSG_NEWSA, FL | NLM_F_ACK); body(&sa, sizeof sa); attr(XFRMA_ALG_AEAD, &al, 72 + 20);
        talk(s, 0) == -EPROTONOSUPPORT;
    }));
    EXPECT("a truncated message is EINVAL", ({
        char half[40];
        memset(half, 0, sizeof half);
        start(XFRM_MSG_NEWSA, FL | NLM_F_ACK); body(half, sizeof half);
        talk(s, 0) == -EINVAL;
    }));
    EXPECT("a program that is not privileged cannot add, read or delete", ({
        int r = 0;
        pid_t c = fork();
        if (c == 0) {
            if (setuid(1000) != 0) _exit(2);
            int t = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
            int a = add(t, FL, 0x0a000007, 0x4001, 1, 0, 16);
            int g = idreq(t, XFRM_MSG_GETSA, 0x0a000002, 0x1002, 0);
            int d = idreq(t, XFRM_MSG_DELSA, 0x0a000002, 0x1002, 0);
            _exit(a == -EPERM && g == -EPERM && d == -EPERM ? 0 : 1);
        }
        int st = 0;
        waitpid(c, &st, 0);
        r = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        r && idreq(s, XFRM_MSG_GETSA, 0x0a000002, 0x1002, 0) == 0;
    }));
    EXPECT("a network namespace has associations of its own", ({
        int r = 0;
        pid_t c = fork();
        if (c == 0) {
            if (unshare(CLONE_NEWNET) != 0) _exit(2);
            int t = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
            int seen = idreq(t, XFRM_MSG_GETSA, 0x0a000002, 0x1002, 0);
            int a = add(t, FL, 0x0a000002, 0x1002, 1, 0, 16);
            int n = dump(t) == 0 ? count() : -1;
            _exit(seen == -ESRCH && a == 0 && n == 1 ? 0 : 1);
        }
        int st = 0;
        waitpid(c, &st, 0);
        r = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        r && dump(s) == 0 && count() == 2;
    }));
    EXPECT("flush removes them all", ({
        char fl = 0;
        start(XFRM_MSG_FLUSHSA, NLM_F_ACK);
        body(&fl, 1);
        int e = talk(s, 0);
        e == 0 && dump(s) == 0 && count() == 0;
    }));
    EXPECT("room for associations is limited, and flushing gives it back", ({
        int n = 0;
        while (n < 200 && add(s, FL, 0x0a010000 + n, 0x2000 + n, 1, 0, 16) == 0) n++;
        int full = n < 200 && n >= 32;
        char fl = 0;
        start(XFRM_MSG_FLUSHSA, NLM_F_ACK);
        body(&fl, 1);
        talk(s, 0);
        full && add(s, FL, 0x0a000002, 0x1001, 1, 0, 16) == 0;
    }));
    /* ---- policies ---- */
    EXPECT("an out policy with an ESP template is added", policy(s, XFRM_MSG_NEWPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0, 100, 7, 0x0a000002, 0, 1, 50) == 0);
    EXPECT("the same one again is EEXIST", policy(s, XFRM_MSG_NEWPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0, 100, 7, 0x0a000002, 0, 1, 50) == -EEXIST);
    EXPECT("the same selector in the other direction is another policy", policy(s, XFRM_MSG_NEWPOLICY, 0, 0x0a000100, 24, 0x0a000200, 24, 0, 100, 7, 0x0a000001, 0, 1, 50) == 0);
    EXPECT("reading one by selector gives its template, priority and direction", ({
        int e = pol_get_sel(s, XFRM_MSG_GETPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0);
        struct userpolicy *up = (void *)(ans + 16);
        struct tmpl *t = (void *)(ans + 16 + sizeof *up + 4);
        e == 0 && anslen >= 16 + (int)sizeof *up + 4 + (int)sizeof *t && up->priority == 100 && up->dir == 1 && up->action == 0
            && up->sel.pd == 24 && up->sel.ps == 24 && up->sel.daddr[2] == 2 && t->reqid == 7 && t->mode == 1
            && t->id.proto == IPPROTO_ESP_ && t->id.daddr[3] == 2;
    }));
    EXPECT("host bits of a selector are cleared", ({
        int e = policy(s, XFRM_MSG_NEWPOLICY, 1, 0x0a000377, 24, 0x0a000100, 24, 0, 5, 1, 0x0a000002, 0, 1, 50);
        int g = pol_get_sel(s, XFRM_MSG_GETPOLICY, 1, 0x0a000300, 24, 0x0a000100, 24, 0);
        struct userpolicy *up = (void *)(ans + 16);
        e == 0 && g == 0 && up->sel.daddr[3] == 0 && up->sel.daddr[2] == 3;
    }));
    EXPECT("a dump lists them", pol_count(s) == 3);
    EXPECT("reading one by its index gives the same policy", ({
        pol_get_sel(s, XFRM_MSG_GETPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0);
        uint32_t idx = ((struct userpolicy *)(ans + 16))->index;
        struct policy_id id;
        memset(&id, 0, sizeof id);
        id.index = idx; id.dir = 1;
        start(XFRM_MSG_GETPOLICY, NLM_F_ACK);
        body(&id, sizeof id);
        int e = talk(s, 0);
        idx != 0 && e == 0 && ((struct userpolicy *)(ans + 16))->priority == 100;
    }));
    EXPECT("UPDPOLICY replaces one, and NEWPOLICY does not", ({
        int e = policy(s, XFRM_MSG_UPDPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0, 50, 9, 0x0a000002, 0, 1, 50);
        int g = pol_get_sel(s, XFRM_MSG_GETPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0);
        struct userpolicy *up = (void *)(ans + 16);
        struct tmpl *t = (void *)(ans + 16 + sizeof *up + 4);
        e == 0 && g == 0 && up->priority == 50 && t->reqid == 9 && pol_count(s) == 3;
    }));
    EXPECT("a block policy is added and read back", ({
        int e = policy(s, XFRM_MSG_NEWPOLICY, 0, 0x0a000500, 32, 0, 0, 17, 1, 0, 0, 1, 0, 0);
        selproto = 17;
        int g = pol_get_sel(s, XFRM_MSG_GETPOLICY, 0, 0x0a000500, 32, 0, 0, 0);
        selproto = 0;
        struct userpolicy *up = (void *)(ans + 16);
        e == 0 && g == 0 && up->action == 1 && up->sel.proto == 17;
    }));
    EXPECT("a block policy with a template is EINVAL", policy(s, XFRM_MSG_NEWPOLICY, 0, 0x0a000600, 32, 0, 0, 0, 1, 1, 0x0a000002, 1, 1, 50) == -EINVAL);
    EXPECT("a prefix longer than 32 is EINVAL", policy(s, XFRM_MSG_NEWPOLICY, 1, 0x0a000600, 33, 0, 0, 0, 1, 0, 0, 0, 0, 0) == -EINVAL);
    EXPECT("a direction that is not in or out is EINVAL", policy(s, XFRM_MSG_NEWPOLICY, 3, 0x0a000600, 32, 0, 0, 0, 1, 0, 0, 0, 0, 0) == -EINVAL);
    EXPECT("the forward direction is EOPNOTSUPP", policy(s, XFRM_MSG_NEWPOLICY, 2, 0x0a000600, 32, 0, 0, 0, 1, 0, 0, 0, 0, 0) == -EOPNOTSUPP);
    EXPECT("two templates are EOPNOTSUPP", policy(s, XFRM_MSG_NEWPOLICY, 1, 0x0a000600, 32, 0, 0, 0, 1, 1, 0x0a000002, 0, 2, 50) == -EOPNOTSUPP);
    EXPECT("a template for a protocol other than ESP is refused", policy(s, XFRM_MSG_NEWPOLICY, 1, 0x0a000600, 32, 0, 0, 0, 1, 1, 0x0a000002, 0, 1, 51) == -EPROTONOSUPPORT);
    EXPECT("a selector with a port is EOPNOTSUPP", ({
        struct userpolicy up;
        memset(&up, 0, sizeof up);
        ip4(up.sel.daddr, 0x0a000600); up.sel.family = AF_INET; up.sel.pd = 32; up.sel.dport = htons(80); up.sel.dport_mask = 0xffff;
        start(XFRM_MSG_NEWPOLICY, NLM_F_ACK); body(&up, sizeof up);
        talk(s, 0) == -EOPNOTSUPP;
    }));
    EXPECT("a truncated policy is EINVAL", ({
        char half[40];
        memset(half, 0, sizeof half);
        start(XFRM_MSG_NEWPOLICY, NLM_F_ACK); body(half, sizeof half);
        talk(s, 0) == -EINVAL;
    }));
    EXPECT("a policy is deleted, and then it is ESRCH", ({
        int d = pol_get_sel(s, XFRM_MSG_DELPOLICY, 1, 0x0a000300, 24, 0x0a000100, 24, 0);
        d == 0 && pol_get_sel(s, XFRM_MSG_DELPOLICY, 1, 0x0a000300, 24, 0x0a000100, 24, 0) == -ESRCH
            && pol_get_sel(s, XFRM_MSG_GETPOLICY, 1, 0x0a000300, 24, 0x0a000100, 24, 0) == -ESRCH;
    }));
    EXPECT("a program that is not privileged cannot add, read or delete a policy", ({
        int r = 0;
        pid_t c = fork();
        if (c == 0) {
            if (setuid(1000) != 0) _exit(2);
            int t = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
            int a = policy(t, XFRM_MSG_NEWPOLICY, 1, 0x0a000700, 24, 0, 0, 0, 1, 0, 0, 0, 0, 0);
            int g = pol_get_sel(t, XFRM_MSG_GETPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0);
            int d = pol_get_sel(t, XFRM_MSG_DELPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0);
            _exit(a == -EPERM && g == -EPERM && d == -EPERM ? 0 : 1);
        }
        int st = 0;
        waitpid(c, &st, 0);
        r = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        r && pol_get_sel(s, XFRM_MSG_GETPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0) == 0;
    }));
    EXPECT("a network namespace has policies of its own", ({
        int r = 0;
        pid_t c = fork();
        if (c == 0) {
            if (unshare(CLONE_NEWNET) != 0) _exit(2);
            int t = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
            int seen = pol_get_sel(t, XFRM_MSG_GETPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0);
            int a = policy(t, XFRM_MSG_NEWPOLICY, 1, 0x0a000200, 24, 0x0a000100, 24, 0, 1, 0, 0, 0, 0, 0);
            int n = pol_count(t);
            _exit(seen == -ESRCH && a == 0 && n == 1 ? 0 : 1);
        }
        int st = 0;
        waitpid(c, &st, 0);
        r = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        r && pol_count(s) == 3;
    }));
    EXPECT("flushing policies removes them all, and gives the room back", ({
        char fl = 0;
        start(XFRM_MSG_FLUSHPOLICY, NLM_F_ACK);
        body(&fl, 1);
        int e = talk(s, 0);
        int z = pol_count(s) == 0;
        int n = 0;
        while (n < 200 && policy(s, XFRM_MSG_NEWPOLICY, 1, 0x0b000000 + n * 256, 24, 0, 0, 0, 1, 0, 0, 0, 0, 0) == 0) n++;
        int full = n < 200 && n >= 16;
        start(XFRM_MSG_FLUSHPOLICY, NLM_F_ACK);
        body(&fl, 1);
        talk(s, 0);
        e == 0 && z && full && pol_count(s) == 0;
    }));
    return done();
}
