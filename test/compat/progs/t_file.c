/* Files: open, read, write, seek, positional and vectored I/O, sizes,
 * renaming, links, metadata. Works in /tmp if it exists, else in /. */
#include "compat.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <utime.h>

static int fd = -1;

int main(void) {
    setup();
    char buf[64];
    struct stat st;

    EXPECT("open O_CREAT|O_TRUNC", (fd = open("cf_a", O_RDWR | O_CREAT | O_TRUNC, 0644)) >= 0);
    EXPECT("write", write(fd, "hello world", 11) == 11);
    EXPECT("lseek SEEK_SET", lseek(fd, 0, SEEK_SET) == 0);
    EXPECT("read back", read(fd, buf, sizeof buf) == 11 && memcmp(buf, "hello world", 11) == 0);
    EXPECT("lseek SEEK_END", lseek(fd, 0, SEEK_END) == 11);
    EXPECT("lseek SEEK_CUR", lseek(fd, -5, SEEK_CUR) == 6);
    EXPECT("pread", pread(fd, buf, 5, 6) == 5 && memcmp(buf, "world", 5) == 0);
    EXPECT("pwrite", pwrite(fd, "W", 1, 6) == 1 && pread(fd, buf, 1, 6) == 1 && buf[0] == 'W');
    EXPECT("pread keeps offset", lseek(fd, 0, SEEK_CUR) == 6);
    {
        struct iovec v[2] = {{"ab", 2}, {"cd", 2}};
        EXPECT("writev", lseek(fd, 0, SEEK_SET) == 0 && writev(fd, v, 2) == 4);
    }
    {
        char x[2], y[3];
        struct iovec v[2] = {{x, 2}, {y, 3}};
        EXPECT("readv", lseek(fd, 0, SEEK_SET) == 0 && readv(fd, v, 2) == 5 &&
               memcmp(x, "ab", 2) == 0 && memcmp(y, "cdo", 3) == 0);
    }
    EXPECT("fstat size", fstat(fd, &st) == 0 && st.st_size == 11 && S_ISREG(st.st_mode));
    EXPECT("ftruncate shrink", ftruncate(fd, 4) == 0 && fstat(fd, &st) == 0 && st.st_size == 4);
    EXPECT("ftruncate grow", ftruncate(fd, 100) == 0 && fstat(fd, &st) == 0 && st.st_size == 100);
    EXPECT("fsync", fsync(fd) == 0);
    EXPECT("fdatasync", fdatasync(fd) == 0);
    EXPECT("close", close(fd) == 0);
    EXPECT("close twice is EBADF", close(fd) == -1 && errno == EBADF);
    EXPECT("stat", stat("cf_a", &st) == 0 && st.st_size == 100);
    EXPECT("truncate", truncate("cf_a", 3) == 0 && stat("cf_a", &st) == 0 && st.st_size == 3);
    {
        /* Close what a wrong success opened, so later checks do not see an
         * open cf_a (BastiOS refuses to unlink open files). */
        int x = -1;
        EXPECT("O_EXCL on existing", (x = open("cf_a", O_RDWR | O_CREAT | O_EXCL, 0644)) == -1 && errno == EEXIST);
        if (x >= 0) close(x);
    }
    EXPECT("open missing is ENOENT", open("cf_none", O_RDONLY) == -1 && errno == ENOENT);
    EXPECT("O_APPEND", (fd = open("cf_a", O_WRONLY | O_APPEND)) >= 0 && write(fd, "XY", 2) == 2 &&
           close(fd) == 0 && stat("cf_a", &st) == 0 && st.st_size == 5);
    EXPECT("O_RDONLY refuses write", (fd = open("cf_a", O_RDONLY)) >= 0 && write(fd, "z", 1) == -1 &&
           errno == EBADF && close(fd) == 0);
    EXPECT("rename", rename("cf_a", "cf_b") == 0 && stat("cf_b", &st) == 0 && stat("cf_a", &st) == -1);
    EXPECT("rename over existing", (fd = open("cf_c", O_CREAT | O_WRONLY, 0644)) >= 0 && close(fd) == 0 &&
           rename("cf_b", "cf_c") == 0 && stat("cf_c", &st) == 0 && st.st_size == 5);
    EXPECT("link", link("cf_c", "cf_d") == 0 && stat("cf_d", &st) == 0 && st.st_nlink == 2);
    EXPECT("symlink", symlink("cf_c", "cf_e") == 0);
    {
        ssize_t n = readlink("cf_e", buf, sizeof buf);
        EXPECT("readlink", n == 4 && memcmp(buf, "cf_c", 4) == 0);
    }
    EXPECT("lstat sees the link", lstat("cf_e", &st) == 0 && S_ISLNK(st.st_mode));
    EXPECT("chmod", chmod("cf_c", 0600) == 0 && stat("cf_c", &st) == 0 && (st.st_mode & 0777) == 0600);
    EXPECT("fchmod", (fd = open("cf_c", O_RDONLY)) >= 0 && fchmod(fd, 0640) == 0 && close(fd) == 0);
    EXPECT("chown to self", chown("cf_c", getuid(), getgid()) == 0);
    {
        struct utimbuf t = {1000000000, 1000000000};
        EXPECT("utime", utime("cf_c", &t) == 0 && stat("cf_c", &st) == 0 && st.st_mtime == 1000000000);
    }
    EXPECT("access F_OK", access("cf_c", F_OK) == 0);
    EXPECT("access missing", access("cf_none", F_OK) == -1 && errno == ENOENT);
    EXPECT("faccessat", faccessat(AT_FDCWD, "cf_c", R_OK, 0) == 0);
    EXPECT("unlink", unlink("cf_c") == 0 && stat("cf_c", &st) == -1 && errno == ENOENT);
    EXPECT("unlink an open file, then read it", (fd = open("cf_o", O_RDWR | O_CREAT, 0644)) >= 0 && write(fd, "kept", 4) == 4 &&
           unlink("cf_o") == 0 && lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, 4) == 4 && memcmp(buf, "kept", 4) == 0);
    if (fd >= 0) close(fd);
    unlink("cf_o");
    unlink("cf_d");
    unlink("cf_e");
    {
        int fds[12], n = 0;
        if (begin("12 open descriptors")) {
            int g = open("cf_n", O_RDWR | O_CREAT, 0644);
            for (; g >= 0 && n < 12; n++)
                if ((fds[n] = open("cf_n", O_RDONLY)) < 0) break;
            int e = errno;
            if (n == 12) pass(); else fail("open number %d of cf_n failed (errno %d %s)", n + 1, e, strerror(e));
            while (n > 0) close(fds[--n]);
            close(g);
            unlink("cf_n");
        }
    }
    EXPECT("/dev/null write", (fd = open("/dev/null", O_WRONLY)) >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
    EXPECT("/dev/zero read", (fd = open("/dev/zero", O_RDONLY)) >= 0 && read(fd, buf, 8) == 8 &&
           buf[0] == 0 && buf[7] == 0 && close(fd) == 0);
    EXPECT("/dev/urandom read", (fd = open("/dev/urandom", O_RDONLY)) >= 0 && read(fd, buf, 8) == 8 && close(fd) == 0);
    {
        FILE *f = fopen("cf_f", "w+");
        int ok = f && fprintf(f, "%d %s\n", 42, "x") == 5 && fseek(f, 0, SEEK_SET) == 0;
        int v = 0; char s[8] = "";
        ok = ok && fscanf(f, "%d %7s", &v, s) == 2 && v == 42 && strcmp(s, "x") == 0;
        EXPECT("stdio fprintf/fscanf", ok);
        if (f) fclose(f);
        unlink("cf_f");
    }
    {
        FILE *f = tmpfile();
        EXPECT("tmpfile", f && fputs("t", f) >= 0 && fclose(f) == 0);
    }
    {
        /* Extended attributes are kept in memory, by file: they follow a
         * rename and a hard link, and go with the file's last name. */
        char v[300], l[300];
        int x = open("cx_f", O_RDWR | O_CREAT, 0644);
        EXPECT("listxattr of a fresh file is empty", listxattr("cx_f", l, sizeof l) == 0);
        EXPECT("getxattr of what is not there is ENODATA", getxattr("cx_f", "user.a", v, sizeof v) == -1 && errno == ENODATA);
        EXPECT("setxattr stores a value", setxattr("cx_f", "user.a", "one", 3, 0) == 0);
        EXPECT("getxattr reads it back", getxattr("cx_f", "user.a", v, sizeof v) == 3 && memcmp(v, "one", 3) == 0);
        EXPECT("fgetxattr reads it by descriptor", fgetxattr(x, "user.a", v, sizeof v) == 3 && memcmp(v, "one", 3) == 0);
        EXPECT("getxattr with size 0 gives the length", getxattr("cx_f", "user.a", NULL, 0) == 3);
        EXPECT("getxattr into too small a buffer is ERANGE", getxattr("cx_f", "user.a", v, 2) == -1 && errno == ERANGE);
        EXPECT("XATTR_CREATE of an existing name is EEXIST", setxattr("cx_f", "user.a", "x", 1, XATTR_CREATE) == -1 && errno == EEXIST);
        EXPECT("XATTR_REPLACE of a missing name is ENODATA", setxattr("cx_f", "user.b", "x", 1, XATTR_REPLACE) == -1 && errno == ENODATA);
        EXPECT("XATTR_REPLACE changes the value", setxattr("cx_f", "user.a", "three", 5, XATTR_REPLACE) == 0 && getxattr("cx_f", "user.a", v, sizeof v) == 5 && memcmp(v, "three", 5) == 0);
        EXPECT("fsetxattr adds a second name", fsetxattr(x, "user.b", "", 0, XATTR_CREATE) == 0 && getxattr("cx_f", "user.b", v, sizeof v) == 0);
        EXPECT("listxattr lists both, NUL separated", listxattr("cx_f", l, sizeof l) == 14 && memcmp(l, "user.a\0user.b\0", 14) == 0);
        EXPECT("listxattr with size 0 gives the length", flistxattr(x, NULL, 0) == 14);
        EXPECT("listxattr into too small a buffer is ERANGE", listxattr("cx_f", l, 5) == -1 && errno == ERANGE);
        EXPECT("a name outside every namespace is EOPNOTSUPP", setxattr("cx_f", "bogus.a", "1", 1, 0) == -1 && errno == EOPNOTSUPP);
        EXPECT("they follow a rename", rename("cx_f", "cx_g") == 0 && getxattr("cx_g", "user.a", v, sizeof v) == 5 && getxattr("cx_f", "user.a", v, sizeof v) == -1 && errno == ENOENT);
        EXPECT("they are shared by a hard link", link("cx_g", "cx_h") == 0 && getxattr("cx_h", "user.a", v, sizeof v) == 5);
        EXPECT("they stay while a name is left", unlink("cx_g") == 0 && getxattr("cx_h", "user.a", v, sizeof v) == 5);
        EXPECT("removexattr removes one", removexattr("cx_h", "user.a") == 0 && getxattr("cx_h", "user.a", v, sizeof v) == -1 && errno == ENODATA);
        EXPECT("removexattr of a missing name is ENODATA", removexattr("cx_h", "user.a") == -1 && errno == ENODATA);
        EXPECT("fremovexattr removes by descriptor", fremovexattr(x, "user.b") == 0 && listxattr("cx_h", l, sizeof l) == 0);
        EXPECT("a directory keeps them too", mkdir("cx_d", 0755) == 0 && setxattr("cx_d", "user.d", "dir", 3, 0) == 0 && getxattr("cx_d", "user.d", v, sizeof v) == 3);
        EXPECT("a symbolic link: l- forms act on the link, others on the target", symlink("cx_d", "cx_s") == 0 && getxattr("cx_s", "user.d", v, sizeof v) == 3 && lgetxattr("cx_s", "user.d", v, sizeof v) == -1);
        EXPECT("an fd opened through a symbolic link holds the target's attributes", ({ int f1 = open("cx_t", O_RDWR | O_CREAT, 0644), f2, f3, r;
               symlink("cx_t", "cx_u"); f2 = open("cx_u", O_RDONLY); f3 = open("cx_f2", O_RDWR | O_CREAT, 0644);
               r = f1 >= 0 && f2 >= 0 && f3 >= 0 && fsetxattr(f3, "user.k", "v", 1, XATTR_CREATE) == 0 && fsetxattr(f2, "user.k", "w", 1, XATTR_CREATE) == 0
                   && fgetxattr(f1, "user.k", v, sizeof v) == 1 && v[0] == 'w' && fgetxattr(f3, "user.k", v, sizeof v) == 1 && v[0] == 'v';
               close(f1); close(f2); close(f3); unlink("cx_u"); unlink("cx_t"); unlink("cx_f2"); r; }));
        EXPECT("a missing file is ENOENT", removexattr("cx_none", "user.a") == -1 && errno == ENOENT);
        EXPECT("a bad descriptor is EBADF", fgetxattr(-1, "user.a", v, sizeof v) == -1 && errno == EBADF);
        EXPECT("a bad name pointer is EFAULT", getxattr("cx_h", (const char *)1, v, sizeof v) == -1 && errno == EFAULT);
        EXPECT("the attributes of a removed file do not come back", setxattr("cx_h", "user.z", "z", 1, 0) == 0 && unlink("cx_h") == 0 && open("cx_h", O_RDWR | O_CREAT, 0644) >= 0 && listxattr("cx_h", l, sizeof l) == 0);
        unlink("cx_h");
        unlink("cx_s");
        rmdir("cx_d");
        if (x >= 0) close(x);
        unlink("cx_f");
    }
    EXPECT("a write of many unaligned blocks is not short", ({
           static char big[70000], back[70000];
           int r = 0, g = open("cf_big", O_RDWR | O_CREAT | O_TRUNC, 0644);
           for (int j = 0; j < (int)sizeof big; j++) big[j] = (char)(j * 7 + 3);
           for (int k = 0; g >= 0 && k < 6; k++) {
               off_t at = 7900 + k * 70001;
               ssize_t w = lseek(g, at, SEEK_SET) == at ? write(g, big, sizeof big) : -1;
               r += (w == (ssize_t)sizeof big && lseek(g, at, SEEK_SET) == at && read(g, back, sizeof back) == (ssize_t)sizeof back && memcmp(big, back, sizeof big) == 0);
           }
           if (g >= 0) close(g);
           unlink("cf_big");
           r == 6; }));
    /* A regular file is not read short for want of cache (mkbuiltins in bash's
     * build reads a whole file in one call and goes on with 4096 bytes). */
    {
        static char big[30000];
        int self = open("/proc/self/exe", O_RDONLY);
        EXPECT("read of 30000 bytes from a program file returns them all", self >= 0 && read(self, big, sizeof big) == (ssize_t)sizeof big);
        if (self >= 0) close(self);
    }
    return done();
}
