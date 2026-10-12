/* First boot on a cloud machine, against musl: ask the instance metadata
 * service (the AWS IMDSv2 protocol, which GCE and others also speak in
 * part) for the host name and the first SSH public key, set the one and
 * install the other for root.
 *
 *     mcloud [ADDRESS[:PORT]]      default 169.254.169.254:80
 *
 * Started by the kernel under the boot option `mcloud` (as `mcloud-boot`,
 * the only way to run a program at boot without arguments), it waits up to
 * a minute for the service to answer, since the network address comes from
 * DHCP some seconds after boot. Started by hand it asks once.
 *
 * IMDSv2 first takes a session token with a PUT and then sends it back as a
 * header on each GET. A key the service does not have (404) is skipped.
 * User data (a script, say) is saved as ~/user-data for root with mode 0700
 * and is never run here: running what a network service hands out is left
 * to whoever reads it.
 * Nothing it reads is trusted further than being a host name (letters,
 * digits, '-' and '.') or one line of key text. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BODY_MAX 4096

static struct sockaddr_in svc;
static char hostport[64];

/* One request; the status code, and the body in `body` (NUL-ended, at most
 * BODY_MAX - 1 bytes). -1 if the service could not be reached. */
static int request(const char *method, const char *path, const char *header, char *body)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    if (connect(s, (struct sockaddr *)&svc, sizeof svc) != 0) {
        close(s);
        return -1;
    }
    char req[512];
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: %s\r\n%sContent-Length: 0\r\nConnection: close\r\n\r\n",
                     method, path, hostport, header);
    if (n <= 0 || n >= (int)sizeof req || write(s, req, n) != n) {
        close(s);
        return -1;
    }
    static char resp[BODY_MAX + 512];
    size_t have = 0;
    ssize_t r;
    while (have < sizeof resp - 1 && (r = read(s, resp + have, sizeof resp - 1 - have)) > 0)
        have += r;
    close(s);
    resp[have] = 0;
    int status = 0;
    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1)
        return -1;
    char *b = strstr(resp, "\r\n\r\n");
    size_t blen = b ? strlen(b + 4) : 0;
    if (blen >= BODY_MAX)
        blen = BODY_MAX - 1;
    memcpy(body, b ? b + 4 : "", blen);
    body[blen] = 0;
    return status;
}

static int host_ok(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 63)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

int main(int argc, char **argv)
{
    const char *addr = argc > 1 ? argv[1] : "169.254.169.254";
    char ip[64];
    int port = 80;
    strncpy(ip, addr, sizeof ip - 1);
    ip[sizeof ip - 1] = 0;
    char *colon = strchr(ip, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    svc.sin_family = AF_INET;
    svc.sin_port = htons(port);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, ip, &svc.sin_addr) != 1) {
        printf("mcloud: %s is not ADDRESS[:PORT]\n", addr);
        return 2;
    }
    snprintf(hostport, sizeof hostport, "%s", ip);

    static char token[BODY_MAX], body[BODY_MAX];
    int st = request("PUT", "/latest/api/token", "X-aws-ec2-metadata-token-ttl-seconds: 60\r\n", token);
    for (int tries = 0; st < 0 && argc > 0 && strstr(argv[0], "mcloud-boot") && tries < 60; tries++) {
        sleep(1);
        st = request("PUT", "/latest/api/token", "X-aws-ec2-metadata-token-ttl-seconds: 60\r\n", token);
    }
    if (st < 0) {
        printf("mcloud: no metadata service at %s\n", addr);
        return 1;
    }
    trim(token);
    if (st != 200 || token[0] == 0 || strpbrk(token, "\r\n")) {
        printf("mcloud: no token (status %d)\n", st);
        return 1;
    }
    char hdr[BODY_MAX + 64];
    snprintf(hdr, sizeof hdr, "X-aws-ec2-metadata-token: %s\r\n", token);

    st = request("GET", "/latest/meta-data/hostname", hdr, body);
    trim(body);
    if (st == 200 && host_ok(body)) {
        if (sethostname(body, strlen(body)) != 0) {
            printf("mcloud: sethostname failed (errno %d)\n", errno);
            return 1;
        }
        printf("mcloud: hostname %s\n", body);
    } else {
        printf("mcloud: no usable hostname (status %d)\n", st);
    }

    st = request("GET", "/latest/meta-data/public-keys/0/openssh-key", hdr, body);
    trim(body);
    if (st == 200 && body[0] && !strchr(body, '\n')) {
        struct passwd *pw = getpwnam("root");
        const char *home = pw && pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : "/root";
        char dir[256], file[300];
        snprintf(dir, sizeof dir, "%s/.ssh", home);
        snprintf(file, sizeof file, "%s/authorized_keys", dir);
        mkdir(home, 0755);
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            printf("mcloud: mkdir %s failed (errno %d)\n", dir, errno);
            return 1;
        }
        chmod(dir, 0700);
        int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        size_t n = strlen(body);
        if (fd < 0 || write(fd, body, n) != (ssize_t)n || write(fd, "\n", 1) != 1) {
            printf("mcloud: writing %s failed (errno %d)\n", file, errno);
            return 1;
        }
        close(fd);
        chmod(file, 0600);
        printf("mcloud: key installed in %s\n", file);
    } else {
        printf("mcloud: no public key (status %d)\n", st);
    }

    st = request("GET", "/latest/user-data", hdr, body);
    if (st == 200 && body[0]) {
        struct passwd *pw = getpwnam("root");
        const char *home = pw && pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : "/root";
        char file[300];
        snprintf(file, sizeof file, "%s/user-data", home);
        mkdir(home, 0755);
        int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0700);
        size_t n = strlen(body);
        if (fd < 0 || write(fd, body, n) != (ssize_t)n) {
            printf("mcloud: writing %s failed (errno %d)\n", file, errno);
            return 1;
        }
        close(fd);
        chmod(file, 0700);
        printf("mcloud: user data saved in %s (%d bytes)\n", file, (int)n);
    } else {
        printf("mcloud: no user data (status %d)\n", st);
    }
    return 0;
}
