/*
 * Platform setup that must happen before the Bastio kernel runs: the
 * interrupt table and the system call MSRs (each processor's task state
 * segment is set up in smp.c). Part of the trusted base.
 */
#include <stdint.h>

extern const uint64_t bst_trap_stubs[256];
extern char bst_syscall_entry[];

static struct __attribute__((packed)) {
    uint16_t off_lo, sel;
    uint8_t ist, type;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} idt[256];

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

#define SYSCALL_VECTOR 0x80

void bst_platform_init(void) {
    /* Each processor's task state segment is set up by bst_core_init (smp.c). */

    /* Interrupt gates: interrupts stay off in the kernel. Only the system
     * call gate and the breakpoint gate (vector 3, for `int3`) can be raised
     * from ring 3 with `int`. The exceptions that
     * can arrive at any instruction whatever IF says (#DB, NMI, #MC), and
     * #DF, switch to the processor's IST1 stack (core.h): the first
     * instructions of bst_syscall_entry run in ring 0 on the user's %rsp,
     * and a frame pushed there would land wherever the user pointed it. */
    for (int v = 0; v < 256; v++) {
        uint64_t a = bst_trap_stubs[v];
        idt[v].off_lo = a & 0xFFFF;
        idt[v].sel = 0x08;
        idt[v].ist = (v == 1 || v == 2 || v == 8 || v == 18) ? 1 : 0;
        idt[v].type = (v == SYSCALL_VECTOR || v == 3) ? 0xEE : 0x8E;   /* int3 raises #BP, which becomes SIGTRAP, not #GP */
        idt[v].off_mid = (a >> 16) & 0xFFFF;
        idt[v].off_hi = a >> 32;
        idt[v].zero = 0;
    }
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(idtr));

    /* The `syscall` instruction (enabled by EFER.SCE in boot.S) enters
     * bst_syscall_entry on kernel code segment 0x08 with IF, DF, TF and AC
     * cleared. STAR's sysret half is unused: the return path is iretq. */
    wrmsr(0xC0000081, (0x10ull << 48) | (0x08ull << 32));   /* STAR */
    wrmsr(0xC0000082, (uint64_t)bst_syscall_entry);         /* LSTAR */
    wrmsr(0xC0000084, 0x40700);                             /* FMASK: AC DF IF TF */

    /* The interrupt controllers, the timer and the UART are set up by the
     * kernel itself, in Bastio (src/pic.bastio, src/pit.bastio, src/uart.bastio). */
}
