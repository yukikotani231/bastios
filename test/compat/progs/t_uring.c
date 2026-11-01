/* io_uring through the raw system calls: a ring is set up and mapped the way
 * liburing does it, requests are queued by writing the shared memory, and
 * io_uring_enter does them. The last checks write the rings behind the
 * kernel's back (a stale head, an index past the end, a tail far ahead): the
 * kernel must only do the requests its own accounting allows. */
#define _GNU_SOURCE
#include "compat.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#define SYS_uring_setup 425
#define SYS_uring_enter 426
#define SYS_uring_register 427
#define OFF_SQ 0ull
#define OFF_CQ 0x8000000ull
#define OFF_SQES 0x10000000ull
#define OP_NOP 0
#define OP_READV 1
#define OP_WRITEV 2
#define OP_FSYNC 3
#define OP_READ 22
#define OP_WRITE 23

struct sqoff { uint32_t head, tail, mask, entries, flags, dropped, array, resv1; uint64_t user_addr; };
struct cqoff { uint32_t head, tail, mask, entries, overflow, cqes, flags, resv1; uint64_t user_addr; };
struct params {
    uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle, features, wq_fd, resv[3];
    struct sqoff sq_off;
    struct cqoff cq_off;
};
struct sqe {
    uint8_t opcode, flags;
    uint16_t ioprio;
    int32_t fd;
    uint64_t off, addr;
    uint32_t len, op_flags;
    uint64_t user_data;
    uint16_t buf_index, personality;
    int32_t splice_fd_in;
    uint64_t addr3, pad;
};
struct cqe { uint64_t user_data; int32_t res; uint32_t flags; };

static int rfd = -1;
static struct params p;
static char *ring;
static struct sqe *sqes;

static long enter(int fd, unsigned submit, unsigned min, unsigned flags) {
    return syscall(SYS_uring_enter, fd, submit, min, flags, 0, 0);
}

static int ring_open(unsigned entries) {
    memset(&p, 0, sizeof p);
    int fd = syscall(SYS_uring_setup, entries, &p);
    if (fd < 0) return fd;
    size_t sz = p.cq_off.cqes + p.cq_entries * sizeof(struct cqe);
    ring = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, OFF_SQ);
    sqes = mmap(0, p.sq_entries * sizeof(struct sqe), PROT_READ | PROT_WRITE, MAP_SHARED, fd, OFF_SQES);
    if (ring == MAP_FAILED || sqes == MAP_FAILED) return -1000;
    return fd;
}

static volatile uint32_t *u32at(unsigned off) { return (volatile uint32_t *)(ring + off); }

/* Queues one entry at the program's tail (the kernel is not told yet). */
static struct sqe *queue(void) {
    uint32_t tail = *u32at(p.sq_off.tail);
    uint32_t idx = tail & *u32at(p.sq_off.mask);
    ((volatile uint32_t *)(ring + p.sq_off.array))[idx] = idx;
    struct sqe *e = &sqes[idx];
    memset(e, 0, sizeof *e);
    *u32at(p.sq_off.tail) = tail + 1;
    return e;
}

/* Takes one completion, or says there is none. */
static int reap(struct cqe *out) {
    uint32_t head = *u32at(p.cq_off.head);
    if (head == *u32at(p.cq_off.tail)) return 0;
    struct cqe *c = (struct cqe *)(ring + p.cq_off.cqes) + (head & *u32at(p.cq_off.mask));
    *out = *c;
    *u32at(p.cq_off.head) = head + 1;
    return 1;
}

