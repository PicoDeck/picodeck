#!/usr/bin/env python3
"""Guard sdk/lua/picocalc.lua (the LuaLS stub) against drift from the bridge.

The stub is hand-written and the only signature reference a Lua app has; a
wrong entry raises no error (arguments are ignored or reinterpreted), so it
is checked mechanically, in both directions.

Stub to bridge. For every Lua name registered in a luaL_Reg table of
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
    stub's `---@return` count must be the largest of them (0 when there is no
    `---@return`), so a failure path's `nil, err` is declared too.
  - A stub function, method, constant or property with no bridge entry under
    that owner is drift too (a name moved or renamed).

Functions the heuristics cannot read (variadic, computed indices, dispatch by
type, helper functions that read the arguments) go in ALLOW below, each with
its reason.

Bridge to stub. firmware_surface() walks lua_bridge_register() with a small
symbolic Lua stack, so what a name is called comes from the code that registers
it: every function, constant and sub-table reachable from `picocalc` and `_G`,
the method tables of lb_register_type(), and the property names of a metatable's
__index function. Each needs a stub entry, or a line in UNSTUBBED with its
reason. A luaL_Reg table the walk reaches must be in OWNERS, under the path it
is registered at. The stub's own quality is checked too: every parameter has a
---@param, every ---@param a parameter, and every function a description.

system/lib/*.lua is out of scope: those are documented libraries, not bridge
names, and the stub does not describe them.

Exit status 0 when everything matches; 1 with a list of mismatches otherwise.
Also imported by tests/unit/test_lua_stub.py.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import NamedTuple

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
    ("lua_bridge_gamepad.c", "l_gamepad_lib"): ("picocalc.gamepad", False),
    ("lua_bridge_video.c", "video_funcs"): ("picocalc.video", False),
    ("lua_bridge_video.c", "video_methods"): ("PicoDeckVideoPlayer", True),
    ("lua_bridge_crypto.c", "l_crypto_lib"): ("picocalc.crypto", False),
    ("lua_bridge_crypto.c", "l_aes_ctr_methods"): ("PicoDeckAesCtr", True),
    ("lua_bridge_crypto.c", "l_ecdh_methods"): ("PicoDeckEcdh", True),
    ("lua_bridge_gfx3d.c", "l_gfx3d_lib"): ("picocalc.gfx3d", False),
    ("lua_bridge_gfx3d.c", "l_mesh_methods"): ("PicoDeckMesh", True),
    ("lua_bridge_gfx3d.c", "l_tmesh_methods"): ("PicoDeckTexturedMesh", True),
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
    ("lua_bridge_chargrid.c", "l_chargrid_lib"): ("picocalc.chargrid", False),
    ("lua_bridge_chargrid.c", "l_noise_lib"): ("picocalc.noise", False),
    ("lua_bridge_chargrid.c", "l_grid_methods"): ("PicoDeckCharGrid", True),
    ("lua_bridge_chargrid.c", "l_shader_methods"): ("PicoDeckCharShader", True),
    ("lua_bridge_display.c", "l_fb_methods"): ("PicoDeckFramebuffer", True),
    ("lua_bridge_game_scene.c", "l_pool_methods"): ("PicoDeckObjectPool", True),
}

# "<owner>.<name>" -> reason. Skipped entirely (args and returns).
ALLOW = {
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
    "PicoDeckFramebuffer.getPixels": "the rect (2-5) is read by fb_check_rect() "
                                     "from a computed index",
    "PicoDeckFramebuffer.setPixels": "the rect (3-6) is read by fb_check_rect() "
                                     "from a computed index",
    "picocalc.graphics.sprite.spriteWithText": "the trailing font argument (5) is "
                                               "read by render_text_image()",
    "require": "after its one argument it uses stack slots 2-3 for itself, which "
               "the argument count cannot tell from arguments",
}

# "<owner>.<name>" -> reason. Registered by the firmware, on purpose no stub entry.
UNSTUBBED = {
    "load": "Lua's own load with bytecode refused: LuaLS's builtin entry describes it",
}

# "<class>.<field>" -> reason. A ---@field on an object type must be a property
# of its __index function, unless it is listed here.
PLAIN_FIELDS = {
    "PicoDeckObjectPool.name": "objectPool() sets it on the plain table it returns",
}

# A reader is any call `name(L, <literal>` whose name says it reads or checks
# an argument. Hard readers error on a missing argument; optional readers
# (opt*) do not; soft readers (lua_to*) coerce a missing argument; the rest
# (lua_is*, lua_type) only probe it.
READ_RE = re.compile(
    r"\b(\w*(?:check|opt|arg|get|read)\w*|qint|lua_to\w+|lua_is\w+|lua_type)"
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
    for m in re.finditer(r"^[ \t]*(?:static\s+)?const\s+luaL_Reg\s+(\w+)\[\]\s*=\s*\{(.*?)\};",
                         src, re.M | re.S):
        entries = re.findall(r"\{\s*\"(\w+)\"\s*,\s*(\w+)\s*\}", m.group(2))
        tables[m.group(1)] = entries
    return tables


class _Value:
    """One value on the symbolic Lua stack of firmware_surface()."""

    def __init__(self, kind: str, file: str | None = None, cfn: str | None = None,
                 literal: str | None = None):
        self.kind, self.file, self.cfn, self.literal = kind, file, cfn, literal
        self.fields: dict[str, _Value] = {}
        self.reg: tuple[str, str] | None = None  # the luaL_Reg table it was filled from


class LuaType(NamedTuple):
    """One lb_register_type() call."""
    file: str
    metatable: str
    methods: str | None
    meta: str | None


class Surface(NamedTuple):
    """What the firmware registers."""
    names: dict[str, _Value]  # "picocalc.ui.toast", "require": functions, constants, tables
    types: list[LuaType]
    regs: dict[str, dict[str, list[tuple[str, str]]]]  # file -> luaL_Reg tables
    funcs: dict[str, dict[str, str]]  # file -> C function bodies


class BridgeWalkError(Exception):
    """The registration code did something firmware_surface() cannot follow."""


_L_CALL_RE = re.compile(r"\b(\w+)\s*\(\s*L\s*[,)]")
_PUSH_KINDS = {
    "lua_newtable": "table", "lua_createtable": "table", "luaL_newmetatable": "table",
    "luaL_requiref": "table", "lua_newuserdatauv": "userdata",
    "lua_pushinteger": "const", "lua_pushnumber": "const", "lua_pushboolean": "const",
    "lua_pushstring": "const", "lua_pushliteral": "const", "lua_pushlstring": "const",
    "lua_pushfstring": "const", "lua_pushlightuserdata": "const", "lua_pushnil": "nil",
    "lua_rawgetp": "unknown", "lua_rawgeti": "unknown", "lua_geti": "unknown",
    "luaL_getmetatable": "unknown",
}
_POPS_ONE = {"lua_setmetatable", "lua_rawsetp", "lua_rawseti", "lua_seti", "lua_replace",
             "lua_remove", "lua_setiuservalue", "luaL_ref"}


def _split_args(text: str) -> list[str]:
    out, depth, cur, quote = [], 0, "", None
    for ch in text:
        if quote:
            cur += ch
            quote = None if ch == quote else quote
            continue
        if ch in "\"'":
            quote = ch
        depth += ch in "([{"
        depth -= ch in ")]}"
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    return out + [cur.strip()] if cur.strip() else out


def _quoted(arg: str) -> str | None:
    m = re.fullmatch(r'"(.*)"', arg.strip())
    return m.group(1) if m else None


def firmware_surface(bridge_dir: Path = BRIDGE_DIR) -> Surface:
    """Walk lua_bridge_register() and every helper it calls with `L`.

    Both arms of a conditional run in sequence: the registration code only
    uses `if` around balanced stack work, and a stack that does not come back
    empty raises BridgeWalkError. So an `if (requirement)` name (applyUpdate,
    sysconfig) counts as registered, and a simulator-only #ifdef is walked too.
    """
    src = {p.name: strip_comments(p.read_text())
           for p in sorted(bridge_dir.glob("lua_bridge*.c"))}
    funcs = {f: c_functions(text) for f, text in src.items()}
    regs = {f: reg_tables(text) for f, text in src.items()}
    home: dict[str, str] = {}
    for f, table in funcs.items():
        for name in table:
            home.setdefault(name, f)

    root = _Value("table")
    stack: list[_Value] = []
    types: list[LuaType] = []

    def pop(n: int) -> None:
        if n > len(stack):
            raise IndexError(f"popping {n} of {len(stack)}")
        del stack[len(stack) - n:]

    def fill(t: _Value, file: str, table: str) -> None:
        if table not in regs[file]:
            raise BridgeWalkError(f"{file}: no luaL_Reg table named {table}")
        t.reg = (file, table)
        for lua_name, cfn in regs[file][table]:
            t.fields[lua_name] = _Value("func", file, cfn)

    def set_named(t: _Value, file: str, name_arg: str, v: _Value) -> None:
        name = _quoted(name_arg)
        if name is not None:
            t.fields[name] = v
            return
        # A constants loop: `lua_setfield(L, -2, k_consts[i].name)`.
        m = re.fullmatch(r"(\w+)\[\w+\]\.\w+", name_arg)
        arr = m and re.search(rf"\b{m.group(1)}\[\]\s*=\s*\{{(.*?)\}};", src[file], re.S)
        if not arr:
            raise BridgeWalkError(f"{file}: cannot read the field name {name_arg!r}")
        for name in re.findall(r'\{\s*"(\w+)"', arr.group(1)):
            t.fields[name] = v

    def run(file: str, fn: str, active: tuple = ()) -> None:
        body = funcs[file][fn]
        blank = blank_literals(body)
        for m in _L_CALL_RE.finditer(blank):
            name = m.group(1)
            open_paren = blank.index("(", m.start())
            i, depth = open_paren + 1, 1
            while depth:  # to the call's closing parenthesis
                depth += {"(": 1, ")": -1}.get(blank[i], 0)
                i += 1
            args = _split_args(body[open_paren + 1:i - 1])
            if name in _PUSH_KINDS:
                last = args[-1] if len(args) > 1 else ""
                stack.append(_Value(_PUSH_KINDS[name], file, literal=_quoted(last)))
            elif name == "lua_pushcfunction":
                stack.append(_Value("func", file, args[1]))
            elif name == "lua_pushcclosure":
                pop(int(args[2]))
                stack.append(_Value("func", file, args[1]))
            elif name == "lua_pushvalue":
                stack.append(stack[int(args[1])])
            elif name in ("lua_getfield", "lua_getglobal"):
                t = root if name == "lua_getglobal" else (
                    None if args[1] == "LUA_REGISTRYINDEX" else stack[int(args[1])])
                key = _quoted(args[-1])
                stack.append(t.fields.get(key, _Value("unknown")) if t else _Value("unknown"))
            elif name == "lua_setfield":
                v = stack.pop()
                if args[1] != "LUA_REGISTRYINDEX":
                    # The index counts the value just popped.
                    set_named(stack[int(args[1]) + 1], file, args[2], v)
            elif name == "lua_setglobal":
                root.fields[_quoted(args[1])] = stack.pop()
            elif name == "lua_settable":
                v, k = stack.pop(), stack.pop()
                stack[-1].fields[k.literal] = v
            elif name == "lua_rawset":
                pop(2)
            elif name in _POPS_ONE:
                stack.pop()
            elif name == "lua_pop":
                pop(int(args[1]))
            elif name in ("lua_call", "lua_pcall"):
                pop(int(args[1]) + 1)
                stack.extend(_Value("unknown") for _ in range(int(args[2])))
            elif name == "luaL_setfuncs":
                pop(int(args[2]))
                fill(stack[-1], file, args[1])
            elif name == "register_subtable":
                t = _Value("table", file)
                fill(t, file, args[2])
                stack[-1].fields[_quoted(args[1])] = t
            elif name == "lb_register_type":
                none = lambda a: None if a == "NULL" else a  # noqa: E731
                types.append(LuaType(file, args[1], none(args[2]), none(args[3])))
            elif name in funcs[file] or name in home:
                target = file if name in funcs[file] else home[name]
                if name not in active:
                    run(target, name, active + (name,))

    names: dict[str, _Value] = {}

    def flatten(t: _Value, prefix: str) -> None:
        for k, v in t.fields.items():
            names[prefix + k] = v
            if v.kind == "table":
                flatten(v, f"{prefix}{k}.")

    try:
        run("lua_bridge.c", "lua_bridge_register")
        flatten(root, "")  # a name it could not read is None: TypeError
    except (IndexError, KeyError, ValueError, TypeError) as e:
        raise BridgeWalkError(f"lost track of the Lua stack: {e!r}") from e
    if stack:
        raise BridgeWalkError(f"{len(stack)} value(s) left on the Lua stack")

    # lua_runner.c sets these just before the app runs, not in the register walk.
    runner = bridge_dir / "lua_runner.c"
    if runner.exists():
        text = strip_comments(runner.read_text())
        for g in re.findall(r'lua_setglobal\(\s*L\s*,\s*"(\w+)"\s*\)', text):
            names[g] = _Value("const", runner.name)
    return Surface(names, types, regs, funcs)


class StubSurface(NamedTuple):
    """Every name the stub declares, with what it takes to resolve one."""
    names: dict[str, int]  # functions and assignments -> line
    classes: set[str]
    fields: dict[str, dict[str, int]]  # class -> ---@field name -> line
    aliases: dict[str, str]  # `picocalc.appconfig = picocalc.config`

    def has(self, name: str) -> bool:
        for lhs, rhs in self.aliases.items():
            if name == lhs or name.startswith(lhs + "."):
                name = rhs + name[len(lhs):]
                break
        owner, _, leaf = name.rpartition(".")
        return name in self.names or name in self.classes or leaf in self.fields.get(owner, {})


def stub_surface(text: str) -> StubSurface:
    names: dict[str, int] = {}
    classes: set[str] = set()
    fields: dict[str, dict[str, int]] = {}
    aliases: dict[str, str] = {}
    cls = None
    for n, line in enumerate(text.splitlines(), 1):
        if not line.startswith("---"):
            cls = None
        if m := re.match(r"---@class\s+([\w.]+)", line):
            cls = m.group(1)
            classes.add(cls)
        elif (m := re.match(r"---@field\s+(\w+)", line)) and cls:
            fields.setdefault(cls, {})[m.group(1)] = n
        elif m := re.match(r"function\s+([\w.:]+)\s*\(", line):
            names[m.group(1).replace(":", ".")] = n
        elif m := re.match(r"([A-Za-z_][\w.]*)\s*=\s*([A-Za-z_][\w.]*)\s*$", line):
            aliases[m.group(1)] = m.group(2)
        elif m := re.match(r"([A-Za-z_][\w.]*)\s*=", line):
            names[m.group(1)] = n
    return StubSurface(names, classes, fields, aliases)


def _properties(body: str) -> set[str]:
    """The keys an __index / __newindex function compares against."""
    return set(re.findall(r'strcmp\(\s*\w+\s*,\s*"(\w+)"\s*\)', body))


def surface_problems(stub_text: str, bridge_dir: Path = BRIDGE_DIR) -> list[str]:
    """Bridge to stub, and the stub's constants and properties back to the bridge."""
    try:
        fw = firmware_surface(bridge_dir)
    except BridgeWalkError as e:
        return [f"cannot read lua_bridge_register(): {e}. firmware_surface() in "
                "tools/check_lua_stub.py needs to learn this"]
    stub = stub_surface(stub_text)
    problems = []

    def need(name: str, source: str) -> None:
        if name not in UNSTUBBED and not stub.has(name):
            problems.append(f"{source} registers {name}: no entry in the stub. Add one to "
                            "sdk/lua/picocalc.lua, or a line with the reason to UNSTUBBED "
                            "in tools/check_lua_stub.py")

    for path, v in sorted(fw.names.items()):
        if v.kind == "nil":
            continue  # removed from the sandbox (dofile, loadfile)
        need(path, f"{v.file}{' ' + v.cfn if v.cfn else ''}")
        if v.kind == "table" and v.reg:
            owner = OWNERS.get(v.reg)
            target = stub.aliases.get(path, path)
            if owner is None:
                problems.append(f"{v.reg[0]} {v.reg[1]} is registered as {path} and not in "
                                "OWNERS, so it is not signature-checked: add it")
            elif owner[0] != target:
                problems.append(f"OWNERS says {v.reg[0]} {v.reg[1]} is {owner[0]}, but it "
                                f"is registered as {path}")
    reached = {v.reg for v in fw.names.values() if v.reg} | {(t.file, t.methods) for t in fw.types}
    for key in sorted(set(OWNERS) - reached):
        problems.append(f"OWNERS lists {key[0]} {key[1]}, but lua_bridge_register() never "
                        "registers it")
    for t in fw.types:
        methods = fw.regs[t.file].get(t.methods, []) if t.methods else []
        owner = OWNERS.get((t.file, t.methods))
        if methods and owner is None:
            problems.append(f"{t.file} {t.methods} is the method table of metatable "
                            f"{t.metatable} and not in OWNERS, so it is not checked: add it")
            continue
        cls = owner[0] if owner else None
        for lua_name, cfn in methods:
            need(f"{cls}.{lua_name}", f"{t.file} {cfn}")
        props = set()
        for meta_name, cfn in fw.regs[t.file].get(t.meta, []) if t.meta else []:
            if meta_name in ("__index", "__newindex"):
                props |= _properties(fw.funcs[t.file].get(cfn, ""))
        for prop in sorted(props):
            need(f"{cls}.{prop}", f"{t.file} __index of {t.metatable}")
        if cls is not None:
            for prop, line in sorted(stub.fields.get(cls, {}).items()):
                if prop not in props and f"{cls}.{prop}" not in PLAIN_FIELDS:
                    problems.append(f"stub line {line}: {cls}.{prop}: no such property in "
                                    f"the __index of {t.metatable}")
    # A module table's @fields and constants must exist. A class no module table is
    # named after is a shape (an event, a stat result), not a table.
    for cls, declared in sorted(stub.fields.items()):
        if cls in fw.names and fw.names[cls].kind == "table":
            for f, line in sorted(declared.items()):
                if f not in fw.names[cls].fields:
                    problems.append(f"stub line {line}: {cls}.{f}: the firmware registers "
                                    "no such constant")
    for name, line in sorted(stub.names.items(), key=lambda kv: kv[1]):
        owner, _, leaf = name.rpartition(".")
        if owner in fw.names and fw.names[owner].kind == "table" and leaf not in fw.names[owner].fields:
            problems.append(f"stub line {line}: {name}: the firmware registers no such name")
        elif not owner and name not in fw.names:
            problems.append(f"stub line {line}: {name}: the firmware sets no such global")
    return problems


