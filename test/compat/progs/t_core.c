/* Core files: a process that dies of SIGSEGV with RLIMIT_CORE above 0 leaves
 * a file called "core" in its working directory, an ELF file of type ET_CORE
 * that holds its registers (NT_PRSTATUS) and the pages it had touched. With
 * RLIMIT_CORE at 0, which is the default, it leaves none. The checks look at
 * the file the way a debugger does: the note for the signal, the program
 * counter inside a loadable segment, and a variable of the dead process at
 * its address in a segment. */
#include "compat.h"
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

static volatile uint64_t marker = 0x5a5a1234deadbeefULL;

/* Forks a child that goes into DIR with RLIMIT_CORE at lim and writes
 * through a null pointer; returns its wait status. */
static int crash(const char *dir, rlim_t lim, pid_t *pid) {
    mkdir(dir, 0755);
    pid_t c = fork();
    if (c == 0) {
        struct rlimit rl = { lim, lim };
        setrlimit(RLIMIT_CORE, &rl);
        if (chdir(dir) != 0)
            _exit(1);
        *(volatile int *)0 = 1;
        _exit(2);
    }
    int st = 0;
    waitpid(c, &st, 0);
    *pid = c;
    return st;
}

#ifdef __x86_64__
#define CORE_MACHINE EM_X86_64
#define CORE_NREGS 27          /* struct user_regs_struct, in 8-byte words */
#define CORE_PC_WORD 16        /* rip */
#else
#define CORE_MACHINE EM_AARCH64
#define CORE_NREGS 34          /* struct user_pt_regs: x0-x30, sp, pc, pstate */
#define CORE_PC_WORD 32        /* pc */
#endif

/* The core file's view of it: 1 if every check on its contents held. */
static int inspect(const char *path, pid_t pid, const char **why) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { *why = "open"; return 0; }
    Elf64_Ehdr eh;
    if (read(fd, &eh, sizeof eh) != sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
        *why = "no ELF header"; return 0;
    }
    if (eh.e_type != ET_CORE || eh.e_machine != CORE_MACHINE) { *why = "type or machine"; return 0; }
    if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum < 2 || eh.e_phnum > 100) {
        *why = "program headers"; return 0;
    }
    Elf64_Phdr ph[100];
    if (pread(fd, ph, eh.e_phnum * sizeof(Elf64_Phdr), eh.e_phoff) != eh.e_phnum * (ssize_t)sizeof(Elf64_Phdr)) {
        *why = "read the program headers"; return 0;
    }
    int saw_sig = 0, saw_pc = 0, saw_var = 0;
    uint64_t rip = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_NOTE)
            continue;
        char buf[2048];
        size_t n = ph[i].p_filesz < sizeof buf ? ph[i].p_filesz : sizeof buf;
        if (pread(fd, buf, n, ph[i].p_offset) != (ssize_t)n) { *why = "read the notes"; return 0; }
        size_t at = 0;
        while (at + sizeof(Elf64_Nhdr) <= n) {
            Elf64_Nhdr *nh = (Elf64_Nhdr *)(buf + at);
            size_t d = at + sizeof *nh + ((nh->n_namesz + 3) & ~3u);
            if (d + nh->n_descsz > n)
                break;
            if (nh->n_type == NT_PRSTATUS && nh->n_descsz >= 112 + CORE_NREGS * 8) {
                int32_t signo, cursig, ppid;
                int32_t id;
                memcpy(&signo, buf + d, 4);
                cursig = *(uint16_t *)(buf + d + 12);
                memcpy(&id, buf + d + 32, 4);
                memcpy(&ppid, buf + d + 36, 4);
                memcpy(&rip, buf + d + 112 + CORE_PC_WORD * 8, 8);
                if (signo == SIGSEGV && cursig == SIGSEGV && id == pid)
                    saw_sig = 1;
            }
            at = d + ((nh->n_descsz + 3) & ~3u);
        }
    }
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (rip >= ph[i].p_vaddr && rip < ph[i].p_vaddr + ph[i].p_filesz && (ph[i].p_flags & PF_R))
            saw_pc = 1;
        uint64_t a = (uint64_t)(uintptr_t)&marker;
        if (a >= ph[i].p_vaddr && a + 8 <= ph[i].p_vaddr + ph[i].p_filesz) {
            uint64_t v = 0;
            if (pread(fd, &v, 8, ph[i].p_offset + (a - ph[i].p_vaddr)) == 8 && v == marker)
                saw_var = 1;
        }
    }
    close(fd);
    if (!saw_sig) { *why = "no NT_PRSTATUS for the signal and the process"; return 0; }
    if (!saw_pc) { *why = "program counter is in no segment"; return 0; }
    if (!saw_var) { *why = "the variable is not in the file"; return 0; }
    return 1;
}

/* Linux hands a core to whatever core_pattern names; only a plain "core"
 * leaves a file to look at. */
static int plain_pattern(void) {
    char p[64] = "";
    int fd = open("/proc/sys/kernel/core_pattern", O_RDONLY);
    if (fd < 0)
        return 1;
    ssize_t n = read(fd, p, sizeof p - 1);
    close(fd);
    return n <= 0 || strncmp(p, "core", 4) == 0;
}

int main(void) {
    pid_t pid;
    int st = crash("co_a", 1 << 26, &pid);
    EXPECT("a null write ends the child with SIGSEGV", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    struct stat sb;
    if (plain_pattern()) {
        EXPECT("RLIMIT_CORE above 0 leaves a core file, and the wait status says so", stat("co_a/core", &sb) == 0 && sb.st_size > 4096 && WCOREDUMP(st));
        const char *why = "";
        int ok = inspect("co_a/core", pid, &why);
        EXPECT("the core file names the signal, the program counter and the variable", ok || (fprintf(stderr, "why: %s\n", why), 0));
        st = crash("co_b", 0, &pid);
        EXPECT("RLIMIT_CORE 0 leaves no core file, and the status has no core flag", WIFSIGNALED(st) && !WCOREDUMP(st) && stat("co_b/core", &sb) != 0);
    }
    return done();
}
