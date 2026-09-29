#!/usr/bin/env python3
"""Guard sdk/lua/picocalc.lua (the LuaLS stub) against drift from the bridge.

The stub is hand-written and the only signature reference a Lua app has; a
wrong entry raises no error (arguments are ignored or reinterpreted), so it
is checked mechanically. For every Lua name registered in a luaL_Reg table of
src/os/lua_bridge*.c the checker finds the C function, reads the argument
indices it uses and the values it returns, and compares them with the stub.

Heuristics, on purpose (arity, not types):

  - Arguments: the highest literal index passed to a luaL_check*/luaL_opt*/
    lua_to*/lua_is*/lua_type/lb_* reader is the C argument count (minus one
    for the self argument of a method); the highest index passed to a
    *required* reader (luaL_check*, lb_check*) is its required count. The stub
    must declare the same total, and at least as many optional params as C
    has optional ones, and the same number of required ones.
  - Returns: the literal `return N;` statements are the C return counts. The
    stub's `---@return` count must be one of them (0 when there is no
    `---@return`).
  - A stub function with no bridge entry under that owner is drift too
    (a name moved or renamed).

Functions the heuristics cannot read (variadic, computed indices, dispatch by
type, helper functions that read the arguments) go in ALLOW below, each with
its reason. A stub method of a class not in OWNERS is not checked; a `picocalc.*` table
not in OWNERS is reported (a moved or renamed table, or a new one to add).

Exit status 0 when everything matches; 1 with a list of mismatches otherwise.
Also imported by tests/unit/test_lua_stub.py.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE_DIR = ROOT / "src" / "os"
STUB = ROOT / "sdk" / "lua" / "picocalc.lua"

# (bridge file, luaL_Reg table) -> (stub owner, is_method). A method table's
# functions receive the object as argument 1.
OWNERS = {
    ("lua_bridge_audio.c", "l_audio_lib"): ("picocalc.audio", False),
    ("lua_bridge_appconfig.c", "l_config_lib"): ("picocalc.config", False),
    ("lua_bridge_config.c", "l_config_lib"): ("picocalc.sysconfig", False),
    ("lua_bridge_input.c", "l_input_lib"): ("picocalc.input", False),
    ("lua_bridge_video.c", "video_funcs"): ("picocalc.video", False),
    ("lua_bridge_video.c", "video_methods"): ("PicoDeckVideoPlayer", True),
    ("lua_bridge_crypto.c", "l_crypto_lib"): ("picocalc.crypto", False),
    ("lua_bridge_crypto.c", "l_aes_ctr_methods"): ("PicoDeckAesCtr", True),
    ("lua_bridge_crypto.c", "l_ecdh_methods"): ("PicoDeckEcdh", True),
    ("lua_bridge_gfx3d.c", "l_gfx3d_lib"): ("picocalc.gfx3d", False),
    ("lua_bridge_gfx3d.c", "l_mesh_methods"): ("PicoDeckMesh", True),
    ("lua_bridge_fs.c", "l_fs_lib"): ("picocalc.fs", False),
    ("lua_bridge_fs.c", "l_fs_file_methods"): ("PicoDeckFile", True),
    ("lua_bridge_terminal.c", "terminal_funcs"): ("picocalc.terminal", False),
    ("lua_bridge_terminal.c", "terminal_methods"): ("PicoDeckTerminal", True),
    ("lua_bridge_json.c", "l_json_lib"): ("picocalc.json", False),
    ("lua_bridge_network.c", "l_wifi_lib"): ("picocalc.wifi", False),
    ("lua_bridge_network.c", "l_network_lib"): ("picocalc.network", False),
    ("lua_bridge_network.c", "l_http_lib"): ("picocalc.network.http", False),
    ("lua_bridge_network.c", "l_http_methods"): ("PicoDeckHttpConn", True),
    ("lua_bridge_repl.c", "l_repl_lib"): ("picocalc.repl", False),
    ("lua_bridge_zip.c", "zip_funcs"): ("picocalc.zip", False),
    ("lua_bridge_zip.c", "archive_methods"): ("PicoDeckZipArchive", True),
    ("lua_bridge_display.c", "l_display_lib"): ("picocalc.display", False),
    ("lua_bridge_mod.c", "modplayer_funcs"): ("picocalc.modplayer", False),
    ("lua_bridge_mod.c", "modplayer_methods"): ("PicoDeckModPlayer", True),
    ("lua_bridge_game_camera.c", "l_camera_lib"): ("picocalc.game.camera", False),
    ("lua_bridge_game_camera.c", "l_camera_methods"): ("PicoDeckCamera", True),
    ("lua_bridge_game_save.c", "l_save_lib"): ("picocalc.game.save", False),
    ("lua_bridge_game_scene.c", "l_scene_lib"): ("picocalc.game.scene", False),
    ("lua_bridge_sys.c", "l_sys_lib"): ("picocalc.sys", False),
    ("lua_bridge_graphics.c", "l_graphics_lib"): ("picocalc.graphics", False),
    ("lua_bridge_graphics.c", "l_graphics_image_lib"): ("picocalc.graphics.image", False),
    ("lua_bridge_graphics.c", "l_graphics_image_methods"): ("PicoDeckImage", True),
    ("lua_bridge_graphics.c", "l_sprite_lib"): ("picocalc.graphics.sprite", False),
    ("lua_bridge_graphics.c", "l_sprite_methods"): ("PicoDeckSprite", True),
    ("lua_bridge_graphics.c", "l_spritesheet_lib"): ("picocalc.graphics.spritesheet", False),
    ("lua_bridge_graphics.c", "l_spritesheet_methods"): ("PicoDeckSpritesheet", True),
    ("lua_bridge_graphics.c", "l_tilemap_lib"): ("picocalc.graphics.tilemap", False),
    ("lua_bridge_graphics.c", "l_tilemap_methods"): ("PicoDeckTilemap", True),
    ("lua_bridge_graphics.c", "l_animation_loop_lib"): ("picocalc.graphics.animation.loop", False),
    ("lua_bridge_graphics.c", "l_animation_loop_methods"): ("PicoDeckAnimationLoop", True),
    ("lua_bridge_graphics.c", "l_animator_lib"): ("picocalc.graphics.animation.animator", False),
    ("lua_bridge_graphics.c", "l_animator_methods"): ("PicoDeckAnimator", True),
    ("lua_bridge_graphics.c", "l_animation_blinker_lib"): ("picocalc.graphics.animation.blinker", False),
    ("lua_bridge_graphics.c", "l_animation_blinker_methods"): ("PicoDeckBlinker", True),
    ("lua_bridge_graphics.c", "l_font_lib"): ("picocalc.graphics.font", False),
    ("lua_bridge_graphics.c", "l_font_methods"): ("PicoDeckFont", True),
    ("lua_bridge_perf.c", "l_perf_lib"): ("picocalc.perf", False),
    ("lua_bridge_ui.c", "l_ui_lib"): ("picocalc.ui", False),
    ("lua_bridge_tcp.c", "l_tcp_lib"): ("picocalc.tcp", False),
    ("lua_bridge_tcp.c", "l_tcp_methods"): ("PicoDeckTcpConn", True),
    ("lua_bridge_sound.c", "sound_funcs"): ("picocalc.sound", False),
    ("lua_bridge_sound.c", "sound_sample_methods"): ("PicoDeckSample", True),
    ("lua_bridge_sound.c", "sound_player_methods"): ("PicoDeckSamplePlayer", True),
    ("lua_bridge_sound.c", "sound_fileplayer_methods"): ("PicoDeckFilePlayer", True),
    ("lua_bridge_sound.c", "sound_mp3player_methods"): ("PicoDeckMp3Player", True),
}

# "<owner>.<name>" -> reason. Skipped entirely (args and returns).
ALLOW = {
    "picocalc.sys.applyUpdate": "registered with lua_setfield, and only for an "
                                "app that declares \"system-update\"",
    "picocalc.graphics.imageWithText": "the trailing font argument (5) is read "
                                       "by render_text_image()",
    "picocalc.graphics.sprite.setClipRectsInRange": "two calling forms (a "
        "rect table, or four integers); the stub declares the table form and "
        "an @overload",
    "picocalc.gfx3d.draw": "the optional scale/bias/sortAsOne (8-10) are read "
                           "by draw_instance()",
    "picocalc.gfx3d.drawBasis": "the optional scale/bias/sortAsOne (11-13) "
                                "are read by draw_instance()",
    "PicoDeckImage.setPixels": "the optional rect (3-6) is read by "
                               "image_check_rect() from a computed index",
}

# A reader is any call `name(L, <literal>` whose name says it reads or checks
# an argument. Hard readers error on a missing argument; optional readers
# (opt*) do not; soft readers (lua_to*) coerce a missing argument; the rest
# (lua_is*, lua_type) only probe it.
READ_RE = re.compile(
    r"\b(\w*(?:check|opt|arg|get|read)\w*|lua_to\w+|lua_is\w+|lua_type)"
    r"\s*\(\s*L\s*,\s*(\d+)\b", re.I)
NOT_READERS = {"luaL_checkstack", "lua_getfield", "lua_geti", "lua_gettable",
               "lua_getmetatable", "lua_getiuservalue", "lua_gettop",
               "lua_rawgeti", "lua_getglobal", "lua_rawget", "lua_rawgetp",
               "luaL_getmetafield", "luaL_getsubtable", "luaL_getmetatable"}
CALL_RE = re.compile(r"\b(\w+)\s*\(\s*L\s*\)")
GETTOP_RE = re.compile(r"lua_gettop\s*\(\s*L\s*\)\s*(>=|>|==|<)\s*(\d+)")
# luaL_checkoption with a default (anything but NULL) is optional.
OPTION_DEFAULT_RE = re.compile(r"luaL_checkoption\s*\(\s*L\s*,\s*(\d+)\s*,\s*(?=\S)(?!NULL\b)")
RETURN_RE = re.compile(r"\breturn\s+([^;]+);")
RAISE_RE = re.compile(r"(?:luaL_error|luaL_argerror|luaL_typeerror|lua_error|"
                      r"luaL_argexpected)\b")


LEX_RE = re.compile(r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.S)


def strip_comments(src: str) -> str:
    """Drop comments, leaving string and char literals alone (a `//` inside a
    string is not a comment)."""
    def sub(m):
        t = m.group(0)
        return t if t[0] in "\"'" else "\n" * t.count("\n")
    return LEX_RE.sub(sub, src)


def blank_literals(src: str) -> str:
    """Same length, string and char contents replaced by spaces, so a brace
    inside a literal is not counted."""
    return LEX_RE.sub(lambda m: m.group(0)[0] + " " * (len(m.group(0)) - 2) + m.group(0)[0]
                      if m.group(0)[0] in "\"'" else m.group(0), src)


def c_functions(src: str) -> dict[str, str]:
    """Function name -> body, for every function definition."""
    out = {}
    blank = blank_literals(src)
    for m in re.finditer(r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;{}]*\)\s*\{", blank, re.M):
        i, depth = m.end(), 1
        while i < len(blank) and depth:
            depth += {"{": 1, "}": -1}.get(blank[i], 0)
            i += 1
        out[m.group(1)] = src[m.end():i]
    return out


def reg_tables(src: str) -> dict[str, list[tuple[str, str]]]:
    tables = {}
    for m in re.finditer(r"^static const luaL_Reg (\w+)\[\]\s*=\s*\{(.*?)\n?\};", src, re.M | re.S):
        entries = re.findall(r"\{\s*\"(\w+)\"\s*,\s*(\w+)\s*\}", m.group(2))
        tables[m.group(1)] = entries
    return tables


def own_returns(body: str, funcs: dict[str, str], seen: frozenset = frozenset()) -> set:
    """Literal return counts of `body` itself (not of helpers it calls).
    `return callee(L);` takes the callee's; anything else non-literal is None."""
    rets = set()
    for expr in RETURN_RE.findall(body):
        expr = expr.strip()
        m = re.fullmatch(r"(\w+)\s*\(\s*L\s*\)", expr)
        if RAISE_RE.match(expr):
            continue  # raises: returns nothing to the caller
        if m and m.group(1) in funcs and m.group(1) not in seen:
            rets |= own_returns(funcs[m.group(1)], funcs, seen | {m.group(1)})
        else:
            rets.add(int(expr) if re.fullmatch(r"\d+", expr) else None)
    return rets


