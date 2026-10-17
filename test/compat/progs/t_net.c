/* IPv4 sockets on this machine's own network: UDP and TCP over loopback,
 * socket options, name lookups that need no server. */
#include "compat.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>

int main(void) {
    setup();
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    char b[16];
    int s = -1, t = -1, st, one = 1;
    pid_t c;

    EXPECT("UDP socket", (s = socket(AF_INET, SOCK_DGRAM, 0)) >= 0);
    EXPECT("UDP bind port 0", bind(s, (void *)&a, sizeof a) == 0 && getsockname(s, (void *)&a, &al) == 0 && a.sin_port != 0);
    EXPECT("UDP loopback", (t = socket(AF_INET, SOCK_DGRAM, 0)) >= 0 && sendto(t, "u", 1, 0, (void *)&a, sizeof a) == 1 &&
           recv(s, b, sizeof b, 0) == 1 && b[0] == 'u');
    close(s); close(t);
    a.sin_port = 0;
    EXPECT("TCP socket", (s = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
    EXPECT("SO_REUSEADDR", setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0);
    EXPECT("TCP bind/listen", bind(s, (void *)&a, sizeof a) == 0 && listen(s, 4) == 0 &&
           getsockname(s, (void *)&a, &al) == 0 && a.sin_port != 0);
    EXPECT("TCP loopback connect/accept", (c = fork()) >= 0 && (c == 0 ? ({ int u = socket(AF_INET, SOCK_STREAM, 0);
           _exit(connect(u, (void *)&a, sizeof a) == 0 && write(u, "tcp", 3) == 3 ? 0 : 1); 0; }) :
           ({ int u = accept(s, 0, 0); u >= 0 && read(u, b, 3) == 3 && memcmp(b, "tcp", 3) == 0; }) &&
           waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 0));
    {
        int v = 0; socklen_t vl = sizeof v;
        EXPECT("getsockopt SO_TYPE", getsockopt(s, SOL_SOCKET, SO_TYPE, &v, &vl) == 0 && v == SOCK_STREAM);
        EXPECT("TCP_NODELAY", (t = socket(AF_INET, SOCK_STREAM, 0)) >= 0 && setsockopt(t, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0);
    }
    {
        struct addrinfo h = {.ai_family = AF_INET, .ai_flags = AI_NUMERICHOST}, *r = 0;
        EXPECT("getaddrinfo numeric", getaddrinfo("10.0.2.2", "80", &h, &r) == 0 && r != NULL);
        EXPECT("getaddrinfo localhost", getaddrinfo("localhost", 0, 0, &r) == 0);
        struct addrinfo h6 = {.ai_family = AF_INET6, .ai_socktype = SOCK_STREAM}, *r6 = 0;
        struct sockaddr_in6 *s6;
        EXPECT("getaddrinfo localhost finds ::1 (/etc/hosts)", getaddrinfo("localhost", "80", &h6, &r6) == 0 && r6 != NULL &&
               (s6 = (void *)r6->ai_addr)->sin6_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&s6->sin6_addr));
    }
    EXPECT("inet_pton", inet_pton(AF_INET, "1.2.3.4", &a.sin_addr) == 1 && a.sin_addr.s_addr == htonl(0x01020304));
    EXPECT("AF_INET6 socket", (t = socket(AF_INET6, SOCK_STREAM, 0)) >= 0);
    {
        struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT}, p6;
        socklen_t l6 = sizeof a6, pl = sizeof p6;
        close(s);
        EXPECT("AF_INET6 TCP over ::1", (s = socket(AF_INET6, SOCK_STREAM, 0)) >= 0 && bind(s, (void *)&a6, sizeof a6) == 0 &&
               listen(s, 4) == 0 && getsockname(s, (void *)&a6, &l6) == 0 && a6.sin6_family == AF_INET6 && a6.sin6_port != 0 &&
               (c = fork()) >= 0 && (c == 0 ? ({ int u = socket(AF_INET6, SOCK_STREAM, 0);
               _exit(connect(u, (void *)&a6, sizeof a6) == 0 && write(u, "six", 3) == 3 ? 0 : 1); 0; }) :
               ({ int u = accept(s, (void *)&p6, &pl); u >= 0 && p6.sin6_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&p6.sin6_addr) &&
               read(u, b, 3) == 3 && memcmp(b, "six", 3) == 0; }) && waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 0));
        a6.sin6_port = 0;
        EXPECT("AF_INET6 UDP over ::1", (t = socket(AF_INET6, SOCK_DGRAM, 0)) >= 0 && bind(t, (void *)&a6, sizeof a6) == 0 &&
               getsockname(t, (void *)&a6, &l6) == 0 && a6.sin6_family == AF_INET6 &&
               sendto(t, "d6", 2, 0, (void *)&a6, sizeof a6) == 2 && recv(t, b, sizeof b, 0) == 2 && b[0] == 'd');
    }
    {
        /* Dual-stack sockets and IPV6_V6ONLY: an IPv6-only socket and an IPv4 one share a port; a dual-stack one cannot. */
        int one = 1, zero = 0, v = 7, a6s, a4s, d6s, ok;
        socklen_t vl = sizeof v;
        struct sockaddr_in6 w6 = {.sin6_family = AF_INET6, .sin6_port = htons(9411)};
        struct sockaddr_in w4 = {.sin_family = AF_INET, .sin_port = htons(9411)};
        a6s = socket(AF_INET6, SOCK_STREAM, 0);
        ok = a6s >= 0 && setsockopt(a6s, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one) == 0 &&
             bind(a6s, (void *)&w6, sizeof w6) == 0 && listen(a6s, 4) == 0;
        a4s = socket(AF_INET, SOCK_STREAM, 0);
        ok = ok && a4s >= 0 && bind(a4s, (void *)&w4, sizeof w4) == 0 && listen(a4s, 4) == 0;
        EXPECT("IPV6_V6ONLY: an IPv6-only listener and an IPv4 one share a port", ok);
        d6s = socket(AF_INET6, SOCK_STREAM, 0);
        EXPECT("a dual-stack listener cannot take it", d6s >= 0 && setsockopt(d6s, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero) == 0 &&
               ((bind(d6s, (void *)&w6, sizeof w6) < 0 && errno == EADDRINUSE) ||
                (listen(d6s, 4) < 0 && errno == EADDRINUSE)));    /* ours says so at listen, Linux at bind */
        EXPECT("IPV6_V6ONLY reads back", getsockopt(a6s, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) == 0 && v == 1 &&
               getsockopt(d6s, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) == 0 && v == 0);
        close(a6s); close(a4s); close(d6s);
    }
    {
        /* ping6 ::1: a raw ICMPv6 socket, the echo reply comes back from ::1. */
        int rs = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
        struct sockaddr_in6 lo = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT}, f;
        struct icmp6_hdr ih = {.icmp6_type = ICMP6_ECHO_REQUEST, .icmp6_id = htons(77), .icmp6_seq = htons(1)};
        socklen_t fl = sizeof f;
        char rb[64];
        EXPECT("ping6 ::1 (raw ICMPv6 socket)", rs >= 0 && sendto(rs, &ih, sizeof ih, 0, (void *)&lo, sizeof lo) == sizeof ih &&
               recvfrom(rs, rb, sizeof rb, 0, (void *)&f, &fl) == sizeof ih && (unsigned char)rb[0] == ICMP6_ECHO_REPLY &&
               IN6_IS_ADDR_LOOPBACK(&f.sin6_addr));
    }
    return done();
}
