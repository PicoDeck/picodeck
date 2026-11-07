"""The gfx3d table (API version 11) from a native app, and the three sys entries
that came with it: getFirmwareId, getClockKhz, resetIdleTimer.

tests/e2e/native/gfx3d_probe.c is one ELF with one behaviour per mode; the mode is
the app's name in app.json. It goes through `api->gfx3d` as an app does, so on the
simulator every argument crosses the Unicorn trampolines, and on the device the
per-launch tracking table. The `both` tests expect the same pixels and numbers on
each. What a mode finds is read from /data/<id>/probe.txt: serial lines can be
dropped on the device. Pixels are compared as RGB565, the framebuffer's own values.

The scene: fovY 90 degrees and near 1 (the probe's context()), so a point at depth 2
lands on screen at 160 + 80 x, 160 - 80 y. Texture rows run down the screen.
"""
from __future__ import annotations

import io
import json
import math
import re
import shutil
import struct
import subprocess
import tempfile
import time
from pathlib import Path

import pytest
from PIL import Image

from helpers import PROJECT_ROOT
from hw_target import SimTarget

pytestmark = pytest.mark.native

PROBE = Path(__file__).parent / "fixtures" / "native_gfx3d"

CLEAR_COLOR = 0x2104
NEUTRAL = 0xAD55
WHITE = 0xFFFF
SPR_COLOR = 0x07E0
TEX4 = [0xF800, 0x07E0, 0x001F, 0xFFFF,
        0x8410, 0xF81F, 0xFFE0, 0x07FF,
        0x4208, 0xC618, 0x2945, 0xA514,
        0x0000, 0x7800, 0x03E0, 0x000F]


def _split(c):
    return (c >> 11) & 31, (c >> 5) & 63, c & 31


def _tex_times(texel, vertex):
    """src/os/gfx3d_tex.h: a channel at its maximum in both means x1.5, saturating."""
    tr, tg, tb = _split(texel)
    vr, vg, vb = _split(vertex)
    mr = int(vr * (1.5 / 31 * 256) + 6)
    mg = int(vg * (1.5 / 63 * 256) + 3)
    mb = int(vb * (1.5 / 31 * 256) + 6)
    return (min((tr * mr) >> 8, 31), min((tg * mg) >> 8, 63), min((tb * mb) >> 8, 31))


CLEAR = _split(CLEAR_COLOR)


def _results_path(mode):
    return f"/data/com.test.gfx3d.{mode}/probe.txt"


def _stage(target, mode, **manifest):
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp) / mode
        d.mkdir()
        shutil.copy(PROBE / "main.elf", d / "main.elf")
        (d / "app.json").write_text(json.dumps({
            "id": f"com.test.gfx3d.{mode}", "name": mode,
            "description": "E2E gfx3d probe", "version": "1.0",
            "author": "PicoDeck E2E", **manifest}))
        target.push_app(d, mode)


