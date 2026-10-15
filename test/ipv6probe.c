/* The guest side of test/ipv6_test.sh: AF_INET6 sockets to peers that the
 * test script plays on the other end of the wire. Prints one line per
 * check ("ok WHAT" or "FAIL WHAT: why") and "done". Built against musl as
 * a static PIE and run from the test's disk. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char from[64] = "";

    report(udp_echo("2001:db8:1::2", 7, "over ipv6", from, sizeof from) && strcmp(from, "2001:db8:1::2") == 0,
           "UDP echo from a global address on the link", from);
    report(udp_echo("fe80::2", 7, "link-local", from, sizeof from) && strcmp(from, "fe80::2") == 0,
           "UDP echo from a link-local address", from);
    report(udp_echo("2001:db8:99::5", 7, "via the router", from, sizeof from) && strcmp(from, "2001:db8:99::5") == 0,
           "UDP echo from off the link, through the router", from);

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
    /* The request, then enough more to need several full segments. */
    static char req[6006] = "GET /\n";
    memset(req + 6, 'x', sizeof req - 6);
    if (c >= 0 && write(c, req, sizeof req) == (ssize_t)sizeof req) {
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
