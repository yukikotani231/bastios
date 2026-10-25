/* What container runtimes ask of the kernel beyond namespaces: close_range,
 * the child-subreaper and keep-capabilities flags, the ambient capability
 * set, and quiet refusals of the keyring and the new mount API. Prints
 * "mcont: ok", or what failed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <stdint.h>
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

#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#define C3_INTO_CGROUP 0x200000000ULL
struct c3args {
	uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};

static volatile int ns_stop, ns_bad;
static void *ns_worker(void *a) {
	while (!ns_stop) {
		int fd = open("/tmp/mcont.file", O_RDONLY);
		if (fd < 0) ns_bad++; else close(fd);
		usleep(500);
	}
	return 0;
}
static void *ns_chroot(void *a) {
	/* what runc's helper thread does: its own mount namespace and root */
	if (unshare(CLONE_FS) != 0 || unshare(CLONE_NEWNS) != 0 || chroot("/tmp/mcont.rr") != 0) ns_bad += 1000;
	usleep(60000);
	return 0;
}

static int fail(const char *what) { printf("mcont: %s\n", what); return 1; }

static volatile int tk_code, tk_pid;
static void on_tk(int s, siginfo_t *i, void *u) { tk_code = i->si_code; tk_pid = i->si_pid; }
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static volatile int tk_tid;
static void *sleeper(void *a) {
	tk_tid = syscall(SYS_gettid);
	pthread_mutex_lock(&mu);
	pthread_cond_wait(&cv, &mu);
	pthread_mutex_unlock(&mu);
	return 0;
}

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

	/* the session keyring exists (keyctl GET_KEYRING_ID, KEY_SPEC_SESSION_KEYRING) */
	if (syscall(250, 0, -3, 0, 0, 0) <= 0) return fail("keyctl");

	/* quiet ENOSYS */
	if (syscall(430, "ext4", 0) != -1 || errno != ENOSYS) return fail("fsopen");
	if (syscall(432, -1, 0, 0) != -1 || errno != ENOSYS) return fail("fsmount");

	/* A named pipe opened O_PATH holds its name, not an end: reopening it through
	 * /proc/self/fd/N for writing waits for a reader like any open (runc's exec.fifo). */
	unlink("/tmp/mcont.fifo");
	if (mkfifo("/tmp/mcont.fifo", 0600) != 0) return fail("mkfifo");
	int pf = open("/tmp/mcont.fifo", O_PATH | O_CLOEXEC);
	if (pf < 0) return fail("O_PATH fifo");
	char lnk[64];
	snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", pf);
	if (open(lnk, O_WRONLY | O_NONBLOCK) != -1 || errno != ENXIO) return fail("fifo write without reader");
	int rd = open("/tmp/mcont.fifo", O_RDONLY | O_NONBLOCK);
	int wr = open(lnk, O_WRONLY);
	if (rd < 0 || wr < 0 || write(wr, "0", 1) != 1) return fail("fifo reopen");
	char fb;
	if (read(rd, &fb, 1) != 1 || fb != '0') return fail("fifo read");
	close(rd); close(wr); close(pf);
	unlink("/tmp/mcont.fifo");

	/* tkill and tgkill signals say SI_TKILL from this process (glibc's setxid handler checks). */
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_tk;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &sa, 0);
	pthread_t th;
	if (pthread_create(&th, 0, sleeper, 0) != 0) return fail("thread");
	while (!tk_tid) usleep(1000);
	usleep(20000);
	if (syscall(SYS_tgkill, getpid(), tk_tid, SIGUSR1) != 0) return fail("tgkill");
	for (int w = 0; w < 500 && !tk_code; w++) usleep(2000);
	if (tk_code != SI_TKILL || tk_pid != getpid()) return fail("SI_TKILL");
	pthread_mutex_lock(&mu);
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
	pthread_join(th, 0);

	/* A sequenced-packet receive with MSG_PEEK | MSG_TRUNC reports the datagram's whole size
	 * (runc's sync socket sizes its buffer that way). */
	int sp[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp) != 0) return fail("seqpacket");
	if (write(sp[0], "0123456789", 10) != 10) return fail("seqpacket write");
	char small[4];
	if (recv(sp[1], small, sizeof small, MSG_PEEK | MSG_TRUNC) != 10) return fail("MSG_TRUNC peek size");
	if (recv(sp[1], small, sizeof small, 0) != 4) return fail("seqpacket truncated read");
	close(sp[0]); close(sp[1]);

	/* bpf: only the cgroup device filter, accepted and attached to nothing. */
	long bpf_r = syscall(321, 99, "", 0);
	if (bpf_r != -1 || errno != EINVAL) return fail("bpf bad command");

	/* A thread that makes its own mount namespace and root, and ends, leaves the others' view
	 * alone: the row of a thread that ended is not handed to the next thread. */
	close(open("/tmp/mcont.file", O_CREAT | O_WRONLY, 0644));
	mkdir("/tmp/mcont.rr", 0755);
	pthread_t nw[2], nt;
	for (int i = 0; i < 2; i++) pthread_create(&nw[i], 0, ns_worker, 0);
	usleep(30000);
	for (int round = 0; round < 3; round++) {
		pthread_create(&nt, 0, ns_chroot, 0);
		pthread_join(nt, 0);
		usleep(30000);
	}
	ns_stop = 1;
	for (int i = 0; i < 2; i++) pthread_join(nw[i], 0);
	unlink("/tmp/mcont.file");
	rmdir("/tmp/mcont.rr");
	if (ns_bad) return fail("a thread's own root leaked to its group");

	/* clone3: a plain fork, then CLONE_INTO_CGROUP puts the child in the group the descriptor names. */
	struct c3args ca;
	memset(&ca, 0, sizeof ca);
	ca.exit_signal = SIGCHLD;
	long c3 = syscall(SYS_clone3, &ca, sizeof ca);
	if (c3 == 0) _exit(7);
	if (c3 < 0) return fail("clone3 fork");
	int st3;
	if (waitpid(c3, &st3, 0) != c3 || !WIFEXITED(st3) || WEXITSTATUS(st3) != 7) return fail("clone3 child");
	if (syscall(SYS_clone3, &ca, 8) != -1 || errno != EINVAL) return fail("clone3 short size");
	if (syscall(SYS_clone3, (void *)8, sizeof ca) != -1 || errno != EFAULT) return fail("clone3 bad pointer");
	ca.exit_signal = 0x100;
	if (syscall(SYS_clone3, &ca, sizeof ca) != -1 || errno != EINVAL) return fail("clone3 bad signal");
	if (mkdir("/sys/fs/cgroup/c3", 0755) != 0) return fail("mkdir cgroup");
	int cgd = open("/sys/fs/cgroup/c3", O_RDONLY | O_DIRECTORY);
	if (cgd < 0) return fail("open cgroup");
	memset(&ca, 0, sizeof ca);
	ca.flags = C3_INTO_CGROUP;
	ca.exit_signal = SIGCHLD;
	int nul = open("/dev/null", O_RDONLY);
	ca.cgroup = nul;                                 /* /dev/null is not a cgroup */
	if (syscall(SYS_clone3, &ca, sizeof ca) != -1 || errno != EBADF) return fail("clone3 non-cgroup fd");
	close(nul);
	ca.cgroup = cgd;
	c3 = syscall(SYS_clone3, &ca, sizeof ca);
	if (c3 == 0) {
		char cg[64] = {0};
		int f = open("/proc/self/cgroup", O_RDONLY);
		int n = f < 0 ? -1 : read(f, cg, sizeof cg - 1);
		_exit(n >= 6 && strncmp(cg, "0::/c3", 6) == 0 ? 0 : 1);
	}
	if (c3 < 0) return fail("clone3 into cgroup");
	if (waitpid(c3, &st3, 0) != c3 || !WIFEXITED(st3) || WEXITSTATUS(st3) != 0) return fail("child not in the cgroup");
	/* A group at its pids.max takes no child: the limit is checked before it is made. */
	int sc = open("/sys/fs/cgroup/cgroup.subtree_control", O_WRONLY);
	if (sc < 0 || write(sc, "+pids", 5) != 5) return fail("enable pids");
	close(sc);
	int pm = open("/sys/fs/cgroup/c3/pids.max", O_WRONLY);
	if (pm < 0 || write(pm, "0", 1) != 1) return fail("pids.max");
	close(pm);
	c3 = syscall(SYS_clone3, &ca, sizeof ca);
	if (c3 == 0) _exit(0);
	if (c3 != -1 || errno != EAGAIN) {
		if (c3 > 0) waitpid(c3, &st3, 0);
		return fail("clone3 past pids.max");
	}
	/* A thread through clone3 shares the address space; INTO_CGROUP is refused for it. */
	ca.flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | C3_INTO_CGROUP;
	ca.exit_signal = 0;
	if (syscall(SYS_clone3, &ca, sizeof ca) != -1 || errno != EINVAL) return fail("clone3 thread into cgroup");
	close(cgd);
	if (rmdir("/sys/fs/cgroup/c3") != 0) return fail("rmdir cgroup");
	pm = open("/sys/fs/cgroup/cgroup.subtree_control", O_WRONLY);
	if (pm >= 0) { write(pm, "-pids", 5); close(pm); }

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
	/* getppid is answered without a trip into the kernel's lock while the parent stays; an exit
	   that gives a process a new parent must still show at once in a process that keeps asking. */
	int fds2[2];
	if (pipe(fds2) != 0) return fail("pipe");
	kid = fork();
	if (kid == 0) {
		pid_t mid = getpid();
		pid_t gk = fork();
		if (gk == 0) {
			pid_t first = getppid();
			write(fds2[1], "x", 1);         /* the middle process may end now */
			for (long i = 0; i < 50000000; i++)
				if (getppid() != first) _exit(first == mid ? 0 : 3);
			_exit(4);                        /* the parent id never changed */
		}
		char ch;
		read(fds2[0], &ch, 1);
		_exit(gk > 0 ? 0 : 1);
	}
	if (waitpid(kid, &st, 0) != kid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return fail("middle process 2");
	got = wait(&st);
	if (got <= 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return fail("getppid after the parent's exit");
	printf("mcont: ok\n");
	return 0;
}
