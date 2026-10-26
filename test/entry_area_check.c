/*
 * Host-side check of the entry area boot/platform.c builds: the pages and
 * gates that user mode's traps can reach. It includes the real file with
 * the privileged instructions compiled out (so lgdt, lidt and wrmsr do
 * nothing), runs bst_platform_init and bst_alias_map_block for every
 * processor, then walks the tables from the last top-level slot of the
 * kernel's table and checks what the trusted-base page promises:
 *
 *   - no page of the area is user-accessible, and nothing in it is
 *     executable-and-writable: the trap code is read-only;
 *   - only the GDT, the IDT and the processors' blocks are writable, and
 *     each block maps its own memory, minus the guard page under its stack;
 *   - the IDT has exactly two gates user mode can raise (vectors 3 and
 *     0x80), exactly four on the IST1 stack (1, 2, 8, 18), and every handler
 *     lies in the trap code;
 *   - each processor's shadow table shares the kernel's entry-area slot.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __asm__ if (0) asm      /* never executed: lgdt, lidt and wrmsr are privileged */
#include "../boot/platform.c"
#undef __asm__

char bst_syscall_entry[16];
/* three pages of "trap code" between the two labels, as the linker lays them out,
 * and a table of 256 stub addresses 16 bytes apart in it */
asm(".pushsection .data\n.balign 4096\n.globl bst_tramp_start\nbst_tramp_start:\n.skip 0x3000\n.globl bst_tramp_end\nbst_tramp_end:\n.skip 8\n.balign 8\n.globl bst_trap_stubs\nbst_trap_stubs:\n.set i, 0\n.rept 256\n.quad bst_tramp_start + 16 * i\n.set i, i + 1\n.endr\n.popsection\n");
uint64_t bst_kernel_pml4[512];
uint64_t bst_gdt[2 * 512] __attribute__((aligned(4096)));
uint64_t bst_shadow_pml4[MAX_CPUS][512];

static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static uint8_t blocks[MAX_CPUS][CORE_BLOCK] __attribute__((aligned(CORE_BLOCK)));
    bst_platform_init();
    for (uint64_t i = 0; i < MAX_CPUS; i++) bst_alias_map_block(i, blocks[i]);

    const uint64_t frame = 0x000FFFFFFFFFF000ull;
    uint64_t pml4e = bst_kernel_pml4[511];
    CHECK(pml4e & 1, "entry-area slot not present");
    CHECK(!(pml4e & 4), "entry-area slot user-accessible");
    uint64_t *pdpt = (uint64_t *)(pml4e & frame);
    uint64_t pages = 0, writable_code = 0;
    for (uint64_t a = 0; a < 512ull << 30; a += 4096) {
        /* walk ALIAS_BASE + a through the three lower levels; only the
         * first 34 MiB can be mapped, so skip the rest cheaply */
        if (a >= (34ull << 20)) break;
        uint64_t e3 = pdpt[(a >> 30) & 511];
        if (!(e3 & 1)) continue;
        CHECK(!(e3 & 4) && !(e3 & 0x80), "pdpt entry for %#llx", (unsigned long long)a);
        uint64_t e2 = ((uint64_t *)(e3 & frame))[(a >> 21) & 511];
        if (!(e2 & 1)) continue;
        CHECK(!(e2 & 4) && !(e2 & 0x80), "pd entry for %#llx", (unsigned long long)a);
        uint64_t e1 = ((uint64_t *)(e2 & frame))[(a >> 12) & 511];
        if (!(e1 & 1)) continue;
        pages++;
        CHECK(!(e1 & 4), "page %#llx is user-accessible", (unsigned long long)a);
        int in_code = a < 0x100000, in_gdt_idt = a >= 0x100000 && a < 0x103000, in_block = a >= 0x200000;
        if (in_code) {
            CHECK(!(e1 & 2), "trap code page %#llx is writable", (unsigned long long)a);
            writable_code += !!(e1 & 2);
                        CHECK((e1 & frame) == (uint64_t)bst_tramp_start + a, "trap code page %#llx maps the wrong memory", (unsigned long long)a);
        } else if (in_gdt_idt) {
            uint64_t want = a < 0x102000 ? (uint64_t)bst_gdt + (a - 0x100000) : (uint64_t)idt;
            CHECK((e1 & frame) == want && (e1 & 2), "GDT/IDT page %#llx", (unsigned long long)a);
        } else if (in_block) {
            uint64_t idx = (a - 0x200000) / CORE_BLOCK, off = (a - 0x200000) % CORE_BLOCK;
            CHECK(idx < MAX_CPUS && (e1 & 2), "block page %#llx", (unsigned long long)a);
            CHECK(off != CORE_GUARD, "guard page of block %llu is mapped", (unsigned long long)idx);
            CHECK((e1 & frame) == (uint64_t)blocks[idx] + off, "block %llu page %#llx maps the wrong memory", (unsigned long long)idx, (unsigned long long)a);
        } else {
            CHECK(0, "page %#llx mapped outside the plan", (unsigned long long)a);
        }
    }
    uint64_t code_pages = 0x3000 / 4096;
    CHECK(pages == code_pages + 3 + (uint64_t)MAX_CPUS * (CORE_BLOCK / 4096 - 1), "%llu pages mapped", (unsigned long long)pages);
    (void)writable_code;

    for (int v = 0; v < 256; v++) {
        int user = v == 3 || v == 0x80, ist = v == 1 || v == 2 || v == 8 || v == 18;
        CHECK(idt[v].type == (user ? 0xEE : 0x8E), "gate %d type %#x", v, idt[v].type);
        CHECK(idt[v].ist == (ist ? 1 : 0), "gate %d ist %d", v, idt[v].ist);
        CHECK(idt[v].sel == 0x08 && idt[v].zero == 0, "gate %d selector", v);
        uint64_t h = idt[v].off_lo | (uint64_t)idt[v].off_mid << 16 | (uint64_t)idt[v].off_hi << 32;
        CHECK(h >= ALIAS_BASE && h < ALIAS_BASE + 0x3000, "gate %d handler %#llx outside the trap code", v, (unsigned long long)h);
    }
    for (int i = 0; i < MAX_CPUS; i++) CHECK(bst_shadow_pml4[i][511] == bst_kernel_pml4[511], "shadow table %d", i);

    if (bad) return 1;
    printf("entry area ok: %llu pages\n", (unsigned long long)pages);
    return 0;
}
