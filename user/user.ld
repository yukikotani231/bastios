/* User programs live at USER_BASE (slot 1 of the top-level page table). */
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
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame*) }
}
