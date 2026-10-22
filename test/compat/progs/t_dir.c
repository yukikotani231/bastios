/* Directories: making, listing, changing into, removing. */
#include "compat.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

int main(void) {
    setup();
    char buf[256];
    struct stat st;
    int fd = -1;

    EXPECT("getcwd", getcwd(buf, sizeof buf) != NULL && buf[0] == '/');
    {
        DIR *d = opendir("/");
        int n = 0;
        struct dirent *e;
        if (d) while ((e = readdir(d)) != NULL) n++;
        EXPECT("opendir/readdir /", d != NULL && n > 0);
        if (d) closedir(d);
    }
    EXPECT("stat / is a directory", stat("/", &st) == 0 && S_ISDIR(st.st_mode));
    EXPECT("mkdir", mkdir("cd_x", 0755) == 0);
    EXPECT("stat new directory", stat("cd_x", &st) == 0 && S_ISDIR(st.st_mode));
    {
        /* A name over 200 bytes (up to NAME_MAX, 255) is kept whole where the disk holds it (it keeps 200 in one entry and the rest in the next), and one byte more is refused. */
        char nm[300], path[400], nm2[300], p2[400];
        const char *dir = access("/disk", W_OK) == 0 ? "/disk" : ".";
        size_t nl = 255;
        memset(nm, 'n', nl); nm[nl] = 0;
        memset(nm2, 'm', 201); nm2[201] = 0;
        snprintf(path, sizeof path, "%s/%s", dir, nm);
        snprintf(p2, sizeof p2, "%s/%s", dir, nm2);
        int ok = (fd = open(path, O_CREAT | O_WRONLY, 0644)) >= 0 && write(fd, "long", 4) == 4 && close(fd) == 0;
        ok = ok && stat(path, &st) == 0 && st.st_size == 4;
        ok = ok && rename(path, p2) == 0 && stat(path, &st) == -1 && stat(p2, &st) == 0 && st.st_size == 4;
        ok = ok && rename(p2, path) == 0 && stat(path, &st) == 0;
        {
            DIR *d = opendir(dir);
            struct dirent *e;
            int seen = 0;
            if (d) while ((e = readdir(d)) != NULL) seen += strcmp(e->d_name, nm) == 0;
            if (d) closedir(d);
            ok = ok && seen == 1;
        }
        strcat(path, "x");
        ok = ok && open(path, O_CREAT | O_WRONLY, 0644) == -1 && errno == ENAMETOOLONG;
        path[strlen(path) - 1] = 0;
        ok = ok && unlink(path) == 0 && stat(path, &st) == -1;
        EXPECT("long name: create, stat, rename, list, remove; one byte more is ENAMETOOLONG", ok);
    }
    EXPECT("mkdir existing is EEXIST", mkdir("cd_x", 0755) == -1 && errno == EEXIST);
    EXPECT("create file in directory", (fd = open("cd_x/f", O_CREAT | O_WRONLY, 0644)) >= 0 &&
           write(fd, "abc", 3) == 3 && close(fd) == 0);
    EXPECT("stat file in directory", stat("cd_x/f", &st) == 0 && st.st_size == 3);
    {
        DIR *d = opendir("cd_x");
        int seen = 0;
        struct dirent *e;
        if (d) while ((e = readdir(d)) != NULL) seen |= strcmp(e->d_name, "f") == 0;
        EXPECT("readdir subdirectory", d != NULL && seen);
        if (d) closedir(d);
    }
    EXPECT("chdir", chdir("cd_x") == 0);
    EXPECT("getcwd after chdir", getcwd(buf, sizeof buf) != NULL && strstr(buf, "cd_x") != NULL);
    EXPECT("relative open after chdir", (fd = open("f", O_RDONLY)) >= 0 && close(fd) == 0);
    EXPECT("chdir ..", chdir("..") == 0);
    EXPECT("fchdir", (fd = open("cd_x", O_RDONLY | O_DIRECTORY)) >= 0 && fchdir(fd) == 0 &&
           close(fd) == 0 && chdir("..") == 0);
    EXPECT("read of a directory is EISDIR", ({ char c1; (fd = open("cd_x", O_RDONLY | O_DIRECTORY)) >= 0 &&
           read(fd, &c1, 1) == -1 && errno == EISDIR && close(fd) == 0; }));
    {
        int g = -1;
        EXPECT("openat relative to a directory fd", (fd = open("cd_x", O_RDONLY | O_DIRECTORY)) >= 0 &&
               (g = openat(fd, "f", O_RDONLY)) >= 0);
        if (g >= 0) close(g);
        if (fd >= 0) close(fd);
    }
    EXPECT("rmdir non-empty is ENOTEMPTY", rmdir("cd_x") == -1 && (errno == ENOTEMPTY || errno == EEXIST));
    EXPECT("unlink file in directory", unlink("cd_x/f") == 0);
    EXPECT("rmdir", rmdir("cd_x") == 0 && stat("cd_x", &st) == -1);
    EXPECT("mkdir nested missing parent is ENOENT", mkdir("cd_y/z", 0755) == -1 && errno == ENOENT);
    if (begin("readdir: d_type and d_ino as lstat sees each entry")) {
        mkdir("cd_t", 0755); mkdir("cd_t/d", 0755);
        close(open("cd_t/f", O_CREAT | O_WRONLY, 0644));
        symlink("f", "cd_t/l"); symlink("d", "cd_t/dl"); symlink("none", "cd_t/b"); mkfifo("cd_t/p", 0644);
        DIR *d = opendir("cd_t");
        struct dirent *e;
        int seen = 0, bad = 0;
        char why[192] = "";
        while (d && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char p[64];
            snprintf(p, sizeof p, "cd_t/%s", e->d_name);
            struct stat ls;
            int want = -1;
            if (lstat(p, &ls) == 0)
                want = S_ISLNK(ls.st_mode) ? DT_LNK : S_ISDIR(ls.st_mode) ? DT_DIR : S_ISFIFO(ls.st_mode) ? DT_FIFO : DT_REG;
            if (e->d_type != want || e->d_ino != ls.st_ino) {
                bad++;
                snprintf(why, sizeof why, "%s: d_type %d (want %d), d_ino %lu (lstat %lu)", e->d_name, e->d_type, want,
                         (unsigned long)e->d_ino, (unsigned long)ls.st_ino);
            }
            seen++;
        }
        if (d) closedir(d);
        unlink("cd_t/f"); unlink("cd_t/l"); unlink("cd_t/dl"); unlink("cd_t/b"); unlink("cd_t/p"); rmdir("cd_t/d"); rmdir("cd_t");
        if (seen == 6 && !bad) pass(); else if (bad) fail("%s", why); else fail("saw %d entries", seen);
    }
    EXPECT("fchmod on a closed descriptor is EBADF", fchmod(1000, 0644) == -1 && errno == EBADF);
    EXPECT("stat /tmp", stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode));
    EXPECT("stat /dev", stat("/dev", &st) == 0 && S_ISDIR(st.st_mode));
    EXPECT("stat /proc/self", stat("/proc/self", &st) == 0);
    {
        struct statvfs v;
        EXPECT("statvfs", statvfs("/", &v) == 0);
    }
    return done();
}
