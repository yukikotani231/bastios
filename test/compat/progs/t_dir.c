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
    int fd;

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
    EXPECT("openat relative to a directory fd", (fd = open("cd_x", O_RDONLY | O_DIRECTORY)) >= 0 &&
           openat(fd, "f", O_RDONLY) >= 0);
    EXPECT("rmdir non-empty is ENOTEMPTY", rmdir("cd_x") == -1 && (errno == ENOTEMPTY || errno == EEXIST));
    EXPECT("unlink file in directory", unlink("cd_x/f") == 0);
    EXPECT("rmdir", rmdir("cd_x") == 0 && stat("cd_x", &st) == -1);
    EXPECT("mkdir nested missing parent is ENOENT", mkdir("cd_y/z", 0755) == -1 && errno == ENOENT);
    EXPECT("stat /tmp", stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode));
    EXPECT("stat /dev", stat("/dev", &st) == 0 && S_ISDIR(st.st_mode));
    EXPECT("stat /proc/self", stat("/proc/self", &st) == 0);
    {
        struct statvfs v;
        EXPECT("statvfs", statvfs("/", &v) == 0);
    }
    return done();
}
