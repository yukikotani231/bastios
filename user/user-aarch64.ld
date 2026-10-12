/* AArch64 user programs live at USER_BASE (slot 1 of the top-level page table; see src/aarch64/arch/userbase.bastio). */
ENTRY(_start)
SECTIONS
{
    . = 0x8000000000;
    __image_start = .;
    .header : { KEEP(*(.header)) }
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(COMMON) *(.bss .bss.*) }
    __image_end = .;
    __image_size = __image_end - __image_start;
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame*) }
}
