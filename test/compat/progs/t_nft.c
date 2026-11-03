/* The packet filter, through the interface nft and iptables-nft use:
 * NETLINK_NETFILTER sockets and nf_tables messages. A batch makes a table, a
 * base chain and rules; listing sends them back; and the rules drop and
 * accept traffic on the loopback interface. Constants are spelled out: the
 * C library's headers lack linux/netfilter. */
#define _GNU_SOURCE
#include "compat.h"
#include "t_nft_replay.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>

#define NETLINK_NETFILTER 12
#define NLM_F_REQUEST 1
#define NLM_F_ACK 4
#define NLM_F_DUMP 0x300
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLM_F_APPEND 0x800
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLA_F_NESTED 0x8000
#define NFNL_BEGIN 0x10
#define NFNL_END 0x11
#define NFT(m) (0x0a00 | (m))
#define MSG_NEWTABLE 0
#define MSG_GETTABLE 1
#define MSG_DELTABLE 2
#define MSG_NEWCHAIN 3
#define MSG_GETCHAIN 4
#define MSG_DELCHAIN 5
#define MSG_NEWRULE 6
#define MSG_GETRULE 7
#define MSG_DELRULE 8
#define MSG_GETGEN 16
#define NFPROTO_INET 1

struct nlh { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct nla { uint16_t len, type; };

/* One request being built, and one batch of them. */
static char buf[8192];
static int blen;
static int seqno;

static uint32_t be32(uint32_t v) { return htonl(v); }

static int nla_open(int type) {
    int at = (blen + 3) & ~3;
    memset(buf + blen, 0, at - blen);
    struct nla *a = (void *)(buf + at);
    a->type = type;
    a->len = 4;
    blen = at + 4;
    return at;
}

static void nla_close(int at) {
    struct nla *a = (void *)(buf + at);
    a->len = blen - at;
}

static void put(int type, const void *d, int len) {
    int at = nla_open(type);
    memcpy(buf + blen, d, len);
    blen += len;
    nla_close(at);
}

static void put32(int type, uint32_t v) { uint32_t b = be32(v); put(type, &b, 4); }
static void put64(int type, uint64_t v) {
    uint32_t b[2] = {be32(v >> 32), be32(v & 0xffffffff)};
    put(type, b, 8);
}
static void putstr(int type, const char *s) { put(type, s, strlen(s) + 1); }

/* A message header and the nfgenmsg after it; msg() ends the previous
 * message first. */
static int mstart;
static void msg(int type, int flags, int family) {
    memset(buf + blen, 0, 8);
    mstart = (blen + 3) & ~3;
    memset(buf + blen, 0, mstart - blen);
    struct nlh *h = (void *)(buf + mstart);
    memset(h, 0, 20);
    h->type = type;
    h->flags = flags | NLM_F_REQUEST;
    h->seq = ++seqno;
    ((unsigned char *)h)[16] = family;
    blen = mstart + 20;
}
static void mend(void) {
    struct nlh *h = (void *)(buf + mstart);
    h->len = blen - mstart;
}

/* Sends buf (finished messages) and reads answers until each request with
 * NLM_F_ACK or a dump has been answered; returns the first error (an errno,
 * 0 if none). The answers stay in ans for the caller. */
static char ans[16384];
static int anslen;

static int talk(int s, int want) {
    if (send(s, buf, blen, 0) != blen) return -errno;
    blen = 0;
    anslen = 0;
    int first = 0;
    struct timeval tv = {2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (want > 0) {
        char rb[8192];
        struct pollfd pf = {s, POLLIN, 0};
        if (poll(&pf, 1, 2000) != 1) return first ? first : -ETIMEDOUT;
        ssize_t n = recv(s, rb, sizeof rb, 0);
        if (n <= 0) return first ? first : -ETIMEDOUT;
        if (anslen + n <= (int)sizeof ans) {
            memcpy(ans + anslen, rb, n);
            anslen += n;
        }
        for (char *p = rb; p < rb + n;) {
            struct nlh *h = (void *)p;
            if (h->len < 16) break;
            if (h->type == NLMSG_ERROR) {
                int e = *(int *)(p + 16);
                if (e && !first) first = -e;
                want--;
            } else if (h->type == NLMSG_DONE) {
                want--;
            } else if (h->flags & 2) {
                /* part of a dump */
            } else {
                want--;
            }
            p += (h->len + 3) & ~3u;
        }
    }
    return first;
}

static void batch_begin(void) { msg(NFNL_BEGIN, 0, 0); buf[mstart + 18] = 0; buf[mstart + 19] = 10; mend(); }
static void batch_end(void) { msg(NFNL_END, 0, 0); buf[mstart + 18] = 0; buf[mstart + 19] = 10; mend(); }

static void new_table(const char *name) {
    msg(NFT(MSG_NEWTABLE), NLM_F_ACK | NLM_F_CREATE, NFPROTO_INET);
    putstr(1, name);
    mend();
}

static void new_chain(const char *table, const char *name, int hook, int prio, int policy) {
    msg(NFT(MSG_NEWCHAIN), NLM_F_ACK | NLM_F_CREATE, NFPROTO_INET);
    putstr(1, table);
    putstr(3, name);
    int h = nla_open(4 | NLA_F_NESTED);
    put32(1, hook);
    put32(2, (uint32_t)prio);
    nla_close(h);
    put32(5, policy);
    putstr(7, "filter");
    mend();
}

/* Expressions. */
static void ex_open(int *e, int *d, const char *name) {
    *e = nla_open(1 | NLA_F_NESTED);
    putstr(1, name);
    *d = nla_open(2 | NLA_F_NESTED);
}
static void ex_close(int e, int d) { nla_close(d); nla_close(e); }

static void data_value(int type, const void *v, int len) {
    int d = nla_open(type | NLA_F_NESTED);
    put(1, v, len);
    nla_close(d);
}

static void ex_meta(int key, int reg) {
    int e, d;
    ex_open(&e, &d, "meta");
    put32(1, reg);
    put32(2, key);
    ex_close(e, d);
}
static void ex_payload(int base, int off, int len, int reg) {
    int e, d;
    ex_open(&e, &d, "payload");
    put32(1, reg);
    put32(2, base);
    put32(3, off);
    put32(4, len);
    ex_close(e, d);
}
static void ex_cmp(int reg, int op, const void *v, int len) {
    int e, d;
    ex_open(&e, &d, "cmp");
    put32(1, reg);
    put32(2, op);
    data_value(3, v, len);
    ex_close(e, d);
}
static void ex_counter(void) {
    int e, d;
    ex_open(&e, &d, "counter");
    put64(1, 0);
    put64(2, 0);
    ex_close(e, d);
}
static void ex_verdict(int code) {
    int e, d;
    ex_open(&e, &d, "immediate");
    put32(1, 0);
    int dd = nla_open(2 | NLA_F_NESTED);
    int v = nla_open(2 | NLA_F_NESTED);
    put32(1, code);
    nla_close(v);
    nla_close(dd);
    ex_close(e, d);
}
static void ex_ct_state(uint32_t mask) {
    int e, d;
    ex_open(&e, &d, "ct");
    put32(1, 8);
    put32(2, 0);                   /* NFT_CT_STATE */
    ex_close(e, d);
    ex_open(&e, &d, "bitwise");
    put32(1, 8);
    put32(2, 8);
    put32(3, 4);
    data_value(4, &mask, 4);
    uint32_t zero = 0;
    data_value(5, &zero, 4);
    ex_close(e, d);
    ex_open(&e, &d, "cmp");
    put32(1, 8);
    put32(2, 1);                   /* neq 0 */
    data_value(3, &zero, 4);
    ex_close(e, d);
}

/* "tcp dport PORT": meta l4proto tcp; payload; cmp. */
static void match_dport(int proto, int port) {
    uint8_t p = proto;
    uint16_t bp = htons(port);
    ex_meta(16, 8);
    ex_cmp(8, 0, &p, 1);
    ex_payload(2, 2, 2, 8);
    ex_cmp(8, 0, &bp, 2);
}

static void rule_start(const char *table, const char *chain, int flags) {
    msg(NFT(MSG_NEWRULE), NLM_F_ACK | NLM_F_CREATE | flags, NFPROTO_INET);
    putstr(1, table);
    putstr(2, chain);
}
static int rule_exprs;
static void exprs_open(void) { rule_exprs = nla_open(4 | NLA_F_NESTED); }
static void exprs_close(void) { nla_close(rule_exprs); mend(); }

static int nf_socket(void) {
    int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER);
    if (s >= 0) {
        struct sockaddr_nl { unsigned short fam; unsigned short pad; unsigned pid, groups; } a = {AF_NETLINK, 0, 0, 0};
        bind(s, (void *)&a, sizeof a);
    }
    return s;
}

static int dump(int s, int msgtype, int family) {
    msg(NFT(msgtype), NLM_F_DUMP, family);
    mend();
    return talk(s, 1);
}

/* Does the last answer hold the string str? */
static int ans_has(const char *str) {
    int l = strlen(str);
    for (int i = 0; i + l <= anslen; i++)
        if (!memcmp(ans + i, str, l)) return 1;
    return 0;
}

static int listen_on(int type, int port) {
    int s = socket(AF_INET, type, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (void *)&a, sizeof a) < 0) return -1;
    if (type == SOCK_STREAM && listen(s, 4) < 0) return -1;
    return s;
}