def analyse_c(body: str, method: bool,
              funcs: dict[str, str] | None = None) -> dict:
    """Argument and return facts of one bridge function.

    A callee taking only `L` (a delegating wrapper or a shared helper) is read
    for its arguments as if inlined.
    """
    funcs = funcs or {}
    own = body
    for callee in set(CALL_RE.findall(body)):
        if callee in funcs:
            body = body + "\n" + funcs[callee]
    hi = hard = nonopt = 0
    reads = [(n, int(i)) for n, i in READ_RE.findall(body)
             if n not in NOT_READERS]
    # An index that is probed (lua_is*, lua_type, lua_gettop) is treated as
    # optional: `lua_isnoneornil(L, 2) ? dflt : luaL_checkstring(L, 2)`.
    probed = {i for n, i in reads if n.lower().startswith(("lua_is", "lua_type"))}
    probed |= {int(n) for _, n in GETTOP_RE.findall(body)}
    for var in re.findall(r"(\w+)\s*=\s*lua_gettop\s*\(\s*L\s*\)", body):
        probed |= {int(n) for n in re.findall(
            rf"\b{var}\s*(?:>=|>|==)\s*(\d+)", body)}
    defaulted = {int(n) for n in OPTION_DEFAULT_RE.findall(body)}
    for name, n in reads:
        low = name.lower()
        hi = max(hi, n)
        if low == "lual_checkoption" and n in defaulted:
            continue
        if "opt" not in low:
            nonopt = max(nonopt, n)
        if n not in probed and low.startswith(("lual_check", "lb_check", "check_")):
            hard = max(hard, n)
    for op, n in GETTOP_RE.findall(body):
        hi = max(hi, int(n) if op != "<" else 0)
    off = 1 if method else 0
    return {"total": max(hi - off, 0), "required": max(hard - off, 0),
            "nonopt": max(nonopt - off, 0), "returns": own_returns(own, funcs)}


