/* The AArch64 vDSO (the layout of vdso.ld with its GOT kept): headers, dynamic tables and code in one read-only,
 * executable segment from address 0, all below byte 0xF80 of the page;
 * the kernel writes the clock from there (src/elf.bastio, VDSO_DATA). */
SECTIONS
{
  . = SIZEOF_HEADERS;
  .hash : { *(.hash) } :text
  .gnu.hash : { *(.gnu.hash) }
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .gnu.version : { *(.gnu.version) }
  .gnu.version_d : { *(.gnu.version_d) }
  .gnu.version_r : { *(.gnu.version_r) }
  .dynamic : { *(.dynamic) } :text :dynamic
  .got : { *(.got) }
  .got.plt : { *(.got.plt) }
  .rodata : { *(.rodata*) } :text
  .text : { *(.text*) }
  . = 0xF80;
  vdso_data = .;
  /DISCARD/ : { *(.data*) *(.bss*) *(.note*) *(.eh_frame*) *(.comment) }
}
PHDRS
{
  text PT_LOAD FLAGS(5) FILEHDR PHDRS;
  dynamic PT_DYNAMIC FLAGS(4);
}
