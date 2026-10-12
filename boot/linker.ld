ENTRY(_start)
SECTIONS
{
    . = 16M;               /* above the holes UEFI firmware leaves low in RAM */
    .note.pvh : { KEEP(*(.note.pvh)) }
    .text : {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }
    .tramp ALIGN(4096) : { *(.tramp.text) }   /* the entry area's code (core.h): page-aligned, mapped on its own */
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(COMMON) *(.bss .bss.*) }
    . = ALIGN(4096);
    _end = .;              /* physical pages after this belong to the page pool */
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame*) }
}
