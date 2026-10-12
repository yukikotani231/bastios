/* User programs live at USER_BASE: address 0, the start of the window. */
ENTRY(_start)
SECTIONS
{
    . = 0;
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