def _read(target, mode, timeout=30.0):
    """The probe's results, once they are all there."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            text = target.read_file(_results_path(mode)).decode()
            if text.endswith("PROBE done\n"):
                return text.splitlines()
        except FileNotFoundError:
            pass
        if time.monotonic() >= deadline:
            raise TimeoutError(f"{mode}: no complete {_results_path(mode)}")
        time.sleep(target.poll_interval)


def _start(target, mode, **manifest):
    """Stage and launch a mode; its results once it has reached `done`."""
    _stage(target, mode, **manifest)
    target.delete_file(_results_path(mode))
    assert target.launch_app(mode)["launched"]
    return _read(target, mode)


def _leave(target):
    assert target.exit_app()["ok"]
    out = target.wait_for_exit(timeout=15)
    assert out["result"] in ("returned", "exit_sentinel"), out


class _Screen:
    def __init__(self, target):
        self.img = Image.open(io.BytesIO(target.screenshot())).convert("RGB")

    def at(self, x, y):
        r, g, b = self.img.getpixel((x, y))
        return r >> 3, g >> 2, b >> 3


def _assert_pixels(screen, expected):
    got = {p: screen.at(*p) for p in expected}
    assert got == expected


def _draw_mode(target, mode):
    """A mode that draws a frame and waits: its screen, and it left."""
    _start(target, mode)
    try:
        return _Screen(target)
    finally:
        _leave(target)


def _stats(lines):
    found = [ln for ln in lines if ln.startswith("PROBE stats ")]
    assert found, lines
    return {k: int(v) for k, v in
            (kv.split("=") for kv in found[-1].split()[2:])}


# --- What the plan asks of it ------------------------------------------------

@pytest.mark.both
def test_gfx3d_draws_expected_pixels(target):
    screen = _draw_mode(target, "draw")
    expected = {}
    for ty in range(4):
        for tx in range(4):  # the middle of each of the 40 px texels
            expected[(100 + 40 * tx, 100 + 40 * ty)] = _tex_times(TEX4[ty * 4 + tx], NEUTRAL)
    expected[(60, 160)] = CLEAR          # beside the quad
    expected[(160, 250)] = CLEAR
    expected[(262, 56)] = _split(SPR_COLOR)  # the sprite's ring
    expected[(280, 40)] = CLEAR          # its keyed middle
    _assert_pixels(screen, expected)


@pytest.mark.both
def test_gfx3d_stats_logged(target):
    cursor = target.log_cursor()
    lines = _start(target, "draw")
    try:
        assert "PROBE made ctx=1 tex=0 img=0 mesh=1 filed=1" in lines, lines
        # 160 x 160 of quad, 40 x 40 of sprite.
        assert _stats(lines) == {"tris_in": 2, "culled": 0, "clipped": 0, "drawn": 2,
                                 "sprites": 1, "overflow": 0, "pixels": 27200}
        assert "PROBE version=11" in lines, lines
        if target.kind == "sim":  # on the device a serial line can be dropped
            target.wait_for_log(r"PROBE stats tris_in=2", since=cursor)
    finally:
        _leave(target)


def _git(*args):
    return subprocess.run(["git", "-C", str(PROJECT_ROOT), *args], capture_output=True,
                          text=True, check=True).stdout.strip()


def _value(lines, key):
    found = [ln for ln in lines if ln.startswith(f"PROBE {key}=")]
    assert found, lines
    return found[-1].split("=", 1)[1]


@pytest.mark.both
def test_firmware_id_and_clock(target):
    """On the device the firmware is what was built from this tree. A simulator built
    a commit ago still names a commit before this one, and is not wrong for it."""
    lines = _start(target, "draw")
    _leave(target)
    fid = _value(lines, "firmware")
    assert re.fullmatch(r"[0-9a-f]{7,40}(-dirty)?", fid), fid
    commit = fid.removesuffix("-dirty")
    if target.kind == "hw":
        dirty = _git("status", "--porcelain", "--untracked-files=no",
                     "--ignore-submodules=untracked")
        want = _git("rev-parse", "--short", "HEAD") + ("-dirty" if dirty else "")
        assert fid == want, (f"the device runs {fid}, this tree is {want}: flash the "
                             "firmware built from it")
        assert int(_value(lines, "clock")) == 200000
    else:
        ancestor = subprocess.run(["git", "-C", str(PROJECT_ROOT), "merge-base",
                                   "--is-ancestor", commit, "HEAD"], capture_output=True)
        assert ancestor.returncode == 0, (f"the simulator was built from {fid}, which is "
                                          "not in this branch: rebuild it")
        # The simulator's clock_get_hz, which its audiostat reports too.
        assert int(_value(lines, "clock")) == 125000


@pytest.mark.hardware
def test_get_clock_khz_is_the_clock_now(target):
    """system_clock_khz in app.json changes the clock, and the call says so."""
    lines = _start(target, "draw_250", name="draw", system_clock_khz=250000)
    _leave(target)
    assert int(_value(lines, "clock")) == 250000


# Leaked: a context, whose records alone are 114 KB. The rest of the difference
# between two runs of mem_report is allocator noise (test_native_resources.py).
MEM_TOLERANCE = 16 * 1024
CONTEXT_AT_LEAST = 100 * 1024
LEAK_MODES = ("leak", "leak_midscene", "leak_mesh_first")


def _mem_sim(sim):
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("mem_report")
    assert sim.wait_for_exit(timeout=20).get("result") == "returned"
    lines = [e["text"] for e in sim.get_log_lines(seq)]
    m = next(re.search(r"MEM free=(\d+) largest=(\d+)", t) for t in lines if "MEM free=" in t)
    return int(m.group(1)), int(m.group(2))


@pytest.mark.sd(fixtures=["mem_report"])
def test_gfx3d_leak_freed_at_exit(sim_factory, test_sd_card):
    """Whatever a native app leaves is freed when it returns: a scene left open, and
    a context older or younger than its mesh, so either is freed first. Here with
    the firmware's allocator (--real-umm). Under ASan a free touching the other
    would be caught too."""
    sim = sim_factory(test_sd_card, extra_args=["--real-umm"])
    target = SimTarget(sim)
    for mode in LEAK_MODES:  # one-time allocations predate the baseline
        _start(target, mode)
        assert target.wait_for_exit(timeout=20)["result"] == "returned"
    _mem_sim(sim)
    free0, largest0 = _mem_sim(sim)

    # While the context lives the heap shows it, so the figures below can tell.
    idle = sim.get_heap_info()
    _start(target, "draw")
    running = sim.get_heap_info()
    _leave(target)
    drop = (idle["lua_heap_free_kb"] - running["lua_heap_free_kb"]) * 1024
    assert drop >= CONTEXT_AT_LEAST, (idle, running)

    for mode in LEAK_MODES:
        _start(target, mode)
        assert target.wait_for_exit(timeout=20)["result"] == "returned"
        free, largest = _mem_sim(sim)
        assert free0 - free <= MEM_TOLERANCE, (mode, free0, free)
        assert largest0 - largest <= MEM_TOLERANCE, (mode, largest0, largest)
        out = sim.get_output()["stdout"]
        assert f"'{mode}' exited holding 2 handle(s)" in out, out[-2000:]


MEM_APP = """
collectgarbage("collect")
collectgarbage("collect")
local m = picocalc.sys.getMemInfo()
local f = picocalc.fs.open("/data/" .. APP_ID .. "/mem.txt", "w")
f:write(string.format("%d %d\\n", m.psram_free, m.psram_largest_block))
f:close()
"""


@pytest.mark.hardware
def test_gfx3d_leak_freed_on_the_device(target):
    target.stage_lua_app("gfx3d_mem", MEM_APP, id="com.test.gfx3d.mem")

    def mem():
        target.delete_file("/data/com.test.gfx3d.mem/mem.txt")
        assert target.launch_app("gfx3d_mem")["launched"]
        assert target.wait_for_exit(timeout=20)["result"] == "returned"
        return tuple(map(int, target.read_file("/data/com.test.gfx3d.mem/mem.txt").split()))

    for mode in LEAK_MODES:
        _start(target, mode)
        assert target.wait_for_exit(timeout=20)["result"] == "returned"
    mem()
    free0, largest0 = mem()
    for mode in LEAK_MODES:
        _start(target, mode)
        assert target.wait_for_exit(timeout=20)["result"] == "returned"
        free, largest = mem()
        assert free0 - free <= MEM_TOLERANCE, (mode, free0, free)
        assert largest0 - largest <= MEM_TOLERANCE, (mode, largest0, largest)


@pytest.mark.sd(fixtures=[])
def test_reset_idle_timer_keeps_screen_lit(sim_factory, test_sd_card):
    """Past the 60 s dim timeout with no keys (on the virtual clock, as
    test_idle_dim.py), a mode calling resetIdleTimer each frame is lit, and the
    same mode without it is dimmed, so the first shows the call and not the setup."""
    sim = sim_factory(test_sd_card, virtual_time=True, extra_args=["--idle-dim"])
    target = SimTarget(sim)
    for mode, dimmed in (("idle_plain", True), ("idle_reset", False)):
        _stage(target, mode)
        cursor = target.log_cursor()
        assert target.launch_app(mode)["launched"]
        target.wait_for_log(r"PROBE idle passed", timeout=30, since=cursor)
        assert sim.call("get_backlight")["dimmed"] is dimmed, mode
        _leave(target)


# --- The rest of the table, argument by argument -----------------------------

@pytest.mark.both
def test_gfx3d_free_all(target):
    """Freed by the app, the context first, each second free ignored, NULL too."""
    lines = _start(target, "free_all")
    assert "PROBE freed" in lines, lines
    assert target.wait_for_exit(timeout=20)["result"] == "returned"


@pytest.mark.both
def test_gfx3d_end_keeps_raw_writes(target):
    screen = _draw_mode(target, "raw_end")
    _assert_pixels(screen, {
        (5, 5): _split(0x001F),                   # raw, before begin
        (90, 90): _tex_times(WHITE, NEUTRAL),     # the scene
        (105, 105): _split(0xF800),               # raw, over the scene after end
        (50, 150): (0, 0, 0),                     # no clear asked for: the display's
    })


@pytest.mark.both
def test_gfx3d_end_honours_the_clip_rect(target):
    screen = _draw_mode(target, "clip")
    _assert_pixels(screen, {
        (100, 160): _tex_times(WHITE, NEUTRAL),
        (40, 160): CLEAR,
        (200, 160): (0, 0, 0),  # the mesh and the clear both stop at x = 159
        (300, 40): (0, 0, 0),
    })


@pytest.mark.both
def test_gfx3d_fog(target):
    screen = _draw_mode(target, "fog")
    # Half way to the fog colour at depth 2 between fog 1 and 3.
    fogged_vertex = (26 << 11) | (21 << 5) | 11
    _assert_pixels(screen, {
        (120, 160): _tex_times(WHITE, fogged_vertex),
        (200, 160): _tex_times(WHITE, NEUTRAL),  # PC3D_TM_NO_FOG
        (22, 56): _split(SPR_COLOR),              # no sprite fog yet
        (262, 56): (16, 30, 0),                   # level 16 of 31 towards red
    })


@pytest.mark.both
def test_gfx3d_sky_image_and_rows(target):
    lines = _start(target, "sky")
    try:
        assert "PROBE sky badslot=0 badrow=0 ok=1" in lines, lines
        screen = _Screen(target)
    finally:
        _leave(target)
    def sky(col, row):
        return col * 4, row * 8, 31
    _assert_pixels(screen, {
        (3, 158): sky(3, 2),    # horizon_row 4 at row 160
        (3, 165): sky(3, 7),    # past the last row, the last again
        (3, 100): sky(3, 0),    # above the first, the first
        (12, 158): sky(4, 2),   # the width repeats
        (3, 200): (0, 0, 0),    # below setSkyRows' 170
    })


@pytest.mark.both
def test_gfx3d_tex_scroll(target):
    screen = _draw_mode(target, "scroll")
    def scrolled(tx, ty):  # du 0.25 is one texel across, dv 0.5 two down
        return _tex_times(TEX4[((ty + 2) % 4) * 4 + (tx + 1) % 4], NEUTRAL)
    _assert_pixels(screen, {(100 + 40 * tx, 100 + 40 * ty): scrolled(tx, ty)
                            for tx, ty in ((0, 0), (3, 0), (1, 2), (2, 3))})


@pytest.mark.both
def test_gfx3d_mirror(target):
    screen = _draw_mode(target, "mirror")
    _assert_pixels(screen, {
        (160, 200): _tex_times(WHITE, NEUTRAL),
        (160, 100): CLEAR,
        (160, 250): CLEAR,
    })


@pytest.mark.both
def test_gfx3d_mesh_flags(target):
    lines = _start(target, "flags")
    try:
        screen = _Screen(target)
    finally:
        _leave(target)
    r, g, b = _tex_times(WHITE, NEUTRAL)
    # 50% over the clear colour: gfx3d_tex.h's masked average.
    packed = (r << 11) | (g << 5) | b
    blended = ((packed & 0xF7DE) + (CLEAR_COLOR & 0xF7DE)) >> 1
    _assert_pixels(screen, {
        (70, 160): _split(blended),
        (150, 160): (r, g, b),   # faces away, PC3D_TM_DOUBLE_SIDED
        (230, 160): CLEAR,       # faces away
    })
    s = _stats(lines)
    assert (s["tris_in"], s["culled"], s["drawn"]) == (6, 2, 4), s


@pytest.mark.both
def test_gfx3d_draw_bias(target):
    screen = _draw_mode(target, "bias")
    red = _tex_times(0xF800, WHITE)
    _assert_pixels(screen, {
        (160, 100): red,                 # bias -1 beats the later mesh
        (102, 176): _split(SPR_COLOR),   # bias -1.5: in front of it
        (182, 176): red,                 # bias +1.5: behind both
    })


@pytest.mark.both
def test_gfx3d_draw_transform(target):
    screen = _draw_mode(target, "xform")
    _assert_pixels(screen, {
        (190, 140): _tex_times(WHITE, NEUTRAL),
        (170, 140): CLEAR,
        (210, 140): CLEAR,
        (190, 110): CLEAR,
        (190, 170): CLEAR,
    })


@pytest.mark.both
def test_gfx3d_two_contexts(target):
    """The second's begin takes the shared SRAM block from the first mid-scene."""
    screen = _draw_mode(target, "two")
    _assert_pixels(screen, {
        (120, 160): _tex_times(0xF800, WHITE),
        (200, 160): _tex_times(0x001F, WHITE),
        (40, 160): CLEAR,
    })


