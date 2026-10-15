/* Concurrent writers for the power-cut test (test/powercut.py).
 *
 *   mpc run DIR [WORKERS]   check DIR, then fork WORKERS (default 4) that
 *                           churn it until killed
 *   mpc check DIR           check DIR and exit
 *   mpc grow DIR [N]        check DIR/g0.., then add numbered files up to N
 *
 * Worker k owns a set of files and directories, each with an identity
 * (k, id). A file's byte i is pat(k, id, i), never zero, so a block that is
 * stale, shared with another file or never written shows at once, and any
 * prefix of what a worker meant to write is a valid file. The files move
 * between DIR/w<k>/ and DIR/shared/ (a directory all workers create,
 * rename and remove in), grow by appends, are cut back, overwritten in
 * place with the same bytes, and the directories d<m> move too. check
 * wants: every name one of those forms, every file's bytes the pattern,
 * and no identity under two names. The sizes are not checked: after a
 * power cut a file may be shorter than the worker wrote, never wrong.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NFILES 8
#define NDIRS 3
#define MAXW 16
#define BIG 150000
#define DIRID 100
#define FINID 150               /* final: replaced whole, by rename from tmp (FINID + 1) */
#define FINLEN 20000
#define HOGK 15                  /* the worker that fills the disk, in DIR/hog */
#define HOGID 160
#define HOGSZ (1 << 20)
#define HOGMAX 64

static unsigned char pat(int k, int id, long i)
{
    return 1 + (unsigned char)((k * 131 + id * 17 + i * 7 + (i >> 12) * 29) % 250);
}

static int bad;
static int found[MAXW][HOGID + 1];
static int nfound;

static void problem(const char *what, const char *path)
{
    printf("mpc: BAD %s: %s\n", what, path);
    bad = 1;
}

static int check_file(const char *path, int k, int id)
{
    long limit = -1;                     /* the exact size, for a final file */
    if (id == FINID) {
        struct stat st;
        limit = FINLEN;
        if (stat(path, &st) < 0 || st.st_size != FINLEN) {
            problem("final file is not whole", path);
            return 0;
        }
    }
    (void)limit;
    int slot = id;
    if (id == FINID + 1)
        id = FINID;                      /* tmp: final's bytes, any prefix */
    if (k < 0 || k >= MAXW) {
        problem("worker out of range", path);
        return 0;
    }
    if (found[k][slot]++) {
        problem("identity under two names", path);
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        problem("cannot open", path);
        return 0;
    }
    static unsigned char buf[65536];
    long off = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            problem("read failed", path);
            break;
        }
        if (n == 0)
            break;
        for (ssize_t j = 0; j < n; j++)
            if (buf[j] != pat(k, id, off + j)) {
                printf("mpc: BAD bytes at %ld (got %d, want %d): %s\n", off + j, buf[j], pat(k, id, off + j), path);
                bad = 1;
                close(fd);
                return 0;
            }
        off += n;
    }
    close(fd);
    nfound++;
    return 1;
}

/* A directory d<m> holding x, or at most that. */
static void check_dirdir(const char *path, int k, int m)
{
    DIR *d = opendir(path);
    if (!d) {
        problem("cannot open directory", path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (!strcmp(e->d_name, "x"))
            check_file(sub, k, DIRID + m);
        else
            problem("unexpected name", sub);
    }
    closedir(d);
}

static void check_hog(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/hog", dir);
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        int n;
        char sub[1024];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (sscanf(e->d_name, "h%d", &n) == 1 && n >= 0 && n < HOGMAX) {
            /* every hog file has the same bytes: any prefix of one pattern */
            found[HOGK][HOGID] = 0;
            check_file(sub, HOGK, HOGID);
        } else
            problem("unexpected name", sub);
    }
    closedir(d);
}

