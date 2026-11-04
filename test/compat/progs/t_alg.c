/* AF_ALG hash sockets: sha256, sha384, sha512, sha1, md5, hmac(sha256), hmac(sha384), hmac(sha1) and hmac(md5)
 * through socket, bind, setsockopt(ALG_SET_KEY), accept, send and read, with
 * the published test vectors. The last checks are about what the kernel
 * keeps: the data and the key are copies, so changing the memory that was
 * sent afterwards changes nothing, and a bad address or an oversize send
 * leaves the message as it was. */
#define _GNU_SOURCE
#include "compat.h"
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

#ifndef EBADMSG
#define EBADMSG 74
#endif
#ifndef ENOKEY
#define ENOKEY 126
#endif
#ifndef AF_ALG
#define AF_ALG 38
#endif
#ifndef SOL_ALG
#define SOL_ALG 279
#endif
#define ALG_SET_KEY 1

struct salg {
    uint16_t family;
    char type[14];
    uint32_t feat, mask;
    char name[64];
};

static int bound(const char *type, const char *name) {
    int s = socket(AF_ALG, SOCK_SEQPACKET, 0);
    if (s < 0) return -1;
    struct salg a;
    memset(&a, 0, sizeof a);
    a.family = AF_ALG;
    strncpy(a.type, type, sizeof a.type - 1);
    strncpy(a.name, name, sizeof a.name - 1);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    return s;
}

/* A socket that is bound to NAME, given KEY if it is an hmac, and accepted. */
static int op_for(const char *name, const void *key, int klen) {
    int s = bound("hash", name);
    if (s < 0) return -1;
    if (key && setsockopt(s, SOL_ALG, ALG_SET_KEY, key, klen) < 0) {
        close(s);
        return -1;
    }
    int op = accept(s, NULL, NULL);
    int e = errno;
    close(s);
    errno = e;
    return op;
}