def _return_count(line: str) -> int:
    """`---@return T name` is one return. `---@return a x, b y` (top-level
    commas, every part exactly a type and a name) is N; a comma in a
    description is not a second return."""
    if not line.startswith("---@return"):
        return 0
    body = line[len("---@return"):].strip()
    parts, depth, cur = [], 0, ""
    for ch in body:
        depth += ch in "<{(["
        depth -= ch in ">})]"
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    if len(parts) > 1 and all(len(p.split()) == 2 for p in parts):
        return len(parts)
    return 1


def parse_stub(text: str) -> dict[str, dict]:
    """'<owner>.<name>' -> facts from the stub's annotations."""
    out = {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"function ([\w.]+)([.:])(\w+)\s*\(([^)]*)\)", line)
        if not m:
            continue
        owner, _, name, plist = m.groups()
        params = [p.strip() for p in plist.split(",") if p.strip()]
        block = []
        j = i - 1
        while j >= 0 and lines[j].startswith("---"):
            block.append(lines[j])
            j -= 1
        optional = set()
        for b in block:
            pm = re.match(r"---@param\s+(\w+)(\?)?\s+(\S+)", b)
            if pm and (pm.group(2) or "nil" in pm.group(3).split("|")):
                optional.add(pm.group(1))
        if "..." in params:
            continue
        nreq = 0
        for p in params:
            if p in optional:
                break
            nreq += 1
        out[f"{owner}.{name}"] = {
            "line": i + 1, "total": len(params), "required": nreq,
            "returns": sum(_return_count(b) for b in block),
            "params": params}
    return out


