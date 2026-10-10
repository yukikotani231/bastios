/* TCP through the Linux system calls, against musl.
 *   mtcp serve PORT        accept one connection, read a line, send back
 *                          "echo: LINE", close
 *   mtcp get A.B.C.D PORT TEXT
 *                          connect, send TEXT and a newline, print what
 *                          comes back until the peer closes
 *   mtcp                   serve 7001; the line read names a port on
 *                          10.0.2.2 (QEMU's host) to get from next. The
 *                          BastiOS shell passes no arguments, so the test
 *                          drives both through this. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("mtcp: %s failed (errno %d)\n", what, errno);
    return 1;
}

static int served_port;

static int serve(int port)
{
    int one = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0)
        return fail("socket");
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0 || listen(s, 1) != 0)
        return fail("bind and listen");
    printf("mtcp: listening on tcp port %d\n", port);
    fflush(stdout);
    struct sockaddr_in from;
    socklen_t flen = sizeof from;
    int c = accept(s, (struct sockaddr *)&from, &flen);
    if (c < 0)
        return fail("accept");
    printf("mtcp: connection from %s\n", inet_ntoa(from.sin_addr));
    char line[200] = "echo: ";
    size_t n = 6;
    while (n < sizeof line - 1 && (n == 6 || line[n - 1] != '\n')) {
        ssize_t r = read(c, line + n, sizeof line - 1 - n);
        if (r <= 0)
            return fail("read");
        n += r;
    }
    if (write(c, line, n) != (ssize_t)n)
        return fail("write");
    line[n] = 0;
    served_port = atoi(line + 6);
    close(c);
    close(s);
    printf("mtcp: served %d bytes\n", (int)n);
    return 0;
}

static int get(const char *ip, int port, const char *text)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (s < 0 || inet_aton(ip, &to.sin_addr) == 0)
        return fail("socket");
    if (connect(s, (struct sockaddr *)&to, sizeof to) != 0)
        return fail("connect");
    char out[200];
    int n = snprintf(out, sizeof out, "%s\n", text);
    if (write(s, out, n) != n)
        return fail("write");
    char in[200];
    size_t got = 0;
    ssize_t r;
    while (got < sizeof in - 1 && (r = read(s, in + got, sizeof in - 1 - got)) > 0)
        got += r;
    in[got] = 0;
    close(s);
    printf("mtcp: got %s", in);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "serve") == 0)
        return serve(atoi(argv[2]));
    if (argc == 1) {
        if (serve(7001) != 0)
            return 1;
        return get("10.0.2.2", served_port, "linux says hi");
    }
    if (argc == 5 && strcmp(argv[1], "get") == 0)
        return get(argv[2], atoi(argv[3]), argv[4]);
    printf("usage: mtcp serve PORT | mtcp get A.B.C.D PORT TEXT\n");
    return 2;
}