def own_returns(body: str, funcs: dict[str, str], seen: frozenset = frozenset()) -> set:
    """Literal return counts of `body` itself (not of helpers it calls).
    `return callee(L, ...);` takes the callee's; anything else non-literal is None."""
    rets = set()
    for expr in RETURN_RE.findall(body):
        expr = expr.strip()
        m = re.fullmatch(r"(\w+)\s*\(\s*L\b[^()]*(?:\([^()]*\)[^()]*)*\)", expr, re.S)
        if RAISE_RE.match(expr):
            continue  # raises: returns nothing to the caller
        if m and m.group(1) in funcs and m.group(1) not in seen:
            rets |= own_returns(funcs[m.group(1)], funcs, seen | {m.group(1)})
        elif t := re.fullmatch(r"[^?]+\?\s*(\d+)\s*:\s*(\d+)", expr):  # `ok ? 1 : 2`
            rets |= {int(t.group(1)), int(t.group(2))}
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
        if n not in probed and low.startswith(("lual_check", "lb_check", "check_", "qint")):
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
    """'<owner>.<name>' (or a global's bare name) -> facts from the stub's annotations."""
    out = {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"function (?:([\w.]+)[.:])?(\w+)\s*\(([^)]*)\)", line)
        if not m:
            continue
        owner, name, plist = m.groups()
        params = [p.strip() for p in plist.split(",") if p.strip()]
        block = []
        j = i - 1
        while j >= 0 and lines[j].startswith("---"):
            block.append(lines[j])
            j -= 1
        optional, annotated = set(), set()
        for b in block:
            pm = re.match(r"---@param\s+(\w+|\.\.\.)(\?)?\s+(\S+)", b)
            if pm:
                annotated.add(pm.group(1))
                if pm.group(2) or "nil" in pm.group(3).split("|"):
                    optional.add(pm.group(1))
        fixed = [p for p in params if p != "..."]
        nreq = 0
        for p in fixed:
            if p in optional:
                break
            nreq += 1
        out[f"{owner}.{name}" if owner else name] = {
            "line": i + 1, "total": len(fixed), "required": nreq,
            "returns": sum(_return_count(b) for b in block),
            "params": params, "variadic": "..." in params, "annotated": annotated,
            "optional": optional,
            "described": any(not b.startswith("---@") for b in block)}
    return out


