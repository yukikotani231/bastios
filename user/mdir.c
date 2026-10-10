/* Directories through the Linux system calls, against musl: openat,
 * fstatat, mkdirat and unlinkat relative to a directory descriptor,
 * fchdir to it, getcwd, a file mapped with mmap and MAP_FIXED over it,
 * running out of descriptors, a named pipe (mkfifo), and cleaning up. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
    if (openat(d, "f", O_CREAT | O_EXCL | O_WRONLY, 0644) != -1 || errno != EEXIST)
        return fail("O_EXCL on a file that exists");
    if (openat(d, "f", O_RDONLY | O_DIRECTORY) != -1 || errno != ENOTDIR)
        return fail("O_DIRECTORY on a file");
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
    char *m = mmap(0, 4096, PROT_READ, MAP_PRIVATE, f, 0);
    if (m == MAP_FAILED || m[0] != 'a' || m[1] != 't' || m[2] != 0)
        return fail("mmap of a file");
    if (mmap(m, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) != m || m[0] != 0)
        return fail("mmap with MAP_FIXED");
    /* Opening until nothing is left ends in EMFILE, not ENOENT. */
    int many[32], got = 0;
    while (got < 32 && (many[got] = open("f", O_RDONLY)) >= 0)
        got++;
    if (got == 32 || errno != EMFILE)
        return fail("running out of descriptors");
    while (got > 0)
        close(many[--got]);
    close(f);
    /* A named pipe: a writer waits for a reader, then they talk. */
    if (mkfifo("ff", 0644) != 0 || mkfifo("ff", 0644) != -1 || errno != EEXIST)
        return fail("mkfifo");
    if (stat("ff", &st) != 0 || !S_ISFIFO(st.st_mode))
        return fail("stat of a FIFO");
    if (open("ff", O_WRONLY | O_NONBLOCK) != -1 || errno != ENXIO)
        return fail("a FIFO writer with no reader");
    pid_t c = fork();
    if (c == 0) {
        int w = open("ff", O_WRONLY);
        _exit(w >= 0 && write(w, "fifo", 4) == 4 && close(w) == 0 ? 0 : 1);
    }
    int r = open("ff", O_RDONLY), status;
    char fb[8] = { 0 };
    int fn = 0, k;
    while (r >= 0 && fn < 7 && (k = read(r, fb + fn, sizeof fb - 1 - fn)) > 0)
        fn += k;
    if (c < 0 || r < 0 || strcmp(fb, "fifo") != 0 || waitpid(c, &status, 0) != c || status != 0)
        return fail("talking through a FIFO");
    close(r);
    if (unlink("ff") != 0 || stat("ff", &st) == 0)
        return fail("removing a FIFO");
    /* Renaming one name of a file to another name of it does nothing. */
    if (link("f", "f2") != 0 || rename("f", "f2") != 0 || stat("f", &st) != 0 || stat("f2", &st) != 0
        || st.st_nlink != 2 || unlink("f2") != 0)
        return fail("renaming between two names of one file");
    if (unlinkat(AT_FDCWD, "f", 0) != 0 || chdir("..") != 0 || rmdir("dx") != 0)
        return fail("cleaning up");
    close(d);
    printf("mdir: ok in %s, %s\n", cwd, "mapped");
    return 0;
}
