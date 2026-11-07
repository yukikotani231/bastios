/* /sys/class/net/lo and /sys/class/net/eth0: the files ip link and shell
 * scripts read for an interface (address, operstate, mtu, carrier, ifindex,
 * type, flags, tx_queue_len), agreeing with what the socket calls say. */
#define _GNU_SOURCE
#include "compat.h"
#include <fcntl.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int attr(const char *ifn, const char *name, char *buf, int n) {
    char p[128];
    snprintf(p, sizeof p, "/sys/class/net/%s/%s", ifn, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    int r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return r;
}

int main(void) {
    setup();
    char b[64];
    EXPECT("lo's address is all zeros", attr("lo", "address", b, sizeof b) > 0 && !strcmp(b, "00:00:00:00:00:00\n"));
    EXPECT("lo's operstate is unknown, as on Linux", attr("lo", "operstate", b, sizeof b) > 0 && !strcmp(b, "unknown\n"));
    EXPECT("lo's mtu is 65536", attr("lo", "mtu", b, sizeof b) > 0 && atoi(b) == 65536);
    EXPECT("lo's ifindex is 1", attr("lo", "ifindex", b, sizeof b) > 0 && atoi(b) == 1);
    EXPECT("lo's type is ARPHRD_LOOPBACK (772)", attr("lo", "type", b, sizeof b) > 0 && atoi(b) == 772);
    EXPECT("lo's flags are up and loopback", attr("lo", "flags", b, sizeof b) > 0 && !strncmp(b, "0x", 2) && (strtoul(b, 0, 16) & (IFF_UP | IFF_LOOPBACK)) == (IFF_UP | IFF_LOOPBACK));
    EXPECT("lo has a carrier", attr("lo", "carrier", b, sizeof b) > 0 && atoi(b) == 1);
    EXPECT("lo's tx_queue_len is a number", attr("lo", "tx_queue_len", b, sizeof b) > 0 && atoi(b) > 0);
    EXPECT("an unknown file is ENOENT", attr("lo", "nonesuch", b, sizeof b) == -1 && errno == ENOENT);
    EXPECT("an unknown interface is ENOENT", attr("nonesuch0", "mtu", b, sizeof b) == -1 && errno == ENOENT);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strcpy(ifr.ifr_name, "eth0");
    if (s >= 0 && ioctl(s, SIOCGIFINDEX, &ifr) == 0) {
        EXPECT("eth0's ifindex agrees with SIOCGIFINDEX", attr("eth0", "ifindex", b, sizeof b) > 0 && atoi(b) == ifr.ifr_ifindex);
        struct ifreq hw;
        memset(&hw, 0, sizeof hw);
        strcpy(hw.ifr_name, "eth0");
        char want[32] = "";
        if (ioctl(s, SIOCGIFHWADDR, &hw) == 0) {
            unsigned char *m = (unsigned char *)hw.ifr_hwaddr.sa_data;
            snprintf(want, sizeof want, "%02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        EXPECT("eth0's address agrees with SIOCGIFHWADDR", want[0] && attr("eth0", "address", b, sizeof b) > 0 && !strcmp(b, want));
        EXPECT("eth0's type is ARPHRD_ETHER (1)", attr("eth0", "type", b, sizeof b) > 0 && atoi(b) == 1);
        EXPECT("eth0's mtu is 1500", attr("eth0", "mtu", b, sizeof b) > 0 && atoi(b) == 1500);
        EXPECT("eth0 is up", attr("eth0", "operstate", b, sizeof b) > 0 && !strcmp(b, "up\n"));
    } else {
        EXPECT("without a card eth0's files are empty", attr("eth0", "mtu", b, sizeof b) == 0);
    }
    return done();
}
