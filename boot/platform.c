/*
 * Platform setup that must happen before the Bastio kernel runs: the
 * interrupt table and the system call MSRs (each processor's task state
 * segment is set up in smp.c). Part of the trusted base.
 */
#include <stdint.h>
#include "core.h"

extern const uint64_t bst_trap_stubs[256];
extern char bst_syscall_entry[], bst_tramp_start[], bst_tramp_end[];
extern uint64_t bst_kernel_pml4[512], bst_gdt[];
extern uint64_t bst_shadow_pml4[MAX_CPUS][512];

static struct __attribute__((packed)) {
    uint16_t off_lo, sel;
    uint8_t ist, type;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} idt[256] __attribute__((aligned(4096)));

/*
 * The entry area (core.h): last top-level slot, 4 KiB pages, in tables of the
 * kernel's own. Every shadow table points at the same level-3 table.
 */
static uint64_t alias_pdpt[512] __attribute__((aligned(4096)));
static uint64_t alias_pd[512] __attribute__((aligned(4096)));
static uint64_t alias_pt0[512] __attribute__((aligned(4096)));        /* code, GDT, IDT */
static uint64_t alias_blk_pt[16][512] __attribute__((aligned(4096))); /* 16 blocks (2 MiB) per table */

/* Tells the compiled kernel (the prelude's BST_DEFERRED) that user code runs
 * on a table of its own, which bst_cpu_enter loads from word 3 of
 * bst_platform_mmu_state: the kernel's tables stay in CR3 in Bastio code. */
const char bst_platform_deferred_cr3 = 1;

/* Where a symbol of the entry area's code is seen from the entry area. */
uint64_t bst_alias_of(const void *sym) {
    return ALIAS_BASE + ((uint64_t)sym - (uint64_t)bst_tramp_start);
}

/* Processor i's block (the bootstrap processor's is in the image, an
 * application processor's comes out of the memory map): mapped in the entry
 * area, minus its guard page, and its shadow table's last slot. Runs on the
 * bootstrap processor before the block's processor starts. */
void bst_alias_map_block(uint64_t i, uint8_t *core) {
    uint64_t *pt = alias_blk_pt[i / 16] + (i % 16) * (CORE_BLOCK / 4096);
    for (uint64_t p = 0; p < CORE_BLOCK / 4096; p++)
        pt[p] = p == CORE_GUARD / 4096 ? 0 : ((uint64_t)core + 4096 * p) | 3;
    bst_shadow_pml4[i][511] = bst_kernel_pml4[511];
}

/* The GDT register, with the table where the entry area sees it. */
void bst_alias_load_gdt(void) {
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr = { 0x28 + 16 * MAX_CPUS - 1, ALIAS_GDT };
    __asm__ volatile ("lgdt %0" : : "m"(gdtr));
}

static void alias_init(void) {
    alias_pdpt[0] = (uint64_t)alias_pd | 3;
    alias_pd[0] = (uint64_t)alias_pt0 | 3;
    for (int k = 0; k < 16; k++) alias_pd[1 + k] = (uint64_t)alias_blk_pt[k] | 3;
    for (uint64_t a = (uint64_t)bst_tramp_start; a < (uint64_t)bst_tramp_end; a += 4096)
        alias_pt0[(a - (uint64_t)bst_tramp_start) / 4096] = a | 1;   /* read-only, executable */
    for (int k = 0; k < 2; k++) alias_pt0[(ALIAS_GDT - ALIAS_BASE) / 4096 + k] = ((uint64_t)bst_gdt + 4096 * k) | 3;
    alias_pt0[(ALIAS_IDT - ALIAS_BASE) / 4096] = (uint64_t)idt | 3;
    bst_kernel_pml4[511] = (uint64_t)alias_pdpt | 3;
    bst_alias_load_gdt();
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

#define SYSCALL_VECTOR 0x80

void bst_platform_init(void) {
    alias_init();

    /* Each processor's task state segment is set up by bst_core_init (smp.c). */

    /* Interrupt gates: interrupts stay off in the kernel. Only the system
     * call gate and the breakpoint gate (vector 3, for `int3`) can be raised
     * from ring 3 with `int`. The exceptions that
     * can arrive at any instruction whatever IF says (#DB, NMI, #MC), and
     * #DF, switch to the processor's IST1 stack (core.h): the first
     * instructions of bst_syscall_entry run in ring 0 on the user's %rsp,
     * and a frame pushed there would land wherever the user pointed it. */
    for (int v = 0; v < 256; v++) {
        uint64_t a = bst_alias_of((const void *)bst_trap_stubs[v]);
        idt[v].off_lo = a & 0xFFFF;
        idt[v].sel = 0x08;
        idt[v].ist = (v == 1 || v == 2 || v == 8 || v == 18) ? 1 : 0;
        idt[v].type = (v == SYSCALL_VECTOR || v == 3) ? 0xEE : 0x8E;   /* int3 raises #BP, which becomes SIGTRAP, not #GP */
        idt[v].off_mid = (a >> 16) & 0xFFFF;
        idt[v].off_hi = a >> 32;
        idt[v].zero = 0;
    }
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = { sizeof idt - 1, ALIAS_IDT };
    __asm__ volatile ("lidt %0" : : "m"(idtr));

    /* The `syscall` instruction (enabled by EFER.SCE in boot.S) enters
     * bst_syscall_entry on kernel code segment 0x08 with IF, DF, TF and AC
     * cleared. STAR's sysret half is unused: the return path is iretq. */
    wrmsr(0xC0000081, (0x10ull << 48) | (0x08ull << 32));   /* STAR */
    wrmsr(0xC0000082, bst_alias_of(bst_syscall_entry));     /* LSTAR */
    wrmsr(0xC0000084, 0x40700);                             /* FMASK: AC DF IF TF */

    /* The interrupt controllers, the timer and the UART are set up by the
     * kernel itself, in Bastio (src/pic.bastio, src/pit.bastio, src/uart.bastio). */
}
