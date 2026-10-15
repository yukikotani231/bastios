/* The device filter of a cgroup: a BPF_PROG_TYPE_CGROUP_DEVICE program
 * loaded with bpf(2), attached to a cgroup directory and run on every open
 * of a device and every mknod of a device node, as runc and crun use it. */
#include "compat.h"
#define _GNU_SOURCE
#include <stdint.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

struct insn { uint8_t code; uint8_t regs; int16_t off; int32_t imm; };
#define I(c, d, s, o, i) ((struct insn){ c, (uint8_t)((s) << 4 | (d)), o, i })

static long bpf(int cmd, void *attr, unsigned size) { return syscall(SYS_bpf, cmd, attr, size); }

/* Allow only the character device 1:3, for reading and writing. */
static struct insn allow_null[] = {
    I(0x61, 2, 1, 0, 0),    /* r2 = ctx->access_type */
    I(0x61, 3, 1, 4, 0),    /* r3 = ctx->major */
    I(0x61, 4, 1, 8, 0),    /* r4 = ctx->minor */
    I(0x54, 2, 0, 0, 0xffff),  /* r2 &= 0xffff */
    I(0x55, 2, 0, 5, 2),    /* if r2 != char goto deny */
    I(0x55, 3, 0, 4, 1),    /* if r3 != 1 goto deny */
    I(0x55, 4, 0, 3, 3),    /* if r4 != 3 goto deny */
    I(0xb7, 0, 0, 0, 1),    /* r0 = 1 */
    I(0x95, 0, 0, 0, 0),    /* exit */
    I(0xb7, 0, 0, 0, 0),    /* deny: r0 = 0 */
    I(0x95, 0, 0, 0, 0),
};

static int load(struct insn *p, int n) {
    static const char lic[] = "GPL";
    struct { uint32_t type, cnt; uint64_t insns, license; } a = { 15, (uint32_t)n, (uint64_t)(uintptr_t)p, (uint64_t)(uintptr_t)lic };
    return (int)bpf(5, &a, sizeof a);
}

static int attach(int cg, int prog, unsigned flags) {
    uint32_t a[8] = { cg, prog, 6, flags };
    return (int)bpf(8, a, 16);
}

static int detach(int cg, int prog) {
    uint32_t a[8] = { cg, prog, 6, 0 };
    return (int)bpf(9, a, 16);
}

static int link_create(int prog, int cg) {
    uint32_t a[8] = { prog, cg, 6, 0 };
    return (int)bpf(28, a, 16);
}

/* Runs fn in a child that has joined the cgroup; returns its exit status. */
static int in_cgroup(const char *procs, int (*fn)(void)) {
    pid_t c = fork();
    if (c == 0) {
        char me[16];
        snprintf(me, sizeof me, "%d", getpid());
        int f = open(procs, O_WRONLY);
        if (f < 0 || write(f, me, strlen(me)) < 0) _exit(100);
        close(f);
        _exit(fn());
    }
    int st = 0;
    waitpid(c, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 101;
}

static int probe(void) {
    int r = 0;
    int f = open("/dev/null", O_RDWR);
    if (f < 0) r |= 1; else close(f);
    f = open("/dev/zero", O_RDONLY);
    if (f >= 0 || errno != EPERM) r |= 2;
    if (f >= 0) close(f);
    f = open("/dev/null", O_RDWR);
    if (f >= 0) close(f);
    f = open("/dev/zero", O_WRONLY);
    if (f >= 0 || errno != EPERM) r |= 4;
    if (f >= 0) close(f);
    if (mknod("/tmp/devcg_z", S_IFCHR | 0600, makedev(1, 5)) == 0 || errno != EPERM) r |= 8;
    unlink("/tmp/devcg_z");
    if (mknod("/tmp/devcg_n", S_IFCHR | 0600, makedev(1, 3)) != 0) r |= 16;
    unlink("/tmp/devcg_n");
    /* a regular file is no device */
    f = open("/tmp/devcg_f", O_CREAT | O_RDWR, 0600);
    if (f < 0) r |= 32; else close(f);
    unlink("/tmp/devcg_f");
    return r;
}

static int open_zero(void) {
    int f = open("/dev/zero", O_RDONLY);
    if (f < 0) return 1;
    close(f);
    return 0;
}

int main(void) {
    setup();
    mkdir("/sys/fs/cgroup/devcg", 0755);
    int cg = open("/sys/fs/cgroup/devcg", O_RDONLY | O_DIRECTORY);
    EXPECT("a cgroup directory opens", cg >= 0);

    EXPECT("a program that does not end in exit is EINVAL", ({
        struct insn bad[] = { I(0xb7, 0, 0, 0, 1) };
        load(bad, 1) == -1 && errno == EINVAL; }));
    EXPECT("a backward jump is EINVAL", ({
        struct insn bad[] = { I(0x05, 0, 0, -1, 0), I(0x95, 0, 0, 0, 0) };
        load(bad, 2) == -1 && errno == EINVAL; }));

    int prog = load(allow_null, sizeof allow_null / sizeof allow_null[0]);
    EXPECT("a device program loads", prog >= 0);
    EXPECT("attaching to a descriptor that is no cgroup is EBADF or EINVAL",
           attach(prog, prog, 2) == -1 && (errno == EBADF || errno == EINVAL));

    EXPECT("nothing filters before the attach", in_cgroup("/sys/fs/cgroup/devcg/cgroup.procs", open_zero) == 0);
    EXPECT("attach with ALLOW_MULTI", attach(cg, prog, 2) == 0);
    EXPECT("the process in the cgroup sees only 1:3", in_cgroup("/sys/fs/cgroup/devcg/cgroup.procs", probe) == 0);
    EXPECT("a process outside the cgroup is not filtered", open_zero() == 0);
    EXPECT("detach", detach(cg, prog) == 0);
    EXPECT("after the detach everything opens", in_cgroup("/sys/fs/cgroup/devcg/cgroup.procs", open_zero) == 0);

    int lk = link_create(prog, cg);
    EXPECT("a link attaches the program", lk >= 0);
    EXPECT("the link filters", in_cgroup("/sys/fs/cgroup/devcg/cgroup.procs", open_zero) == 1);
    close(lk);
    EXPECT("closing the link detaches the program", in_cgroup("/sys/fs/cgroup/devcg/cgroup.procs", open_zero) == 0);

    /* A cgroup below inherits the filter of the one above. */
    mkdir("/sys/fs/cgroup/devcg/sub", 0755);
    int lk2 = link_create(prog, cg);
    EXPECT("a child cgroup is filtered by its parent's program",
           lk2 >= 0 && in_cgroup("/sys/fs/cgroup/devcg/sub/cgroup.procs", open_zero) == 1);
    close(lk2);
    close(prog);
    close(cg);
    rmdir("/sys/fs/cgroup/devcg/sub");
    EXPECT("the cgroup goes away with its program attached or not", rmdir("/sys/fs/cgroup/devcg") == 0);
    return done();
}
