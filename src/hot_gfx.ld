/*
 * Experiment (CMake PICODECK_HOT_GFX, off by default): the code a Lua 3D game
 * keeps hot, on an 8 KB boundary after .hot_text, so a change elsewhere stops
 * moving it to other cache sets (see src/hot_text.ld). It does not avoid Lua's
 * own sets: the Lua group alone spans all of them nine times over.
 */
SECTIONS
{
    .hot_gfx : ALIGN(8192)
    {
        __hot_gfx_start__ = .;
        *src/os/gfx3d.c.o(.text* .rodata*)
        *src/os/lua_bridge_gfx3d.c.o(.text* .rodata*)
        *src/fonts/font.c.o(.text* .rodata*)
        *src/os/lua_bridge_display.c.o(.text* .rodata*)
        *src/drivers/display.c.o(.text* .rodata*)
        . = ALIGN(4);
        __hot_gfx_end__ = .;
    } > FLASH
}
INSERT AFTER .cold_libgcc;