def bridge_facts(bridge_dir: Path = BRIDGE_DIR) -> dict[str, dict]:
    facts = {}
    all_funcs = {}
    for path in sorted(bridge_dir.glob("lua_bridge*.c")):
        src = strip_comments(path.read_text())
        funcs, tables = c_functions(src), reg_tables(src)
        all_funcs[path.name] = funcs
        for table, entries in tables.items():
            if (path.name, table) not in OWNERS:
                continue
            owner, method = OWNERS[(path.name, table)]
            for lua_name, cfn in entries:
                f = analyse_c(funcs[cfn], method, funcs) if cfn in funcs else None
                facts[f"{owner}.{lua_name}"] = {"cfn": cfn, "file": path.name, "facts": f}
    # Functions set one at a time with lua_setfield/lua_setglobal, not by a table.
    try:
        names = firmware_surface(bridge_dir).names
    except BridgeWalkError:
        return facts  # surface_problems() reports it
    for path, v in names.items():
        if v.kind == "func" and path not in facts and v.cfn in all_funcs.get(v.file, {}):
            funcs = all_funcs[v.file]
            facts[path] = {"cfn": v.cfn, "file": v.file,
                           "facts": analyse_c(funcs[v.cfn], False, funcs)}
    return facts


def annotation_problems(stub: dict[str, dict]) -> list[str]:
    """LuaLS shows what is written here, so what is missing here is missing there."""
    problems = []
    for key, s in sorted(stub.items(), key=lambda kv: kv[1]["line"]):
        where = f"stub line {s['line']}: {key}"
        for p in s["params"]:
            if p not in s["annotated"]:
                problems.append(f"{where}: parameter {p} has no ---@param")
        for p in sorted(s["annotated"] - set(s["params"])):
            problems.append(f"{where}: ---@param {p} names no parameter")
        if not s["described"]:
            problems.append(f"{where}: no description line")
    return problems


