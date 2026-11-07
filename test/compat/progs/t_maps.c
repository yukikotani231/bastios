/* /proc/PID/maps as Crashpad (Chromium's crash handler) reads it: every line
 * parses with its sscanf format, the file reads the same in small pieces as in
 * one go, and another process's maps (it reads the crashed process's) read the
 * same way. */
#include "compat.h"
#include <inttypes.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

static char bufs[3][1 << 20];   /* static, so that reading does not change the maps read */

static char *slurp(const char *path, size_t chunk, size_t *len, int which) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = (1 << 20) - 1, n = 0;
    char *b = bufs[which];
    for (;;) {
        ssize_t r = read(fd, b + n, n + chunk > cap ? cap - n : chunk);
        if (r <= 0) break;
        n += r;
    }
    close(fd);
    b[n] = 0;
    *len = n;
    return b;
}

/* The first line that Crashpad's format rejects, or NULL. */
static const char *bad_line(char *text, char *keep, size_t keep_n) {
    char *save = NULL;
    for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        uint64_t s, e, off; char perms[8]; unsigned maj, min; int64_t ino; int at = -1;
        int n = sscanf(l, "%" SCNx64 "-%" SCNx64 " %4s %" SCNx64 " %x:%x %" SCNd64 " %n", &s, &e, perms, &off, &maj, &min, &ino, &at);
        if (n != 7 || at < 0 || s >= e) {
            snprintf(keep, keep_n, "%s", l);
            return keep;
        }
    }
    return NULL;
}

int main(void) {
    setup();
    int fd = open("/proc/self/exe", O_RDONLY);
    size_t page = 4096;
    void *a = mmap(NULL, 8 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *f = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 0);
    void *g = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, page);
    void *h = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    mprotect((char *)a + 2 * page, page, PROT_READ | PROT_WRITE);
    munmap((char *)a + 5 * page, page);
    (void)f; (void)g; (void)h;
    static char bad[512];
    size_t n1, n2;
    char *one = slurp("/proc/self/maps", 1 << 16, &n1, 0);
    char *small = slurp("/proc/self/maps", 100, &n2, 1);
    EXPECT("every line of /proc/self/maps parses with Crashpad's format", ({
           char *c = strdup(one); const char *b = bad_line(c, bad, sizeof bad);
           if (b) printf("bad line: [%s]\n", b); b == NULL; }));
    EXPECT("/proc/self/maps read 100 bytes at a time is the same text", ({
           int ok = n1 == n2 && !strcmp(one, small); if (!ok) printf("lens %zu %zu\n", n1, n2); ok; }));
    EXPECT("every line of another process's maps parses too", ({
           int p[2]; pipe(p);
           pid_t c = fork();
           if (c == 0) { char x; mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); read(p[0], &x, 1); _exit(0); }
           usleep(50000);
           char path[64]; snprintf(path, sizeof path, "/proc/%d/maps", c);
           size_t n; char *t = slurp(path, 100, &n, 2);
           const char *b = t ? bad_line(t, bad, sizeof bad) : "(cannot read)";
           if (b) printf("bad line: [%s]\n", b);
           write(p[1], "x", 1); int st; waitpid(c, &st, 0); b == NULL; }));
    /* Thousands of mappings, as a browser has: the file is many reads long. */
    if (begin("a few thousand mappings read back as that many parseable lines, in order, in pieces")) {
        size_t np = 6000; /* PROT_NONE pages are not listed, so half of them */
        char *big = mmap(NULL, np * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        for (size_t i = 0; i < np; i += 2) mprotect(big + i * page, page, PROT_READ | PROT_WRITE);
        size_t n; char *t = slurp("/proc/self/maps", 4096, &n, 0);
        size_t lines = 0;
        for (size_t i = 0; i < n; i++) lines += t[i] == '\n';
        char *c = strdup(t);
        const char *b = bad_line(c, bad, sizeof bad);
        uint64_t last = 0; int ordered = 1; char *save = NULL;
        char *c2 = strdup(t);
        for (char *l = strtok_r(c2, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
            uint64_t s0 = strtoull(l, NULL, 16); if (s0 < last) ordered = 0; last = s0; }
        if (b) fail("bad line [%s] (%zu lines, %zu bytes)", b, lines, n);
        else if (!ordered || lines < np / 2) fail("%zu lines (want at least %zu), ordered %d, %zu bytes", lines, np / 2, ordered, n);
        else pass();
    }
    if (begin("another process with thousands of mappings reads back whole in 64 KiB, 4096 and 100 byte pieces")) {
        int p[2]; pipe(p);
        pid_t c = fork();
        if (c == 0) {
            char *m = mmap(NULL, 6000 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            for (size_t i = 0; i < 6000; i += 2) mprotect(m + i * page, page, PROT_READ | PROT_WRITE);
            int ef = open("/proc/self/exe", O_RDONLY);
            for (int i = 0; i < 1500; i++) mmap(NULL, page, i % 2 ? PROT_READ : PROT_READ | PROT_WRITE, MAP_PRIVATE, ef, (i % 7) * page);
            char x; write(p[1], "r", 1); read(p[0], &x, 1); _exit(0);
        }
        char r; read(p[0], &r, 1);
        char path[64]; snprintf(path, sizeof path, "/proc/%d/maps", c);
        size_t na, nb, nc;
        char *a = slurp(path, 1 << 16, &na, 0), *b = slurp(path, 4096, &nb, 1), *d = slurp(path, 100, &nc, 2);
        int same = na == nb && nb == nc && !strcmp(a, b) && !strcmp(b, d);
        char *cp = strdup(b); const char *bl = bad_line(cp, bad, sizeof bad);
        if (!same) fail("the pieces differ: %zu, %zu and %zu bytes", na, nb, nc);
        else if (bl) fail("bad line [%s] (%zu bytes)", bl, nb);
        else pass();
        write(p[1], "x", 1); int st; waitpid(c, &st, 0);
    }
    return done();
}