/* Can a TCP connection to 127.0.0.1:port be made within about a second? */
static int tcp_reaches(int port) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(c, F_SETFL, O_NONBLOCK);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int r = connect(c, (void *)&a, sizeof a);
    int ok = 0;
    if (r == 0) ok = 1;
    else if (errno == EINPROGRESS) {
        struct pollfd p = {c, POLLOUT, 0};
        if (poll(&p, 1, 1000) == 1) {
            int e = 0;
            socklen_t l = sizeof e;
            getsockopt(c, SOL_SOCKET, SO_ERROR, &e, &l);
            ok = e == 0;
        }
    }
    close(c);
    return ok;
}

/* Does a datagram sent to 127.0.0.1:port reach a socket bound to it? */
static int udp_reaches(int rx, int port) {
    int c = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    sendto(c, "ping", 4, 0, (void *)&a, sizeof a);
    struct pollfd p = {rx, POLLIN, 0};
    int got = poll(&p, 1, 500) == 1;
    if (got) {
        char b[8];
        recv(rx, b, sizeof b, 0);
    }
    close(c);
    return got;
}

/* The counter packets of the one rule in `ans` (a GETRULE dump). */
static long rule_packets(void) {
    for (int i = 0; i + 24 <= anslen; i += 4) {
        if (!memcmp(ans + i + 4, "counter", 8)) {
            /* NAME attr, then DATA: BYTES (be64), PACKETS (be64) */
            for (int j = i; j + 12 <= anslen && j < i + 64; j += 4) {
                struct nla *a = (void *)(ans + j);
                if ((a->type & 0x3fff) == 2 && a->len == 12 && j > i + 16) {
                    uint32_t hi, lo;
                    memcpy(&hi, ans + j + 4, 4);
                    memcpy(&lo, ans + j + 8, 4);
                    return ((long)ntohl(hi) << 32) | ntohl(lo);
                }
            }
        }
    }
    return -1;
}

