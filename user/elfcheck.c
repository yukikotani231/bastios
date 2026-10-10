/*
 * A freestanding C program linked as a static Linux ELF executable, twice:
 * at a fixed address (elfexec, ET_EXEC) and as a static PIE (elfpie,
 * ET_DYN). It checks what the ELF loader promises: initialized data and a
 * zeroed .bss, and the initial stack of the Linux ABI (argc, argv, envp and
 * the auxiliary vector). It prints "<name>: ok" and exits 0, or exits with
 * a bit set for each check that failed. It talks to the kernel only with
 * Linux system calls (the `syscall` instruction).
 */
typedef unsigned long u64;

static volatile u64 data_word = 0x1122334455667788;  /* .data */
static volatile char bss[8192];                      /* .bss, spans pages */

static u64 sys(u64 n, u64 a, u64 b, u64 c) {
    u64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static void say(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    sys(1, 1, (u64)s, n);                    /* write(1, s, n) */
}

extern char _start[];

struct phdr { unsigned p_type, p_flags; u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; };

u64 elf_main(u64 *sp) {
    u64 bad = 0;
    if (!sp) return 1;                       /* rsp was not 16-byte aligned */
    if (data_word != 0x1122334455667788) bad |= 2;
    for (u64 i = 0; i < sizeof bss; i++)
        if (bss[i]) { bad |= 4; break; }
    bss[sizeof bss - 1] = 1;                 /* .bss is writable */
    u64 argc = sp[0];
    char **argv = (char **)(sp + 1);
    if (argc < 1 || argc > 32 || argv[argc]) bad |= 8;
    for (u64 i = 0; i < argc && i < 32; i++)
        if (!argv[i]) bad |= 8;
    /* The environment: empty when started from the BastiOS shell, NAME=VALUE
       strings when started by execve. */
    char **envp = argv + argc + 1;
    u64 envc = 0;
    for (; envp[envc]; envc++) {
        const char *e = envp[envc];
        while (*e && *e != '=') e++;
        if (*e != '=' || envc >= 16) { bad |= 16; break; }
    }
    u64 *aux = (u64 *)(envp + envc + 1);
    u64 seen = 0, phdr = 0, phnum = 0, phent = 0;
    for (; aux[0]; aux += 2) {
        u64 k = aux[0], v = aux[1];
        if (k == 6 && v == 4096) seen |= 1;              /* AT_PAGESZ */
        if (k == 9 && v == (u64)_start) seen |= 2;       /* AT_ENTRY */
        if (k == 3) phdr = v;                            /* AT_PHDR */
        if (k == 4) phent = v;                           /* AT_PHENT */
        if (k == 5) phnum = v;                           /* AT_PHNUM */
        if (k == 25 && v) {                              /* AT_RANDOM: 16 readable bytes */
            u64 sum = 0;
            for (int i = 0; i < 16; i++) sum += ((unsigned char *)v)[i];
            if (sum) seen |= 4;
        }
    }
    if (seen != 7) bad |= 32;
    /* AT_PHDR must point at the loaded program headers: one of them is the
       PT_LOAD holding the code we are running. */
    int found = 0;
    if (phdr && phent == sizeof(struct phdr)) {
        struct phdr *ph = (struct phdr *)phdr;
        for (u64 i = 0; i < phnum; i++)
            if (ph[i].p_type == 1 && (ph[i].p_flags & 1)) found = 1;
    }
    if (!found) bad |= 64;
    /* Linux system calls: an unknown one fails with -ENOSYS, a bad fd with
       -EBADF, getpid is positive, and a write reports its count. */
    if (sys(9999, 0, 0, 0) != (u64)-38) bad |= 128;
    if (sys(1, 7, (u64)"x", 1) != (u64)-9) bad |= 128;
    if ((long)sys(39, 0, 0, 0) <= 0) bad |= 128;
    if (sys(1, 1, (u64)"", 0) != 0) bad |= 128;
    if (bad) return bad;
    say(argv[0]);
    say(": ok\r\n");
    return 0;
}
