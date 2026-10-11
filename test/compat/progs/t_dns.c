/* Name lookups through a name server, as programs make them: the C
 * library's resolver (getaddrinfo, gethostbyname, getnameinfo) reading
 * /etc/resolv.conf and asking over UDP. A child plays the name server on
 * 127.0.0.1:53 (A records for *.test, an empty answer for AAAA, "no such
 * name" for the rest); /etc/resolv.conf points at it for the checks and is
 * put back after. Binding port 53 and writing /etc need root: run as root
 * on Linux (compat-host) too. */
#include "compat.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define CONF "/etc/resolv.conf"

/* The name server: answers queries until killed. */
static void serve(int s) {
    unsigned char q[512], r[512];
    for (;;) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(s, q, sizeof q, 0, (void *)&from, &fl);
        if (n < 12 + 5) continue;
        /* The question: one name, then type and class. */
        size_t o = 12;
        char name[256];
        size_t nl = 0;
        while (o < (size_t)n && q[o] != 0 && nl + q[o] + 1 < sizeof name) {
            if (nl) name[nl++] = '.';
            memcpy(name + nl, q + o + 1, q[o]);
            nl += q[o];
            o += q[o] + 1;
        }
        name[nl] = 0;
        if (o + 5 > (size_t)n) continue;
        unsigned type = q[o + 1] << 8 | q[o + 2];
        size_t qend = o + 5;
        memcpy(r, q, qend);
        r[2] = 0x84 | (q[2] & 1);           /* a response, authoritative, RD copied */
        r[3] = 0x80;                        /* recursion available, NOERROR */
        r[6] = r[7] = r[8] = r[9] = r[10] = r[11] = 0;
        size_t len = qend;
        int test = nl > 5 && strcmp(name + nl - 5, ".test") == 0;
        if (!test && type != 12) {
            r[3] |= 3;                      /* no such name */
        } else if (type == 1) {
            /* one A record: 10.0.2.2, or 127.0.0.1 for lo.test */
            static const unsigned char rr[] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            memcpy(r + len, rr, sizeof rr);
            len += sizeof rr;
            if (strcmp(name, "lo.test") == 0) {
                memcpy(r + len, "\x7f\x00\x00\x01", 4);
            } else {
                memcpy(r + len, "\x0a\x00\x02\x02", 4);
            }
            len += 4;
            r[7] = 1;
        } else if (type == 12 && strcmp(name, "2.2.0.10.in-addr.arpa") != 0) {
            r[3] |= 3;
        } else if (type == 12) {
            /* the reverse of 10.0.2.2: host.test */
            static const unsigned char rr[] = {0xC0, 12, 0, 12, 0, 1, 0, 0, 0, 60, 0, 11,
                                               4, 'h', 'o', 's', 't', 4, 't', 'e', 's', 't', 0};
            memcpy(r + len, rr, sizeof rr);
            len += sizeof rr;
            r[7] = 1;
        }
        sendto(s, r, len, 0, (void *)&from, fl);
    }
}

static char saved[4096];
static ssize_t nsaved = -1;

static void restore(void) {
    int fd = open(CONF, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (nsaved > 0) write(fd, saved, nsaved);
        close(fd);
    }
    if (nsaved < 0) unlink(CONF);
}

static int lookup4(const char *name, int family, const char *want) {
    struct addrinfo h = {.ai_family = family, .ai_socktype = SOCK_STREAM}, *r = 0, *p;
    int e = getaddrinfo(name, "80", &h, &r);
    if (e) {
        printf("(getaddrinfo: %s) ", gai_strerror(e));
        return 0;
    }
    int ok = 0;
    for (p = r; p; p = p->ai_next) {
        char b[64] = "";
        if (p->ai_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ai_addr)->sin_addr, b, sizeof b);
        else
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr, b, sizeof b);
        if (strcmp(b, want) == 0) ok = 1;
        else printf("(got %s) ", b);
    }
    freeaddrinfo(r);
    return ok;
}

