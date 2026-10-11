/*
 * Per-processor state, shared by boot.S, ap.S and the C platform code.
 *
 * Each processor owns one naturally aligned block of CORE_BLOCK bytes in
 * bst_cores: its saved user context and bookkeeping at the bottom, then its
 * kernel stack, then the trap stack that user-mode traps land on (TSS.rsp0
 * is the end of the block). A 4 KiB stack just above the bookkeeping is
 * IST1, for #DB, NMI, #DF and #MC, which can arrive while %rsp is not yet
 * a kernel stack (see platform.c). Code running on either stack finds its block by
 * rounding %rsp down, so no processor ever needs a global variable to know
 * which one it is. The `syscall` entry, which arrives on the user's stack,
 * finds it through KERNEL_GS_BASE and `swapgs`.
 */
#define MAX_CPUS     16
#define CORE_BLOCK   0x20000                    /* 128 KiB */
#define CORE_KSTACK  (CORE_BLOCK - 0x8000)      /* kernel stack top; 32 KiB trap stack above */

/* Byte offsets of the fields at the bottom of a block. */
#define CORE_CTX       0                        /* 21 words: the saved user context */
#define CORE_KRSP      168                      /* kernel %rsp while in user mode */
#define CORE_URSP      176                      /* user %rsp at `syscall` */
#define CORE_IDLE      184                      /* vector that ended bst_cpu_wait */
#define CORE_TRAP_TOP  192                      /* end of the block, for `syscall` */
#define CORE_MMU       200                      /* 3 words: the prelude's lazy CR3 state */
#define CORE_TSS       256                      /* 104-byte task state segment */
#define CORE_IST_TOP   0x1400                   /* top of the 4 KiB IST1 stack above the TSS */

#define TSS_SEL(i)     (0x28 + 16 * (i))        /* GDT selector of processor i's TSS */
#define LAPIC_TIMER_VECTOR 48                   /* preemption on application processors */
