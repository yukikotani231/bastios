/* Directories through the Linux system calls, against musl: openat,
 * fstatat, mkdirat and unlinkat relative to a directory descriptor,
 * fchdir to it, getcwd, a file mapped with mmap and MAP_FIXED over it,
 * running out of descriptors, a named pipe (mkfifo), the errors a path
 * walk gives (ENOTDIR, ELOOP), renameat2 with RENAME_EXCHANGE, mknod of a
 * regular file, one write of more than 4096 bytes, O_TMPFILE refused, a
 * full in-memory table (ENOSPC), a tmpfs mount (read-only too), a
 * BastiOS file system made with mkfs.bstdisk3 on a disk image and
 * mounted (bstdisk3), and cleaning up. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

static char big[10000];

static int fail(const char *what)
{
    printf("mdir: %s failed (errno %d)\n", what, errno);
    return 1;
}

/* A disk image: refused until mkfs.bstdisk3 formats it, then mounted,
 * written, unmounted, and mounted again with everything still there. */
static int lomount(void)
{
    int im = open("img", O_CREAT | O_RDWR, 0644);
    if (im < 0 || ftruncate(im, 1 << 20) != 0 || close(im) != 0 || mkdir("lm", 0755) != 0)
        return fail("a disk image");
    if (mount("img", "lm", "bstdisk3", 0, 0) != -1 || errno != EINVAL)
        return fail("mount of an unformatted image");
    int status = -1;
    pid_t c = fork();
    if (c == 0) {
        execl("/bin/mkfs.bstdisk3", "mkfs.bstdisk3", "-F", "img", (char *)0);
        _exit(127);
    }
    if (c < 0 || waitpid(c, &status, 0) != c || status != 0)
        return fail("mkfs.bstdisk3");
    if (mount("img", "lm", "bstdisk3", 0, 0) != 0)
        return fail("mount of a bstdisk3 image");
    if (mount("img", "lm", "bstdisk3", 0, 0) != -1 || errno != EBUSY)
        return fail("a second mount on it");
    char mb[1024] = { 0 };
    int pm = open("/proc/self/mounts", O_RDONLY);
    if (pm < 0 || read(pm, mb, sizeof mb - 1) <= 0 || !strstr(mb, " /dx/lm bstdisk3 rw"))
        return fail("bstdisk3 in /proc/self/mounts");
    close(pm);
    int w = open("lm/a", O_CREAT | O_WRONLY, 0644);
    if (w < 0 || write(w, "hello", 5) != 5 || mkdir("lm/d", 0755) != 0)
        return fail("writing to the mount");
    if (umount2("lm", 0) != -1 || errno != EBUSY || rmdir("lm") != -1 || errno != EBUSY)
        return fail("a busy bstdisk3 mount");
    close(w);
    w = open("lm/d/b", O_CREAT | O_WRONLY, 0600);
    if (w < 0 || write(w, big, sizeof big) != sizeof big || close(w) != 0 || symlink("d/b", "lm/l") != 0)
        return fail("a file in a directory in the mount");
    if (rename("lm/a", "moved") != -1 || errno != EXDEV)
        return fail("a rename out of the mount");
    int n = 0;
    DIR *dir = opendir("lm");
    struct dirent *e;
    while (dir && (e = readdir(dir)))
        n += !strcmp(e->d_name, "a") || !strcmp(e->d_name, "d") || !strcmp(e->d_name, "l");
    if (!dir || n != 3)
        return fail("listing the mount");
    closedir(dir);
    if (umount2("lm", 0) != 0)
        return fail("umount2 of bstdisk3");
    struct stat st;
    if (stat("lm/a", &st) == 0 || mount("img", "lm", "bstdisk3", MS_RDONLY, 0) != 0)
        return fail("mounting it again");
    char rb[8] = { 0 };
    int r = open("lm/a", O_RDONLY);
    if (r < 0 || read(r, rb, sizeof rb) != 5 || strcmp(rb, "hello") != 0 || close(r) != 0
        || stat("lm/d/b", &st) != 0 || st.st_size != sizeof big || (st.st_mode & 0777) != 0600
        || lstat("lm/l", &st) != 0 || !S_ISLNK(st.st_mode))
        return fail("what the image kept");
    if (open("lm/new", O_CREAT | O_WRONLY, 0644) != -1 || errno != EROFS || unlink("lm/a") != -1 || errno != EROFS)
        return fail("a read-only bstdisk3 mount");
    /* The image's name can go while it is mounted; the file goes at the unmount. */
    if (unlink("img") != 0 || stat("img", &st) == 0 || stat("lm/a", &st) != 0
        || umount2("lm", 0) != 0 || rmdir("lm") != 0)
        return fail("removing the image");
    return 0;
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
    int many[300], got = 0;
    while (got < 300 && (many[got] = open("f", O_RDONLY)) >= 0)
        got++;
    if (got == 300 || errno != EMFILE)
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
    /* A file in the middle of a path is not a directory; a loop of links
     * ends in ELOOP; "link/" names what the link points to. */
    if (open("f/x", O_RDONLY) != -1 || errno != ENOTDIR)
        return fail("ENOTDIR for a file in a path");
    if (symlink("lb", "la") != 0 || symlink("la", "lb") != 0 || open("la", O_RDONLY) != -1 || errno != ELOOP)
        return fail("ELOOP for a loop of links");
    if (symlink(".", "ld") != 0 || lstat("ld", &st) != 0 || !S_ISLNK(st.st_mode)
        || lstat("ld/", &st) != 0 || !S_ISDIR(st.st_mode))
        return fail("lstat of \"link/\"");
    if (unlink("la") != 0 || unlink("lb") != 0 || unlink("ld") != 0)
        return fail("removing the links");
    /* RENAME_EXCHANGE swaps two names. */
    int e1 = open("e1", O_CREAT | O_WRONLY, 0644), e2 = open("e2", O_CREAT | O_WRONLY, 0644);
    if (e1 < 0 || e2 < 0 || write(e1, "1", 1) != 1 || write(e2, "22", 2) != 2)
        return fail("making two files to swap");
    close(e1);
    close(e2);
    if (syscall(SYS_renameat2, AT_FDCWD, "e1", AT_FDCWD, "e2", 2) != 0 || stat("e1", &st) != 0 || st.st_size != 2
        || stat("e2", &st) != 0 || st.st_size != 1 || unlink("e1") != 0 || unlink("e2") != 0)
        return fail("renameat2(RENAME_EXCHANGE)");
    /* mknod makes a regular file too. */
    if (mknod("rn", S_IFREG | 0600, 0) != 0 || stat("rn", &st) != 0 || !S_ISREG(st.st_mode)
        || (st.st_mode & 0777) != 0600 || unlink("rn") != 0)
        return fail("mknod of a regular file");
    /* One write call moves all of a buffer bigger than a page. */
    memset(big, 'b', sizeof big);
    int bw = open("bw", O_CREAT | O_RDWR, 0644);
    if (bw < 0 || write(bw, big, sizeof big) != (ssize_t)sizeof big || fstat(bw, &st) != 0
        || st.st_size != (off_t)sizeof big)
        return fail("a write of 10000 bytes");
    close(bw);
    if (unlink("bw") != 0)
        return fail("removing bw");
    if (open(".", O_TMPFILE | O_RDWR, 0600) != -1 || errno != EOPNOTSUPP)
        return fail("O_TMPFILE refused");
    /* Making directories until the in-memory table is full ends in ENOSPC. */
    int made = 0;
    char nm[16];
    while (made < 1000) {
        snprintf(nm, sizeof nm, "n%d", made);
        if (mkdir(nm, 0755) != 0)
            break;
        made++;
    }
    if (made == 1000 || errno != ENOSPC)
        return fail("filling the in-memory table");
    while (made > 0) {
        snprintf(nm, sizeof nm, "n%d", --made);
        if (rmdir(nm) != 0)
            return fail("emptying the in-memory table");
    }
    /* tmpfs: mounted on an empty directory, listed in /proc/self/mounts,
     * busy while a file in it is open, emptied by umount2; read-only
     * with MS_RDONLY until a remount. */
    if (mkdir("mt", 0755) != 0 || mount("none", "mt", "ext2", 0, 0) != -1 || errno != ENODEV)
        return fail("mount of a type BastiOS lacks");
    if (mount("none", "mt", "tmpfs", 0, 0) != 0 || mount("none", "mt", "tmpfs", 0, 0) != -1 || errno != EBUSY)
        return fail("mount of tmpfs");
    char mb[512] = { 0 };
    int pm = open("/proc/self/mounts", O_RDONLY);
    if (pm < 0 || read(pm, mb, sizeof mb - 1) <= 0 || !strstr(mb, "tmpfs /dx/mt tmpfs rw"))
        return fail("tmpfs in /proc/self/mounts");
    close(pm);
    struct statfs sf;
    if (statfs("mt", &sf) != 0 || sf.f_type != 0x01021994 || statfs(".", &sf) != 0 || sf.f_type == 0x01021994)
        return fail("statfs of a tmpfs");
    int mf = open("mt/in", O_CREAT | O_WRONLY, 0644);
    if (mf < 0 || write(mf, "x", 1) != 1)
        return fail("a file in tmpfs");
    if (fstatfs(mf, &sf) != 0 || sf.f_type != 0x01021994 || fstatfs(d, &sf) != 0 || sf.f_type == 0x01021994)
        return fail("fstatfs of a file in tmpfs");
    if (rmdir("mt") != -1 || errno != EBUSY || umount2("mt", 0) != -1 || errno != EBUSY)
        return fail("a busy mount");
    close(mf);
    if (umount2("mt", 0) != 0 || stat("mt/in", &st) == 0 || umount2("mt", 0) != -1 || errno != EINVAL)
        return fail("umount2");
    if (mount("none", "mt", "tmpfs", MS_RDONLY, 0) != 0 || open("mt/ro", O_CREAT | O_WRONLY, 0644) != -1
        || errno != EROFS || mkdir("mt/d", 0755) != -1 || errno != EROFS)
        return fail("a read-only mount");
    if (mount(0, "mt", 0, MS_REMOUNT, 0) != 0 || (mf = open("mt/rw", O_CREAT | O_WRONLY, 0644)) < 0)
        return fail("a remount read-write");
    close(mf);
    if (umount2("mt", 0) != 0 || rmdir("mt") != 0)
        return fail("removing the mount");
    if (lomount() != 0)
        return 1;
    if (unlinkat(AT_FDCWD, "f", 0) != 0 || chdir("..") != 0 || rmdir("dx") != 0)
        return fail("cleaning up");
    close(d);
    printf("mdir: ok in %s, %s\n", cwd, "mapped");
    return 0;
}
