/*
 * A freestanding C program linked as a static Linux ELF executable, twice:
 * at a fixed address (elfexec, ET_EXEC) and as a static PIE (elfpie,
 * ET_DYN). It checks what the ELF loader promises: initialized data and a
 * zeroed .bss, and the initial stack of the Linux ABI (argc, argv, envp and
 * the auxiliary vector). It prints "<name>: ok" and exits 0, or exits with
 * a bit set for each check that failed. Output goes through the BastiOS
 * system call ABI, as the kernel has no Linux system calls yet.
 */
typedef unsigned long u64;

static volatile u64 data_word = 0x1122334455667788;  /* .data */
static volatile char bss[8192];                      /* .bss, spans pages */

static u64 sys(u64 n, u64 a, u64 b) {
    u64 r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "memory");
    return r;
}

static void say(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    sys(1, (u64)s, n);
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
    if (argc != 1 || !argv[0] || argv[1]) bad |= 8;
    char **envp = argv + argc + 1;
    if (envp[0]) bad |= 16;
    u64 *aux = (u64 *)(envp + 1);
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
    if (bad) return bad;
    say(argv[0]);
    say(": ok\r\n");
    return 0;
}
