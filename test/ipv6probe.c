/* The guest side of test/ipv6_test.sh: AF_INET6 sockets to peers that the
 * test script plays on the other end of the wire. Prints one line per
 * check ("ok WHAT" or "FAIL WHAT: why") and "done". Built against musl as
 * a static PIE and run from the test's disk. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void report(int ok, const char *what, const char *why) {
    if (ok) printf("ok %s\n", what);
    else { printf("FAIL %s: %s (errno %d %s)\n", what, why, errno, strerror(errno)); fails++; }
}

static struct sockaddr_in6 addr(const char *a, int port) {
    struct sockaddr_in6 s = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
    inet_pton(AF_INET6, a, &s.sin6_addr);
    return s;
}

/* Sends msg to a:port over UDP and waits for it to come back from the
 * same address, retrying while the network settles (a router
 * advertisement, a neighbor solicitation). */
static int udp_echo(const char *a, int port, const char *msg, char *from, size_t fl) {
    int s = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in6 to = addr(a, port);
    for (int tries = 0; tries < 20; tries++) {
        if (sendto(s, msg, strlen(msg), 0, (void *)&to, sizeof to) < 0) {
            usleep(300000);
            continue;
        }
        struct pollfd p = {.fd = s, .events = POLLIN};
        if (poll(&p, 1, 500) == 1) {
            char b[256];
            struct sockaddr_in6 f;
            socklen_t l = sizeof f;
            ssize_t n = recvfrom(s, b, sizeof b, 0, (void *)&f, &l);
            close(s);
            inet_ntop(AF_INET6, &f.sin6_addr, from, fl);
            return n == (ssize_t)strlen(msg) && memcmp(b, msg, n) == 0 && f.sin6_family == AF_INET6 &&
                   ntohs(f.sin6_port) == port;
        }
    }
    close(s);
    return 0;
}

/* What busybox ping6 does: a raw ICMPv6 socket, the filter and checksum
 * options it sets, an echo request to a:, the reply read back with its
 * source address. 0 on failure. */
static int ping6(const char *a, char *from, size_t fl) {
    int s = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (s < 0) return 0;
    struct icmp6_filter filt;
    ICMP6_FILTER_SETBLOCKALL(&filt);
    ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filt);
    int off = 2, one = 1;
    if (setsockopt(s, IPPROTO_ICMPV6, ICMP6_FILTER, &filt, sizeof filt) < 0 ||
        setsockopt(s, IPPROTO_RAW, IPV6_CHECKSUM, &off, sizeof off) < 0 ||
        setsockopt(s, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof one) < 0) {
        close(s);
        return 0;
    }
    struct sockaddr_in6 to = addr(a, 0);
    for (int tries = 0; tries < 20; tries++) {
        struct icmp6_hdr h = {.icmp6_type = ICMP6_ECHO_REQUEST};
        char pkt[8 + 16];
        h.icmp6_id = htons(0x4242);
        h.icmp6_seq = htons(tries);
        memcpy(pkt, &h, 8);
        memcpy(pkt + 8, "bastios ping6 ok", 16);
        if (sendto(s, pkt, sizeof pkt, 0, (void *)&to, sizeof to) < 0) {
            usleep(300000);
            continue;
        }
        struct pollfd p = {.fd = s, .events = POLLIN};
        if (poll(&p, 1, 500) == 1) {
            char b[256];
            struct sockaddr_in6 f;
            socklen_t l = sizeof f;
            ssize_t n = recvfrom(s, b, sizeof b, 0, (void *)&f, &l);
            close(s);
            inet_ntop(AF_INET6, &f.sin6_addr, from, fl);
            return n == sizeof pkt && (unsigned char)b[0] == ICMP6_ECHO_REPLY && memcmp(b + 8, "bastios ping6 ok", 16) == 0 &&
                   f.sin6_family == AF_INET6;
        }
    }
    close(s);
    return 0;
}

/* The server half, started in the background by the test script, which
 * plays the clients: a dual-stack UDP echo on 9001 and a dual-stack TCP
 * greeter on 9002, after checks that an IPv6-only socket and an IPv4 one
 * can share a port and that a dual-stack one cannot. */
static int bound(int family, int type, int v6only, int port) {
    int s = socket(family, type, 0);
    int one = v6only;
    if (s < 0) return -1;
    if (family == AF_INET6 && setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one) < 0) return -1;
    if (family == AF_INET6) {
        struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
        if (bind(s, (void *)&a, sizeof a) < 0) { close(s); return -2; }
    } else {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
        if (bind(s, (void *)&a, sizeof a) < 0) { close(s); return -2; }
    }
    if (type == SOCK_STREAM && listen(s, 4) < 0) { close(s); return -2; }
    return s;
}

