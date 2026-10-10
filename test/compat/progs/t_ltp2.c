/* Small system call corners LTP's tests found: file status flags, the
 * supplementary groups, fsync on a pipe, mlockall, personality, mincore,
 * and socket errors (listen on UDP, send before connect, a bad address
 * length, MSG_OOB on UDP). */
#include "compat.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/socket.h>

int main(void) {
    setup();
    signal(SIGPIPE, SIG_IGN);

    EXPECT("F_GETFL reports the access mode of a file", ({ int fd = open("cf_ltp2", O_WRONLY | O_CREAT | O_TRUNC, 0644);
           int f = fcntl(fd, F_GETFL); close(fd); unlink("cf_ltp2"); (f & O_ACCMODE) == O_WRONLY; }));
    EXPECT("F_SETFL O_APPEND appends to a file", ({ int fd = open("cf_ltp2", O_RDWR | O_CREAT | O_TRUNC, 0644);
           write(fd, "ab", 2); lseek(fd, 0, SEEK_SET); int s = fcntl(fd, F_SETFL, O_APPEND | O_NONBLOCK);
           write(fd, "c", 1); int f = fcntl(fd, F_GETFL); char b[4] = {0}; pread(fd, b, 3, 0); close(fd); unlink("cf_ltp2");
           s == 0 && (f & (O_ACCMODE | O_APPEND | O_NONBLOCK)) == (O_RDWR | O_APPEND | O_NONBLOCK) && memcmp(b, "abc", 3) == 0; }));
    EXPECT("getgroups counts and lists the groups", ({ gid_t g[64]; int n = getgroups(0, 0);
           n >= 0 && n <= 64 && getgroups(64, g) == n && (n < 1 || getgroups(n - 1, g) == -1 || n == 1); }));
    EXPECT("getgroups with a negative size is EINVAL", getgroups(-1, 0) == -1 && errno == EINVAL);
    EXPECT("fsync of a pipe is EINVAL", ({ int p[2]; pipe(p); int r = fsync(p[0]); int e = errno; close(p[0]); close(p[1]);
           r == -1 && e == EINVAL; }));
    EXPECT("mlockall and munlockall succeed", mlockall(MCL_CURRENT | MCL_FUTURE) == 0 && munlockall() == 0);
    EXPECT("personality is kept", ({ int o = personality(0xffffffff); personality(PER_LINUX32);
           int n = personality(0xffffffff); personality(o); n == PER_LINUX32 && personality(0xffffffff) == o; }));
    EXPECT("mincore marks mapped pages", ({ char *a = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
           a[0] = a[8192] = 1; unsigned char v[3] = {0}; int r = mincore(a, 3 * 4096, v); munmap(a, 3 * 4096); r == 0 && (v[0] & 1) && (v[2] & 1); }));
    EXPECT("mincore errors", ({ char *a = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); unsigned char v;
           int e1 = mincore(a + 1, 1, &v) == -1 && errno == EINVAL;
           int e2 = mincore(a, 4096, (void *)8) == -1 && errno == EFAULT;
           munmap(a, 4096);
           int e3 = mincore(a, 4096, &v) == -1 && errno == ENOMEM; e1 && e2 && e3; }));
    EXPECT("listen on a UDP socket is EOPNOTSUPP", ({ int s = socket(AF_INET, SOCK_DGRAM, 0); int r = listen(s, 1); int e = errno;
           close(s); r == -1 && e == EOPNOTSUPP; }));
    EXPECT("send on an unconnected TCP socket is EPIPE", ({ int s = socket(AF_INET, SOCK_STREAM, 0); int r = send(s, "x", 1, MSG_NOSIGNAL);
           int e = errno; close(s); r == -1 && e == EPIPE; }));
    EXPECT("sendto with a huge address length is EINVAL", ({ int s = socket(AF_INET, SOCK_DGRAM, 0);
           struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
           int r = sendto(s, "x", 1, 0, (struct sockaddr *)&to, (socklen_t)-1); int e = errno; close(s); r == -1 && e == EINVAL; }));
    EXPECT("MSG_OOB on UDP is EOPNOTSUPP", ({ int s = socket(AF_INET, SOCK_DGRAM, 0);
           struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
           int r = sendto(s, "x", 1, MSG_OOB, (struct sockaddr *)&to, sizeof to); int e = errno; close(s); r == -1 && e == EOPNOTSUPP; }));
    return done();
}
