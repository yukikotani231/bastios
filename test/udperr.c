/* The guest side of the ICMP error scenario in test/tcp_test.sh: a UDP
 * datagram to 10.0.2.2:9999 draws an ICMP port unreachable from the test
 * script. A connected socket must see ECONNREFUSED once (RFC 1122 4.1.3.3);
 * an unconnected one sees nothing. Prints "ok WHAT" or "FAIL WHAT: why" per
 * check, then "done N". Built against musl as a static PIE. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;

static void report(int ok, const char *what) {
    if (ok) printf("ok %s\n", what);
    else { printf("FAIL %s (errno %d %s)\n", what, errno, strerror(errno)); fails++; }
}

int main(void) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(9999)};
    inet_pton(AF_INET, "10.0.2.2", &a.sin_addr);
    char buf[16];

    int c = socket(AF_INET, SOCK_DGRAM, 0);
    report(connect(c, (struct sockaddr *)&a, sizeof a) == 0, "udp: connect");
    report(send(c, "ping", 4, 0) == 4, "udp: send to a closed port");
    struct pollfd p = {.fd = c, .events = POLLIN};
    poll(&p, 1, 1500);                         /* the ICMP error comes in meanwhile */
    errno = 0;
    ssize_t r = recv(c, buf, sizeof buf, MSG_DONTWAIT);
    report(r < 0 && errno == ECONNREFUSED, "udp: a connected socket's receive reports ECONNREFUSED");
    errno = 0;
    r = recv(c, buf, sizeof buf, MSG_DONTWAIT);
    report(r < 0 && errno == EAGAIN, "udp: the error is reported once");

    send(c, "ping", 4, 0);
    usleep(1500000);
    errno = 0;
    report(send(c, "ping", 4, 0) < 0 && errno == ECONNREFUSED, "udp: the next send reports ECONNREFUSED");
    report(send(c, "ping", 4, 0) == 4, "udp: and the one after goes out");

    /* poll reports POLLERR, and SO_ERROR takes the error. First drop the
     * error the send above drew. */
    usleep(1500000);
    recv(c, buf, sizeof buf, MSG_DONTWAIT);
    report(send(c, "ping", 4, 0) == 4, "udp: a send goes out");
    p.events = 0;
    int pr = poll(&p, 1, 3000);
    report(pr == 1 && (p.revents & POLLERR), "udp: poll reports POLLERR");
    int soe = -1; socklen_t sl = sizeof soe;
    getsockopt(c, SOL_SOCKET, SO_ERROR, &soe, &sl);
    errno = soe;
    report(soe == ECONNREFUSED, "udp: SO_ERROR reports ECONNREFUSED");
    soe = -1; sl = sizeof soe;
    getsockopt(c, SOL_SOCKET, SO_ERROR, &soe, &sl);
    report(soe == 0, "udp: SO_ERROR reports it once");
    pr = poll(&p, 1, 0);
    report(pr == 0, "udp: and POLLERR is gone");
    report(send(c, "ping", 4, 0) == 4, "udp: a send after SO_ERROR goes out");

    int u = socket(AF_INET, SOCK_DGRAM, 0);
    report(sendto(u, "ping", 4, 0, (struct sockaddr *)&a, sizeof a) == 4, "udp: unconnected send");
    usleep(1500000);
    errno = 0;
    r = recv(u, buf, sizeof buf, MSG_DONTWAIT);
    report(r < 0 && errno == EAGAIN, "udp: an unconnected socket sees no error");
    errno = 0;
    report(sendto(u, "ping", 4, 0, (struct sockaddr *)&a, sizeof a) == 4, "udp: and sends again");

    printf("done %d\n", fails);
    return fails != 0;
}
