/* TCP through the Linux system calls, against musl.
 *   mtcp serve PORT        accept one connection, read a line, send back
 *                          "echo: LINE", close
 *   mtcp echo PORT         accept one connection and send back everything
 *                          it reads until the peer closes (test/netbench.sh)
 *   mtcp get A.B.C.D PORT TEXT
 *                          connect, send TEXT and a newline, print what
 *                          comes back until the peer closes
 *   mtcp send A.B.C.D PORT BYTES
 *                          connect and send BYTES of a counting pattern,
 *                          then close (test/tcpperf.sh)
 *   mtcp sink PORT         accept one connection and read it to the end,
 *                          checking the counting pattern (test/tcpperf.sh)
 *   mtcp loop BYTES        send BYTES of a counting pattern to ourselves over
 *                          127.0.0.1 (a forked child writes, we read and
 *                          check every byte): loopback segments are longer
 *                          than a card's frames
 *   mtcp many N            N connections to ourselves over 127.0.0.1 at once
 *                          (2N connections in the kernel), a byte each way
 *                          on every one, then 30 UDP sockets that each get
 *                          a datagram
 *   mtcp                   serve 7001; the line read names a port on
 *                          10.0.2.2 (QEMU's host) to get from next. The
 *                          BastiOS shell passes no arguments, so the test
 *                          drives both through this. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("mtcp: %s failed (errno %d)\n", what, errno);
    return 1;
}

static int served_port;

static int serve(int port)
{
    int one = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0)
        return fail("socket");
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0 || listen(s, 1) != 0)
        return fail("bind and listen");
    printf("mtcp: listening on tcp port %d\n", port);
    fflush(stdout);
    struct sockaddr_in from;
    socklen_t flen = sizeof from;
    int c = accept(s, (struct sockaddr *)&from, &flen);
    if (c < 0)
        return fail("accept");
    printf("mtcp: connection from %s\n", inet_ntoa(from.sin_addr));
    char line[200] = "echo: ";
    size_t n = 6;
    while (n < sizeof line - 1 && (n == 6 || line[n - 1] != '\n')) {
        ssize_t r = read(c, line + n, sizeof line - 1 - n);
        if (r <= 0)
            return fail("read");
        n += r;
    }
    if (write(c, line, n) != (ssize_t)n)
        return fail("write");
    line[n] = 0;
    served_port = atoi(line + 6);
    close(c);
    close(s);
    printf("mtcp: served %d bytes\n", (int)n);
    return 0;
}

static int echo(int port)
{
    int one = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0)
        return fail("socket");
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0 || listen(s, 1) != 0)
        return fail("bind and listen");
    printf("mtcp: listening on tcp port %d\n", port);
    fflush(stdout);
    int c = accept(s, 0, 0);
    if (c < 0)
        return fail("accept");
    char buf[2048];
    long total = 0;
    ssize_t r;
    while ((r = read(c, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < r; ) {
            ssize_t w = write(c, buf + off, r - off);
            if (w <= 0)
                return fail("write");
            off += w;
        }
        total += r;
    }
    close(c);
    close(s);
    printf("mtcp: echoed %ld bytes\n", total);
    return 0;
}

static int get(const char *ip, int port, const char *text)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (s < 0 || inet_aton(ip, &to.sin_addr) == 0)
        return fail("socket");
    if (connect(s, (struct sockaddr *)&to, sizeof to) != 0)
        return fail("connect");
    char out[200];
    int n = snprintf(out, sizeof out, "%s\n", text);
    if (write(s, out, n) != n)
        return fail("write");
    char in[200];
    size_t got = 0;
    ssize_t r;
    while (got < sizeof in - 1 && (r = read(s, in + got, sizeof in - 1 - got)) > 0)
        got += r;
    in[got] = 0;
    close(s);
    printf("mtcp: got %s", in);
    return 0;
}

/* The pattern's byte at offset i. */
static unsigned char pat(unsigned long i)
{
    return (unsigned char)(i * 7 + (i >> 8));
}

static int loop(unsigned long total)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(9100) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 1) != 0)
        return fail("listen");
    if (fork() == 0) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        if (c < 0 || connect(c, (struct sockaddr *)&a, sizeof a) != 0)
            _exit(1);
        static unsigned char buf[65536];
        unsigned long at = 0;
        while (at < total) {
            size_t n = sizeof buf < total - at ? sizeof buf : total - at;
            for (size_t i = 0; i < n; i++)
                buf[i] = pat(at + i);
            for (size_t off = 0; off < n;) {
                ssize_t w = write(c, buf + off, n - off);
                if (w <= 0)
                    _exit(2);
                off += w;
            }
            at += n;
        }
        _exit(0);
    }
    int s = accept(l, 0, 0);
    if (s < 0)
        return fail("accept");
    static unsigned char in[65536];
    unsigned long got = 0, bad = 0;
    size_t longest = 0;
    ssize_t r;
    while ((r = read(s, in, sizeof in)) > 0) {
        if ((size_t)r > longest)
            longest = r;
        for (ssize_t i = 0; i < r; i++)
            if (in[i] != pat(got + i))
                bad++;
        got += r;
    }
    printf("mtcp: loop %lu bytes, %lu wrong\n", got, bad);
    return got == total && bad == 0 ? 0 : 1;
}

#define MANY_MAX 200

/* N connections open at once to ourselves, then UDP sockets side by side:
 * the kernel's tables of connections and sockets hold more than a handful. */
