#!/usr/bin/env python3
"""Check the firmware's .hot_text layout (src/hot_text.ld) after a build.

Each of the three groups (Lua's runtime, the bridge glue and chargrid, the
shader VM) must start on an 8 KB boundary, so its XIP cache sets do not
depend on the rest of the firmware, and must hold the functions that
pc_prof.py found hot in Lua apps (ascii-scene's fire, fjord and aurora
pieces). A selector in hot_text.ld that stopped matching (a renamed file)
leaves its functions in .text, where any change moves them again: that
fails here, and so the build.

usage: check_hot_text.py [--nm NM] ELF
"""

import argparse
import subprocess
import sys

SET_PERIOD = 8192  # 16 KB XIP cache, 2 ways: the sets repeat every 8 KB

# group: (start symbol, end symbol, the hot functions it must hold)
GROUPS = {
    "lua": ("__hot_text_start__", "__hot_text_glue__",
            ["luaV_execute", "luaV_finishget", "luaD_precall", "luaD_poscall",
             "luaH_getshortstr", "luaH_get", "lua_gettop", "lua_tointegerx",
             "luaL_checkinteger", "propagatemark", "math_random", "str_byte",
             "luaG_traceexec"]),
    "glue": ("__hot_text_glue__", "__hot_text_vm__",
             ["lua_service", "picodeck_lua_array_getnum", "chargrid_shade",
              "chargrid_render", "row_w5", "l_grid_shadeRow", "l_shader_run"]),
    "vm": ("__hot_text_vm__", "__hot_text_end__",
           ["charshader_run", "exec_rare", "cs_pow"]),
}


# What src/hot_gfx.ld must hold. raster_scene is not here: it runs from SRAM.
HOT_GFX = ["gfx3d_draw", "gfx3d_draw_xf", "gfx3d_draw_tmesh", "gfx3d_end", "l_gfx3d_draw",
           "l_gfx3d_endScene", "display_flush"]

# Functions src/hot_gfx.ld pins in a section of their own, each at the symbol that marks its
# place: function, symbol. A pin that slipped (the section attribute lost, the function inlined
# or renamed) lets an edit elsewhere in gfx3d.c move it to other cache sets again.
PINNED_GFX = [("process_mesh", "__hot_gfx_start__"), ("raster_sprite", "__hot_gfx_sprite__")]

# libgcc the SDK's script would copy to SRAM for nothing (src/hot_text.ld).
COLD_LIBGCC = ["__aeabi_unwind_cpp_pr0", "_Unwind_VRS_Pop", "__gnu_unwind_execute",
               "_Unwind_Backtrace", "__aeabi_f2d"]


def symbols(nm, elf):
    out = subprocess.run([nm, elf], check=True, capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms.setdefault(parts[2], int(parts[0], 16))
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("elf")
    args = ap.parse_args()
    syms = symbols(args.nm, args.elf)
    errors, sizes = [], []
    for name, (lo_sym, hi_sym, funcs) in GROUPS.items():
        if lo_sym not in syms or hi_sym not in syms:
            errors.append(f"{name}: no {lo_sym}/{hi_sym}: is src/hot_text.ld linked?")
            continue
        lo, hi = syms[lo_sym], syms[hi_sym]
        if lo % SET_PERIOD:
            errors.append(f"{name}: starts at {lo:#x}, not on an 8 KB boundary")
        sizes.append(f"{name} {hi - lo} B")
        for f in funcs:
            addr = syms.get(f)
            if addr is None:
                errors.append(f"{name}: no symbol {f} (renamed? update GROUPS)")
            elif not lo <= addr < hi:
                errors.append(f"{name}: {f} at {addr:#x} is outside {lo:#x}-{hi:#x}")
    lo, hi = syms.get("__cold_libgcc_start__"), syms.get("__cold_libgcc_end__")
    for f in COLD_LIBGCC:
        addr = syms.get(f)
        if lo is None or hi is None:
            errors.append("no .cold_libgcc: is src/hot_text.ld linked?")
            break
        if addr is not None and not lo <= addr < hi:
            errors.append(f"{f} at {addr:#x} is outside .cold_libgcc: it costs SRAM again")
    gfx, gfx_end = syms.get("__hot_gfx_start__"), syms.get("__hot_gfx_end__")  # src/hot_gfx.ld
    if gfx is None or gfx_end is None:
        errors.append("no .hot_gfx: is src/hot_gfx.ld linked?")
    else:
        if gfx % SET_PERIOD or gfx < syms["__cold_libgcc_end__"]:
            errors.append(f".hot_gfx at {gfx:#x}: not 8 KB aligned after .cold_libgcc")
        for f in HOT_GFX:
            addr = syms.get(f)
            if addr is None:
                errors.append(f"gfx: no symbol {f} (renamed? update HOT_GFX)")
            elif not gfx <= addr < gfx_end:
                errors.append(f"gfx: {f} at {addr:#x} is outside {gfx:#x}-{gfx_end:#x}")
        for f, at in PINNED_GFX:
            addr, want = syms.get(f), syms.get(at)
            if addr is None or want is None:
                errors.append(f"gfx: no symbol {f if addr is None else at} (update PINNED_GFX)")
            elif addr != want:
                errors.append(f"gfx: {f} at {addr:#x}, not at {at} {want:#x}: its pin slipped")
        sizes.append(f"gfx {gfx_end - gfx} B")
    if errors:
        print("check_hot_text: FAILED\n  " + "\n  ".join(errors), file=sys.stderr)
        return 1
    print(f"check_hot_text: ok, .hot_text at {syms['__hot_text_start__']:#x}: " + ", ".join(sizes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
