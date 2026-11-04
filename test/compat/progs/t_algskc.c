/* AF_ALG symmetric ciphers: ctr(aes), cbc(aes) and ecb(aes) through socket, bind,
 * setsockopt(ALG_SET_KEY), accept, sendmsg with ALG_SET_OP and ALG_SET_IV,
 * and read, with the vectors of NIST SP 800-38A (F.1, F.2 and F.5, both key
 * sizes, and AES-192). Also what the kernel keeps and what it refuses: the input is a
 * copy (changing the memory that was sent changes nothing), the result goes
 * only to the buffer read into, a CBC message that is not whole blocks, a
 * buffer too small, a wrong IV length, a missing operation or IV and an
 * a key of 23 bytes are errors that leave no output, and the IV goes on from one
 * message to the next as in Linux. Last, sendfile and splice of a file's
 * bytes into the socket: they are a copy, and the file is never written to
 * (decrypting from a file must not turn the file into the plaintext), and
 * changing the file after the call changes nothing. */
#define _GNU_SOURCE
#include "compat.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/uio.h>

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
#define ALG_SET_IV 2
#define ALG_SET_OP 3
#define ALG_OP_DECRYPT 0
#define ALG_OP_ENCRYPT 1

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

static int skc_op(const char *name, const void *key, int klen) {
    int s = bound("skcipher", name);
    if (s < 0) return -1;
    if (key && setsockopt(s, SOL_ALG, ALG_SET_KEY, key, klen) < 0) { close(s); return -1; }
    int op = accept(s, NULL, NULL);
    int e = errno;
    close(s);
    errno = e;
    return op;
}

static int unhex(const char *h, unsigned char *out) {
    int n = strlen(h) / 2;
    for (int i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = v;
    }
    return n;
}

/* Sends IN with the operation and, if iv is not NULL, an IV of ivlen bytes
 * (op < 0: no operation message). Returns the send's result or -errno. */
