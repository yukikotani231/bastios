/* compat-timeout: 240
 * mmap when memory is short: the pages a shared file mapping needs come
 * from the page cache, and when no page is free the kernel gives cached
 * pages back first, as Linux does, instead of saying ENOMEM.
 */
#include "compat.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

static long meminfo(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long v = -1;
    size_t n = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, key, n) == 0) v = atol(line + n);
    if (f) fclose(f);
    return v;
}

int main(void) {
    setup();
    const char *big = "/tmp/t_mmapmem.big", *small = "/tmp/t_mmapmem.sh";
    static char buf[1 << 16];
    memset(buf, 'b', sizeof buf);
    int fd = open(big, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int ok = fd >= 0;
    for (int i = 0; ok && i < 256; i++) ok = write(fd, buf, sizeof buf) == sizeof buf;   /* 16 MiB */
    close(fd);
    int sfd = open(small, O_RDWR | O_CREAT | O_TRUNC, 0600);
    EXPECT("a 16 MiB file and an empty one of 1 MiB", ok && sfd >= 0 && ftruncate(sfd, 1 << 20) == 0);
    if (begin("with memory full, a shared mapping of a file gets its pages")) {
        int pfd[2];
        pipe(pfd);
        pid_t c = fork();
        if (c == 0) {
            /* Nearly all free memory, mapped now, then the big file read
             * through the cache until no page is free. */
            long kb = meminfo("MemFree:");
            long want = (kb - 512) * 1024L;
            char *a = want > 0 ? mmap(0, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0) : MAP_FAILED;
            int bfd = open(big, O_RDONLY);
            while (read(bfd, buf, sizeof buf) > 0) {}
            close(bfd);
            long left = meminfo("MemFree:");
            char *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);   /* 256 pages: more than are free */
            int e = errno;
            char r[128];
            snprintf(r, sizeof r, "%d %d %ld %ld", a != MAP_FAILED, m != MAP_FAILED ? 0 : e, kb, left);
            write(pfd[1], r, strlen(r) + 1);
            if (m != MAP_FAILED) m[4096] = 'x';
            _exit(0);
        }
        close(pfd[1]);
        char r[128] = {0};
        read(pfd[0], r, sizeof r - 1);
        int st = 0;
        waitpid(c, &st, 0);
        int filled = 0, err = -1;
        long kb = 0, left = 0;
        sscanf(r, "%d %d %ld %ld", &filled, &err, &kb, &left);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && filled && err == 0) pass();
        else fail("status %#x, memory taken %d, mmap errno %d (free %ld kB, then %ld kB)", st, filled, err, kb, left);
    }
    char *m = mmap(0, 1 << 20, PROT_READ, MAP_SHARED, sfd, 0);
    EXPECT("the child's write is in the file", m != MAP_FAILED && m[4096] == 'x');
    close(sfd);
    unlink(big);
    unlink(small);
    return done();
}
