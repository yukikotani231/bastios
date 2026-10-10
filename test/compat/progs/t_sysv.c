/* System V IPC: message queues and semaphore sets, their keys and
 * permissions, IPC_STAT/IPC_SET/IPC_RMID, blocking waits that a signal or
 * a removal ends, and SEM_UNDO at exit. */
#include "compat.h"
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <time.h>

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

int main(void) {
    setup();
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
    return done();
}
