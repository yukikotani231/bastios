/* Directories through the Linux system calls, against musl: openat,
 * fstatat, mkdirat and unlinkat relative to a directory descriptor,
 * fchdir to it, getcwd, and cleaning up. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("mdir: %s failed (errno %d)\n", what, errno);
    return 1;
}

int main(void)
{
    if (mkdir("dx", 0755) != 0)
        return fail("mkdir");
    int d = open("dx", O_RDONLY | O_DIRECTORY);
    if (d < 0)
        return fail("open of a directory");
    int f = openat(d, "f", O_CREAT | O_WRONLY, 0644);
    if (f < 0 || write(f, "at", 2) != 2)
        return fail("openat");
    close(f);
    struct stat st;
    if (fstatat(d, "f", &st, 0) != 0 || st.st_size != 2 || !S_ISREG(st.st_mode))
        return fail("fstatat");
    if (mkdirat(d, "s", 0755) != 0 || fstatat(d, "s", &st, 0) != 0 || !S_ISDIR(st.st_mode))
        return fail("mkdirat");
    if (unlinkat(d, "s", AT_REMOVEDIR) != 0 || fstatat(d, "s", &st, 0) == 0)
        return fail("unlinkat(AT_REMOVEDIR)");
    if (fchdir(d) != 0)
        return fail("fchdir");
    char cwd[64];
    if (!getcwd(cwd, sizeof cwd))
        return fail("getcwd");
    char buf[8] = { 0 };
    f = open("f", O_RDONLY);
    if (f < 0 || read(f, buf, sizeof buf) != 2 || strcmp(buf, "at") != 0)
        return fail("reading back");
    close(f);
    if (unlinkat(AT_FDCWD, "f", 0) != 0 || chdir("..") != 0 || rmdir("dx") != 0)
        return fail("cleaning up");
    close(d);
    printf("mdir: ok in %s\n", cwd);
    return 0;
}