static int serve(void) {
    printf("\n");                          /* the shell's job notice comes before the first line */
    static const int types[2] = {SOCK_DGRAM, SOCK_STREAM};
    for (int t = 0; t < 2; t++) {
        int a = bound(AF_INET6, types[t], 1, 9003 + t);
        int b = bound(AF_INET, types[t], 0, 9003 + t);
        report(a >= 0 && b >= 0, t ? "TCP: an IPv6-only listener and an IPv4 one share a port" :
               "UDP: an IPv6-only socket and an IPv4 one share a port", "bind");
        int c = bound(AF_INET6, types[t], 0, 9003 + t);
        report(c == -2 && errno == EADDRINUSE, t ? "TCP: a dual-stack listener cannot share it" :
               "UDP: a dual-stack socket cannot share it", "bound twice");
        int v = -1;
        socklen_t vl = sizeof v;
        report(a >= 0 && getsockopt(a, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) == 0 && v == 1,
               "IPV6_V6ONLY reads back", "getsockopt");
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        if (c >= 0) close(c);
    }
    int u = bound(AF_INET6, SOCK_DGRAM, 0, 9001);
    int l = bound(AF_INET6, SOCK_STREAM, 0, 9002);
    if (u < 0 || l < 0) { report(0, "the servers listen", "bind"); return 1; }
    printf("ready\n");
    for (;;) {
        struct pollfd p[2] = {{.fd = u, .events = POLLIN}, {.fd = l, .events = POLLIN}};
        if (poll(p, 2, 60000) <= 0) return 1;
        if (p[0].revents & POLLIN) {
            char b[256];
            struct sockaddr_in6 f;
            socklen_t fl = sizeof f;
            ssize_t n = recvfrom(u, b, sizeof b, 0, (void *)&f, &fl);
            if (n > 0) sendto(u, b, n, 0, (void *)&f, fl);
        }
        if (p[1].revents & POLLIN) {
            struct sockaddr_in6 f, me;
            socklen_t fl = sizeof f, ml = sizeof me;
            int c = accept(l, (void *)&f, &fl);
            if (c < 0) continue;
            char b[64], peer[64] = "?", own[64] = "?", out[200];
            getsockname(c, (void *)&me, &ml);
            inet_ntop(AF_INET6, &f.sin6_addr, peer, sizeof peer);
            inet_ntop(AF_INET6, &me.sin6_addr, own, sizeof own);
            if (read(c, b, sizeof b) > 0) {
                int m = snprintf(out, sizeof out, "peer %s port %d own %s\n", peer, ntohs(f.sin6_port), own);
                write(c, out, m);
            }
            close(c);
        }
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) return serve();
    char from[64] = "";

    report(udp_echo("2001:db8:1::2", 7, "over ipv6", from, sizeof from) && strcmp(from, "2001:db8:1::2") == 0,
           "UDP echo from a global address on the link", from);
    report(udp_echo("fe80::2", 7, "link-local", from, sizeof from) && strcmp(from, "fe80::2") == 0,
           "UDP echo from a link-local address", from);
    report(udp_echo("2001:db8:99::5", 7, "via the router", from, sizeof from) && strcmp(from, "2001:db8:99::5") == 0,
           "UDP echo from off the link, through the router", from);

    report(ping6("2001:db8:1::2", from, sizeof from) && strcmp(from, "2001:db8:1::2") == 0,
           "ping6 of a host on the link", from);
    report(ping6("fe80::2", from, sizeof from) && strcmp(from, "fe80::2") == 0, "ping6 of a link-local host", from);
    report(ping6("2001:db8:99::5", from, sizeof from) && strcmp(from, "2001:db8:99::5") == 0,
           "ping6 of a host through the router", from);
    report(ping6("::1", from, sizeof from) && strcmp(from, "::1") == 0, "ping6 of ::1", from);

    /* TCP: a request, the whole reply, the server's close. */
    int c = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 to = addr("2001:db8:1::2", 80), peer;
    socklen_t pl = sizeof peer;
    char got[4096];
    size_t n = 0;
    int ok = c >= 0 && connect(c, (void *)&to, sizeof to) == 0;
    report(ok, "TCP connect", "connect");
    ok = ok && getpeername(c, (void *)&peer, &pl) == 0 && peer.sin6_family == AF_INET6 &&
         memcmp(&peer.sin6_addr, &to.sin6_addr, 16) == 0;
    report(ok, "getpeername gives the IPv6 address", "getpeername");
    if (c >= 0 && write(c, "GET /\n", 6) == 6) {
        ssize_t r;
        while (n < sizeof got && (r = read(c, got + n, sizeof got - n)) > 0) n += r;
    }
    int whole = n == 3000;
    for (size_t i = 0; whole && i < n; i++) whole = got[i] == "0123456789abcdef"[i % 16];
    report(whole, "TCP reply (3000 bytes, several segments) and close", "short or wrong");
    if (c >= 0) close(c);
    printf("done %d\n", fails);
    return fails != 0;
}