def check(stub_text: str, bridge_dir: Path = BRIDGE_DIR) -> list[str]:
    stub = parse_stub(stub_text)
    bridge = bridge_facts(bridge_dir)
    owners = {o for o, _ in OWNERS.values()}
    problems = []
    for key, s in sorted(stub.items(), key=lambda kv: kv[1]["line"]):
        owner = key.rsplit(".", 1)[0]
        if key in ALLOW:
            continue
        if owner not in owners and "." in key and key not in bridge:
            problems.append(f"stub line {s['line']}: {key}: {owner} is not a table or "
                            "class the bridge registers (moved or renamed?); a new one "
                            "needs an OWNERS entry in tools/check_lua_stub.py")
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
        if not s["variadic"]:
            if s["total"] < f["total"] or (s["total"] > f["total"] > 0):
                problems.append(f"{where}: stub declares {s['total']} params, "
                                f"{src} reads {f['total']}")
            elif f["total"] and not f["required"] <= s["required"] <= f["nonopt"]:
                problems.append(f"{where}: stub has {s['required']} required params, "
                                f"{src} requires {f['required']} and reads "
                                f"{f['nonopt']} without a default")
        known = {r for r in f["returns"] if r is not None}
        if known and None not in f["returns"] and s["returns"] != max(known):
            problems.append(f"{where}: stub has {s['returns']} @return, "
                            f"{src} returns {sorted(known)}: declare the longest, "
                            "so a failure path's values are not lost")
    return problems + annotation_problems(stub) + surface_problems(stub_text, bridge_dir)


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
