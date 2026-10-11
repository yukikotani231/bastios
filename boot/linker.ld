ENTRY(_start)
SECTIONS
{
    . = 16M;               /* above the holes UEFI firmware leaves low in RAM */
    .text : {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(COMMON) *(.bss .bss.*) }
    . = ALIGN(4096);
    _end = .;              /* physical pages after this belong to the page pool */
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame*) }
}
