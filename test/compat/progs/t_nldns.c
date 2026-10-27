/* Docker's embedded DNS server, as a container sees it: the server listens on
 * 127.0.0.11 at a port of its own (UDP and TCP) and an iptables rule sends
 * port 53 there. BastiOS has no netfilter, so in a network namespace of its
 * own, 127.0.0.11:53 goes to the port a socket bound with that address, and
 * an answer from that socket reads as coming from port 53. The first network
 * namespace has no such rule. */
#define _GNU_SOURCE
#include "compat.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

static struct sockaddr_in at(const char *ip, int port) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static int lo_up(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq r;
    memset(&r, 0, sizeof r);
    strcpy(r.ifr_name, "lo");
    r.ifr_flags = IFF_UP;
    int e = ioctl(s, SIOCSIFFLAGS, &r);
    close(s);
    return e;
}

static int bound_port(int s) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    return getsockname(s, (void *)&a, &l) == 0 ? ntohs(a.sin_port) : -1;
}

/* One query to 127.0.0.11:53 over UDP, answered from the server's socket:
 * the answer's source as the client sees it. */
static int udp_round(int srv, int *src_port, char *src_ip) {
    int c = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = at("127.0.0.11", 53), from;
    socklen_t fl = sizeof from;
    char q[16], r[16];
    struct timeval tv = {2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (sendto(c, "query", 5, 0, (void *)&to, sizeof to) != 5) return -1;
    ssize_t n = recvfrom(srv, q, sizeof q, 0, (void *)&from, &fl);
    if (n != 5 || memcmp(q, "query", 5) != 0) return -2;
    if (sendto(srv, "answer", 6, 0, (void *)&from, fl) != 6) return -3;
    fl = sizeof from;
    n = recvfrom(c, r, sizeof r, 0, (void *)&from, &fl);
    if (n != 6 || memcmp(r, "answer", 6) != 0) return -4;
    *src_port = ntohs(from.sin_port);
    inet_ntop(AF_INET, &from.sin_addr, src_ip, 16);
    close(c);
    return 0;
}

int main(void) {
    setup();
    int s = -1;
    struct sockaddr_in a = at("127.0.0.11", 53);

    EXPECT("the first namespace has no redirect: a stream connect to 127.0.0.11:53 is refused",
           ({ int t = socket(AF_INET, SOCK_STREAM, 0); int e = connect(t, (void *)&a, sizeof a); int er = errno; close(t); e == -1 && er == ECONNREFUSED; }));
    EXPECT("a network namespace of its own with lo up", unshare(CLONE_NEWNET) == 0 && lo_up() == 0);

    struct sockaddr_in any = at("127.0.0.11", 0);
    s = socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT("a UDP socket binds 127.0.0.11 at a port of its own", bind(s, (void *)&any, sizeof any) == 0 && bound_port(s) > 0 && bound_port(s) != 53);

    int port = -1;
    char ip[16] = "";
    EXPECT("a datagram for 127.0.0.11:53 reaches that socket and the answer reads as from port 53",
           udp_round(s, &port, ip) == 0 && port == 53 && strcmp(ip, "127.0.0.11") == 0);
    EXPECT("a datagram for another port is not redirected",
           ({ int c = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in o = at("127.0.0.11", 5300); char b[8];
              sendto(c, "x", 1, 0, (void *)&o, sizeof o);
              int r = recv(s, b, sizeof b, MSG_DONTWAIT); close(c); r == -1 && errno == EAGAIN; }));

    int l = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT("a stream listener binds 127.0.0.11 at a port of its own",
           bind(l, (void *)&any, sizeof any) == 0 && listen(l, 4) == 0 && bound_port(l) > 0 && bound_port(l) != 53);
    struct sockaddr_in own = at("127.0.0.11", bound_port(l));
    EXPECT("a stream connection to the listener's own port works",
           ({ int c = socket(AF_INET, SOCK_STREAM, 0); int ok = connect(c, (void *)&own, sizeof own) == 0; int d = ok ? accept(l, 0, 0) : -1; ok = ok && d >= 0; close(c); if (d >= 0) close(d); ok; }));
    EXPECT("a stream connection to 127.0.0.11:53 reaches that listener",
           ({ int c = socket(AF_INET, SOCK_STREAM, 0); char b[8];
              int ok = connect(c, (void *)&a, sizeof a) == 0;
              int d = ok ? accept(l, 0, 0) : -1;
              ok = ok && d >= 0 && write(c, "tcp", 3) == 3 && read(d, b, 3) == 3 && memcmp(b, "tcp", 3) == 0;
              close(c); if (d >= 0) close(d); ok; }));
    return done();
}