static int many(int n)
{
    static int cl[MANY_MAX], sv[MANY_MAX];
    if (n < 1 || n > MANY_MAX)
        return 2;
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(9101) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 128) != 0)
        return fail("listen");
    for (int i = 0; i < n; i++) {
        cl[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (cl[i] < 0 || connect(cl[i], (struct sockaddr *)&a, sizeof a) != 0)
            return fail("connect");
    }
    for (int i = 0; i < n; i++) {
        sv[i] = accept(l, 0, 0);
        if (sv[i] < 0)
            return fail("accept");
    }
    for (int i = 0; i < n; i++) {
        char b = (char)('a' + i % 26), got = 0;
        if (write(cl[i], &b, 1) != 1 || read(sv[i], &got, 1) != 1 || got != b)
            return fail("client to server");
        b = (char)('A' + i % 26);
        if (write(sv[i], &b, 1) != 1 || read(cl[i], &got, 1) != 1 || got != b)
            return fail("server to client");
    }
    for (int i = 0; i < n; i++) {
        close(cl[i]);
        close(sv[i]);
    }
    close(l);
    int us[30], tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx < 0)
        return fail("udp socket");
    for (int i = 0; i < 30; i++) {
        struct sockaddr_in u = { .sin_family = AF_INET, .sin_port = htons(9200 + i) };
        u.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        us[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (us[i] < 0 || bind(us[i], (struct sockaddr *)&u, sizeof u) != 0)
            return fail("udp bind");
    }
    for (int i = 0; i < 30; i++) {
        struct sockaddr_in u = { .sin_family = AF_INET, .sin_port = htons(9200 + i) };
        u.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        char b = (char)('0' + i % 10), got = 0;
        if (sendto(tx, &b, 1, 0, (struct sockaddr *)&u, sizeof u) != 1
            || recv(us[i], &got, 1, 0) != 1 || got != b)
            return fail("udp datagram");
    }
    /* One socket queues sixteen datagrams before it is read. */
    struct sockaddr_in q = { .sin_family = AF_INET, .sin_port = htons(9200) };
    q.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 16; i++) {
        char b = (char)('a' + i);
        if (sendto(tx, &b, 1, 0, (struct sockaddr *)&q, sizeof q) != 1)
            return fail("udp queue send");
    }
    for (int i = 0; i < 16; i++) {
        char got = 0;
        if (recv(us[0], &got, 1, 0) != 1 || got != (char)('a' + i))
            return fail("udp queue order");
    }
    for (int i = 0; i < 30; i++)
        close(us[i]);
    close(tx);
    printf("mtcp: %d connections and 30 udp sockets, 16 queued\n", n);
    return 0;
}

static int send_bytes(const char *ip, int port, long bytes)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (s < 0 || inet_aton(ip, &to.sin_addr) == 0)
        return fail("socket");
    if (connect(s, (struct sockaddr *)&to, sizeof to) != 0)
        return fail("connect");
    static char buf[16384];
    long sent = 0;
    while (sent < bytes) {
        long k = bytes - sent < (long)sizeof buf ? bytes - sent : (long)sizeof buf;
        for (long i = 0; i < k; i++)
            buf[i] = (char)((sent + i) * 7 + 3);
        for (long off = 0; off < k; ) {
            ssize_t w = write(s, buf + off, k - off);
            if (w <= 0)
                return fail("write");
            off += w;
        }
        sent += k;
    }
    close(s);
    printf("mtcp: sent %ld bytes\n", sent);
    return 0;
}

static int sink(int port)
{
    int one = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0)
        return fail("socket");
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0 || listen(s, 1) != 0)
        return fail("bind and listen");
    printf("mtcp: listening on tcp port %d\n", port);
    fflush(stdout);
    int c = accept(s, 0, 0);
    if (c < 0)
        return fail("accept");
    static char buf[16384];
    long total = 0, bad = 0;
    ssize_t r;
    while ((r = read(c, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < r; i++)
            bad += buf[i] != (char)((total + i) * 7 + 3);
        total += r;
    }
    close(c);
    close(s);
    printf("mtcp: sunk %ld bytes, %ld wrong\n", total, bad);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "send") == 0)
        return send_bytes(argv[2], atoi(argv[3]), atol(argv[4]));
    if (argc == 3 && strcmp(argv[1], "sink") == 0)
        return sink(atoi(argv[2]));
    if (argc == 3 && strcmp(argv[1], "many") == 0)
        return many(atoi(argv[2]));
    if (argc == 3 && strcmp(argv[1], "loop") == 0)
        return loop(strtoul(argv[2], 0, 10));
    if (argc == 3 && strcmp(argv[1], "echo") == 0)
        return echo(atoi(argv[2]));
    if (argc == 3 && strcmp(argv[1], "serve") == 0)
        return serve(atoi(argv[2]));
    if (argc == 1) {
        if (serve(7001) != 0)
            return 1;
        return get("10.0.2.2", served_port, "linux says hi");
    }
    if (argc == 5 && strcmp(argv[1], "get") == 0)
        return get(argv[2], atoi(argv[3]), argv[4]);
    printf("usage: mtcp serve PORT | mtcp echo PORT | mtcp send A.B.C.D PORT BYTES | mtcp sink PORT | mtcp loop BYTES | mtcp many N | mtcp get A.B.C.D PORT TEXT\n");
    return 2;
}