static void check_tree(const char *dir)
{
    char path[1024];
    check_hog(dir);
    for (int pass = 0; pass < 2; pass++) {
        /* pass 0: DIR/w<k>, pass 1: DIR/shared */
        if (pass == 1) {
            snprintf(path, sizeof path, "%s/shared", dir);
            DIR *d = opendir(path);
            if (!d) {
                problem("no shared directory", path);
                return;
            }
            struct dirent *e;
            while ((e = readdir(d))) {
                char sub[1024];
                int k, n, m;
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
                if (sscanf(e->d_name, "k%df%d", &k, &n) == 2 && n >= 0 && n < NFILES)
                    check_file(sub, k, n);
                else if (sscanf(e->d_name, "dk%dm%d", &k, &m) == 2 && m >= 0 && m < NDIRS)
                    check_dirdir(sub, k, m);
                else
                    problem("unexpected name", sub);
            }
            closedir(d);
            continue;
        }
        DIR *d = opendir(dir);
        if (!d) {
            problem("cannot open", dir);
            return;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            int k;
            if (sscanf(e->d_name, "w%d", &k) != 1)
                continue;
            char wdir[1024];
            snprintf(wdir, sizeof wdir, "%s/%s", dir, e->d_name);
            DIR *w = opendir(wdir);
            if (!w) {
                problem("cannot open", wdir);
                continue;
            }
            struct dirent *f;
            while ((f = readdir(w))) {
                int n, m;
                char sub[1024];
                if (!strcmp(f->d_name, ".") || !strcmp(f->d_name, ".."))
                    continue;
                snprintf(sub, sizeof sub, "%s/%s", wdir, f->d_name);
                if (!strcmp(f->d_name, "final"))
                    check_file(sub, k, FINID);
                else if (!strcmp(f->d_name, "tmp"))
                    check_file(sub, k, FINID + 1);
                else if (sscanf(f->d_name, "f%d", &n) == 1 && n >= 0 && n < NFILES)
                    check_file(sub, k, n);
                else if (sscanf(f->d_name, "d%d", &m) == 1 && m >= 0 && m < NDIRS)
                    check_dirdir(sub, k, m);
                else
                    problem("unexpected name", sub);
            }
            closedir(w);
        }
        closedir(d);
    }
}

static int check(const char *dir)
{
    bad = 0;
    nfound = 0;
    memset(found, 0, sizeof found);
    struct stat st;
    if (stat(dir, &st) < 0) {
        printf("mpc: clean, 0 files (no directory yet)\n");
        return 0;
    }
    check_tree(dir);
    if (bad) {
        printf("mpc: damaged\n");
        return 1;
    }
    printf("mpc: clean, %d files\n", nfound);
    return 0;
}

static unsigned long rng;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng % n);
}

static const char *top;
static int me;

static void oops(const char *op, const char *path)
{
    if (errno == ENOSPC || errno == EMFILE || errno == ENFILE || errno == ENOMEM)
        return;
    printf("mpc: worker %d: %s %s failed (errno %d)\n", me, op, path, errno);
    exit(2);
}

/* Where file n is: 0 absent, 1 in w<k>, 2 in shared. Own names only, so no
 * other worker changes the answer. */
static void fpath(char *p, size_t n, int where, int f)
{
    if (where == 1)
        snprintf(p, n, "%s/w%d/f%d", top, me, f);
    else
        snprintf(p, n, "%s/shared/k%df%d", top, me, f);
}

static int where_file(int f)
{
    char p[1024];
    struct stat st;
    for (int w = 1; w <= 2; w++) {
        fpath(p, sizeof p, w, f);
        if (stat(p, &st) == 0)
            return w;
    }
    return 0;
}

static void dpath(char *p, size_t n, int where, int m)
{
    if (where == 1)
        snprintf(p, n, "%s/w%d/d%d", top, me, m);
    else
        snprintf(p, n, "%s/shared/dk%dm%d", top, me, m);
}

