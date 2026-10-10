/*
 * Platform setup that must happen before the Bastio kernel runs: the task
 * state segment (the stack user-mode traps land on) and the interrupt
 * table. Part of the trusted base.
 */
#include <stdint.h>

extern uint64_t bst_gdt[];
extern const uint64_t bst_trap_stubs[256];
extern char bst_trap_stack_top[];

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

    /* The interrupt controllers, the timer and the UART are set up by the
     * kernel itself, in Bastio (src/pic.bastio, src/pit.bastio, src/uart.bastio). */
}
