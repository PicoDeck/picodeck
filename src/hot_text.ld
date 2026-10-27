/*
 * The hot code of Lua apps at fixed XIP cache sets (CMakeLists.txt adds this
 * script after the SDK's memmap_default.ld; tools/check_hot_text.py checks
 * the result after every build).
 *
 * The XIP cache (16 KB, 2-way, 8-byte lines) picks a line's set from its
 * address modulo 8 KB, and on the device most of a Lua app's time is cache
 * misses. In .text, every byte any change adds ahead of the Lua core moved
 * it to other sets: the same app (ascii-scene's fire, pure Lua) ran at 34,
 * 43 and 25 fps on three builds that differed only in unrelated code.
 *
 * So the code that Lua apps keep hot (pc_prof.py: the interpreter and its
 * runtime, the bridge's glue, chargrid and its shader VM) is taken out of
 * .text into .hot_text, in three groups, each starting on an 8 KB boundary.
 * A group's sets then depend only on its own code: a change elsewhere in the
 * firmware moves .hot_text by whole multiples of 8 KB, and a change inside
 * one group moves no other group. The groups go from the code that changes
 * least (Lua, third party) to the code that changes most (the VM). The
 * padding costs up to 24 KB of flash.
 *
 * An input section goes to the first rule that matches it, and an INSERT
 * script's rules come before the default script's, so these win over the
 * SDK's *(.text*) and *(.rodata*). Their .rodata comes along: the
 * interpreter's dispatch table is read on every opcode. A selector that
 * matches nothing (a renamed file) fails check_hot_text.py, not the link.
 */
SECTIONS
{
    .hot_text : ALIGN(8192)
    {
        __hot_text_start__ = .;
        /* Lua's runtime: the interpreter, calls, tables, the C API and the
           auxiliary library, the collector, strings, closures, metamethods,
           debug hooks (the bridge's service hook), and math and string. */
        *liblua.a:lvm.c.o(.text* .rodata*)
        *liblua.a:ldo.c.o(.text* .rodata*)
        *liblua.a:ltable.c.o(.text* .rodata*)
        *liblua.a:lapi.c.o(.text* .rodata*)
        *liblua.a:lauxlib.c.o(.text* .rodata*)
        *liblua.a:lgc.c.o(.text* .rodata*)
        *liblua.a:lstring.c.o(.text* .rodata*)
        *liblua.a:lfunc.c.o(.text* .rodata*)
        *liblua.a:ltm.c.o(.text* .rodata*)
        *liblua.a:lobject.c.o(.text* .rodata*)
        *liblua.a:lstate.c.o(.text* .rodata*)
        *liblua.a:ldebug.c.o(.text* .rodata*)
        *liblua.a:lmathlib.c.o(.text* .rodata*)
        *liblua.a:lstrlib.c.o(.text* .rodata*)
        . = ALIGN(8192);
        __hot_text_glue__ = .;
        /* The bridge's per-call glue (lua_fastarg.c is built into liblua),
           and chargrid's grid, renderer and bindings. */
        *src/os/lua_bridge.c.o(.text* .rodata*)
        *liblua.a:lua_fastarg.c.o(.text* .rodata*)
        *src/os/chargrid.c.o(.text* .rodata*)
        *src/os/chargrid_render.c.o(.text* .rodata*)
        *src/os/lua_bridge_chargrid.c.o(.text* .rodata*)
        . = ALIGN(8192);
        __hot_text_vm__ = .;
        /* The chargrid shader VM. */
        *src/os/charshader_vm.c.o(.text* .rodata*)
        . = ALIGN(4);
        __hot_text_end__ = .;
    } > FLASH
}
INSERT AFTER .text;
