/* BastiOS for AArch64: QEMU's virt machine loads the image (bastios.bin, with
 * the Linux arm64 Image header boot.S starts with) at the start of RAM plus
 * 512 KiB and enters _start at EL1 with the MMU off and the device tree's
 * address in x0. */
ENTRY(_start)
SECTIONS {
    . = 0x40080000;
    .text : { *(.text.boot) *(.text*) }
    .tramp : ALIGN(4096) { *(.tramp) . = ALIGN(4096); }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.got) *(.got.plt) *(.igot*) . = ALIGN(4096); }
    __file_end = .;
    .bss (NOLOAD) : ALIGN(4096) {
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    }
    . = ALIGN(4096);
    . += 0x10000;
    stack_top = .;
    _end = .;
    __image_size = _end - 0x40080000;
    __file_size = __file_end - 0x40080000;
    __efi_entry_rva = efi_entry - 0x40080000;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