static int where_dir(int m)
{
    char p[1024];
    struct stat st;
    for (int w = 1; w <= 2; w++) {
        dpath(p, sizeof p, w, m);
        if (stat(p, &st) == 0)
            return w;
    }
    return 0;
}

static void write_pat(int fd, int id, long at, long len)
{
    static unsigned char buf[9000];
    while (len > 0) {
        long n = len > (long)sizeof buf ? (long)sizeof buf : len;
        for (long j = 0; j < n; j++)
            buf[j] = pat(me, id, at + j);
        ssize_t w = pwrite(fd, buf, n, at);
        if (w <= 0)
            return;
        at += w;
        len -= w;
    }
}

static void do_file(int f)
{
    char p[1024], q[1024];
    int w = where_file(f);
    int op = rnd(10);
    if (w == 0) {
        fpath(p, sizeof p, 1 + rnd(2), f);
        int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            oops("create", p);
            return;
        }
        write_pat(fd, f, 0, 1 + rnd(9000));
        close(fd);
        return;
    }
    fpath(p, sizeof p, w, f);
    if (op < 4) {                        /* append */
        int fd = open(p, O_WRONLY);
        if (fd < 0) {
            oops("open", p);
            return;
        }
        long size = lseek(fd, 0, SEEK_END);
        if (size > BIG)
            ftruncate(fd, size = rnd(BIG / 2));
        write_pat(fd, f, size, 1 + rnd(9000));
        if (rnd(8) == 0)
            fsync(fd);
        close(fd);
    } else if (op < 5) {                 /* rewrite in place with the same bytes */
        int fd = open(p, O_WRONLY);
        if (fd < 0) {
            oops("open", p);
            return;
        }
        long size = lseek(fd, 0, SEEK_END);
        if (size > 0) {
            long at = rnd(size);
            write_pat(fd, f, at, size - at > 6000 ? 6000 : size - at);
        }
        close(fd);
    } else if (op < 8) {                 /* move */
        fpath(q, sizeof q, 3 - w, f);
        if (rename(p, q) < 0)
            oops("rename", p);
    } else if (op < 9) {                 /* cut back */
        int fd = open(p, O_WRONLY);
        if (fd >= 0) {
            long size = lseek(fd, 0, SEEK_END);
            if (size > 0)
                ftruncate(fd, rnd(size));
            close(fd);
        }
    } else if (unlink(p) < 0) {
        oops("unlink", p);
    }
}

static void do_dir(int m)
{
    char p[1024], q[1024], x[1100];
    int w = where_dir(m);
    if (w == 0) {
        dpath(p, sizeof p, 1 + rnd(2), m);
        if (mkdir(p, 0755) < 0) {
            oops("mkdir", p);
            return;
        }
        snprintf(x, sizeof x, "%s/x", p);
        int fd = open(x, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            write_pat(fd, DIRID + m, 0, 1 + rnd(6000));
            close(fd);
        }
        return;
    }
    dpath(p, sizeof p, w, m);
    snprintf(x, sizeof x, "%s/x", p);
    int op = rnd(4);
    if (op < 2) {
        dpath(q, sizeof q, 3 - w, m);
        if (rename(p, q) < 0)
            oops("rename dir", p);
    } else if (op == 2) {
        int fd = open(x, O_WRONLY);
        if (fd >= 0) {
            long size = lseek(fd, 0, SEEK_END);
            write_pat(fd, DIRID + m, size, 1 + rnd(5000));
            close(fd);
        }
    } else {
        unlink(x);
        if (rmdir(p) < 0)
            oops("rmdir", p);
    }
}

/* Write tmp whole, fsync, rename it over final: after any power cut final
 * is a whole file (an older or a newer one), never a prefix. */
