/* What container runtimes ask of the kernel beyond namespaces: close_range,
 * the child-subreaper and keep-capabilities flags, the ambient capability
 * set, and quiet refusals of the keyring and the new mount API. Prints
 * "mcont: ok", or what failed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_CHILD_SUBREAPER 37
#endif
#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#endif

static int fail(const char *what) { printf("mcont: %s\n", what); return 1; }

int main(void) {
	/* close_range: closes a span, marks a span close-on-exec, refuses bad arguments. */
	int a = open("/dev/null", O_RDONLY), b = open("/dev/null", O_RDONLY), c = open("/dev/null", O_RDONLY);
	if (a < 0 || b != a + 1 || c != b + 1) return fail("open");
	if (syscall(SYS_close_range, b, c, 4) != 0) return fail("close_range cloexec");
	if (!(fcntl(b, F_GETFD) & FD_CLOEXEC) || !(fcntl(c, F_GETFD) & FD_CLOEXEC) || (fcntl(a, F_GETFD) & FD_CLOEXEC))
		return fail("cloexec span");
	if (syscall(SYS_close_range, b, c, 0) != 0) return fail("close_range");
	if (fcntl(b, F_GETFD) != -1 || fcntl(c, F_GETFD) != -1 || fcntl(a, F_GETFD) == -1) return fail("closed span");
	if (syscall(SYS_close_range, 5, 3, 0) != -1 || errno != EINVAL) return fail("first > last");
	if (syscall(SYS_close_range, 3, 4, 0x100) != -1 || errno != EINVAL) return fail("bad flags");
	if (syscall(SYS_close_range, a, ~0U, 0) != 0 || fcntl(a, F_GETFD) != -1) return fail("to the end");

	/* keep-capabilities */
	if (prctl(PR_GET_KEEPCAPS) != 0) return fail("keepcaps start");
	if (prctl(PR_SET_KEEPCAPS, 1) != 0 || prctl(PR_GET_KEEPCAPS) != 1) return fail("keepcaps set");
	if (prctl(PR_SET_KEEPCAPS, 2) != -1 || errno != EINVAL) return fail("keepcaps 2");
	prctl(PR_SET_KEEPCAPS, 0);

	/* ambient set: raise needs the capability inheritable, lower and clear-all work */
	if (prctl(PR_CAP_AMBIENT, 1, 12, 0, 0) != 0) return fail("ambient start");
	if (prctl(PR_CAP_AMBIENT, 1, 99, 0, 0) != -1 || errno != EINVAL) return fail("ambient bad cap");
	if (prctl(PR_CAP_AMBIENT, 9, 12, 0, 0) != -1 || errno != EINVAL) return fail("ambient bad op");
	if (prctl(PR_CAP_AMBIENT, 2, 12, 0, 0) != -1 || errno != EPERM) return fail("ambient raise without inheritable");
	if (prctl(PR_CAP_AMBIENT, 4, 0, 0, 0) != 0) return fail("ambient clear all");

	/* quiet ENOSYS */
	if (syscall(250, 0, 0, 0, 0, 0) != -1 || errno != ENOSYS) return fail("keyctl");
	if (syscall(430, "ext4", 0) != -1 || errno != ENOSYS) return fail("fsopen");
	if (syscall(432, -1, 0, 0) != -1 || errno != ENOSYS) return fail("fsmount");

	/* A subreaper adopts its descendants' orphans: the grandchild's parent becomes this process. */
	int fds[2];
	if (pipe(fds) != 0) return fail("pipe");
	if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) return fail("set subreaper");
	int g = 0;
	if (prctl(PR_GET_CHILD_SUBREAPER, &g) != 0 || g != 1) return fail("get subreaper");
	pid_t kid = fork();
	if (kid == 0) {
		pid_t gk = fork();
		if (gk == 0) {
			char ch;
			read(fds[0], &ch, 1);            /* wait until the middle process is gone */
			_exit(getppid() == 0 ? 2 : 0);   /* parent ids differ by namespace: only 0 is wrong */
		}
		_exit(gk > 0 ? 0 : 1);               /* the middle process ends, orphaning gk */
	}
	int st;
	if (waitpid(kid, &st, 0) != kid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return fail("middle process");
	write(fds[1], "x", 1);
	pid_t got = wait(&st);                   /* the orphan reaches us, not init */
	if (got <= 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return fail("adopted orphan");
	printf("mcont: ok\n");
	return 0;
}
