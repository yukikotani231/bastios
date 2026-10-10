/*
 * Per-processor state, shared by boot.S, ap.S and the C platform code.
 *
 * Each processor owns one naturally aligned block of CORE_BLOCK bytes in
 * bst_cores: its saved user context and bookkeeping at the bottom, then its
 * kernel stack, then the trap stack that user-mode traps land on (TSS.rsp0
 * is the end of the block). The 4 KiB page above the bookkeeping is the
 * IST1 stack, for #DB, NMI, #DF and #MC, which can arrive while %rsp is not
 * yet a kernel stack (see platform.c). The page above that is a guard,
 * left unmapped (smp.c): nothing bounds how deep Bastio code recurses, and
 * a kernel stack that ran past its bottom would otherwise write over the
 * IST1 stack, the TSS and the saved user context without a sound. It
 * faults instead, and the fault, finding no stack, becomes a double fault
 * on IST1 that halts the processor. Code running on either stack finds its block by
 * rounding %rsp down, so no processor ever needs a global variable to know
 * which one it is. The `syscall` entry, which arrives on the user's stack,
 * finds it through KERNEL_GS_BASE and `swapgs`.
 */
#define MAX_CPUS     16
#define CORE_BLOCK   0x20000                    /* 128 KiB */
#define CORE_KSTACK  (CORE_BLOCK - 0x6000)      /* kernel stack top (92 KiB); 24 KiB trap stack above */

/* Byte offsets of the fields at the bottom of a block. */
#define CORE_CTX       0                        /* 21 words: the saved user context */
#define CORE_KRSP      168                      /* kernel %rsp while in user mode */
#define CORE_URSP      176                      /* user %rsp at `syscall` */
#define CORE_IDLE      184                      /* vector that ended bst_cpu_wait */
#define CORE_TRAP_TOP  192                      /* end of the block, for `syscall` */
#define CORE_TSS       256                      /* 104-byte task state segment */
#define CORE_IST_TOP   0x2000                   /* top of the 4 KiB IST1 stack, the block's second page */
#define CORE_GUARD     0x2000                   /* the unmapped page below the kernel stack */

#define TSS_SEL(i)     (0x28 + 16 * (i))        /* GDT selector of processor i's TSS */
#define LAPIC_TIMER_VECTOR 48                   /* preemption on application processors */
