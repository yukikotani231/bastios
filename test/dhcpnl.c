/* The guest side of the rtnetlink part of test/dhcp_test.sh: joins the IPv4
 * address and route groups, says "ready", and waits for the test script's
 * server to give the address 10.9.0.15/24 (gateway 10.9.0.1) and then refuse
 * its renewal. Prints one line per check ("ok WHAT" or "FAIL WHAT: why") and
 * "done N". Built against musl as a static PIE and run from the test's disk. */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;

static void report(int ok, const char *what, const char *why) {
    if (ok) printf("ok %s\n", what);
    else { printf("FAIL %s: %s\n", what, why); fails++; }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int s = socket(16 /* AF_NETLINK */, SOCK_RAW, 0 /* NETLINK_ROUTE */);
    struct { uint16_t family, pad; uint32_t pid, groups; } sa = {16, 0, 0, 16 | 64};   /* IPV4_IFADDR, IPV4_ROUTE */
    report(s >= 0 && bind(s, (void *)&sa, sizeof sa) == 0, "binds to the IPv4 address and route groups", "bind");
    unsigned char addr[4], net[4], gw[4];
    inet_pton(AF_INET, "10.9.0.15", addr);
    inet_pton(AF_INET, "10.9.0.0", net);
    inet_pton(AF_INET, "10.9.0.1", gw);
    printf("ready\n");
    /* new/del address, new/del connected route, new/del default route */
    int seen[6] = {0}, stray = 0;
    for (int round = 0; round < 80; round++) {
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
            int addr_msg = ty == 20 || ty == 21, route = ty == 24 || ty == 25;
            if (!addr_msg && !route) stray++;
            size_t hdr = addr_msg ? 8 : 12;                                     /* ifaddrmsg, rtmsg */
            unsigned dstlen = route ? b[o + 16 + 1] : 0;
            int is_new = ty == 20 || ty == 24;
            for (size_t a = o + 16 + hdr; (addr_msg || route) && a + 4 <= o + ml;) {
                uint16_t al, at;
                memcpy(&al, b + a, 2);
                memcpy(&at, b + a + 2, 2);
                if (al < 4 || a + al > o + ml) break;
                if (al == 8) {
                    const unsigned char *v = b + a + 4;
                    if (addr_msg && at == 1 && !memcmp(v, addr, 4)) seen[is_new ? 0 : 1] = 1;                /* IFA_ADDRESS */
                    if (route && dstlen == 24 && at == 1 && !memcmp(v, net, 4)) seen[is_new ? 2 : 3] = 1;    /* RTA_DST */
                    if (route && dstlen == 0 && at == 5 && !memcmp(v, gw, 4)) seen[is_new ? 4 : 5] = 1;      /* RTA_GATEWAY */
                }
                a += (al + 3) & ~3u;
            }
            o += (ml + 3) & ~3u;
        }
        int all = 1;
        for (int i = 0; i < 6; i++) all &= seen[i];
        if (all) break;
    }
    report(seen[0], "a lease announces its address (RTM_NEWADDR)", "not heard");
    report(seen[2], "a lease announces the connected route (RTM_NEWROUTE)", "not heard");
    report(seen[4], "a lease announces the default route through the gateway (RTM_NEWROUTE)", "not heard");
    report(seen[1], "a refused renewal withdraws the address (RTM_DELADDR)", "not heard");
    report(seen[3], "a refused renewal withdraws the connected route (RTM_DELROUTE)", "not heard");
    report(seen[5], "a refused renewal withdraws the default route (RTM_DELROUTE)", "not heard");
    report(stray == 0, "nothing but address and route messages in those groups", "other message");
    printf("done %d\n", fails);
    return fails != 0;
}