def bridge_facts(bridge_dir: Path = BRIDGE_DIR) -> dict[str, dict]:
    facts = {}
    for path in sorted(bridge_dir.glob("lua_bridge*.c")):
        src = strip_comments(path.read_text())
        funcs, tables = c_functions(src), reg_tables(src)
        for table, entries in tables.items():
            if (path.name, table) not in OWNERS:
                continue
            owner, method = OWNERS[(path.name, table)]
            for lua_name, cfn in entries:
                f = analyse_c(funcs[cfn], method, funcs) if cfn in funcs else None
                facts[f"{owner}.{lua_name}"] = {"cfn": cfn, "file": path.name, "facts": f}
    return facts


def check(stub_text: str, bridge_dir: Path = BRIDGE_DIR) -> list[str]:
    stub = parse_stub(stub_text)
    bridge = bridge_facts(bridge_dir)
    owners = {o for o, _ in OWNERS.values()}
    problems = []
    for key, s in sorted(stub.items(), key=lambda kv: kv[1]["line"]):
        owner = key.rsplit(".", 1)[0]
        if key in ALLOW:
            continue
        if owner not in owners:
            if owner.startswith("picocalc."):
                problems.append(f"stub line {s['line']}: {key}: {owner} is not a "
                                "table the bridge registers (moved or renamed?); "
                                "a new table needs an OWNERS entry in "
                                "tools/check_lua_stub.py")
            continue
        b = bridge.get(key)
        where = f"stub line {s['line']}: {key}({', '.join(s['params'])})"
        if b is None:
            problems.append(f"{where}: no such function registered in the bridge")
            continue
        f = b["facts"]
        if f is None:
            continue
        src = f"{b['file']} {b['cfn']}"
        if s["total"] < f["total"] or (s["total"] > f["total"] > 0):
            problems.append(f"{where}: stub declares {s['total']} params, "
                            f"{src} reads {f['total']}")
        elif f["total"] and not f["required"] <= s["required"] <= f["nonopt"]:
            problems.append(f"{where}: stub has {s['required']} required params, "
                            f"{src} requires {f['required']} and reads "
                            f"{f['nonopt']} without a default")
        known = {r for r in f["returns"] if r is not None}
        if known and None not in f["returns"] and s["returns"] not in known:
            problems.append(f"{where}: stub has {s['returns']} @return, "
                            f"{src} returns {sorted(known)}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("stub", nargs="?", type=Path, default=STUB)
    problems = check(ap.parse_args().stub.read_text())
    for p in problems:
        print(p)
    print(f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