int main(void) {
    setup();
    int s = -1, fd = -1;
    pid_t srv = -1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};

    EXPECT("resolv.conf names a server", (fd = open(CONF, O_RDONLY)) >= 0 &&
           (nsaved = read(fd, saved, sizeof saved - 1)) > 0 && (saved[nsaved] = 0, strstr(saved, "nameserver ") != NULL));
    if (fd >= 0) close(fd);
    EXPECT("name server binds UDP port 53", (s = socket(AF_INET, SOCK_DGRAM, 0)) >= 0 && bind(s, (void *)&a, sizeof a) == 0);
    if (s >= 0 && (srv = fork()) == 0) {
        serve(s);
        _exit(0);
    }
    if (s >= 0) close(s);
    EXPECT("point resolv.conf at it", (fd = open(CONF, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0 &&
           write(fd, "nameserver 127.0.0.1\noptions timeout:2 attempts:2\n", 51) == 51 && close(fd) == 0);

    EXPECT("getaddrinfo AF_INET", lookup4("www.test", AF_INET, "10.0.2.2"));
    EXPECT("getaddrinfo AF_UNSPEC (A and AAAA)", lookup4("www.test", AF_UNSPEC, "10.0.2.2"));
    EXPECT("getaddrinfo second name", lookup4("lo.test", AF_UNSPEC, "127.0.0.1"));
    {
        struct addrinfo h = {.ai_family = AF_INET6, .ai_flags = AI_V4MAPPED, .ai_socktype = SOCK_STREAM};
        struct addrinfo *r = 0;
        EXPECT("getaddrinfo AF_INET6 v4-mapped", getaddrinfo("www.test", "80", &h, &r) == 0 && r && r->ai_family == AF_INET6 &&
               memcmp(((struct sockaddr_in6 *)r->ai_addr)->sin6_addr.s6_addr + 10, "\xff\xff\x0a\x00\x02\x02", 6) == 0);
        if (r) freeaddrinfo(r);
    }
    {
        struct addrinfo h = {.ai_family = AF_UNSPEC}, *r = 0;
        int e = getaddrinfo("nowhere.example", "80", &h, &r);
        EXPECT("getaddrinfo: no such name", e == EAI_NONAME);
        if (!e) freeaddrinfo(r);
    }
    {
        struct hostent *h = gethostbyname("www.test");
        EXPECT("gethostbyname", h && h->h_length == 4 && memcmp(h->h_addr_list[0], "\x0a\x00\x02\x02", 4) == 0);
    }
    {
        struct sockaddr_in b = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(0x0A000202)};
        char host[64] = "";
        EXPECT("getnameinfo reverse lookup", getnameinfo((void *)&b, sizeof b, host, sizeof host, 0, 0, NI_NAMEREQD) == 0 &&
               strcmp(host, "host.test") == 0);
    }
    {
        /* A TCP connection to a looked-up name: a server here on lo.test. */
        struct addrinfo h = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *r = 0;
        struct sockaddr_in l = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t ll = sizeof l;
        int ls = socket(AF_INET, SOCK_STREAM, 0), c = -1, u = -1;
        char port[8], b[4];
        if (ls >= 0 && bind(ls, (void *)&l, sizeof l) == 0 && listen(ls, 1) == 0 && getsockname(ls, (void *)&l, &ll) == 0)
            snprintf(port, sizeof port, "%d", ntohs(l.sin_port));
        EXPECT("connect to a looked-up name", getaddrinfo("lo.test", port, &h, &r) == 0 &&
               (c = socket(r->ai_family, SOCK_STREAM, 0)) >= 0 && connect(c, r->ai_addr, r->ai_addrlen) == 0 &&
               write(c, "dns", 3) == 3 && (u = accept(ls, 0, 0)) >= 0 && read(u, b, 3) == 3 && memcmp(b, "dns", 3) == 0);
        if (r) freeaddrinfo(r);
        close(c); close(u); close(ls);
    }

    if (srv > 0) {
        kill(srv, SIGKILL);
        waitpid(srv, 0, 0);
    }
    restore();
    EXPECT("resolv.conf put back", (fd = open(CONF, O_RDONLY)) >= 0 && close(fd) == 0);
    return done();
}
