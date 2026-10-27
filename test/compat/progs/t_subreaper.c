/* PR_SET_CHILD_SUBREAPER belongs to the process, not to the thread that set
 * it: containerd's shim sets it on one thread while runc's children are
 * forked from another, and an orphan must still reach the shim. */
#define _GNU_SOURCE
#include "compat.h"
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/wait.h>

static int go[2];     /* the grandchild waits on this pipe */
static int up[2];     /* the grandchild says it is running */
static pid_t child;

/* A thread other than the one that set the flag forks a child, which forks a
 * grandchild and exits at once. */
static void *forker(void *arg) {
    (void)arg;
    int g = -1;
    child = fork();
    if (child == 0) {
        pid_t gc = fork();
        if (gc == 0) {
            char c;
            write(up[1], "u", 1);
            read(go[0], &c, 1);
            _exit(7);
        }
        g = gc;
        _exit(0);
    }
    (void)g;
    return 0;
}

int main(void) {
    setup();
    int st;
    EXPECT("pipes", pipe(go) == 0 && pipe(up) == 0);
    EXPECT("set the flag in the main thread", prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    pthread_t t;
    EXPECT("a second thread", pthread_create(&t, 0, forker, 0) == 0);
    pthread_join(t, 0);
    EXPECT("the child was forked", child > 0);
    {
        char c;
        EXPECT("the grandchild runs", read(up[0], &c, 1) == 1);
    }
    EXPECT("the child ended", waitpid(child, &st, 0) == child && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    {
        int v = -1;
        EXPECT("the flag reads back in another thread", prctl(PR_GET_CHILD_SUBREAPER, &v) == 0 && v == 1);
    }
    /* The grandchild is orphaned; it must now be a child of this process. */
    EXPECT("the grandchild is still running", waitpid(-1, &st, WNOHANG) == 0);
    EXPECT("release the grandchild", write(go[1], "g", 1) == 1);
    {
        pid_t w = waitpid(-1, &st, 0);
        EXPECT("the orphan is reparented to the subreaper and its exit is seen",
               w > 0 && w != child && WIFEXITED(st) && WEXITSTATUS(st) == 7);
    }
    return done();
}
