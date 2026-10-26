/* System V IPC: message queues, semaphore sets and shared memory, their keys and
 * permissions, IPC_STAT/IPC_SET/IPC_RMID, blocking waits that a signal or
 * a removal ends, and SEM_UNDO at exit. */
#include "compat.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MSG_COPY
#define MSG_COPY 040000
#endif
struct m { long type; char text[64]; };
union semun { int val; struct semid_ds *buf; unsigned short *array; };
static void nop(int s) { (void)s; }

static int queue_basics(void) {
    int q = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (q < 0) return 0;
    struct m a = {2, "two"}, b = {1, "one"}, r;
    int ok = msgsnd(q, &a, 4, 0) == 0 && msgsnd(q, &b, 4, 0) == 0;
    struct msqid_ds ds;
    ok = ok && msgctl(q, IPC_STAT, &ds) == 0 && ds.msg_qnum == 2 && ds.msg_cbytes == 8 && ds.msg_lspid == getpid()
         && (ds.msg_perm.mode & 0777) == 0600 && ds.msg_qbytes > 0;
    ok = ok && msgrcv(q, &r, sizeof r.text, 1, 0) == 4 && strcmp(r.text, "one") == 0;   /* by type */
    ok = ok && msgrcv(q, &r, 2, 0, IPC_NOWAIT) == -1 && errno == E2BIG;
    ok = ok && msgrcv(q, &r, 2, 0, MSG_NOERROR) == 2 && r.type == 2;                    /* truncated */
    ok = ok && msgrcv(q, &r, sizeof r.text, 0, IPC_NOWAIT) == -1 && errno == ENOMSG;
    ok = ok && msgctl(q, IPC_RMID, 0) == 0 && msgsnd(q, &a, 4, 0) == -1 && errno == EINVAL;
    return ok;
}

static int queue_types(void) {
    int q = msgget(IPC_PRIVATE, 0600);
    struct m s = {0, ""}, r;
    long order[] = {5, 3, 4, 3};
    for (int i = 0; i < 4; i++) { s.type = order[i]; s.text[0] = 'a' + i; msgsnd(q, &s, 1, 0); }
    int ok = msgrcv(q, &r, 1, -4, 0) == 1 && r.type == 3 && r.text[0] == 'b';          /* lowest <= 4, first */
    ok = ok && msgrcv(q, &r, 1, 5, MSG_EXCEPT) == 1 && r.type == 4;
    ok = ok && msgrcv(q, &r, 1, 0, 0) == 1 && r.type == 5;
    msgctl(q, IPC_RMID, 0);
    return ok;
}

static int queue_keys(void) {
    key_t key = 0x5157;
    int q = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    int ok = q >= 0 && msgget(key, 0) == q && msgget(key, IPC_CREAT | IPC_EXCL) == -1 && errno == EEXIST;
    ok = ok && msgget(0x5158, 0) == -1 && errno == ENOENT;
    msgctl(q, IPC_RMID, 0);
    return ok;
}