FOCAL = 160.0 / math.tan(1.5707964 / 2.0)


def _mul(a, v):
    return [sum(a[r][c] * v[c] for c in range(3)) for r in range(3)]


def _matmul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def _project(view, cam, p):
    v = _mul(view, [p[i] - cam[i] for i in range(3)])
    depth = -v[2]
    k = FOCAL / depth
    return 160.0 + v[0] * k, 160.0 - v[1] * k, depth


def _projected(lines, what):
    found = [ln for ln in lines if ln.startswith(f"PROBE project {what} ")]
    assert found, lines
    parts = found[-1].split()
    ok = parts[3] == "ok=1"
    return ok, [struct.unpack("<f", struct.pack("<I", int(h, 16)))[0] for h in parts[4:]]


def _assert_projected(lines, what, want):
    ok, got = _projected(lines, what)
    assert ok, lines
    assert got == pytest.approx(want, abs=1e-2), what


def _euler_view(yaw, pitch, roll):
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cr, sr = math.cos(roll), math.sin(roll)
    ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    rx = [[1, 0, 0], [0, cp, -sp], [0, sp, cp]]
    rz = [[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]]
    r = _matmul(_matmul(ry, rx), rz)
    return [[r[c][k] for c in range(3)] for k in range(3)]  # the inverse: its transpose