static void replace_final(void)
{
    char t[1024], f[1024];
    snprintf(t, sizeof t, "%s/w%d/tmp", top, me);
    snprintf(f, sizeof f, "%s/w%d/final", top, me);
    unlink(t);
    int fd = open(t, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        oops("create", t);
        return;
    }
    write_pat(fd, FINID, 0, FINLEN);
    fsync(fd);
    close(fd);
    if (rename(t, f) < 0)
        oops("rename", t);
}

/* Fill the disk with 1 MiB files until a write fails, then remove them in
 * a different order, and again: the power is cut with the disk full, with
 * the free map nearly empty and with blocks freed and reused at once. */
static void hog(void)
{
    char p[1024];
    me = HOGK;
    rng = 7777777ul + getpid();
    snprintf(p, sizeof p, "%s/hog", top);
    mkdir(p, 0755);
    for (;;) {
        int made = 0;
        for (int n = 0; n < HOGMAX; n++) {
            snprintf(p, sizeof p, "%s/hog/h%d", top, n);
            int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0)
                break;
            static unsigned char buf[65536];
            int full = 0;
            for (long at = 0; at < HOGSZ && !full; at += sizeof buf) {
                for (long j = 0; j < (long)sizeof buf; j++)
                    buf[j] = pat(HOGK, HOGID, at + j);
                if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf)
                    full = 1;
            }
            close(fd);
            made = n + 1;
            if (full)
                break;
        }
        int start = made ? rnd(made) : 0;
        for (int i = 0; i < made; i++) {
            snprintf(p, sizeof p, "%s/hog/h%d", top, (start + i) % made);
            unlink(p);
        }
    }
}

static void worker(int k)
{
    me = k;
    rng = 88172538ul + k * 2654435761ul + getpid();
    char p[1024];
    snprintf(p, sizeof p, "%s/w%d", top, k);
    mkdir(p, 0755);
    for (;;) {
        if (rnd(12) == 0)
            replace_final();
        else if (rnd(4) == 0)
            do_dir(rnd(NDIRS));
        else
            do_file(rnd(NFILES));
    }
}

/* grow DIR [N]: files g0, g1, ... each holding its number as 8 digits,
 * created one after the other (up to N in all), so the directory outgrows
 * the blocks the superblock can name while the power is cut. After a cut
 * the files are a run g0..g(K-1), each whole but the last, which may be
 * empty (cut between its create and its write). */
static int grow(const char *dir, long n)
{
    char p[1024], want[16], got[16];
    mkdir(dir, 0755);
    long k = 0;
    for (;; k++) {
        snprintf(p, sizeof p, "%s/g%ld", dir, k);
        int fd = open(p, O_RDONLY);
        if (fd < 0)
            break;
        ssize_t r = read(fd, got, sizeof got);
        close(fd);
        snprintf(want, sizeof want, "%08ld", k);
        if (r == 0) {
            /* the last one may be empty: it is rewritten below */
            break;
        }
        if (r != 8 || memcmp(got, want, 8)) {
            printf("mpc: BAD g%ld holds the wrong bytes\n", k);
            printf("mpc: damaged\n");
            return 1;
        }
    }
    /* anything past the run is a gap */
    long extra = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] == 'g')
            extra++;
    if (d)
        closedir(d);
    long have = k;
    if (extra != have && extra != have + 1) {
        printf("mpc: BAD %ld names but a run of %ld\n", extra, have);
        printf("mpc: damaged\n");
        return 1;
    }
    printf("mpc: clean, %ld files\n", have);
    if (n == 0)
        return 0;
    printf("mpc: running\n");
    fflush(stdout);
    for (; k < n; k++) {
        snprintf(p, sizeof p, "%s/g%ld", dir, k);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return 1;
        snprintf(want, sizeof want, "%08ld", k);
        write(fd, want, 8);
        close(fd);
    }
    printf("mpc: done %ld\n", k);
    return 0;
}