static int queue_perms(void) {
    int q = msgget(IPC_PRIVATE, 0600);
    pid_t c = fork();
    if (c == 0) {
        struct m s = {1, "x"};
        setuid(65534);
        _exit(msgsnd(q, &s, 1, IPC_NOWAIT) == -1 && errno == EACCES && msgctl(q, IPC_RMID, 0) == -1 && errno == EPERM ? 0 : 1);
    }
    int st; waitpid(c, &st, 0);
    msgctl(q, IPC_RMID, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int queue_wait(void) {
    int q = msgget(IPC_PRIVATE, 0600);
    pid_t c = fork();
    if (c == 0) {
        struct m r;
        _exit(msgrcv(q, &r, sizeof r.text, 7, 0) == 3 && strcmp(r.text, "hi") == 0 ? 0 : 1);
    }
    struct timespec t = {0, 100000000}; nanosleep(&t, 0);
    struct m s = {7, "hi"};
    msgsnd(q, &s, 3, 0);
    int st; waitpid(c, &st, 0);
    msgctl(q, IPC_RMID, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int queue_removed(void) {
    int q = msgget(IPC_PRIVATE, 0600);
    pid_t c = fork();
    if (c == 0) { struct m r; _exit(msgrcv(q, &r, 8, 0, 0) == -1 && errno == EIDRM ? 0 : 1); }
    struct timespec t = {0, 100000000}; nanosleep(&t, 0);
    msgctl(q, IPC_RMID, 0);
    int st; waitpid(c, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int sem_basics(void) {
    int s = semget(IPC_PRIVATE, 3, IPC_CREAT | 0600);
    if (s < 0) return 0;
    unsigned short v[3] = {1, 2, 3}, w[3];
    int ok = semctl(s, 0, SETALL, (union semun){.array = v}) == 0 && semctl(s, 1, GETVAL) == 2;
    struct sembuf ops[2] = {{0, -1, 0}, {2, 2, 0}};
    ok = ok && semop(s, ops, 2) == 0 && semctl(s, 0, GETALL, (union semun){.array = w}) == 0 && w[0] == 0 && w[2] == 5;
    ok = ok && semctl(s, 2, GETPID) == getpid();
    struct sembuf take = {0, -1, IPC_NOWAIT};
    ok = ok && semop(s, &take, 1) == -1 && errno == EAGAIN;
    struct sembuf both[2] = {{1, -1, 0}, {0, -1, IPC_NOWAIT}};                          /* all or nothing */
    ok = ok && semop(s, both, 2) == -1 && errno == EAGAIN && semctl(s, 1, GETVAL) == 2;
    ok = ok && semctl(s, 0, SETVAL, (union semun){.val = 40000}) == -1 && errno == ERANGE;
    struct sembuf big = {5, 1, 0};
    ok = ok && semop(s, &big, 1) == -1 && errno == EFBIG;
    struct semid_ds ds;
    ok = ok && semctl(s, 0, IPC_STAT, (union semun){.buf = &ds}) == 0 && ds.sem_nsems == 3 && ds.sem_otime != 0;
    ok = ok && semctl(s, 0, IPC_RMID) == 0 && semop(s, ops, 1) == -1 && errno == EINVAL;
    return ok;
}

static int sem_wait_count(void) {
    int s = semget(IPC_PRIVATE, 1, 0600);
    pid_t c = fork();
    if (c == 0) { struct sembuf p = {0, -1, 0}; _exit(semop(s, &p, 1) == 0 ? 0 : 1); }
    int n = 0;
    for (int i = 0; i < 100 && n == 0; i++) { struct timespec t = {0, 10000000}; nanosleep(&t, 0); n = semctl(s, 0, GETNCNT); }
    struct sembuf v = {0, 1, 0};
    semop(s, &v, 1);
    int st; waitpid(c, &st, 0);
    semctl(s, 0, IPC_RMID);
    return n == 1 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int sem_signal(void) {
    int s = semget(IPC_PRIVATE, 1, 0600);
    struct sigaction sa = {.sa_handler = nop, .sa_flags = SA_RESTART};
    sigaction(SIGALRM, &sa, 0);
    alarm(1);
    struct sembuf p = {0, -1, 0};
    int r = semop(s, &p, 1), e = errno;                                                  /* never restarted */
    signal(SIGALRM, SIG_DFL);
    struct timespec to = {0, 50000000};
    int r2 = semtimedop(s, &p, 1, &to), e2 = errno;
    semctl(s, 0, IPC_RMID);
    return r == -1 && e == EINTR && r2 == -1 && e2 == EAGAIN;
}

static int sem_undo(void) {
    int s = semget(IPC_PRIVATE, 1, 0600);
    semctl(s, 0, SETVAL, (union semun){.val = 1});
    pid_t c = fork();
    if (c == 0) { struct sembuf p = {0, -1, SEM_UNDO}; semop(s, &p, 1); _exit(semctl(s, 0, GETVAL) == 0 ? 0 : 1); }
    int st; waitpid(c, &st, 0);
    int v = semctl(s, 0, GETVAL);
    semctl(s, 0, IPC_RMID);
    return v == 1 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* 512 SEM_UNDO adjustments at once in one process (eight sets of 64), all undone at exit. */
static int many_undos(void) {
    int s[8], ok = 1;
    unsigned short ones[64];
    for (int i = 0; i < 64; i++) ones[i] = 1;
    for (int j = 0; j < 8; j++) {
        s[j] = semget(IPC_PRIVATE, 64, IPC_CREAT | 0600);
        if (s[j] < 0 || semctl(s[j], 0, SETALL, (union semun){.array = ones}) != 0) ok = 0;
    }
    pid_t c = ok ? fork() : -1;
    if (c == 0) {
        struct sembuf ops[64];
        for (int i = 0; i < 64; i++) ops[i] = (struct sembuf){i, -1, SEM_UNDO};
        int bad = 0;
        for (int j = 0; j < 8; j++) bad |= semop(s[j], ops, 64) != 0;
        _exit(bad);
    }
    int st = 1;
    if (c > 0) waitpid(c, &st, 0);
    ok = ok && c > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    for (int j = 0; ok && j < 8; j++) {
        unsigned short w[64];
        ok = semctl(s[j], 0, GETALL, (union semun){.array = w}) == 0;
        for (int i = 0; ok && i < 64; i++) ok = w[i] == 1;
    }
    for (int j = 0; j < 8; j++) if (s[j] >= 0) semctl(s[j], 0, IPC_RMID);
    return ok;
}

static int shm_basics(void) {
    int id = shmget(IPC_PRIVATE, 10000, IPC_CREAT | 0600);
    if (id < 0) return 0;
    char *p = shmat(id, 0, 0);
    if (p == (void *)-1) return 0;
    int ok = p[0] == 0 && p[9999] == 0;
    p[0] = 'a'; p[9000] = 'b';
    char *q = shmat(id, 0, SHM_RDONLY);                                                  /* a second view */
    ok = ok && q != (void *)-1 && q != p && q[0] == 'a' && q[9000] == 'b';
    struct shmid_ds ds;
    ok = ok && shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_segsz == 10000 && ds.shm_nattch == 2 && ds.shm_cpid == getpid();
    pid_t c = fork();
    if (c == 0) { p[1] = 'c'; _exit(shmdt(p) == 0 ? 0 : 1); }                       /* the child shares it */
    int st; waitpid(c, &st, 0);
    ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0 && q[1] == 'c';
    ok = ok && shmdt(q) == 0 && shmdt(q) == -1 && errno == EINVAL;
    ok = ok && shmctl(id, IPC_RMID, 0) == 0 && shmctl(id, IPC_STAT, &ds) == 0 && (ds.shm_perm.mode & SHM_DEST);
    ok = ok && p[9000] == 'b' && shmdt(p) == 0 && shmctl(id, IPC_STAT, &ds) == -1 && errno == EINVAL;  /* gone with the last detach */
    return ok;
}

/* A process that attaches and detaches in a loop gets the same address space
 * back each time: 20000 attachments of 256 KiB are 5 GiB, more than a window. */
static int shm_loop(void) {
    int id = shmget(IPC_PRIVATE, 256 * 1024, IPC_CREAT | 0600);
    if (id < 0) return 0;
    int ok = 1;
    char *first = 0;
    for (int i = 0; ok && i < 20000; i++) {
        char *p = shmat(id, 0, 0);
        if (p == (void *)-1) { ok = 0; break; }
        if (i == 0) first = p;
        ok = p == first && shmdt(p) == 0;
    }
    shmctl(id, IPC_RMID, 0);
    return ok;
}

static int shm_errors(void) {
    int ok = shmget(IPC_PRIVATE, 0, 0600) == -1 && errno == EINVAL;
    int id = shmget(0x5159, 4096, IPC_CREAT | 0600);
    ok = ok && shmget(0x5159, 8192, 0) == -1 && errno == EINVAL;                         /* bigger than it is */
    ok = ok && shmat(id, (void *)0x1001, 0) == (void *)-1 && errno == EINVAL;
    ok = ok && shmat(id + 1, 0, 0) == (void *)-1 && errno == EINVAL;
    shmctl(id, IPC_RMID, 0);
    return ok;
}

static int slurp(const char *path, char *buf, int n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int got = (int)fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got < 0 ? 0 : got] = 0;
    return got > 0;
}

static int proc_files(void) {
    char buf[2048];
    int ok = slurp("/proc/sys/kernel/shmmax", buf, sizeof buf) && strtoul(buf, 0, 10) >= 4096;
    ok = ok && slurp("/proc/sys/kernel/msgmax", buf, sizeof buf) && strtoul(buf, 0, 10) >= 8192;
    ok = ok && slurp("/proc/sys/kernel/sem", buf, sizeof buf);
    int id = shmget(0x515a, 4096, IPC_CREAT | 0600);
    char want[32];
    snprintf(want, sizeof want, " %d ", id);
    ok = ok && id >= 0 && slurp("/proc/sysvipc/shm", buf, sizeof buf)
        && strncmp(buf, "       key      shmid", 20) == 0
        && strstr(buf, "     20826 ") != 0 && strstr(buf, want) != 0;            /* 0x515a = 20826 */
    shmctl(id, IPC_RMID, 0);
    ok = ok && slurp("/proc/sysvipc/msg", buf, sizeof buf) && slurp("/proc/sysvipc/sem", buf, sizeof buf);
    return ok;
}

/* Writes str to a /proc/sys file; returns 0 on success. */
static int sysctl_put(const char *path, const char *str) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int r = write(fd, str, strlen(str)) == (ssize_t)strlen(str) ? 0 : -1;
    close(fd);
    return r;
}

/* msg_next_id and shm_next_id name the id of the next new queue or
 * segment, once; MSG_COPY reads the message at an index and leaves it. */
static int next_ids_and_copy(void) {
    char b[64];
    struct m mm = {.type = 3};
    int ok = slurp("/proc/sys/kernel/msg_next_id", b, sizeof b) && strncmp(b, "-1", 2) == 0;
    ok = ok && sysctl_put("/proc/sys/kernel/msg_next_id", "7") == 0;
    int q = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    ok = ok && q == 7 && slurp("/proc/sys/kernel/msg_next_id", b, sizeof b) && strncmp(b, "-1", 2) == 0;
    /* A slot in use: the request is spent and another id comes. */
    ok = ok && sysctl_put("/proc/sys/kernel/msg_next_id", "7") == 0;
    int q2 = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    ok = ok && q2 >= 0 && q2 != 7;
    strcpy(mm.text, "one"); ok = ok && msgsnd(q, &mm, 4, 0) == 0;
    mm.type = 5; strcpy(mm.text, "two"); ok = ok && msgsnd(q, &mm, 4, 0) == 0;
    struct m r = {0};
    ok = ok && msgrcv(q, &r, sizeof r.text, 1, MSG_COPY | IPC_NOWAIT) == 4 && r.type == 5 && !strcmp(r.text, "two");
    ok = ok && msgrcv(q, &r, sizeof r.text, 2, MSG_COPY | IPC_NOWAIT) == -1 && errno == ENOMSG;
    ok = ok && msgrcv(q, &r, sizeof r.text, 0, MSG_COPY) == -1 && errno == EINVAL;
    struct msqid_ds ds;
    ok = ok && msgctl(q, IPC_STAT, &ds) == 0 && ds.msg_qnum == 2;   /* the copies left both queued */
    ok = ok && msgrcv(q, &r, sizeof r.text, 0, IPC_NOWAIT) == 4 && r.type == 3;
    ok = ok && sysctl_put("/proc/sys/kernel/shm_next_id", "9") == 0;
    int sm = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    ok = ok && sm == 9;
    ok = ok && sysctl_put("/proc/sys/kernel/msg_next_id", "-2") == -1 && sysctl_put("/proc/sys/kernel/msg_next_id", "x") == -1;
    msgctl(q, IPC_RMID, 0); msgctl(q2, IPC_RMID, 0); shmctl(sm, IPC_RMID, 0);
    return ok;
}

/* shmmax is writable: a segment larger than it is EINVAL, and the file reads back what was written. */
static int shmmax_written(void) {
    char was[32] = "", now[32] = "";
    FILE *f = fopen("/proc/sys/kernel/shmmax", "r");
    if (!f || !fgets(was, sizeof was, f)) return 0;
    fclose(f);
    f = fopen("/proc/sys/kernel/shmmax", "w");
    if (!f || fputs("8192\n", f) < 0) return 0;
    fclose(f);
    f = fopen("/proc/sys/kernel/shmmax", "r");
    if (!f || !fgets(now, sizeof now, f)) return 0;
    fclose(f);
    int big = shmget(IPC_PRIVATE, 8193, IPC_CREAT | 0600);
    int e = errno;
    int small = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
    int huge = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 04000 | 0600);
    int e2 = errno;
    if (small >= 0) shmctl(small, IPC_RMID, 0);
    if (huge >= 0) shmctl(huge, IPC_RMID, 0);
    f = fopen("/proc/sys/kernel/shmmax", "w");
    int back = f && fputs(was, f) >= 0;
    if (f) fclose(f);
    return atol(now) == 8192 && big < 0 && e == EINVAL && small >= 0 && huge < 0 && e2 == EINVAL && back;
}

/* 32 queues, 32 semaphore sets and 32 segments at once; the 33rd of each is ENOSPC. */
static int many_objects(void) {
    int q[33], m[33], g[33], ok = 1;
    for (int i = 0; i < 33; i++) q[i] = m[i] = g[i] = -1;
    for (int i = 0; i < 33; i++) {
        q[i] = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
        m[i] = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
        g[i] = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
        if (i < 32) ok = ok && q[i] >= 0 && m[i] >= 0 && g[i] >= 0;
    }
    ok = ok && q[32] < 0 && errno == ENOSPC && m[32] < 0 && g[32] < 0;
    for (int i = 0; ok && i < 32; i++) {
        struct { long t; char c; } mb = { 1, 'a' + i % 26 };
        ok = msgsnd(q[i], &mb, 1, 0) == 0 && semctl(m[i], 1, SETVAL, i + 1) == 0;
        char *p = shmat(g[i], 0, 0);
        if (p == (char *)-1) ok = 0; else { *p = 'a' + i % 26; shmdt(p); }
    }
    for (int i = 0; ok && i < 32; i++) {
        struct { long t; char c; } mb;
        ok = msgrcv(q[i], &mb, 1, 0, 0) == 1 && mb.c == 'a' + i % 26 && semctl(m[i], 1, GETVAL) == i + 1;
        char *p = shmat(g[i], 0, 0);
        if (p == (char *)-1) ok = 0; else { ok = ok && *p == 'a' + i % 26; shmdt(p); }
    }
    for (int i = 0; i < 33; i++) {
        if (q[i] >= 0) msgctl(q[i], IPC_RMID, 0);
        if (m[i] >= 0) semctl(m[i], 0, IPC_RMID);
        if (g[i] >= 0) shmctl(g[i], IPC_RMID, 0);
    }
    return ok;
}

/* One process holds 16 attachments at once; the 17th is EMFILE. */
static int many_attachments(void) {
    int id = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
    if (id < 0) return 0;
    char *v[33];
    int ok = 1, n = 0, e = 0;
    for (int i = 0; i < 33; i++) {
        v[i] = shmat(id, 0, 0);
        if (v[i] == (char *)-1) { e = errno; break; }
        n++;
        v[i][i] = 'a' + i % 26;
    }
    ok = n == 32 && e == EMFILE;
    for (int i = 0; ok && i < 32; i++) ok = v[0][i] == 'a' + i % 26 && v[31][i] == 'a' + i % 26;
    for (int i = 0; i < n; i++) ok = shmdt(v[i]) == 0 && ok;
    char *again = shmat(id, 0, 0);                                                       /* room again */
    ok = ok && again != (char *)-1 && shmdt(again) == 0;
    shmctl(id, IPC_RMID, 0);
    return ok;
}

/* A segment of 24 MiB and a page spans two page-table pages of the store; the
 * first and last bytes of each, a fork, and a second view all see the same
 * memory. One byte past 32 MiB is EINVAL. */
static int big_segment(void) {
    size_t size = 24 * 1024 * 1024 + 4096;
    int id = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    if (id < 0) return 0;
    char *p = shmat(id, 0, 0);
    if (p == (char *)-1) { shmctl(id, IPC_RMID, 0); return 0; }
    size_t at[] = { 0, 2 * 1024 * 1024 - 1, 2 * 1024 * 1024, 13 * 1024 * 1024, size - 1 };
    int ok = 1;
    for (int i = 0; i < 5; i++) p[at[i]] = 'a' + i;
    char *q = shmat(id, 0, SHM_RDONLY);
    ok = ok && q != (char *)-1;
    for (int i = 0; ok && i < 5; i++) ok = q[at[i]] == 'a' + i;
    pid_t c = fork();
    if (c == 0) { p[13 * 1024 * 1024] = 'z'; _exit(0); }
    int st = 0;
    waitpid(c, &st, 0);
    ok = ok && q[13 * 1024 * 1024] == 'z' && p[size - 1] == 'e';
    struct shmid_ds ds;
    ok = ok && shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_segsz == size;
    ok = ok && shmdt(p) == 0 && shmdt(q) == 0;
    shmctl(id, IPC_RMID, 0);
    ok = ok && shmget(IPC_PRIVATE, 32 * 1024 * 1024 + 1, IPC_CREAT | 0600) == -1 && errno == EINVAL;
    return ok;
}

/* msgmni is written: that many queues, then ENOSPC; a larger value changes nothing past 32. */
static int msgmni_written(void) {
    char buf[32] = {0};
    int fd = open("/proc/sys/kernel/msgmni", O_RDWR);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    long old = n > 0 ? atol(buf) : -1;
    if (dprintf(fd, "3\n") <= 0)
        return 0;
    close(fd);
    int ids[4], made = 0, ok = 1;
    for (int i = 0; i < 3; i++) {
        ids[i] = msgget(0x6d6e0000 + i, IPC_CREAT | IPC_EXCL | 0600);
        made += ids[i] >= 0;
    }
    int fourth = msgget(0x6d6e0003, IPC_CREAT | IPC_EXCL | 0600);
    ok = made == 3 && fourth < 0 && errno == ENOSPC;
    fd = open("/proc/sys/kernel/msgmni", O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0)
        close(fd);
    ok = ok && n > 0 && atol(buf) == 3;
    for (int i = 0; i < 3; i++)
        if (ids[i] >= 0)
            msgctl(ids[i], IPC_RMID, 0);
    fd = open("/proc/sys/kernel/msgmni", O_WRONLY);
    if (fd >= 0) {
        dprintf(fd, "%ld\n", old);
        close(fd);
    }
    return ok;
}

static int shm_remap_loop(void) {
    int id = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
    int ok = id >= 0;
    char *p = ok ? shmat(id, 0, 0) : (char *)-1;
    ok = ok && p != (char *)-1;
    /* Attaching over the same address again replaces the first attachment: one shmdt ends it. */
    for (int i = 0; ok && i < 200; i++) {
        char *q = shmat(id, p, SHM_REMAP);
        ok = q == p;
    }
    ok = ok && shmdt(p) == 0 && shmdt(p) == -1 && errno == EINVAL;
    shmctl(id, IPC_RMID, 0);
    return ok;
}

int main(void) {
    setup();
    EXPECT("shmat over its own address with SHM_REMAP replaces the attachment", shm_remap_loop());
    EXPECT("msgsnd, msgrcv and IPC_STAT", queue_basics());
    EXPECT("msgrcv picks by type", queue_types());
    EXPECT("msgget keys: IPC_EXCL, ENOENT", queue_keys());
    EXPECT("queue permissions follow the ids", queue_perms());
    EXPECT("msgrcv waits for a message", queue_wait());
    EXPECT("IPC_RMID ends a wait with EIDRM", queue_removed());
    EXPECT("semop, SETALL, GETALL, GETPID, errors", sem_basics());
    EXPECT("GETNCNT counts a waiting process", sem_wait_count());
    EXPECT("a signal ends semop; semtimedop times out", sem_signal());
    EXPECT("SEM_UNDO is undone at exit", sem_undo());
    EXPECT("shmat, a second view, fork, IPC_RMID", shm_basics());
    EXPECT("shmget and shmat errors", shm_errors());
    EXPECT("shmat and shmdt in a loop reuse the address space", shm_loop());
    EXPECT("/proc/sysvipc and /proc/sys/kernel limits", proc_files());
    EXPECT("shmmax is written; a segment past it, or with SHM_HUGETLB, is EINVAL", shmmax_written());
    EXPECT("msgmni is written: that many queues, then ENOSPC", msgmni_written());
    EXPECT("msg_next_id and shm_next_id name the next id once; MSG_COPY leaves the message", next_ids_and_copy());
    EXPECT("32 queues, semaphore sets and segments at once, the 33rd of each is ENOSPC", many_objects());
    EXPECT("32 attachments in one process, the 33rd is EMFILE", many_attachments());
    EXPECT("a 24 MiB segment is shared by a second view and a child; past 32 MiB is EINVAL", big_segment());
    EXPECT("512 SEM_UNDO adjustments in one process, all undone at exit", many_undos());
    return done();
}