def _unit(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


@pytest.mark.both
def test_gfx3d_calls(target):
    """Each call's results, so that an argument taken from the wrong place on the
    way to the renderer shows: the camera's three angles and position, for one."""
    lines = _start(target, "calls")
    assert target.wait_for_exit(timeout=20)["result"] == "returned"

    for want in ("PROBE create bad=0,0",
                 "PROBE projection bad=0,0,0 ok=1",
                 "PROBE lookat at_eye=0 along_up=0 ok=1",
                 "PROBE tex bad=-1,-1,-1,-1,-1",
                 "PROBE tex first=0 mips=1",
                 "PROBE img bad=-1,-1,-1 first=0",
                 "PROBE mesh good=1 bad=0,0,0,0",
                 "PROBE sprites none=0,0",
                 "PROBE sprites many=699"):
        assert want in lines, (want, lines)

    _assert_projected(lines, "origin", [240.0, 80.0, 2.0])
    ok, got = _projected(lines, "near")
    assert not ok and got == [0.0, 0.0, 0.0], lines

    _assert_projected(lines, "camera",
                      _project(_euler_view(0.3, 0.2, 0.1), [1.0, 2.0, 3.0], [0.5, -0.5, -4.0]))

    eye, target_pt = [3.0, 1.0, 2.0], [-1.0, 0.5, -2.0]
    fwd = _unit([target_pt[i] - eye[i] for i in range(3)])
    right = _unit(_cross(fwd, [0.0, 1.0, 0.0]))
    up = _cross(right, fwd)
    view = [right, up, [-x for x in fwd]]
    depth = math.sqrt(sum((target_pt[i] - eye[i]) ** 2 for i in range(3)))
    _assert_projected(lines, "lookat", [160.0, 160.0, depth])
    _assert_projected(lines, "lookat_off", _project(view, eye, [0.0, 1.0, -1.0]))

    stats = [ln for ln in lines if ln.startswith("PROBE stats ")]
    assert len(stats) == 2, lines
    small = dict(kv.split("=") for kv in stats[0].split()[2:])
    assert (small["tris_in"], small["drawn"], small["overflow"]) == ("2", "1", "1"), small
    assert stats[1] == ("PROBE stats tris_in=0 culled=0 clipped=0 drawn=0 sprites=0 "
                        "overflow=0 pixels=0"), stats[1]
