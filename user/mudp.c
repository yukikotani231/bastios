/* UDP sockets through the Linux system calls, against musl: a
 * nonblocking recv with nothing queued, then bind, getsockname, a
 * blocking recvfrom, sendto back to the sender, and connect + write. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("mudp: %s failed (errno %d)\n", what, errno);
    return 1;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 7000;
    char buf[256];

    int nb = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (nb < 0)
        return fail("socket");
    if (recv(nb, buf, sizeof buf, 0) != -1 || errno != EAGAIN)
        return fail("nonblocking recv");
    close(nb);
    if (socket(AF_INET, SOCK_STREAM, 0) != -1)
        return fail("refusing TCP");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (s < 0 || bind(s, (struct sockaddr *)&me, sizeof me) != 0)
        return fail("bind");
    struct sockaddr_in name;
    socklen_t len = sizeof name;
    if (getsockname(s, (struct sockaddr *)&name, &len) != 0 || ntohs(name.sin_port) != port)
        return fail("getsockname");
    printf("mudp: listening on udp port %d\n", port);
    fflush(stdout);

    struct sockaddr_in from;
    socklen_t flen = sizeof from;
    ssize_t n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &flen);
    if (n < 0)
        return fail("recvfrom");
    buf[n] = 0;
    printf("mudp: %d bytes from %s: %s\n", (int)n, inet_ntoa(from.sin_addr), buf);
    if (sendto(s, buf, n, 0, (struct sockaddr *)&from, flen) != n)
        return fail("sendto");
    if (connect(s, (struct sockaddr *)&from, flen) != 0 || write(s, "bye", 3) != 3)
        return fail("connect and write");
    printf("mudp: sent it back\n");
    close(s);
    return 0;
}