/* big DIR [run]: one file with pieces at offsets from 0 to 4 GiB (past the
 * 1 GiB a root map reaches: BSTDISK5's super map). Piece i is the 24 bytes
 * "big i  NNNNNNNNNNNNNN" at i GiB + 4096 * i + 17; the writer puts them
 * down in turn, cuts the file back to nothing now and then, and grows it
 * again. After a cut every piece is whole or absent (a hole reads zeros),
 * and nothing else is in the file. */
#define NPIECE 5
static int big(const char *dir, int run)
{
    char p[1024], want[32], got[32];
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/b", dir);
    int fd = open(p, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return 1;
    struct stat st;
    fstat(fd, &st);
    static char buf[65536];
    for (int i = 0; i < NPIECE; i++) {
        long long at = ((long long)i << 30) + 4096 * i + 17;
        ssize_t r = pread(fd, got, 24, at);
        int zero = 1, whole = 0;
        if (r == 24) {
            for (int j = 0; j < 24; j++)
                zero &= got[j] == 0;
            whole = got[0] == 'b' && got[1] == 'i' && got[2] == 'g' && got[4] == (char)(48 + i);
        }
        if (r > 0 && r < 24)
            zero = 1;                    /* past the end: absent */
        if (r > 0 && !zero && !whole) {
            printf("mpc: BAD piece %d is neither whole nor absent\n", i);
            printf("mpc: damaged\n");
            return 1;
        }
        (void)buf;
    }
    /* Nothing but pieces: scan the file's data with SEEK_DATA is not
     * available everywhere, so read the blocks around each piece. */
    for (int i = 0; i < NPIECE; i++) {
        long long base = ((long long)i << 30) + 4096 * i;
        if (pread(fd, buf, 4096, base) > 0)
            for (int j = 0; j < 4096; j++)
                if (buf[j] && (j < 17 || j >= 41)) {
                    printf("mpc: BAD stray byte at %lld\n", base + j);
                    printf("mpc: damaged\n");
                    return 1;
                }
    }
    printf("mpc: clean, %lld bytes\n", (long long)st.st_size);
    if (!run)
        return 0;
    printf("mpc: running\n");
    fflush(stdout);
    unsigned long gen = 0;
    for (;; gen++) {
        for (int i = 0; i < NPIECE; i++) {
            long long at = ((long long)i << 30) + 4096 * i + 17;
            snprintf(want, sizeof want, "big %d  %014lu", i, gen);
            if (pwrite(fd, want, 24, at) != 24)
                break;
        }
        if (gen % 3 == 2)
            ftruncate(fd, 0);
        else if (gen % 3 == 1)
            fsync(fd);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: mpc run|check DIR [WORKERS]\n");
        return 2;
    }
    if (!strcmp(argv[1], "big"))
        return big(argv[2], argc > 3);
    if (!strcmp(argv[1], "grow"))
        return grow(argv[2], argc > 3 ? atol(argv[3]) : 100000);
    if (!strcmp(argv[1], "check"))
        return check(argv[2]);
    if (strcmp(argv[1], "run"))
        return 2;
    top = argv[2];
    int workers = argc > 3 ? atoi(argv[3]) : 4;
    if (workers < 1 || workers > MAXW)
        workers = 4;
    if (check(top))
        return 1;
    mkdir(top, 0755);
    char p[1024];
    snprintf(p, sizeof p, "%s/shared", top);
    mkdir(p, 0755);
    for (int k = 0; k < workers; k++) {
        pid_t pid = fork();
        if (pid == 0) {
            worker(k);
            return 0;
        }
        if (pid < 0) {
            printf("mpc: fork failed\n");
            return 2;
        }
    }
    if (workers >= 2 && getenv("MPC_HOG")) {
        pid_t pid = fork();
        if (pid == 0) {
            hog();
            return 0;
        }
    }
    printf("mpc: running %d workers\n", workers);
    fflush(stdout);
    int status, failed = 0;
    while (wait(&status) > 0)
        if (WIFEXITED(status) && WEXITSTATUS(status))
            failed = 1;
    return failed;
}
