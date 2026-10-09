ENTRY(_start)
SECTIONS
{
    . = 1M;
    .text : {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(COMMON) *(.bss .bss.*) }
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame*) }
}