int main(void) {
    setup();
    int f = open("/tmp/uring.dat", O_RDWR | O_CREAT | O_TRUNC, 0600);

    EXPECT("io_uring_setup returns a ring", (rfd = ring_open(8)) >= 0);
    EXPECT("readahead on a ring is EINVAL", syscall(SYS_readahead, rfd, 0, 4096) == -1 && errno == EINVAL);
    EXPECT("the ring has the sizes asked for, and the single-map feature",
           p.sq_entries == 8 && p.cq_entries == 16 && (p.features & 1) && *u32at(p.sq_off.mask) == 7 && *u32at(p.cq_off.mask) == 15);
    EXPECT("entries are rounded up to a power of two", ({ struct params q; memset(&q, 0, sizeof q); int g = syscall(SYS_uring_setup, 5, &q); int ok = g >= 0 && q.sq_entries == 8; if (g >= 0) close(g); ok; }));
    EXPECT("zero entries and setup flags are refused", ({ struct params q; memset(&q, 0, sizeof q); int a = syscall(SYS_uring_setup, 0, &q); q.flags = 1; int b = syscall(SYS_uring_setup, 4, &q); a == -1 && errno == EINVAL && b == -1 && errno == EINVAL; }));
    EXPECT("an unmapped bad offset is refused", mmap(0, 4096, PROT_READ, MAP_SHARED, rfd, 0x20000000ull) == MAP_FAILED);

    EXPECT("a NOP completes with its user data", ({
        struct sqe *e = queue(); e->opcode = OP_NOP; e->user_data = 0x1234abcd;
        struct cqe c; long n = enter(rfd, 1, 1, 1);
        n == 1 && reap(&c) && c.user_data == 0x1234abcd && c.res == 0;
    }));

    static char msg[] = "hello from io_uring";
    EXPECT("a write goes to the file", ({
        struct sqe *e = queue(); e->opcode = OP_WRITE; e->fd = f; e->addr = (uint64_t)msg; e->len = sizeof msg; e->off = 0; e->user_data = 7;
        struct cqe c; long n = enter(rfd, 1, 1, 1);
        char back[64] = {0};
        n == 1 && reap(&c) && c.user_data == 7 && c.res == (int)sizeof msg && pread(f, back, sizeof msg, 0) == (ssize_t)sizeof msg && !strcmp(back, msg);
    }));
    EXPECT("a read comes back", ({
        static char buf[64];
        struct sqe *e = queue(); e->opcode = OP_READ; e->fd = f; e->addr = (uint64_t)buf; e->len = sizeof msg; e->off = 0; e->user_data = 8;
        struct cqe c; long n = enter(rfd, 1, 1, 1);
        n == 1 && reap(&c) && c.res == (int)sizeof msg && !strcmp(buf, msg);
    }));
    EXPECT("readv and writev take vectors", ({
        char a[4] = "abcd", b[4] = "efgh";
        struct iovec w[2] = {{a, 4}, {b, 4}};
        struct sqe *e = queue(); e->opcode = OP_WRITEV; e->fd = f; e->addr = (uint64_t)w; e->len = 2; e->off = 100; e->user_data = 1;
        struct cqe c1, c2; long n1 = enter(rfd, 1, 1, 1); int r1 = reap(&c1);
        char x[4] = {0}, y[4] = {0};
        struct iovec r[2] = {{x, 4}, {y, 4}};
        e = queue(); e->opcode = OP_READV; e->fd = f; e->addr = (uint64_t)r; e->len = 2; e->off = 100; e->user_data = 2;
        long n2 = enter(rfd, 1, 1, 1); int r2 = reap(&c2);
        n1 == 1 && n2 == 1 && r1 && r2 && c1.res == 8 && c2.res == 8 && !memcmp(x, "abcd", 4) && !memcmp(y, "efgh", 4);
    }));
    EXPECT("an offset of -1 uses and moves the file position", ({
        lseek(f, 0, SEEK_SET);
        char two[2];
        struct sqe *e = queue(); e->opcode = OP_READ; e->fd = f; e->addr = (uint64_t)two; e->len = 2; e->off = (uint64_t)-1; e->user_data = 3;
        struct cqe c; long n = enter(rfd, 1, 1, 1);
        n == 1 && reap(&c) && c.res == 2 && two[0] == 'h' && lseek(f, 0, SEEK_CUR) == 2;
    }));
    EXPECT("fsync completes", ({
        struct sqe *e = queue(); e->opcode = OP_FSYNC; e->fd = f; e->user_data = 4;
        struct cqe c; long n = enter(rfd, 1, 1, 1);
        n == 1 && reap(&c) && c.res == 0;
    }));
    EXPECT("errors are in the completion: bad descriptor, unknown opcode, pipe", ({
        struct cqe a, b, d;
        struct sqe *e = queue(); e->opcode = OP_READ; e->fd = 999; e->user_data = 1;
        e = queue(); e->opcode = 200; e->user_data = 2;
        int pp[2]; pipe(pp);
        char z[1];
        e = queue(); e->opcode = OP_READ; e->fd = pp[0]; e->addr = (uint64_t)z; e->len = 1; e->user_data = 3;
        long n = enter(rfd, 3, 3, 1);
        int ok = n == 3 && reap(&a) && reap(&b) && reap(&d);
        ok && a.res == -EBADF && b.res == -EINVAL && d.res < 0;
    }));
    EXPECT("several requests at once complete in order", ({
        struct cqe c[4]; int ok = 1;
        for (int i = 0; i < 4; i++) { struct sqe *e = queue(); e->opcode = OP_NOP; e->user_data = 100 + i; }
        long n = enter(rfd, 4, 4, 1);
        for (int i = 0; i < 4; i++) ok = ok && reap(&c[i]) && c[i].user_data == (uint64_t)(100 + i);
        n == 4 && ok;
    }));
    EXPECT("the probe lists the operations", ({
        struct { uint8_t last, len; uint16_t r; uint32_t r2[3]; struct { uint8_t op, r; uint16_t flags; uint32_t r2; } ops[24]; } pr;
        memset(&pr, 0, sizeof pr);
        long r = syscall(SYS_uring_register, rfd, 8, &pr, 24);
        r == 0 && pr.ops[OP_WRITE].flags == 1 && pr.ops[OP_READ].flags == 1 && pr.ops[OP_NOP].flags == 1 && pr.ops[5].flags == 0;
    }));
    EXPECT("other register calls are refused", syscall(SYS_uring_register, rfd, 0, 0, 0) == -1 && errno == EINVAL);
    EXPECT("enter on a file that is not a ring is refused", enter(f, 1, 0, 0) == -1 && errno == EOPNOTSUPP);
    EXPECT("enter on a bad descriptor is EBADF", enter(777, 1, 0, 0) == -1 && errno == EBADF);
    EXPECT("unknown enter flags are refused", enter(rfd, 0, 0, 0x100) == -1 && errno == EINVAL);

    /* The program owns the tail and the completion head. The kernel owns the
     * submission head and the completion tail, and must not take them from the
     * shared memory. */
    EXPECT("a head written back to zero does not run old requests again", ({
        struct cqe c;
        for (int i = 0; i < 3; i++) { struct sqe *e = queue(); e->opcode = OP_NOP; e->user_data = 500 + i; }
        long n = enter(rfd, 3, 3, 1);
        int got = 0;
        while (reap(&c)) got++;
        *u32at(p.sq_off.head) = 0;                    /* the kernel's field, clobbered */
        long n2 = enter(rfd, 8, 0, 0);                /* nothing new is queued */
        n == 3 && got == 3 && n2 == 0 && !reap(&c);
    }));
    EXPECT("an index past the end of the array is dropped, not run", ({
        struct cqe c;
        uint32_t tail = *u32at(p.sq_off.tail);
        ((volatile uint32_t *)(ring + p.sq_off.array))[tail & 7] = 0x7fffffff;
        *u32at(p.sq_off.tail) = tail + 1;
        long n = enter(rfd, 1, 0, 0);
        n == 1 && !reap(&c) && *u32at(p.sq_off.dropped) == 1;
    }));
    EXPECT("a tail far ahead is limited to the ring", ({
        struct cqe c;
        uint32_t tail = *u32at(p.sq_off.tail);
        for (unsigned i = 0; i < 8; i++) {
            ((volatile uint32_t *)(ring + p.sq_off.array))[(tail + i) & 7] = (tail + i) & 7;
            struct sqe *e = &sqes[(tail + i) & 7]; memset(e, 0, sizeof *e); e->opcode = OP_NOP; e->user_data = 900 + i;
        }
        *u32at(p.sq_off.tail) = tail + 0x40000000u;
        long n = enter(rfd, 1000, 0, 0);
        int got = 0;
        while (reap(&c)) got++;
        n >= 1 && n <= 8 && got == n;
    }));
    EXPECT("a full completion ring stops the kernel taking requests", ({
        struct cqe c;
        while (reap(&c)) {}
        uint32_t t = *u32at(p.sq_off.tail);
        *u32at(p.sq_off.tail) = t;                    /* nothing queued */
        long total = 0;
        for (int round = 0; round < 4; round++) {
            for (int i = 0; i < 8; i++) { struct sqe *e = queue(); e->opcode = OP_NOP; e->user_data = i; }
            long n = enter(rfd, 8, 0, 0);
            if (n > 0) total += n;
        }
        int left = 0;
        while (reap(&c)) left++;
        total <= 16 && left == total;
    }));
    EXPECT("the mapping stays valid after the descriptor is closed", ({
        close(rfd);
        *u32at(p.sq_off.tail) = *u32at(p.sq_off.tail);
        1;
    }));
    close(f);
    unlink("/tmp/uring.dat");
    return done();
}
