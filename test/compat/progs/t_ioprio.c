/* I/O priorities: ioprio_set and ioprio_get, with Linux 6.1's checks (as
 * LTP's ioprio tests and ionice use them). */
#include "compat.h"
#include <sys/syscall.h>
#include <sys/wait.h>

#define V(class, data) (((class) << 13) | (data))
#define WHO_PROCESS 1
#define WHO_PGRP 2
#define WHO_USER 3

static long get(int which, int who) { return syscall(SYS_ioprio_get, which, who); }
static long set(int which, int who, int v) { return syscall(SYS_ioprio_set, which, who, v); }

int main(void) {
    setup();
    long v0 = get(WHO_PROCESS, 0);
    EXPECT("ioprio_get of a new process is in range", v0 >= 0 && (v0 >> 13) <= 3);
    EXPECT("best effort, level 3", set(WHO_PROCESS, 0, V(2, 3)) == 0 && get(WHO_PROCESS, 0) == V(2, 3));
    EXPECT("idle", set(WHO_PROCESS, 0, V(3, 0)) == 0 && get(WHO_PROCESS, 0) == V(3, 0));
    EXPECT("real time as root", set(WHO_PROCESS, getpid(), V(1, 2)) == 0 && get(WHO_PROCESS, getpid()) == V(1, 2));
    EXPECT("none", set(WHO_PROCESS, 0, V(0, 0)) == 0 && get(WHO_PROCESS, 0) == 0);
    set(WHO_PROCESS, 0, V(2, 4));
    EXPECT("none with a level is EINVAL", set(WHO_PROCESS, 0, V(0, 4)) == -1 && errno == EINVAL);
    EXPECT("an unknown class is EINVAL", set(WHO_PROCESS, 0, V(4, 0)) == -1 && errno == EINVAL);
    EXPECT("an unknown which is EINVAL", set(0, 0, V(2, 1)) == -1 && errno == EINVAL &&
           get(4, 0) == -1 && errno == EINVAL);
    EXPECT("a failed set changes nothing", get(WHO_PROCESS, 0) == V(2, 4));
    EXPECT("no such process is ESRCH", set(WHO_PROCESS, 30000, V(2, 1)) == -1 && errno == ESRCH &&
           get(WHO_PROCESS, 30000) == -1 && errno == ESRCH);
    EXPECT("the process group", set(WHO_PGRP, 0, V(2, 5)) == 0 && get(WHO_PROCESS, 0) == V(2, 5) &&
           get(WHO_PGRP, 0) >= 0);
    EXPECT("the user", set(WHO_USER, getuid(), V(2, 6)) == 0 && get(WHO_PROCESS, 0) == V(2, 6) &&
           get(WHO_USER, getuid()) >= 0);
    set(WHO_PROCESS, 0, V(3, 0));
    pid_t c = fork();
    if (c == 0) _exit(get(WHO_PROCESS, 0) == V(3, 0) ? 0 : 1);
    int st = 0;
    waitpid(c, &st, 0);
    EXPECT("a child inherits it", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    set(WHO_PROCESS, 0, V(0, 0));
    return done();
}
