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
#include <sys/uio.h>
#include <fcntl.h>
#include <stdint.h>
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
            char b[256], ctl[64];
            struct sockaddr_in6 f;
            struct iovec iov = {b, sizeof b};
            struct msghdr m = {.msg_name = &f, .msg_namelen = sizeof f, .msg_iov = &iov, .msg_iovlen = 1,
                               .msg_control = ctl, .msg_controllen = sizeof ctl};
            ssize_t n = recvmsg(s, &m, 0);
            int hl = -1;
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
                if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_HOPLIMIT) memcpy(&hl, CMSG_DATA(c), sizeof hl);
            close(s);
            inet_ntop(AF_INET6, &f.sin6_addr, from, fl);
            if (hl != 64) snprintf(from, fl, "hop limit %d", hl);
            return n == sizeof pkt && (unsigned char)b[0] == ICMP6_ECHO_REPLY && memcmp(b + 8, "bastios ping6 ok", 16) == 0 &&
                   f.sin6_family == AF_INET6 && hl == 64;
        }
    }
    close(s);
    return 0;
}

/* Whether the file at path holds the text (read whole). */
static int file_has(const char *path, const char *text) {
    char buf[4096];
    int f = open(path, O_RDONLY);
    ssize_t n = f < 0 ? 0 : read(f, buf, sizeof buf - 1);
    if (f >= 0) close(f);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, text) != NULL;
}

/* Asks rtnetlink (musl has no linux/ headers, so the few layouts are here)
 * for the addresses (RTM_GETADDR) or routes (RTM_GETROUTE) of family
 * AF_INET6 and returns whether one of them carries the 16 bytes `want` as
 * the attribute `attr`. */
#define AF_NETLINK_ 16
#define NETLINK_ROUTE_ 0
#define RTM_GETROUTE_ 26
#define RTM_GETADDR_ 22
#define IFA_ADDRESS_ 1
#define RTA_GATEWAY_ 5
static int nl_has(int kind, int attr, const unsigned char *want) {
    int s = socket(AF_NETLINK_, SOCK_RAW, NETLINK_ROUTE_);
    if (s < 0) return 0;
    struct { uint32_t len; uint16_t type, flags; uint32_t seq, pid; unsigned char family, pad[3]; } req = {
        sizeof req, kind, 1 | 0x300, 1, 0, AF_INET6, {0}};
    int found = 0;
    if (send(s, &req, sizeof req, 0) == sizeof req) {
        unsigned char buf[8192];
        for (int done = 0; !done;) {
            ssize_t n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) break;
            for (ssize_t o = 0; o + 16 <= n;) {
                uint32_t ml;
                uint16_t ty;
                memcpy(&ml, buf + o, 4);
                memcpy(&ty, buf + o + 4, 2);
                if (ml < 16 || o + ml > (size_t)n) { done = 1; break; }
                if (ty == 3) { done = 1; break; }                   /* NLMSG_DONE */
                size_t hdr = kind == RTM_GETADDR_ ? 8 : 12;         /* ifaddrmsg, rtmsg */
                for (size_t a = o + 16 + hdr; a + 4 <= o + ml;) {
                    uint16_t al, at;
                    memcpy(&al, buf + a, 2);
                    memcpy(&at, buf + a + 2, 2);
                    if (al < 4 || a + al > o + ml) break;
                    if (at == attr && al == 20 && memcmp(buf + a + 4, want, 16) == 0) found = 1;
                    a += (al + 3) & ~3u;
                }
                o += (ml + 3) & ~3u;
            }
        }
    }
    close(s);
    return found;
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

/* Router advertisement changes, heard on rtnetlink: joins the IPv6 address
 * and route groups (and, on a second socket, only the IPv4 address group),
 * says "ready", and waits for the test script's second advertisement, which
 * moves the prefix from 2001:db8:1::/64 to 2001:db8:2::/64. */
static int nl_group_socket(unsigned groups) {
    int s = socket(AF_NETLINK_, SOCK_RAW, NETLINK_ROUTE_);
    struct { uint16_t family, pad; uint32_t pid, groups; } sa = {AF_NETLINK_, 0, 0, groups};
    if (s < 0 || bind(s, (void *)&sa, sizeof sa) < 0) return -1;
    return s;
}

