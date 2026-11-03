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
    return done();
}
