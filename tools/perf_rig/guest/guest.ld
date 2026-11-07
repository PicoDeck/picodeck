/* Places the guest as the firmware places the same code. The bases come from
   the Makefile (--defsym), so a map can move code, stack or framebuffer. */
ENTRY(_start)
SECTIONS
{
  /* Text at gfx3d.c's firmware address, its functions first and in object
     order, so its lines fall in the same cache sets as in the firmware. */
  .text TEXT_BASE : {
    KEEP(*gfx3d.o(.gfx3d_mesh_text))  /* first, as in src/hot_gfx.ld */
    __sprite_slot = .;                  /* then a sprite's fill in its slot, as there */
    KEEP(*gfx3d.o(.gfx3d_sprite_text))
    KEEP(*gfx3d.o(.gfx3d_sprite_aux_text))
    . = __sprite_slot + 11628;
    KEEP(*gfx3d.o(.text .text.*))
    *gfx3d_main.o(.text .text.*)
    *start.o(.text .text.*)
    *gfx3d.o(.gfx3d_tex_text)  /* after what a flat scene runs, as in src/hot_gfx.ld */
  }
  /* Code the firmware keeps in SRAM: .time_critical*, and newlib's sqrtf. */
  .ramtext RAMTEXT_BASE : {
    *(.time_critical*)
    *libm*.a:*sqrt*.o(.text .text.*)
  }
  .libtext LIB_BASE : {
    *(.text .text.*)
  }
  .rodata RODATA_BASE : {
    *(.rodata .rodata.*)
    *(.ARM.exidx* .ARM.extab*)
  }
  .data DATA_BASE : {
    *(.data .data.*)
    . = ALIGN(4);
    *(.bss .bss.* COMMON)
    . = ALIGN(8);
    __end__ = .;
    end = .;
  }
  .scene 0x30000000 : { *(.scene) }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) }
  __stack_top = STACK_TOP;
  __heap_base = HEAP_BASE;
  __fb_base = FB_BASE;
}
