/*
 * Platform setup that must happen before the Bastio kernel runs: the task
 * state segment (the stack user-mode traps land on) and the interrupt
 * table. Part of the trusted base.
 */
#include <stdint.h>

extern uint64_t bst_gdt[];
extern const uint64_t bst_trap_stubs[256];
extern char bst_trap_stack_top[];
extern char bst_syscall_entry[];

static struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss;

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
    /* TSS: user-mode traps switch to the trap stack. No I/O permission map. */
    tss.rsp0 = (uint64_t)bst_trap_stack_top;
    tss.iomap_base = sizeof tss;
    uint64_t base = (uint64_t)&tss, limit = sizeof tss - 1;
    bst_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40)
               | (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    bst_gdt[6] = base >> 32;
    __asm__ volatile ("ltr %w0" : : "r"(0x28));

    /* Interrupt gates: interrupts stay off in the kernel. Only the system
     * call gate can be raised from ring 3 with `int`. */
    for (int v = 0; v < 256; v++) {
        uint64_t a = bst_trap_stubs[v];
        idt[v].off_lo = a & 0xFFFF;
        idt[v].sel = 0x08;
        idt[v].ist = 0;
        idt[v].type = v == SYSCALL_VECTOR ? 0xEE : 0x8E;
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
