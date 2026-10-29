/*
 * Per-processor state, shared by boot.S, ap.S and the C platform code.
 *
 * Each processor owns one naturally aligned block of CORE_BLOCK bytes (the
 * bootstrap processor's is bst_core0; an application processor's is taken
 * out of the memory map when it starts, smp.c): its saved user context and bookkeeping at the bottom, then its
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
#define MAX_CPUS     256                        /* the GDT has a TSS slot for each */
#define WINDOW_SEGS  16                         /* level-3 tables in a user window (WNSEG, src/space.bastio) */
#define CORE_BLOCK   0x20000                    /* 128 KiB */
#define CORE_KSTACK  (CORE_BLOCK - 0x6000)      /* kernel stack top (92 KiB); 24 KiB trap stack above */

/* Byte offsets of the fields at the bottom of a block. */
#define CORE_CTX       0                        /* 21 words: the saved user context */
#define CORE_KRSP      168                      /* kernel %rsp while in user mode */
#define CORE_URSP      176                      /* user %rsp at `syscall` */
#define CORE_IDLE      184                      /* vector that ended bst_cpu_halt, or a parked AP's hlt */
#define CORE_TRAP_TOP  192                      /* end of the block, for `syscall` */
#define CORE_INDEX     200                      /* the processor's number: 0 for the bootstrap one */
#define CORE_MMU       208                      /* 4 words: the prelude's CR3 state */
#define CORE_UROOT     232                      /* the fourth: the user table the next bst_cpu_enter loads into CR3; bst_cpu_enter sets bit 0, which the prelude's next entry or exit of a table clears: still set means the lease never lapsed */
#define CORE_TSS       256                      /* 104-byte task state segment */
#define CORE_ALIAS     368                      /* where the block is mapped in every table (ALIAS_CORES + 128 KiB * index) */
#define CORE_UMODE     376                      /* nonzero while CR3 holds the shadow table: the next trap switches back */
#define CORE_CR3       384                      /* the shadow table bst_cpu_enter built (with its PCID), for its last instructions */
#define CORE_KCR3      392                      /* what a trap from user mode loads into CR3: the kernel's table, with its PCID and no-flush bit when PCIDs are on; 0 until this processor's first bst_cpu_enter */
#define CORE_UPCID     400                      /* the PCID of the shadow table (0 without PCIDs), or'ed into CORE_CR3 */
#define CORE_FSB       408                      /* the FS base the processor holds, with bit 63 set (0: unknown): bst_cpu_enter writes the register only when it differs */
#define CORE_LROOT     416                      /* the process table of this processor's last flushing load of the shadow table, and */
#define CORE_LGEN      424                      /* bst_pt_gen as read before it: the same table and count mean the shadow table's translations are still good */
#define CORE_VERW      432                      /* nonzero: the CPU can be affected by MDS, so the return to user mode runs `verw` */
#define CORE_IST_TOP   0x2000                   /* top of the 4 KiB IST1 stack, the block's second page */
#define CORE_GUARD     0x2000                   /* the unmapped page below the kernel stack */

#define TSS_SEL(i)     (0x28 + 16 * (i))        /* GDT selector of processor i's TSS */
#define LAPIC_TIMER_VECTOR 48                   /* preemption on application processors */

/*
 * The entry area. User code runs on a table of its own: the process's window
 * at virtual address 0 (a program linked at 0x400000 finds itself there), the
 * kernel's memory not mapped at all, so the identity map, the kernel image and
 * the page pool never collide with what a program asks for. A processor in
 * user mode can still reach the few things the hardware touches by itself at
 * a trap, and the code that switches back, because every table (the kernel's
 * and each processor's shadow table) maps the same entry area in the last
 * top-level slot, at ALIAS_BASE:
 *
 *   ALIAS_BASE            the trap stubs and the code around them (.tramp.text)
 *   ALIAS_GDT, ALIAS_IDT  the GDT (two pages) and the IDT, the same memory as
 *                         the kernel's own copies
 *   ALIAS_CORES + 128 KiB * i   processor i's block, minus its guard page:
 *                         trap stack, IST1 stack, TSS and saved context
 *
 * The processor's TSS, IST1 and trap-stack pointers, KERNEL_GS_BASE, the GDT
 * and IDT registers hold these addresses. The kernel stack is not mapped
 * here: a trap switches to the kernel's table before it leaves the area.
 */
#define ALIAS_BASE     0xFFFFFF8000000000
#define ALIAS_GDT      (ALIAS_BASE + 0x100000)
#define ALIAS_IDT      (ALIAS_BASE + 0x102000)
#define ALIAS_CORES    (ALIAS_BASE + 0x200000)