static long skc_send(int op, int enc, const unsigned char *iv, unsigned ivlen, const void *in, int inlen) {
    union { char b[256]; struct cmsghdr a; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    struct iovec v = { (void *)in, inlen };
    m.msg_iov = &v;
    m.msg_iovlen = 1;
    size_t used = 0;
    if (enc >= 0) {
        struct cmsghdr *c = (struct cmsghdr *)ctl.b;
        c->cmsg_level = SOL_ALG;
        c->cmsg_type = ALG_SET_OP;
        c->cmsg_len = CMSG_LEN(4);
        *(uint32_t *)CMSG_DATA(c) = enc ? ALG_OP_ENCRYPT : ALG_OP_DECRYPT;
        used += CMSG_SPACE(4);
    }
    if (iv) {
        struct cmsghdr *c = (struct cmsghdr *)(ctl.b + used);
        c->cmsg_level = SOL_ALG;
        c->cmsg_type = ALG_SET_IV;
        c->cmsg_len = CMSG_LEN(4 + ivlen);
        *(uint32_t *)CMSG_DATA(c) = ivlen;
        memcpy(CMSG_DATA(c) + 4, iv, ivlen);
        used += CMSG_SPACE(4 + ivlen);
    }
    if (used) {
        m.msg_control = ctl.b;
        m.msg_controllen = used;
    }
    long r = sendmsg(op, &m, 0);
    return r < 0 ? -errno : r;
}

/* One request: send, then read into OUT. Returns what read gave or -errno
 * (a failed send is returned as it is). */
static long skc_run(int op, int enc, const unsigned char *iv, unsigned ivlen, const void *in, int inlen, void *out, int cap) {
    long s = skc_send(op, enc, iv, ivlen, in, inlen);
    if (s < 0) return s;
    long r = read(op, out, cap);
    return r < 0 ? -errno : r;
}

/* Does the request give exactly the bytes of the hex text WANT? */
static int same(int op, int enc, const char *ivh, const char *inh, const char *wanth) {
    unsigned char iv[16], in[64], want[64], out[64];
    unsigned ivn = unhex(ivh, iv);
    int n = unhex(inh, in);
    int w = unhex(wanth, want);
    long r = skc_run(op, enc, iv, ivn, in, n, out, sizeof out);
    return r == w && memcmp(out, want, w) == 0;
}

#define K128 "2b7e151628aed2a6abf7158809cf4f3c"
#define K192 "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b"
#define K256 "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"
#define P12 "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
#define IVCBC "000102030405060708090a0b0c0d0e0f"
#define IVCTR "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"

int main(void) {
    setup();
    unsigned char k128[16], k256[32], k192[24], k23[23] = {1}, zero[16] = {0}, ff[16], out[128];
    unhex(K128, k128);
    unhex(K192, k192);
    unhex(K256, k256);
    memset(ff, 0xff, sizeof ff);

    EXPECT("a skcipher socket binds to cbc(aes) and ctr(aes)", ({
        int a = bound("skcipher", "cbc(aes)"), b = bound("skcipher", "ctr(aes)");
        int ok = a >= 0 && b >= 0;
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        ok;
    }));
    EXPECT("an unknown skcipher is ENOENT, and so is a hash name of that type", ({
        int a = bound("skcipher", "xts(aes)");
        int e1 = errno;
        int b = bound("skcipher", "sha256");
        int e2 = errno;
        a < 0 && e1 == ENOENT && b < 0 && e2 == ENOENT;
    }));
    EXPECT("an skcipher cannot be bound as a hash or an aead", ({
        int a = bound("hash", "cbc(aes)");
        int b = bound("aead", "ctr(aes)");
        a < 0 && b < 0;
    }));

    EXPECT("cbc(aes) with a 128-bit key encrypts the SP 800-38A F.2.1 message", ({
        int op = skc_op("cbc(aes)", k128, 16);
        int ok = op >= 0 && same(op, 1, IVCBC, P12,
            "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2");
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("and decrypts it", ({
        int op = skc_op("cbc(aes)", k128, 16);
        int ok = op >= 0 && same(op, 0, IVCBC,
            "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", P12);
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("cbc(aes) with a 256-bit key encrypts F.2.5 and decrypts it", ({
        int op = skc_op("cbc(aes)", k256, 32);
        int ok = op >= 0 && same(op, 1, IVCBC, P12,
            "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d");
        ok = ok && same(op, 0, IVCBC,
            "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d", P12);
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("ctr(aes) with a 128-bit key encrypts the SP 800-38A F.5.1 message and decrypts it", ({
        int op = skc_op("ctr(aes)", k128, 16);
        int ok = op >= 0 && same(op, 1, IVCTR, P12,
            "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff");
        ok = ok && same(op, 0, IVCTR,
            "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", P12);
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("ctr(aes) with a 256-bit key gives F.5.5", ({
        int op = skc_op("ctr(aes)", k256, 32);
        int ok = op >= 0 && same(op, 1, IVCTR, P12,
            "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5");
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("cbc(aes) with a 192-bit key encrypts SP 800-38A F.2.3 and decrypts it", ({
        int op = skc_op("cbc(aes)", k192, 24);
        int ok = op >= 0 && same(op, 1, IVCBC, P12, "4f021db243bc633d7178183a9fa071e8b4d9ada9ad7dedf4e5e738763f69145a");
        ok = ok && same(op, 0, IVCBC, "4f021db243bc633d7178183a9fa071e8b4d9ada9ad7dedf4e5e738763f69145a", P12);
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("ctr(aes) with a 192-bit key gives F.5.3", ({
        int op = skc_op("ctr(aes)", k192, 24);
        int ok = op >= 0 && same(op, 1, IVCTR, P12, "1abc932417521ca24f2b0459fe7e6e0b090339ec0aa6faefd5ccc2c6f4ce8e94");
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("ecb(aes) with a 128-bit key encrypts SP 800-38A F.1.1 and decrypts it, with no IV", ({
        int op = skc_op("ecb(aes)", k128, 16);
        unsigned char in[32], want[32];
        unhex(P12, in);
        unhex("3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf", want);
        long r = op >= 0 ? skc_run(op, 1, NULL, 0, in, 32, out, sizeof out) : -1;
        int ok = r == 32 && memcmp(out, want, 32) == 0;
        long d = op >= 0 ? skc_run(op, 0, NULL, 0, want, 32, out, sizeof out) : -1;
        ok = ok && d == 32 && memcmp(out, in, 32) == 0;
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("ecb(aes) with a 192-bit key gives F.1.3", ({
        int op = skc_op("ecb(aes)", k192, 24);
        unsigned char in[32], want[32];
        unhex(P12, in);
        unhex("bd334f1d6e45f25ff712a214571fa5cc974104846d0ad3ad7734ecb3ecee4eef", want);
        long r = op >= 0 ? skc_run(op, 1, NULL, 0, in, 32, out, sizeof out) : -1;
        int ok = r == 32 && memcmp(out, want, 32) == 0;
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("ecb(aes) takes a zero-length IV and refuses a 16-byte one", ({
        int op = skc_op("ecb(aes)", k128, 16);
        unsigned char in[16] = {0};
        long a = op >= 0 ? skc_run(op, 1, zero, 0, in, 16, out, sizeof out) : -1;
        long b = op >= 0 ? skc_send(op, 1, zero, 16, in, 16) : 0;
        if (op >= 0) close(op);
        a == 16 && b == -EINVAL;
    }));
    EXPECT("ecb(aes) gives the same block twice for the same input, and a short message is EINVAL", ({
        int op = skc_op("ecb(aes)", k128, 16);
        unsigned char in[32], x[48] = {0};
        unhex(P12, in);
        memcpy(in + 16, in, 16);
        long r = op >= 0 ? skc_run(op, 1, NULL, 0, in, 32, out, sizeof out) : -1;
        int ok = r == 32 && memcmp(out, out + 16, 16) == 0;
        memset(out, 0x5a, 48);
        long e = op >= 0 ? skc_run(op, 1, NULL, 0, x, 20, out, sizeof out) : 0;
        ok = ok && e == -EINVAL && out[0] == 0x5a;
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("ctr(aes) takes any length: 20 bytes give the first 20 of the 32", ({
        int op = skc_op("ctr(aes)", k128, 16);
        unsigned char in[20];
        unhex(P12, in);
        unsigned char iv[16];
        unhex(IVCTR, iv);
        long r = op >= 0 ? skc_run(op, 1, iv, 16, in, 20, out, sizeof out) : -1;
        unsigned char want[32];
        unhex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", want);
        int ok = r == 20 && memcmp(out, want, 20) == 0;
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("an empty message reads as nothing", ({
        int op = skc_op("ctr(aes)", k128, 16);
        long r = op >= 0 ? skc_run(op, 1, zero, 16, "", 0, out, sizeof out) : -1;
        if (op >= 0) close(op);
        r == 0;
    }));

    EXPECT("the counter carries through all 16 bytes: ff..ff then 00..00", ({
        int c = skc_op("cbc(aes)", k128, 16);
        unsigned char e_ff[16], e_0[16], blk[16], want[32], got[32];
        long r1 = c >= 0 ? skc_run(c, 1, zero, 16, ff, 16, e_ff, 16) : -1;
        long r2 = c >= 0 ? skc_run(c, 1, zero, 16, zero, 16, e_0, 16) : -1;
        if (c >= 0) close(c);
        int op = skc_op("ctr(aes)", k128, 16);
        memset(blk, 0, sizeof blk);
        unsigned char z32[32] = {0};
        long r3 = op >= 0 ? skc_run(op, 1, ff, 16, z32, 32, got, sizeof got) : -1;
        memcpy(want, e_ff, 16);
        memcpy(want + 16, e_0, 16);
        if (op >= 0) close(op);
        r1 == 16 && r2 == 16 && r3 == 32 && memcmp(got, want, 32) == 0;
    }));

    EXPECT("the IV goes on: a second message with no new IV continues the CBC chain", ({
        int op = skc_op("cbc(aes)", k128, 16);
        unsigned char p[32], iv[16], c1[16], c2[16], want[32];
        unhex(P12, p);
        unhex(IVCBC, iv);
        unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want);
        long r1 = op >= 0 ? skc_run(op, 1, iv, 16, p, 16, c1, 16) : -1;
        long r2 = op >= 0 ? skc_run(op, -1, NULL, 0, p + 16, 16, c2, 16) : -1;
        if (op >= 0) close(op);
        r1 == 16 && r2 == 16 && memcmp(c1, want, 16) == 0 && memcmp(c2, want + 16, 16) == 0;
    }));
    EXPECT("and a counter continues after the blocks it used", ({
        int op = skc_op("ctr(aes)", k128, 16);
        unsigned char p[32], iv[16], c1[16], c2[16], want[32];
        unhex(P12, p);
        unhex(IVCTR, iv);
        unhex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", want);
        long r1 = op >= 0 ? skc_run(op, 1, iv, 16, p, 16, c1, 16) : -1;
        long r2 = op >= 0 ? skc_run(op, -1, NULL, 0, p + 16, 16, c2, 16) : -1;
        if (op >= 0) close(op);
        r1 == 16 && r2 == 16 && memcmp(c1, want, 16) == 0 && memcmp(c2, want + 16, 16) == 0;
    }));

    EXPECT("a CBC message that is not whole blocks is EINVAL and gives nothing", ({
        int op = skc_op("cbc(aes)", k128, 16);
        unsigned char in[17] = {0};
        memset(out, 0x5a, sizeof out);
        long r = op >= 0 ? skc_run(op, 1, zero, 16, in, 17, out, sizeof out) : 0;
        int ok = r == -EINVAL && out[0] == 0x5a;
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("a buffer too small for the result is EINVAL and nothing is written", ({
        int op = skc_op("ctr(aes)", k128, 16);
        unsigned char in[32] = {0};
        memset(out, 0x5a, sizeof out);
        long r = op >= 0 ? skc_run(op, 1, zero, 16, in, 32, out, 16) : 0;
        int ok = r == -EINVAL && out[0] == 0x5a;
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("a 12-byte IV is refused, as is a 23-byte key", ({
        int op = skc_op("cbc(aes)", k128, 16);
        unsigned char in[16] = {0};
        long r = op >= 0 ? skc_send(op, 1, zero, 12, in, 16) : 0;
        if (op >= 0) close(op);
        int op2 = skc_op("cbc(aes)", k23, 23);
        r == -EINVAL && op2 < 0 && errno == EINVAL;
    }));
    EXPECT("no operation, or no IV, is EINVAL", ({
        int op = skc_op("cbc(aes)", k128, 16);
        unsigned char in[16] = {0};
        long a = op >= 0 ? skc_run(op, -1, zero, 16, in, 16, out, sizeof out) : 0;
        if (op >= 0) close(op);
        int op2 = skc_op("cbc(aes)", k128, 16);
        long b = op2 >= 0 ? skc_run(op2, 1, NULL, 0, in, 16, out, sizeof out) : 0;
        if (op2 >= 0) close(op2);
        a == -EINVAL && b == -EINVAL;
    }));
    EXPECT("a socket with no key cannot be accepted", ({
        int s = bound("skcipher", "cbc(aes)");
        int op = accept(s, NULL, NULL);
        int ok = op < 0 && errno == ENOKEY;
        close(s);
        ok;
    }));
    EXPECT("an aead operation is refused on a skcipher: no associated length", ({
        int op = skc_op("cbc(aes)", k128, 16);
        union { char b[64]; struct cmsghdr a; } ctl;
        memset(&ctl, 0, sizeof ctl);
        struct cmsghdr *c = (struct cmsghdr *)ctl.b;
        c->cmsg_level = SOL_ALG;
        c->cmsg_type = 4;                     /* ALG_SET_AEAD_ASSOCLEN */
        c->cmsg_len = CMSG_LEN(4);
        *(uint32_t *)CMSG_DATA(c) = 8;
        unsigned char in[16] = {0};
        struct iovec v = { in, 16 };
        struct msghdr m = {0};
        m.msg_iov = &v;
        m.msg_iovlen = 1;
        m.msg_control = ctl.b;
        m.msg_controllen = CMSG_SPACE(4);
        long r = op >= 0 ? sendmsg(op, &m, 0) : 0;
        int ok = r < 0 && errno == EINVAL;
        if (op >= 0) close(op);
        ok;
    }));

    EXPECT("the input is a copy: changing the memory after the send changes nothing", ({
        int op = skc_op("cbc(aes)", k128, 16);
        unsigned char in[32], iv[16], want[32];
        unhex(P12, in);
        unhex(IVCBC, iv);
        unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want);
        long s = op >= 0 ? skc_send(op, 1, iv, 16, in, 32) : -1;
        memset(in, 0, sizeof in);
        long r = op >= 0 ? read(op, out, sizeof out) : -1;
        int ok = s == 32 && r == 32 && memcmp(out, want, 32) == 0;
        if (op >= 0) close(op);
        ok;
    }));
    EXPECT("the result goes only to the buffer read into: the sent memory is not changed", ({
        int op = skc_op("ctr(aes)", k128, 16);
        unsigned char in[32], keep[32], iv[16];
        unhex(P12, in);
        memcpy(keep, in, sizeof in);
        unhex(IVCTR, iv);
        long r = op >= 0 ? skc_run(op, 1, iv, 16, in, 32, out, sizeof out) : -1;
        if (op >= 0) close(op);
        r == 32 && memcmp(in, keep, 32) == 0;
    }));
    EXPECT("reading into the memory that was sent is fine and gives the same result", ({
        int op = skc_op("ctr(aes)", k128, 16);
        unsigned char buf[32], iv[16], want[32];
        unhex(P12, buf);
        unhex(IVCTR, iv);
        unhex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", want);
        long r = op >= 0 ? skc_run(op, 1, iv, 16, buf, 32, buf, 32) : -1;
        if (op >= 0) close(op);
        r == 32 && memcmp(buf, want, 32) == 0;
    }));
    EXPECT("a bad address for the result leaves the message to be read again", ({
        int op = skc_op("ctr(aes)", k128, 16);
        unsigned char in[16], iv[16], want[16];
        unhex(P12, in);
        unhex(IVCTR, iv);
        unhex("874d6191b620e3261bef6864990db6ce", want);
        long s = op >= 0 ? skc_send(op, 1, iv, 16, in, 16) : -1;
        long bad = op >= 0 ? read(op, (void *)8, 16) : 0;
        int e = errno;
        long r = op >= 0 ? read(op, out, 16) : -1;
        if (op >= 0) close(op);
        s == 16 && bad < 0 && e == EFAULT && r == 16 && memcmp(out, want, 16) == 0;
    }));
    EXPECT("the key is a copy: a socket accepted before a new key keeps the old one", ({
        int s = bound("skcipher", "ctr(aes)");
        setsockopt(s, SOL_ALG, ALG_SET_KEY, k128, 16);
        int op = accept(s, NULL, NULL);
        setsockopt(s, SOL_ALG, ALG_SET_KEY, k256, 32);
        int ok = op >= 0 && same(op, 1, IVCTR, P12,
            "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff");
        if (op >= 0) close(op);
        close(s);
        ok;
    }));

    /* sendfile and splice into the socket */
    {
        const char *path = "algskc.dat";
        unsigned char ct[32], pt[32], iv[16];
        unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", ct);
        unhex(P12, pt);
        unhex(IVCBC, iv);
        EXPECT("sendfile of a file of ciphertext decrypts it, and the file stays the ciphertext", ({
            int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
            long w = fd >= 0 ? write(fd, ct, 32) : -1;
            int op = skc_op("cbc(aes)", k128, 16);
            long s0 = op >= 0 ? skc_send(op, 0, iv, 16, "", 0) : -1;   /* decrypt: the op and the IV, no data */
            off_t off = 0;
            long sf = op >= 0 && fd >= 0 ? sendfile(op, fd, &off, 32) : -1;
            long r = op >= 0 ? read(op, out, sizeof out) : -1;
            unsigned char back[32];
            long rd = fd >= 0 ? pread(fd, back, 32, 0) : -1;
            int ok = w == 32 && s0 == 0 && sf == 32 && r == 32 && memcmp(out, pt, 32) == 0 && rd == 32 && memcmp(back, ct, 32) == 0;
            if (op >= 0) close(op);
            if (fd >= 0) close(fd);
            ok;
        }));
        EXPECT("the file changed after the sendfile does not change the result", ({
            int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
            long w = fd >= 0 ? write(fd, pt, 32) : -1;
            int op = skc_op("cbc(aes)", k128, 16);
            long s0 = op >= 0 ? skc_send(op, 1, iv, 16, "", 0) : -1;
            off_t off = 0;
            long sf = op >= 0 && fd >= 0 ? sendfile(op, fd, &off, 32) : -1;
            unsigned char z[32] = {0};
            long wz = fd >= 0 ? pwrite(fd, z, 32, 0) : -1;
            long r = op >= 0 ? read(op, out, sizeof out) : -1;
            unsigned char want[32];
            unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want);
            int ok = w == 32 && s0 == 0 && sf == 32 && wz == 32 && r == 32 && memcmp(out, want, 32) == 0;
            if (op >= 0) close(op);
            if (fd >= 0) close(fd);
            ok;
        }));
        EXPECT("encrypting from a file leaves the file as the plaintext", ({
            int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
            long w = fd >= 0 ? write(fd, pt, 32) : -1;
            int op = skc_op("ctr(aes)", k128, 16);
            unsigned char civ[16];
            unhex(IVCTR, civ);
            long s0 = op >= 0 ? skc_send(op, 1, civ, 16, "", 0) : -1;
            off_t off = 0;
            long sf = op >= 0 && fd >= 0 ? sendfile(op, fd, &off, 32) : -1;
            long r = op >= 0 ? read(op, out, sizeof out) : -1;
            unsigned char back[32];
            long rd = fd >= 0 ? pread(fd, back, 32, 0) : -1;
            int ok = w == 32 && s0 == 0 && sf == 32 && r == 32 && memcmp(out, pt, 32) != 0 && rd == 32 && memcmp(back, pt, 32) == 0;
            if (op >= 0) close(op);
            if (fd >= 0) close(fd);
            ok;
        }));
        EXPECT("splice from a pipe into the socket adds to the message like send", ({
            int p[2];
            long pr = pipe(p);
            int op = skc_op("cbc(aes)", k128, 16);
            long s0 = op >= 0 ? skc_send(op, 1, iv, 16, "", 0) : -1;
            long w = pr == 0 ? write(p[1], pt, 32) : -1;
            long sp = op >= 0 && pr == 0 ? splice(p[0], NULL, op, NULL, 32, 0) : -1;
            long r = op >= 0 ? read(op, out, sizeof out) : -1;
            unsigned char want[32];
            unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want);
            int ok = pr == 0 && s0 == 0 && w == 32 && sp == 32 && r == 32 && memcmp(out, want, 32) == 0;
            if (pr == 0) { close(p[0]); close(p[1]); }
            if (op >= 0) close(op);
            ok;
        }));
        EXPECT("splice from a file through a pipe, as a program like OpenSSL does, gives the same", ({
            int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
            long w = fd >= 0 ? write(fd, pt, 32) : -1;
            int p[2];
            long pr = pipe(p);
            int op = skc_op("cbc(aes)", k128, 16);
            long s0 = op >= 0 ? skc_send(op, 1, iv, 16, "", 0) : -1;
            loff_t off = 0;
            long a = pr == 0 && fd >= 0 ? splice(fd, &off, p[1], NULL, 32, 0) : -1;
            long b = op >= 0 && pr == 0 ? splice(p[0], NULL, op, NULL, 32, 0) : -1;
            long r = op >= 0 ? read(op, out, sizeof out) : -1;
            unsigned char want[32], back[32];
            unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want);
            long rd = fd >= 0 ? pread(fd, back, 32, 0) : -1;
            int ok = w == 32 && pr == 0 && s0 == 0 && a == 32 && b == 32 && r == 32 && memcmp(out, want, 32) == 0
                     && rd == 32 && memcmp(back, pt, 32) == 0;
            if (pr == 0) { close(p[0]); close(p[1]); }
            if (op >= 0) close(op);
            if (fd >= 0) close(fd);
            ok;
        }));
        EXPECT("sendfile into a hash socket gives the digest of the file", ({
            int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
            long w = fd >= 0 ? write(fd, "abc", 3) : -1;
            int s = bound("hash", "sha256");
            int op = s >= 0 ? accept(s, NULL, NULL) : -1;
            off_t off = 0;
            long sf = op >= 0 && fd >= 0 ? sendfile(op, fd, &off, 3) : -1;
            unsigned char d[32];
            long r = op >= 0 ? read(op, d, sizeof d) : -1;
            unsigned char want[32];
            unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
            int ok = w == 3 && sf == 3 && r == 32 && memcmp(d, want, 32) == 0;
            if (op >= 0) close(op);
            if (s >= 0) close(s);
            if (fd >= 0) close(fd);
            ok;
        }));
        EXPECT("sendfile to a socket that is bound but not accepted fails", ({
            int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
            long w = fd >= 0 ? write(fd, "abc", 3) : -1;
            int s = skc_op("cbc(aes)", k128, 16);   /* accepted: use a bound-only one below */
            if (s >= 0) close(s);
            int b = bound("skcipher", "cbc(aes)");
            off_t off = 0;
            long sf = b >= 0 && fd >= 0 ? sendfile(b, fd, &off, 3) : 0;
            int ok = w == 3 && sf < 0;
            if (b >= 0) close(b);
            if (fd >= 0) close(fd);
            ok;
        }));
        unlink(path);
    }
    return done();
}
