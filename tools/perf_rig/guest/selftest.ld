ENTRY(_start)
SECTIONS
{
  .text 0x10001000 : { *(.text .text.* .rodata .rodata.*) }
  .data 0x20002000 : { *(.data .data.* .bss .bss.* COMMON) }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) }
}
