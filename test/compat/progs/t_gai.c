/* The source address a connected UDP socket reads back, which glibc's
 * getaddrinfo sorts by: connecting to "this host" (0.0.0.0, ::) gives
 * the loopback address; an AF_INET6 socket connected to a struct
 * sockaddr_in reads back ::ffff:127.0.0.1. Without these, a dual-stack
 * getaddrinfo(NULL, port, AI_PASSIVE) aborts in a static glibc program. */
#include "compat.h"
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int name_of(int s, struct sockaddr_storage *ss) {
    socklen_t sl = sizeof *ss;
    memset(ss, 0, sizeof *ss);
    return getsockname(s, (void *)ss, &sl);
}

int main(void) {
    setup();
    struct sockaddr_storage ss;

    int s4 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a4 = {.sin_family = AF_INET, .sin_port = htons(22)};
    EXPECT("AF_INET UDP connect to 0.0.0.0 reads back 127.0.0.1",
           connect(s4, (void *)&a4, sizeof a4) == 0 && name_of(s4, &ss) == 0 && ss.ss_family == AF_INET &&
           ((struct sockaddr_in *)&ss)->sin_addr.s_addr == htonl(INADDR_LOOPBACK));

    int s6 = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_port = htons(22)};
    EXPECT("AF_INET6 UDP connect to :: reads back ::1",
           connect(s6, (void *)&a6, sizeof a6) == 0 && name_of(s6, &ss) == 0 && ss.ss_family == AF_INET6 &&
           memcmp(&((struct sockaddr_in6 *)&ss)->sin6_addr, &in6addr_loopback, 16) == 0);

    int m6 = socket(AF_INET6, SOCK_DGRAM, 0);
    EXPECT("AF_INET6 UDP connect to a sockaddr_in reads back ::ffff:127.0.0.1",
           connect(m6, (void *)&a4, sizeof a4) == 0 && name_of(m6, &ss) == 0 && ss.ss_family == AF_INET6 &&
           IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)&ss)->sin6_addr) &&
           ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr[15] == 1);

    struct addrinfo h, *r;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_UNSPEC;
    h.ai_socktype = SOCK_STREAM;
    h.ai_flags = AI_PASSIVE;
    EXPECT("getaddrinfo(NULL, port, AI_PASSIVE) with AF_UNSPEC", getaddrinfo(NULL, "22", &h, &r) == 0 && r != NULL);
    return done();
}