int main(void) {
    setup();
    int s = nf_socket();
    if (begin("netlink netfilter socket")) {
        if (s < 0) fail("socket: %s", strerror(errno));
        else pass();
    }
    if (s < 0) return done();

    if (begin("generation id")) {
        msg(NFT(MSG_GETGEN), 0, 0);
        mend();
        int e = talk(s, 1);
        if (e) fail("%d", e);
        else pass();
    }

    if (begin("a batch makes a table, a chain and rules")) {
        batch_begin();
        new_table("tt");
        new_chain("tt", "in", 1, 0, 1);
        rule_start("tt", "in", NLM_F_APPEND);
        exprs_open();
        match_dport(6, 7778);
        ex_counter();
        ex_verdict(0);
        exprs_close();
        rule_start("tt", "in", NLM_F_APPEND);
        exprs_open();
        match_dport(17, 7780);
        ex_verdict(0);
        exprs_close();
        batch_end();
        int e = talk(s, 4);
        if (e) fail("batch: %d", e);
        else pass();
    }

    if (begin("the table, the chain and the rules are listed")) {
        int e = dump(s, MSG_GETTABLE, 0);
        int t = ans_has("tt");
        e = e ? e : dump(s, MSG_GETCHAIN, 0);
        int c = ans_has("in") && ans_has("filter");
        e = e ? e : dump(s, MSG_GETRULE, 0);
        int r = ans_has("payload") && ans_has("counter") && ans_has("immediate");
        if (e) fail("%d", e);
        else if (!t || !c || !r) fail("table %d chain %d rule %d", t, c, r);
        else pass();
    }

    if (begin("a rule drops TCP to its port")) {
        int a = listen_on(SOCK_STREAM, 7778), b = listen_on(SOCK_STREAM, 7779);
        int x = tcp_reaches(7778), y = tcp_reaches(7779);
        if (a < 0 || b < 0) fail("listen");
        else if (x) fail("7778 was reached");
        else if (!y) fail("7779 was not reached");
        else pass();
        close(a);
        close(b);
    }

    if (begin("the rule's counter counted the dropped packets")) {
        int e = dump(s, MSG_GETRULE, 0);
        long n = rule_packets();
        if (e) fail("%d", e);
        else if (n < 1) fail("packets %ld", n);
        else pass();
    }

    if (begin("a rule drops UDP to its port")) {
        int a = listen_on(SOCK_DGRAM, 7780), b = listen_on(SOCK_DGRAM, 7781);
        int x = udp_reaches(a, 7780), y = udp_reaches(b, 7781);
        if (a < 0 || b < 0) fail("bind");
        else if (x) fail("7780 was reached");
        else if (!y) fail("7781 was not reached");
        else pass();
        close(a);
        close(b);
    }

    if (begin("a batch with a rule that cannot run changes nothing")) {
        batch_begin();
        new_table("tr");
        new_chain("tr", "c", 1, 0, 1);
        rule_start("tr", "c", NLM_F_APPEND);
        exprs_open();
        int e1, d1;
        ex_open(&e1, &d1, "nosuchexpression");
        ex_close(e1, d1);
        exprs_close();
        batch_end();
        int e = talk(s, 3);
        int e2 = dump(s, MSG_GETTABLE, 0);
        if (e == 0) fail("the batch was accepted");
        else if (e2) fail("dump: %d", e2);
        else if (ans_has("tr")) fail("table tr is there");
        else pass();
    }

    if (begin("a table that exists is refused with NLM_F_EXCL")) {
        batch_begin();
        msg(NFT(MSG_NEWTABLE), NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL, NFPROTO_INET);
        putstr(1, "tt");
        mend();
        batch_end();
        int e = talk(s, 1);
        if (e != -EEXIST && e != EEXIST) fail("got %d", e);
        else pass();
    }

    if (begin("connection state: only answers to a connection made here come in")) {
        batch_begin();
        new_table("tc");
        new_chain("tc", "in", 1, 0, 0);          /* policy drop */
        rule_start("tc", "in", NLM_F_APPEND);
        exprs_open();
        ex_ct_state(2 | 4);                      /* established, related */
        ex_verdict(1);
        exprs_close();
        rule_start("tc", "in", NLM_F_APPEND);
        exprs_open();
        match_dport(6, 7790);
        ex_verdict(1);
        exprs_close();
        batch_end();
        int e = talk(s, 4);
        int a = listen_on(SOCK_STREAM, 7790), b = listen_on(SOCK_STREAM, 7791);
        int x = tcp_reaches(7790), y = tcp_reaches(7791);
        if (e) fail("batch: %d", e);
        else if (a < 0 || b < 0) fail("listen");
        else if (!x) fail("7790 was not reached");
        else if (y) fail("7791 was reached");
        else pass();
        close(a);
        close(b);
    }

    if (begin("deleting the table lifts its rules")) {
        batch_begin();
        msg(NFT(MSG_DELTABLE), NLM_F_ACK, NFPROTO_INET);
        putstr(1, "tc");
        mend();
        msg(NFT(MSG_DELTABLE), NLM_F_ACK, NFPROTO_INET);
        putstr(1, "tt");
        mend();
        batch_end();
        int e = talk(s, 2);
        int a = listen_on(SOCK_STREAM, 7778), b = listen_on(SOCK_STREAM, 7791);
        int x = tcp_reaches(7778), y = tcp_reaches(7791);
        int e2 = dump(s, MSG_GETTABLE, 0);
        if (e) fail("delete: %d", e);
        else if (!x || !y) fail("still dropped: %d %d", x, y);
        else if (e2 || ans_has("tt") || ans_has("tc")) fail("listed after delete");
        else pass();
        close(a);
        close(b);
    }

    if (begin("a named set: a rule drops what the set holds, and elements can be removed")) {
        batch_begin();
        new_table("ts");
        new_chain("ts", "in", 1, 0, 1);
        msg(NFT(9), NLM_F_ACK | NLM_F_CREATE, NFPROTO_INET);    /* NEWSET */
        putstr(1, "ts");
        putstr(2, "bad");
        put32(3, 0);
        put32(4, 13);
        put32(5, 2);
        put32(10, 1);
        mend();
        for (int i = 0; i < 2; i++) {
            msg(NFT(12), NLM_F_ACK | NLM_F_CREATE, NFPROTO_INET); /* NEWSETELEM */
            putstr(1, "ts");
            putstr(2, "bad");
            int l = nla_open(3 | NLA_F_NESTED);
            int el = nla_open(1 | NLA_F_NESTED);
            uint16_t k = htons(7800 + i);
            data_value(1, &k, 2);
            nla_close(el);
            nla_close(l);
            mend();
        }
        rule_start("ts", "in", NLM_F_APPEND);
        exprs_open();
        uint8_t tcp = 6;
        uint16_t none = 0;
        (void)none;
        ex_meta(16, 8);
        ex_cmp(8, 0, &tcp, 1);
        ex_payload(2, 2, 2, 8);
        {
            int e, d;
            ex_open(&e, &d, "lookup");
            putstr(1, "bad");
            put32(2, 8);
            ex_close(e, d);
        }
        ex_verdict(0);
        exprs_close();
        batch_end();
        int e = talk(s, 6);
        int a = listen_on(SOCK_STREAM, 7800), b = listen_on(SOCK_STREAM, 7801),
            c = listen_on(SOCK_STREAM, 7802);
        int x = tcp_reaches(7800), y = tcp_reaches(7801), z = tcp_reaches(7802);
        /* Remove 7800 and try again. */
        batch_begin();
        msg(NFT(14), NLM_F_ACK, NFPROTO_INET);                  /* DELSETELEM */
        putstr(1, "ts");
        putstr(2, "bad");
        int l = nla_open(3 | NLA_F_NESTED);
        int el = nla_open(1 | NLA_F_NESTED);
        uint16_t k = htons(7800);
        data_value(1, &k, 2);
        nla_close(el);
        nla_close(l);
        mend();
        batch_end();
        int e2 = talk(s, 1);
        int x2 = tcp_reaches(7800), y2 = tcp_reaches(7801);
        if (e) fail("batch: %d", e);
        else if (a < 0 || b < 0 || c < 0) fail("listen");
        else if (x || y) fail("members reached: %d %d", x, y);
        else if (!z) fail("7802 was not reached");
        else if (e2) fail("delete element: %d", e2);
        else if (!x2) fail("7800 still dropped");
        else if (y2) fail("7801 reached");
        else pass();
        close(a);
        close(b);
        close(c);
    }

    if (begin("reject answers at once instead of dropping")) {
        batch_begin();
        new_table("tr");
        new_chain("tr", "in", 1, 0, 1);
        rule_start("tr", "in", NLM_F_APPEND);
        exprs_open();
        match_dport(6, 7810);
        {
            int e, d;
            ex_open(&e, &d, "reject");
            put32(1, 1);                                        /* tcp reset */
            ex_close(e, d);
        }
        exprs_close();
        batch_end();
        int e = talk(s, 3);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(7810)};
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        int c = socket(AF_INET, SOCK_STREAM, 0);
        struct timeval t0, t1;
        gettimeofday(&t0, 0);
        int r = connect(c, (void *)&a, sizeof a);
        int er = errno;
        gettimeofday(&t1, 0);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
        close(c);
        if (e) fail("batch: %d", e);
        else if (r == 0) fail("connected");
        else if (er != ECONNREFUSED) fail("errno %d after %ld ms", er, ms);
        else pass();
    }

    if (begin("limit lets a burst through and then drops")) {
        batch_begin();
        new_table("tl");
        new_chain("tl", "in", 1, 0, 1);
        rule_start("tl", "in", NLM_F_APPEND);
        exprs_open();
        match_dport(17, 7820);
        {
            int e, d;
            ex_open(&e, &d, "limit");
            put64(1, 1);                                        /* 1 per */
            put64(2, 3600);                                     /* hour  */
            put32(3, 2);                                        /* burst 2 */
            put32(4, 0);
            put32(5, 1);                                        /* inverted: over the limit */
            ex_close(e, d);
        }
        ex_verdict(0);
        exprs_close();
        batch_end();
        int e = talk(s, 3);
        int rx = listen_on(SOCK_DGRAM, 7820);
        int got = 0;
        for (int i = 0; i < 6; i++) got += udp_reaches(rx, 7820);
        if (e) fail("batch: %d", e);
        else if (rx < 0) fail("bind");
        else if (got < 2 || got > 4) fail("%d of 6 arrived", got);
        else pass();
        close(rx);
    }

    if (begin("stale references: a set a rule uses cannot go, and a vanished jump target is skipped")) {
        /* The set "bad" in table ts is read by a rule. */
        batch_begin();
        msg(NFT(11), NLM_F_ACK, NFPROTO_INET);                  /* DELSET */
        putstr(1, "ts");
        putstr(2, "bad");
        mend();
        batch_end();
        int e = talk(s, 1);
        /* A chain that is only jumped to, and then deleted under the jump. */
        batch_begin();
        new_table("tj");
        new_chain("tj", "in", 1, 0, 1);
        msg(NFT(MSG_NEWCHAIN), NLM_F_ACK | NLM_F_CREATE, NFPROTO_INET);
        putstr(1, "tj");
        putstr(3, "sub");
        mend();
        rule_start("tj", "in", NLM_F_APPEND);
        exprs_open();
        match_dport(6, 7830);
        {
            int ee, d;
            ex_open(&ee, &d, "immediate");
            put32(1, 0);
            int dd = nla_open(2 | NLA_F_NESTED);
            int v = nla_open(2 | NLA_F_NESTED);
            put32(1, (uint32_t)-3);                             /* NFT_JUMP */
            putstr(2, "sub");
            nla_close(v);
            nla_close(dd);
            ex_close(ee, d);
        }
        exprs_close();
        batch_end();
        int e2 = talk(s, 4);
        batch_begin();
        msg(NFT(MSG_DELCHAIN), NLM_F_ACK, NFPROTO_INET);
        putstr(1, "tj");
        putstr(3, "sub");
        mend();
        batch_end();
        int e3 = talk(s, 1);
        int a = listen_on(SOCK_STREAM, 7830);
        int x = tcp_reaches(7830);
        if (e != 16) fail("deleting a set in use gave %d", e);
        else if (e2) fail("setup: %d", e2);
        else if (e3 != 0 && e3 != 16) fail("delete chain: %d", e3);
        else if (!x) fail("the vanished jump target blocked the packet");
        else pass();
        close(a);
    }

    {
        const struct { const char *name; const unsigned char *d; const int *l; int n; } reps[] = {
            {"nft 1.0.9 loads a ruleset with sets, intervals and counters", rep_r3, rep_r3_len, REP_R3_N},
            {"nft 1.0.9 loads a ruleset with limit, jump, reject, ct and notrack", rep_r4, rep_r4_len, REP_R4_N},
        };
        for (int i = 0; i < 2; i++) {
            if (!begin(reps[i].name)) continue;
            int bad = 0, at = 0;
            for (int k = 0; k < reps[i].n; k++) {
                int len = reps[i].l[k];
                if (send(s, reps[i].d + at, len, 0) != len) { bad = -1; break; }
                at += len;
                char rb[8192];
                struct timeval tv = {1, 0};
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                for (;;) {
                    struct pollfd pf = {s, POLLIN, 0};
                    if (poll(&pf, 1, 300) != 1) break;
                    ssize_t n = recv(s, rb, sizeof rb, 0);
                    if (n <= 0) break;
                    for (char *p = rb; p < rb + n;) {
                        struct nlh *h = (void *)p;
                        if (h->len < 16) break;
                        if (h->type == NLMSG_ERROR && *(int *)(p + 16)) bad = -*(int *)(p + 16);
                        p += (h->len + 3) & ~3u;
                    }
                }
            }
            if (bad) fail("request failed: %d", bad);
            else pass();
        }
    }

    /* Leave nothing behind: later programs of the run share this network. */
    {
        const char *left[] = {"ts", "tr", "tl", "tj", "f3", "f4", "tc", "tt"};
        for (unsigned i = 0; i < sizeof left / sizeof *left; i++) {
            batch_begin();
            msg(NFT(MSG_DELTABLE), NLM_F_ACK, NFPROTO_INET);
            putstr(1, left[i]);
            mend();
            batch_end();
            talk(s, 1);
        }
    }

    close(s);
    return done();
}