static int rtnl_events(void) {
    int s = nl_group_socket(256 | 1024), other = nl_group_socket(16);
    unsigned char old_gl[16], new_gl[16], old_pfx[16], new_pfx[16];
    inet_pton(AF_INET6, "2001:db8:1::5054:ff:fe12:3456", old_gl);
    inet_pton(AF_INET6, "2001:db8:2::5054:ff:fe12:3456", new_gl);
    inet_pton(AF_INET6, "2001:db8:1::", old_pfx);
    inet_pton(AF_INET6, "2001:db8:2::", new_pfx);
    report(s >= 0 && other >= 0, "netlink sockets bind to the IPv6 and IPv4 groups", "bind");
    printf("ready\n");
    int del_addr = 0, new_addr = 0, del_route = 0, new_route = 0, stray = 0;
    char why[64] = "other message";
    for (int round = 0; round < 40 && !(del_addr && new_addr && del_route && new_route); round++) {
        struct pollfd p = {.fd = s, .events = POLLIN};
        if (poll(&p, 1, 500) != 1) continue;
        unsigned char b[8192];
        ssize_t n = recv(s, b, sizeof b, 0);
        for (ssize_t o = 0; o + 16 <= n;) {
            uint32_t ml;
            uint16_t ty;
            memcpy(&ml, b + o, 4);
            memcpy(&ty, b + o + 4, 2);
            if (ml < 16 || o + ml > (size_t)n) break;
            int addr = ty == 20 || ty == 21, route = ty == 24 || ty == 25;     /* NEW/DELADDR, NEW/DELROUTE */
            if (!addr && !route) { stray++; snprintf(why, sizeof why, "message type %u", ty); }
            size_t hdr = addr ? 8 : 12;                                         /* ifaddrmsg, rtmsg */
            for (size_t a = o + 16 + hdr; (addr || route) && a + 4 <= o + ml;) {
                uint16_t al, at;
                memcpy(&al, b + a, 2);
                memcpy(&at, b + a + 2, 2);
                if (al < 4 || a + al > o + ml) break;
                if (al == 20 && at == 1) {                         /* IFA_ADDRESS, RTA_DST */
                    const void *v = b + a + 4;
                    if (ty == 21 && !memcmp(v, old_gl, 16)) del_addr = 1;
                    if (ty == 20 && !memcmp(v, new_gl, 16)) new_addr = 1;
                    if (ty == 25 && !memcmp(v, old_pfx, 16)) del_route = 1;
                    if (ty == 24 && !memcmp(v, new_pfx, 16)) new_route = 1;
                }
                a += (al + 3) & ~3u;
            }
            o += (ml + 3) & ~3u;
        }
    }
    report(del_addr, "a new prefix withdraws the old IPv6 address (RTM_DELADDR)", "not heard");
    report(new_addr, "a new prefix announces the new IPv6 address (RTM_NEWADDR)", "not heard");
    report(del_route, "a new prefix withdraws the old on-link route (RTM_DELROUTE)", "not heard");
    report(new_route, "a new prefix announces the new on-link route (RTM_NEWROUTE)", "not heard");
    report(stray == 0, "nothing but address and route messages in those groups", why);
    char b[512];
    report(recv(other, b, sizeof b, MSG_DONTWAIT) < 0 && errno == EAGAIN,
           "a socket in the IPv4 address group alone hears nothing of it", "heard something");
    printf("done %d\n", fails);
    return fails != 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "rtnl") == 0) return rtnl_events();
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

    /* What the system tells about its IPv6 configuration. */
    {
        unsigned char gl[16], ll[16], rt[16];
        inet_pton(AF_INET6, "2001:db8:1:0:5054:ff:fe12:3456", gl);
        inet_pton(AF_INET6, "fe80::5054:ff:fe12:3456", ll);
        inet_pton(AF_INET6, "fe80::2", rt);
        report(file_has("/proc/net/if_inet6", "00000000000000000000000000000001 01 80 10 80       lo\n"),
               "/proc/net/if_inet6 lists ::1 on lo", "lo line");
        report(file_has("/proc/net/if_inet6", "20010db800010000505400fffe123456 02 40 00 80     eth0\n") &&
               file_has("/proc/net/if_inet6", "fe80000000000000505400fffe123456 02 40 20 80     eth0\n"),
               "/proc/net/if_inet6 lists the global and link-local addresses of eth0", "eth0 lines");
        report(file_has("/proc/net/ipv6_route", "00000000000000000000000000000000 00 00000000000000000000000000000000 00 fe800000000000000000000000000002 "),
               "/proc/net/ipv6_route has the default route through the router", "default route");
        report(nl_has(RTM_GETADDR_, IFA_ADDRESS_, gl) && nl_has(RTM_GETADDR_, IFA_ADDRESS_, ll),
               "rtnetlink lists the IPv6 addresses", "RTM_GETADDR");
        report(nl_has(RTM_GETROUTE_, RTA_GATEWAY_, rt), "rtnetlink lists the default IPv6 route's gateway", "RTM_GETROUTE");
    }

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
