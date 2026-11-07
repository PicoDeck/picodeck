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
        /* First, at the boundary, so that nothing in gfx3d.c moves it: process_mesh
           (tools/check_hot_text.py fails the build if it is not at __hot_gfx_start__). */
        *src/os/gfx3d.c.o(.gfx3d_mesh_text)
        /* Then a sprite's fill in a slot of fixed size: raster_sprite at its start (checked
           the same way) in a part of its own, then what only it calls (fog_table and the
           table blit, then the blits that replay runs), then room for more. A change to
           raster_sprite moves nothing else, a change to its helpers moves nothing after the
           slot, and nothing else moves them.
           Outgrowing either part fails the link here ("cannot move location counter
           backwards"): widen it by 8 KB, and the perf rig's (tools/perf_rig/guest/guest.ld)
           with it. The sizes are 8 KB more than what the code needed when it was pinned
           (raster_sprite 2372 B; the slot 3436 B), so every function kept its cache set, the
           sets repeating every 8 KB. */
        __hot_gfx_sprite__ = .;
        *src/os/gfx3d.c.o(.gfx3d_sprite_text)
        . = __hot_gfx_sprite__ + 10564;
        *src/os/gfx3d.c.o(.gfx3d_sprite_aux_text)
        *src/os/gfx3d.c.o(.gfx3d_sprite_runs_text)
        . = __hot_gfx_sprite__ + 19820;
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
