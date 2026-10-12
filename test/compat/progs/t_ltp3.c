/* More system call corners LTP's tests found: O_PATH descriptors given to
 * epoll_ctl and the socket calls, readlinkat of an O_PATH handle on a
 * link, fchdir into a directory without search permission,
 * fallocate(FALLOC_FL_KEEP_SIZE), file handles (name_to_handle_at and
 * open_by_handle_at), unlinkat's flag check, and fchmod and fchown on a
 * closed descriptor, on a file of a read-only mount, and chmod of a file
 * that keeps no mode; link and rename across a mount (EXDEV). */
#include "compat.h"
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <utime.h>

struct fh { unsigned int bytes; int type; unsigned char h[128]; };

/* Runs f in a child that has dropped to user nobody; its exit status. */
static int as_nobody(int (*f)(void)) {
    pid_t c = fork();
    if (c == 0) {
        if (setgid(65534) || setuid(65534)) _exit(2);
        _exit(f());
    }
    int st = 0;
    waitpid(c, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}

static int nosearch_fd = -1;
static int fchdir_nosearch(void) { return fchdir(nosearch_fd) == -1 && errno == EACCES ? 0 : 1; }
static int chmod_devnull(void) { return chmod("/dev/null", 0666) == -1 && errno == EPERM ? 0 : 1; }
static int ro_fd = -1;
static int fchmod_ro(void) { return fchmod(ro_fd, 0600) == -1 && errno == EROFS ? 0 : 1; }

int main(void) {
    setup();
    mkdir("l3_d", 0755);
    close(open("l3_d/f", O_CREAT | O_WRONLY, 0644));
    unlink("l3_link");
    if (symlink("l3_d/f", "l3_link")) perror("symlink");

    int p = open("l3_d/f", O_PATH);
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN };
    EXPECT("epoll_ctl of an O_PATH descriptor is EBADF",
           p >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, p, &ev) == -1 && errno == EBADF);
    EXPECT("accept on an O_PATH descriptor is EBADF",
           accept(p, NULL, NULL) == -1 && errno == EBADF);
    EXPECT("getsockopt on an O_PATH descriptor is EBADF",
           getsockopt(p, SOL_SOCKET, SO_TYPE, &(int){0}, &(socklen_t){sizeof(int)}) == -1 && errno == EBADF);
    close(ep);
    close(p);

    int l = open("l3_link", O_PATH | O_NOFOLLOW);
    char buf[64] = {0};
    struct stat st;
    EXPECT("O_PATH|O_NOFOLLOW opens the link itself",
           l >= 0 && fstatat(l, "", &st, AT_EMPTY_PATH) == 0 && S_ISLNK(st.st_mode));
    EXPECT("readlinkat of that descriptor and \"\" reads the link",
           readlinkat(l, "", buf, sizeof buf - 1) == 6 && strcmp(buf, "l3_d/f") == 0);
    close(l);

    mkdir("l3_nx", 0700);
    nosearch_fd = open("l3_nx", O_RDONLY | O_DIRECTORY);
    EXPECT("fchdir into a directory without search permission is EACCES",
           nosearch_fd >= 0 && as_nobody(fchdir_nosearch) == 0);
    close(nosearch_fd);

    int f = open("l3_d/f", O_RDWR);
    EXPECT("fallocate with KEEP_SIZE succeeds and keeps the size",
           f >= 0 && fallocate(f, FALLOC_FL_KEEP_SIZE, 0, 4096) == 0 && fstat(f, &st) == 0 && st.st_size == 0);
    EXPECT("fallocate without it grows the file",
           fallocate(f, 0, 0, 100) == 0 && fstat(f, &st) == 0 && st.st_size == 100);
    close(f);

    struct fh h = { .bytes = 0 };
    int mnt = -1;
    EXPECT("name_to_handle_at with no room is EOVERFLOW and says how much it needs",
           syscall(SYS_name_to_handle_at, AT_FDCWD, "l3_d/f", &h, &mnt, 0) == -1 && errno == EOVERFLOW &&
           h.bytes > 0 && h.bytes <= 128);
    unsigned int need = h.bytes;
    EXPECT("with that room it gives the handle and a mount id",
           syscall(SYS_name_to_handle_at, AT_FDCWD, "l3_d/f", &h, &mnt, 0) == 0 && h.bytes == need && mnt >= 0);
    struct fh hd = { .bytes = 128 };
    EXPECT("a directory has a handle too",
           syscall(SYS_name_to_handle_at, AT_FDCWD, "l3_d", &hd, &mnt, 0) == 0);
    EXPECT("bad flags are EINVAL",
           syscall(SYS_name_to_handle_at, AT_FDCWD, "l3_d/f", &h, &mnt, 0x8000) == -1 && errno == EINVAL);
    int mfd = open("l3_d", O_RDONLY | O_DIRECTORY);
    int g = syscall(SYS_open_by_handle_at, mfd, &h, O_RDONLY);
    EXPECT("open_by_handle_at opens the same file",
           g >= 0 && fstat(g, &st) == 0 && st.st_size == 100);
    close(g);
    g = syscall(SYS_open_by_handle_at, mfd, &hd, O_RDONLY | O_DIRECTORY);
    EXPECT("and the same directory", g >= 0 && fstatat(g, "f", &st, 0) == 0);
    close(g);
    rename("l3_d/f", "l3_d/f2");
    g = syscall(SYS_open_by_handle_at, mfd, &h, O_RDONLY);
    EXPECT("a handle still opens its file after a rename", g >= 0 && fstat(g, &st) == 0 && st.st_size == 100);
    close(g);
    struct fh bad = { .bytes = 0 };
    EXPECT("a handle of no bytes is EINVAL",
           syscall(SYS_open_by_handle_at, mfd, &bad, O_RDONLY) == -1 && errno == EINVAL);
    close(mfd);

    EXPECT("unlinkat with an unknown flag is EINVAL, before the path",
           unlinkat(AT_FDCWD, "l3_no_such", 9999) == -1 && errno == EINVAL);
    EXPECT("unlinkat with AT_REMOVEDIR of a file is ENOTDIR",
           unlinkat(AT_FDCWD, "l3_d/f2", AT_REMOVEDIR) == -1 && errno == ENOTDIR);

    int c = open("l3_d/f2", O_RDONLY);
    close(c);
    EXPECT("fchmod of a closed descriptor is EBADF", fchmod(c, 0600) == -1 && errno == EBADF);
    EXPECT("fchown of a closed descriptor is EBADF", fchown(c, 0, 0) == -1 && errno == EBADF);
    EXPECT("chmod of /dev/null by another user is EPERM", as_nobody(chmod_devnull) == 0);

    mkdir("l3_ro", 0755);
    int m = mount("none", "l3_ro", "tmpfs", 0, NULL);
    close(open("l3_ro/f", O_CREAT | O_WRONLY, 0644));
    EXPECT("link out of a mount is EXDEV", m == 0 && link("l3_ro/f", "l3_x") == -1 && errno == EXDEV);
    EXPECT("rename out of a mount is EXDEV", rename("l3_ro/f", "l3_x") == -1 && errno == EXDEV);
    EXPECT("link within the mount works", link("l3_ro/f", "l3_ro/g") == 0 && unlink("l3_ro/g") == 0);
    m = m || mount("none", "l3_ro", "tmpfs", MS_REMOUNT | MS_RDONLY, NULL);
    ro_fd = open("l3_ro/f", O_RDONLY);
    EXPECT("fchmod of a file on a read-only mount is EROFS",
           m == 0 && ro_fd >= 0 && fchmod(ro_fd, 0600) == -1 && errno == EROFS);
    EXPECT("fchown of it is EROFS", fchown(ro_fd, 1, 1) == -1 && errno == EROFS);
    EXPECT("and EROFS comes before EPERM for another user", as_nobody(fchmod_ro) == 0);
    close(ro_fd);
    umount("l3_ro");
    rmdir("l3_ro");

    int mf = open("l3_mm", O_CREAT | O_RDWR | O_TRUNC, 0644);
    char pg[4096];
    memset(pg, 'a', sizeof pg);
    int mok = mf >= 0 && write(mf, pg, sizeof pg) == sizeof pg && ftruncate(mf, 3 * 4096) == 0;
    char *m1 = mok ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0) : MAP_FAILED;
    char *m2 = m1 == MAP_FAILED ? MAP_FAILED : mremap(m1, 4096, 3 * 4096, MREMAP_MAYMOVE);
    if (m2 != MAP_FAILED) { m2[0] = 'x'; m2[2 * 4096 + 5] = 'z'; msync(m2, 3 * 4096, MS_SYNC); }
    char rb[3 * 4096] = {0};
    EXPECT("mremap grows a shared file mapping and its tail writes reach the file",
           m2 != MAP_FAILED && pread(mf, rb, sizeof rb, 0) == sizeof rb
           && rb[0] == 'x' && rb[1] == 'a' && rb[2 * 4096 + 5] == 'z');
    if (m2 != MAP_FAILED) munmap(m2, 3 * 4096);
    if (mf >= 0) close(mf);
    unlink("l3_mm");

    {
        int pf[2], n = 0, e = 0, opened[1024];
        struct rlimit old, lo;
        getrlimit(RLIMIT_NOFILE, &old);
        lo = old;
        lo.rlim_cur = 64;
        setrlimit(RLIMIT_NOFILE, &lo);
        long lim = sysconf(_SC_OPEN_MAX);
        while (n < 1020) {
            if (pipe(pf) == -1) { e = errno; break; }
            opened[n++] = pf[0];
            opened[n++] = pf[1];
        }
        printf("# pipes: %d fds opened, limit %ld, errno %d\n", n, lim, e);
        EXPECT("pipe on a full descriptor table is EMFILE", e == EMFILE);
        for (int i = 0; i < n; i++) close(opened[i]);
        setrlimit(RLIMIT_NOFILE, &old);
    }

    {
        unsigned char vec[3] = {9, 9, 9};
        char *a = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int r0 = a == MAP_FAILED ? -1 : mincore(a, 3 * 4096, vec);
        EXPECT("mincore says untouched anonymous pages are not resident",
               r0 == 0 && !(vec[0] & 1) && !(vec[1] & 1) && !(vec[2] & 1));
        if (a != MAP_FAILED) a[4096] = 1;
        int r1 = a == MAP_FAILED ? -1 : mincore(a, 3 * 4096, vec);
        EXPECT("and a touched one is", r1 == 0 && !(vec[0] & 1) && (vec[1] & 1) && !(vec[2] & 1));
        if (a != MAP_FAILED) munmap(a, 3 * 4096);
        a = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int rl = a == MAP_FAILED ? -1 : mlock(a, 3 * 4096);
        int r2 = rl == 0 ? mincore(a, 3 * 4096, vec) : -1;
        EXPECT("mlock makes the pages resident", r2 == 0 && (vec[0] & 1) && (vec[1] & 1) && (vec[2] & 1));
        if (rl == 0) munlock(a, 3 * 4096);
        if (a != MAP_FAILED) munmap(a, 3 * 4096);
    }

    {
        mkdir("l3_ren", 0755);
        mkdir("l3_ren/sub", 0755);
        EXPECT("rename of a directory into itself is EINVAL",
               rename("l3_ren", "l3_ren/sub/in") == -1 && errno == EINVAL);
        EXPECT("umount of a name that does not exist is ENOENT",
               umount("l3_nonexistent") == -1 && errno == ENOENT);
        int cs = open("l3_cfr", O_CREAT | O_RDWR | O_TRUNC, 0644);
        int cd = open("l3_ren", O_RDONLY | O_DIRECTORY);
        int ca = open("l3_cfr2", O_CREAT | O_WRONLY | O_APPEND | O_TRUNC, 0644);
        off_t off = 0;
        if (cs >= 0 && write(cs, "0123456789abcdef", 16) == 16) lseek(cs, 0, SEEK_SET);
        EXPECT("copy_file_range into a directory is EISDIR",
               cs >= 0 && copy_file_range(cs, NULL, cd, &off, 16, 0) == -1 && errno == EISDIR);
        EXPECT("copy_file_range into an O_APPEND file is EBADF",
               ca >= 0 && copy_file_range(cs, NULL, ca, &off, 16, 0) == -1 && errno == EBADF);
        off = 0x7fffffffffffffffLL;
        EXPECT("copy_file_range past the largest file is EFBIG",
               copy_file_range(cs, NULL, cs, &off, 8, 0) == -1 && errno == EFBIG);
        off = 0x7fffffffffffffffLL;
        EXPECT("and a length that wraps is EOVERFLOW",
               copy_file_range(cs, NULL, cs, &off, ~0ULL, 0) == -1 && errno == EOVERFLOW);
        off = 4;
        EXPECT("copy_file_range of overlapping ranges in one file is EINVAL",
               copy_file_range(cs, NULL, cs, &off, 8, 0) == -1 && errno == EINVAL);
        if (cs >= 0) close(cs);
        if (cd >= 0) close(cd);
        if (ca >= 0) close(ca);
        unlink("l3_cfr");
        unlink("l3_cfr2");
        rmdir("l3_ren/sub");
        rmdir("l3_ren");
    }

    {
        int pp[20][2], got[20] = {0}, added = 0, ok = 1;
        int efd = epoll_create1(0);
        for (int i = 0; i < 20; i++) {
            pp[i][0] = pp[i][1] = -1;
            if (pipe(pp[i]) == 0 && write(pp[i][1], "x", 1) == 1) {
                struct epoll_event e = { .events = EPOLLIN, .data.u32 = i };
                if (epoll_ctl(efd, EPOLL_CTL_ADD, pp[i][0], &e) == 0) added++;
            }
        }
        for (int r = 0; r < 20 && ok; r++) {
            struct epoll_event out[3];
            int n = epoll_wait(efd, out, 3, 0);
            if (n != 3) ok = 0;
            for (int i = 0; i < n; i++) got[out[i].data.u32]++;
        }
        for (int i = 0; i < 20; i++) if (got[i] != 3) ok = 0;
        EXPECT("an epoll instance watches 20 descriptors", added == 20);
        EXPECT("epoll_wait with room for 3 reports each of 20 ready descriptors in turn", ok);
        for (int i = 0; i < 20; i++) { if (pp[i][0] >= 0) close(pp[i][0]); if (pp[i][1] >= 0) close(pp[i][1]); }
        close(efd);
    }

    /* utime sets the access time apart from the modification time;
       utimensat can leave either alone. */
    {
        int fd = open("l3_ut", O_CREAT | O_WRONLY, 0644);
        struct stat st;
        struct utimbuf ub = { .actime = 12345, .modtime = 67890 };
        int r1 = utime("l3_ut", &ub);
        int r2 = stat("l3_ut", &st);
        EXPECT("utime sets the access time", r1 == 0 && r2 == 0 && st.st_atime == 12345);
        EXPECT("utime sets the modification time", st.st_mtime == 67890);
        struct timespec ts[2] = { { 0, UTIME_OMIT }, { 4242, 0 } };
        int r3 = utimensat(AT_FDCWD, "l3_ut", ts, 0);
        stat("l3_ut", &st);
        EXPECT("utimensat with UTIME_OMIT keeps the access time", r3 == 0 && st.st_atime == 12345 && st.st_mtime == 4242);
        struct timespec ts2[2] = { { 777, 0 }, { 0, UTIME_OMIT } };
        int r4 = utimensat(AT_FDCWD, "l3_ut", ts2, 0);
        stat("l3_ut", &st);
        EXPECT("utimensat with UTIME_OMIT keeps the modification time", r4 == 0 && st.st_atime == 777 && st.st_mtime == 4242);
        if (fd >= 0) close(fd);
        unlink("l3_ut");
    }

    unlink("l3_d/f2");
    unlink("l3_link");
    rmdir("l3_d");
    rmdir("l3_nx");
    return done();
}
