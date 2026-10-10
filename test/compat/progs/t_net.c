/* IPv4 sockets on this machine's own network: UDP and TCP over loopback,
 * socket options, name lookups that need no server. */
#include "compat.h"
#include <arpa/inet.h>
#include <netdb.h>
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
    return done();
}
