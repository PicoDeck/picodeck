/* The Lua workload: luaV_execute at its firmware address (LUAV_BASE), so its
   lines fall in the same cache sets. The rest of Lua follows it, not at its
   firmware addresses: an empty loop runs nothing else once started. */
ENTRY(_start)
SECTIONS
{
  .text LUAV_BASE : {
    *(.text.luaV_execute)
    . = ALIGN(4);
    *(.text .text.*)
  }
  .rodata : {
    *(.rodata .rodata.*)
    *(.ARM.extab*)
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  }
  .data DATA_BASE : {
    *(.data .data.*)
    . = ALIGN(4);
    *(.bss .bss.* COMMON)
    . = ALIGN(8);
    __end__ = .;
    end = .;
  }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) }
  __stack_top = STACK_TOP;
  __heap_base = HEAP_BASE;
}
