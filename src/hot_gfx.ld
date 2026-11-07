/*
 * The code a Lua 3D game keeps hot, on an 8 KB boundary after .hot_text, so a
 * change elsewhere stops moving it to other cache sets (see src/hot_text.ld). It
 * does not avoid Lua's own sets: the Lua group alone spans all of them nine times
 * over. On the device it took 1.8% off Nova Rail's frame work and 4.2% off the
 * Lua interpreter bench, and nothing measured got slower.
 */
SECTIONS
{
    .hot_gfx : ALIGN(8192)
    {
        __hot_gfx_start__ = .;
        /* First, at the boundary, so that nothing in gfx3d.c moves it: process_mesh. */
        *src/os/gfx3d.c.o(.gfx3d_mesh_text)
        *src/os/gfx3d.c.o(.text* .rodata*)
        *src/os/lua_bridge_gfx3d.c.o(.text* .rodata*)
        *src/fonts/font.c.o(.text* .rodata*)
        *src/os/lua_bridge_display.c.o(.text* .rodata*)
        *src/drivers/display.c.o(.text* .rodata*)
        /* Last, so that textured code moves nothing a flat scene runs: gfx3d.c. */
        *src/os/gfx3d.c.o(.gfx3d_tex_text)
        . = ALIGN(4);
        __hot_gfx_end__ = .;
    } > FLASH
}
INSERT AFTER .cold_libgcc;
