/* BastiOS for AArch64: QEMU's virt machine loads the image at the start of
 * RAM plus the usual 512 KiB and enters _start at EL1 with the MMU off and
 * the device tree's address in x0. */
ENTRY(_start)
SECTIONS {
    . = 0x40080000;
    .text : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : ALIGN(4096) {
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    }
    . = ALIGN(4096);
    . += 0x10000;
    stack_top = .;
    _end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