static void hex(const unsigned char *d, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* The digest of DATA by NAME (with KEY), in hex; an empty string on failure. */
static const char *digest(const char *name, const void *key, int klen, const void *data, int n) {
    static char out[132];
    unsigned char d[64];
    out[0] = 0;
    int op = op_for(name, key, klen);
    if (op < 0) return out;
    if (n > 0 && send(op, data, n, 0) != n) { close(op); return out; }
    int want = strstr(name, "sha512") ? 64 : strstr(name, "384") ? 48 : strstr(name, "sha1") ? 20 : strstr(name, "md5") ? 16 : 32;
    if (read(op, d, sizeof d) != want) { close(op); return out; }
    hex(d, want, out);
    close(op);
    return out;
}

#define ALG_SET_IV 2
#define ALG_SET_OP 3
#define ALG_SET_AEAD_ASSOCLEN 4
#define ALG_SET_AEAD_AUTHSIZE 5

/* One AEAD request on OP: the control messages, IN (the associated data and
 * then the text, or the ciphertext and tag), and the answer read into OUT.
 * Returns what read gave, or -errno of the send or read. */
static long aead_run(int op, int enc, const unsigned char *iv, unsigned ivlen, unsigned assoclen,
                     const void *in, int inlen, void *out, int outcap) {
    union { char b[256]; struct cmsghdr a; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    struct iovec v = { (void *)in, inlen };
    m.msg_iov = &v;
    m.msg_iovlen = 1;
    m.msg_control = ctl.b;
    struct cmsghdr *c = (struct cmsghdr *)ctl.b;
    size_t used = 0;
    c->cmsg_level = SOL_ALG;
    c->cmsg_type = ALG_SET_OP;
    c->cmsg_len = CMSG_LEN(4);
    *(uint32_t *)CMSG_DATA(c) = enc ? 1 : 0;
    used += CMSG_SPACE(4);
    c = (struct cmsghdr *)(ctl.b + used);
    c->cmsg_level = SOL_ALG;
    c->cmsg_type = ALG_SET_IV;
    c->cmsg_len = CMSG_LEN(4 + ivlen);
    *(uint32_t *)CMSG_DATA(c) = ivlen;
    memcpy(CMSG_DATA(c) + 4, iv, ivlen);
    used += CMSG_SPACE(4 + ivlen);
    c = (struct cmsghdr *)(ctl.b + used);
    c->cmsg_level = SOL_ALG;
    c->cmsg_type = ALG_SET_AEAD_ASSOCLEN;
    c->cmsg_len = CMSG_LEN(4);
    *(uint32_t *)CMSG_DATA(c) = assoclen;
    used += CMSG_SPACE(4);
    m.msg_controllen = used;
    if (sendmsg(op, &m, 0) < 0) return -errno;
    long r = read(op, out, outcap);
    return r < 0 ? -errno : r;
}

static int aead_op(const char *name, const void *key, int klen) {
    int s = bound("aead", name);
    if (s < 0) return -1;
    if (key && setsockopt(s, SOL_ALG, ALG_SET_KEY, key, klen) < 0) { close(s); return -1; }
    int op = accept(s, NULL, NULL);
    int e = errno;
    close(s);
    errno = e;
    return op;
}

#define SHA256_ABC "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define SHA256_EMPTY "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define SHA384_ABC "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"
#define SHA512_ABC "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"
#define HMAC256_1 "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
#define HMAC256_2 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
#define HMAC384_2 "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e8e2240ca5e69e2c78b3239ecfab21649"

int main(void) {
    setup();
    EXPECT("socket(AF_ALG, SOCK_SEQPACKET) makes a socket", ({ int s = socket(AF_ALG, SOCK_SEQPACKET, 0); int r = s >= 0; if (r) close(s); r; }));
    EXPECT("a datagram AF_ALG socket is refused", ({ int s = socket(AF_ALG, SOCK_DGRAM, 0); int r = s < 0 && errno == ESOCKTNOSUPPORT; if (s >= 0) close(s); r; }));
    EXPECT("an unknown algorithm is ENOENT", ({ int s = bound("hash", "nosuchhash"); s < 0 && errno == ENOENT; }));
    EXPECT("a type that is not hash is ENOENT", ({ int s = bound("rng", "stdrng"); s < 0 && errno == ENOENT; }));
    EXPECT("sha256 of abc", strcmp(digest("sha256", NULL, 0, "abc", 3), SHA256_ABC) == 0);
    EXPECT("sha256 of nothing", strcmp(digest("sha256", NULL, 0, NULL, 0), SHA256_EMPTY) == 0);
    EXPECT("sha384 of abc", strcmp(digest("sha384", NULL, 0, "abc", 3), SHA384_ABC) == 0);
    EXPECT("sha512 of abc", strcmp(digest("sha512", NULL, 0, "abc", 3), SHA512_ABC) == 0);
    EXPECT("hmac(sha256), RFC 4231 test case 1", ({
        unsigned char key[20];
        memset(key, 0x0b, sizeof key);
        strcmp(digest("hmac(sha256)", key, 20, "Hi There", 8), HMAC256_1) == 0;
    }));
    EXPECT("hmac(sha256), RFC 4231 test case 2", strcmp(digest("hmac(sha256)", "Jefe", 4, "what do ya want for nothing?", 28), HMAC256_2) == 0);
    EXPECT("hmac(sha384), RFC 4231 test case 2", strcmp(digest("hmac(sha384)", "Jefe", 4, "what do ya want for nothing?", 28), HMAC384_2) == 0);
    EXPECT("sha1 of abc", strcmp(digest("sha1", NULL, 0, "abc", 3), "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
    EXPECT("md5 of abc", strcmp(digest("md5", NULL, 0, "abc", 3), "900150983cd24fb0d6963f7d28e17f72") == 0);
    EXPECT("hmac(sha1), RFC 2202 test case 1", ({
        unsigned char key[20];
        memset(key, 0x0b, sizeof key);
        strcmp(digest("hmac(sha1)", key, 20, "Hi There", 8), "b617318655057264e28bc0b6fb378c8ef146be00") == 0;
    }));
    EXPECT("hmac(sha1), RFC 2202 test case 2", strcmp(digest("hmac(sha1)", "Jefe", 4, "what do ya want for nothing?", 28), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79") == 0);
    EXPECT("hmac(md5), RFC 2202 test case 1", ({
        unsigned char key[16];
        memset(key, 0x0b, sizeof key);
        strcmp(digest("hmac(md5)", key, 16, "Hi There", 8), "9294727a3638bb1c13f48ef8158bfc9d") == 0;
    }));
    EXPECT("hmac(md5), RFC 2202 test case 2", strcmp(digest("hmac(md5)", "Jefe", 4, "what do ya want for nothing?", 28), "750c783e6ab0b503eaa86e310a5db738") == 0);
    EXPECT("accepting hmac(sha1) with no key is ENOKEY", ({ int s = bound("hash", "hmac(sha1)"); int op = accept(s, NULL, NULL); int r = op < 0 && errno == ENOKEY; close(s); r; }));
    EXPECT("a key setting on sha1 is ENOSYS", ({ int s = bound("hash", "sha1"); int r = setsockopt(s, SOL_ALG, ALG_SET_KEY, "k", 1) < 0 && errno == ENOSYS; close(s); r; }));
    EXPECT("sha1 is not an aead", ({ int s = bound("aead", "sha1"); s < 0 && errno == ENOENT; }));
    EXPECT("a key setting on a hash with no key is ENOSYS", ({ int s = bound("hash", "sha256"); int r = setsockopt(s, SOL_ALG, ALG_SET_KEY, "k", 1) < 0 && errno == ENOSYS; close(s); r; }));
    EXPECT("accepting an hmac with no key is ENOKEY", ({ int s = bound("hash", "hmac(sha256)"); int op = accept(s, NULL, NULL); int r = op < 0 && errno == ENOKEY; close(s); r; }));
    EXPECT("accepting on a socket not bound is EINVAL", ({ int s = socket(AF_ALG, SOCK_SEQPACKET, 0); int op = accept(s, NULL, NULL); int r = op < 0 && errno == EINVAL; close(s); r; }));
    EXPECT("a key longer than the socket takes is EINVAL", ({
        char big[300];
        memset(big, 'k', sizeof big);
        int s = bound("hash", "hmac(sha256)");
        int r = setsockopt(s, SOL_ALG, ALG_SET_KEY, big, sizeof big) < 0 && errno == EINVAL;
        close(s);
        r;
    }));
    EXPECT("sending to a socket that was not accepted is refused", ({ int s = bound("hash", "sha256"); int r = send(s, "abc", 3, 0) < 0; close(s); r; }));
    EXPECT("pieces sent with MSG_MORE hash as one message", ({
        int op = op_for("sha256", NULL, 0);
        unsigned char d[32];
        char h[65];
        send(op, "a", 1, MSG_MORE);
        send(op, "b", 1, MSG_MORE);
        send(op, "c", 1, 0);
        int r = read(op, d, 32) == 32;
        hex(d, 32, h);
        close(op);
        r && strcmp(h, SHA256_ABC) == 0;
    }));
    EXPECT("sendmsg with three buffers is one message", ({
        int op = op_for("sha256", NULL, 0);
        struct iovec v[3] = { { "a", 1 }, { "b", 1 }, { "c", 1 } };
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_iov = v;
        m.msg_iovlen = 3;
        unsigned char d[32];
        char h[65];
        int r = sendmsg(op, &m, 0) == 3 && read(op, d, 32) == 32;
        hex(d, 32, h);
        close(op);
        r && strcmp(h, SHA256_ABC) == 0;
    }));
    EXPECT("reading the digest begins a new message", ({
        int op = op_for("sha256", NULL, 0);
        unsigned char d[32];
        char h1[65], h2[65];
        send(op, "abc", 3, 0);
        read(op, d, 32);
        hex(d, 32, h1);
        send(op, "abc", 3, 0);
        read(op, d, 32);
        hex(d, 32, h2);
        close(op);
        strcmp(h1, SHA256_ABC) == 0 && strcmp(h2, SHA256_ABC) == 0;
    }));
    EXPECT("a short read returns the start of the digest", ({
        int op = op_for("sha256", NULL, 0);
        unsigned char d[16];
        char h[33];
        send(op, "abc", 3, 0);
        int r = read(op, d, 16) == 16;
        hex(d, 16, h);
        close(op);
        r && strncmp(h, SHA256_ABC, 32) == 0;
    }));
    EXPECT("the data sent are a copy: changing the buffer afterwards changes nothing", ({
        char buf[4] = "abc";
        int op = op_for("sha256", NULL, 0);
        unsigned char d[32];
        char h[65];
        send(op, buf, 3, 0);
        memset(buf, 'x', 3);
        int r = read(op, d, 32) == 32;
        hex(d, 32, h);
        close(op);
        r && strcmp(h, SHA256_ABC) == 0;
    }));
    EXPECT("the key is a copy: a later key for the same socket does not change an accepted one", ({
        int s = bound("hash", "hmac(sha256)");
        setsockopt(s, SOL_ALG, ALG_SET_KEY, "Jefe", 4);
        int op = accept(s, NULL, NULL);
        setsockopt(s, SOL_ALG, ALG_SET_KEY, "other", 5);
        unsigned char d[32];
        char h[65];
        send(op, "what do ya want for nothing?", 28, 0);
        int r = read(op, d, 32) == 32;
        hex(d, 32, h);
        close(op);
        close(s);
        r && strcmp(h, HMAC256_2) == 0;
    }));
    EXPECT("an accepted socket works after the bound one is closed", ({
        int s = bound("hash", "sha256");
        int op = accept(s, NULL, NULL);
        close(s);
        unsigned char d[32];
        char h[65];
        send(op, "abc", 3, 0);
        int r = read(op, d, 32) == 32;
        hex(d, 32, h);
        close(op);
        r && strcmp(h, SHA256_ABC) == 0;
    }));
    EXPECT("a bad address is EFAULT and the message is as it was", ({
        int op = op_for("sha256", NULL, 0);
        unsigned char d[32];
        char h[65];
        send(op, "abc", 3, MSG_MORE);
        int bad = send(op, (void *)8, 5, 0);
        int e = errno;
        int r = read(op, d, 32) == 32;
        hex(d, 32, h);
        close(op);
        bad < 0 && e == EFAULT && r && strcmp(h, SHA256_ABC) == 0;
    }));
    EXPECT("a message past the limit is refused and the message is as it was", ({
        static char big[70000];
        int op = op_for("sha256", NULL, 0);
        unsigned char d[32];
        char h[65];
        send(op, "abc", 3, MSG_MORE);
        int bad = send(op, big, sizeof big, 0);
        int e = errno;
        int r = read(op, d, 32) == 32;
        hex(d, 32, h);
        close(op);
        bad < 0 && e == EMSGSIZE && r && strcmp(h, SHA256_ABC) == 0;
    }));
    EXPECT("poll reports an accepted socket ready", ({
        int op = op_for("sha256", NULL, 0);
        struct pollfd p = { op, POLLIN | POLLOUT, 0 };
        int r = poll(&p, 1, 0) == 1 && (p.revents & POLLOUT);
        close(op);
        r;
    }));
    EXPECT("sockets are limited, and closing them gives the room back", ({
        int fds[40], n = 0;
        while (n < 40 && (fds[n] = socket(AF_ALG, SOCK_SEQPACKET, 0)) >= 0) n++;
        int full = n < 40 && errno == ENOMEM;
        for (int i = 0; i < n; i++) close(fds[i]);
        int again = socket(AF_ALG, SOCK_SEQPACKET, 0);
        int r = full && again >= 0;
        if (again >= 0) close(again);
        r;
    }));
    {
        static const unsigned char zero[32];
        static const unsigned char gct[32] = {
            0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92, 0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78,
            0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd, 0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf };
        unsigned char out[128], back[128];
        EXPECT("gcm(aes) seals the NIST test case 2", ({
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 1, zero, 12, 0, zero, 16, out, sizeof out);
            close(op);
            r == 32 && memcmp(out, gct, 32) == 0;
        }));
        EXPECT("gcm(aes) opens it again", ({
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 0, zero, 12, 0, gct, 32, back, sizeof back);
            close(op);
            r == 16 && memcmp(back, zero, 16) == 0;
        }));
        EXPECT("gcm(aes) with a 32-byte key and associated data comes back, and the data come first", ({
            unsigned char key[32], in[40];
            memset(key, 7, 32);
            memcpy(in, "ASSOCIAT", 8);
            memset(in + 8, 'p', 32);
            int op = aead_op("gcm(aes)", key, 32);
            long r = aead_run(op, 1, zero, 12, 8, in, 40, out, sizeof out);
            long r2 = r > 0 ? aead_run(op, 0, zero, 12, 8, out, r, back, sizeof back) : -1;
            close(op);
            r == 56 && memcmp(out, "ASSOCIAT", 8) == 0 && r2 == 40 && memcmp(back, in, 40) == 0;
        }));
        EXPECT("a changed associated datum is EBADMSG", ({
            unsigned char in[24];
            memcpy(in, "ASSOCIAT", 8);
            memset(in + 8, 'p', 16);
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 1, zero, 12, 8, in, 24, out, sizeof out);
            out[0] ^= 1;
            long r2 = aead_run(op, 0, zero, 12, 8, out, r, back, sizeof back);
            close(op);
            r == 40 && r2 == -EBADMSG;
        }));
        EXPECT("a changed tag is EBADMSG and no text comes out", ({
            unsigned char bad[32];
            memcpy(bad, gct, 32);
            bad[31] ^= 0x80;
            memset(back, 0x5a, sizeof back);
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 0, zero, 12, 0, bad, 32, back, sizeof back);
            close(op);
            r == -EBADMSG && back[0] == 0x5a && back[15] == 0x5a;
        }));
        EXPECT("a changed ciphertext byte is EBADMSG", ({
            unsigned char bad[32];
            memcpy(bad, gct, 32);
            bad[3] ^= 1;
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 0, zero, 12, 0, bad, 32, back, sizeof back);
            close(op);
            r == -EBADMSG;
        }));
        EXPECT("a request with no operation is EINVAL", ({
            int op = aead_op("gcm(aes)", zero, 16);
            send(op, zero, 16, 0);
            long r = read(op, out, sizeof out);
            int e = errno;
            close(op);
            r < 0 && e == EINVAL;
        }));
        EXPECT("a result buffer too small is EINVAL", ({
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 1, zero, 12, 0, zero, 16, out, 31);
            close(op);
            r == -EINVAL;
        }));
        EXPECT("an IV that is not 12 bytes is EINVAL", ({
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 1, zero, 8, 0, zero, 16, out, sizeof out);
            close(op);
            r == -EINVAL;
        }));
        EXPECT("associated data longer than the message is EINVAL", ({
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 1, zero, 12, 17, zero, 16, out, sizeof out);
            close(op);
            r == -EINVAL;
        }));
        EXPECT("a ciphertext shorter than a tag is EINVAL", ({
            int op = aead_op("gcm(aes)", zero, 16);
            long r = aead_run(op, 0, zero, 12, 0, zero, 15, back, sizeof back);
            close(op);
            r == -EINVAL;
        }));
        EXPECT("the text is copied: changing the buffer after the send changes nothing", ({
            unsigned char buf[16];
            memset(buf, 0, 16);
            int op = aead_op("gcm(aes)", zero, 16);
            union { char b[64]; struct cmsghdr a; } ctl;
            memset(&ctl, 0, sizeof ctl);
            struct msghdr m;
            memset(&m, 0, sizeof m);
            struct iovec v = { buf, 16 };
            m.msg_iov = &v;
            m.msg_iovlen = 1;
            m.msg_control = ctl.b;
            struct cmsghdr *c = (struct cmsghdr *)ctl.b;
            c->cmsg_level = SOL_ALG;
            c->cmsg_type = ALG_SET_OP;
            c->cmsg_len = CMSG_LEN(4);
            *(uint32_t *)CMSG_DATA(c) = 1;
            m.msg_controllen = CMSG_SPACE(4);
            int ok = sendmsg(op, &m, 0) == 16;
            memset(buf, 0xff, 16);
            /* no IV yet: give it in a second message with no data */
            struct msghdr m2;
            memset(&m2, 0, sizeof m2);
            memset(&ctl, 0, sizeof ctl);
            c = (struct cmsghdr *)ctl.b;
            c->cmsg_level = SOL_ALG;
            c->cmsg_type = ALG_SET_IV;
            c->cmsg_len = CMSG_LEN(16);
            *(uint32_t *)CMSG_DATA(c) = 12;
            m2.msg_control = ctl.b;
            m2.msg_controllen = CMSG_SPACE(16);
            ok = ok && sendmsg(op, &m2, 0) == 0;
            long r = read(op, out, sizeof out);
            close(op);
            ok && r == 32 && memcmp(out, gct, 32) == 0;
        }));
        EXPECT("rfc7539(chacha20,poly1305) opens what it sealed, with associated data", ({
            unsigned char key[32], iv[12], in[30];
            memset(key, 3, 32);
            memset(iv, 9, 12);
            memcpy(in, "header", 6);
            memset(in + 6, 'x', 24);
            int op = aead_op("rfc7539(chacha20,poly1305)", key, 32);
            long r = aead_run(op, 1, iv, 12, 6, in, 30, out, sizeof out);
            long r2 = r > 0 ? aead_run(op, 0, iv, 12, 6, out, r, back, sizeof back) : -1;
            close(op);
            r == 46 && memcmp(out, "header", 6) == 0 && memcmp(out + 6, in + 6, 24) != 0
                && r2 == 30 && memcmp(back, in, 30) == 0;
        }));
        EXPECT("rfc7539 refuses a changed tag", ({
            unsigned char key[32], iv[12], in[8];
            memset(key, 3, 32);
            memset(iv, 9, 12);
            memset(in, 'x', 8);
            int op = aead_op("rfc7539(chacha20,poly1305)", key, 32);
            long r = aead_run(op, 1, iv, 12, 0, in, 8, out, sizeof out);
            out[r - 1] ^= 1;
            long r2 = aead_run(op, 0, iv, 12, 0, out, r, back, sizeof back);
            close(op);
            r == 24 && r2 == -EBADMSG;
        }));
        EXPECT("AEAD key lengths are checked", ({
            char k24[24] = { 0 };
            int s = bound("aead", "gcm(aes)");
            int a = setsockopt(s, SOL_ALG, ALG_SET_KEY, k24, 24) < 0 && errno == EINVAL;
            close(s);
            s = bound("aead", "rfc7539(chacha20,poly1305)");
            int b = setsockopt(s, SOL_ALG, ALG_SET_KEY, zero, 16) < 0 && errno == EINVAL;
            close(s);
            a && b;
        }));
        EXPECT("an AEAD with no key cannot be accepted", ({ int s = bound("aead", "gcm(aes)"); int op = accept(s, NULL, NULL); int r = op < 0 && errno == ENOKEY; close(s); r; }));
        EXPECT("the tag size can be set to 16 and not to 8", ({
            int s = bound("aead", "gcm(aes)");
            int a = setsockopt(s, SOL_ALG, ALG_SET_AEAD_AUTHSIZE, NULL, 16) == 0;
            int b = setsockopt(s, SOL_ALG, ALG_SET_AEAD_AUTHSIZE, NULL, 8) < 0 && errno == EINVAL;
            close(s);
            a && b;
        }));
        EXPECT("a hash name under type aead, and an AEAD name under type hash, are ENOENT", ({
            int a = bound("aead", "sha256") < 0 && errno == ENOENT;
            int b = bound("hash", "gcm(aes)") < 0 && errno == ENOENT;
            a && b;
        }));
    }
    return done();
}
