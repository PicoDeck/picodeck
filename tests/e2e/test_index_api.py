"""E2E tests for the index-addressed and bulk-pixel Lua API calls.

Covers the additions that let Lua reach the same ground as the C API by index
rather than by name — `zip:numEntries/locate/statIndex/extractEntry` — plus
`fs.isDir`, handle-based `file:fsize()` and the `display.getBackBuffer()`
framebuffer handle with its bulk `getPixels`/`setPixels`.

The fixture app is staged at runtime rather than checked in under
`tests/e2e/apps/`: that directory is staged whole onto every test's SD card and
already fills the launcher's MAX_APPS budget, so a permanent app there evicts
the others. `fixtures=[]` + `reserve=1` is the supported way to add one.

Behaviour is asserted from the "IA ok|FAIL <name>" lines the app logs, so a
failure names the specific contract that broke rather than just a diff.
"""
import io
import re
import zipfile

import pytest

from helpers import stage_lua_app

APP = "index_api_probe"

def _bundle_bytes() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("a.txt", "first")           # 5 bytes
        z.writestr("d/three.txt", "three")     # 5 bytes, in a subdirectory
        z.writestr("big.bin", b"B" * 5000)     # compresses well
    return buf.getvalue()


LUA = r"""
local pc = picocalc
local fs, disp = pc.fs, pc.display
local ZIP = APP_DIR .. "/bundle.zip"

local function check(name, got, want)
  if got == want then pc.sys.log("IA ok " .. name)
  else pc.sys.log("IA FAIL " .. name .. " got=" .. tostring(got) .. " want=" .. tostring(want)) end
end
local function check_true(name, cond)
  if cond then pc.sys.log("IA ok " .. name)
  else pc.sys.log("IA FAIL " .. name .. " got=false want=true") end
end
local function check_raises(name, fn)
  if pcall(fn) then pc.sys.log("IA FAIL " .. name .. " got=no error want=error")
  else pc.sys.log("IA ok " .. name) end
end

-- fs.isDir
check_true("isDir.appdir", fs.isDir(APP_DIR))
check("isDir.file", fs.isDir(ZIP), false)
check("isDir.missing", fs.isDir(APP_DIR .. "/absent"), false)
check("isDir.denied", fs.isDir("/etc/shadow"), false)

-- file:fsize() — the size without disturbing the read position
local h = fs.open(ZIP, "rb")
local total = h:fsize()
check_true("fsize.positive", type(total) == "number" and total > 0)
check("fsize.position_unchanged", h:tell(), 0)
h:seek(0, "end")
check("fsize.matches_seek_tell", h:tell(), total)
h:seek(0)
check("fsize.stable", h:fsize(), total)
h:close()

-- zip, addressed by index
local z = pc.zip.open(ZIP)
check("zip.numEntries", z:numEntries(), 3)

local i = z:locate("a.txt")
check("zip.locate", i, 0)
check("zip.locate.missing", z:locate("nope.txt"), nil)

local st = z:statIndex(i)
check("zip.statIndex.name", st and st.name, "a.txt")
check("zip.statIndex.size", st and st.size, 5)
check("zip.statIndex.is_dir", st and st.is_dir, false)
check_true("zip.statIndex.compressed_size", st and type(st.compressed_size) == "number")
check("zip.statIndex.bad_index", z:statIndex(99), nil)

local big = z:statIndex(z:locate("big.bin"))
check("zip.statIndex.big.size", big and big.size, 5000)
check_true("zip.statIndex.compresses", big and big.compressed_size < big.size)

local dest = "/data/" .. APP_ID .. "/out.bin"
check_true("zip.extractEntry.ok", (z:extractEntry(z:locate("d/three.txt"), dest)))
check("zip.extractEntry.contents", fs.readFile(dest), "three")
check("zip.read_matches_statIndex", z:read("a.txt"), "first")

z:close()
-- Every archive method raises on a closed handle (the Lua contract; C's
-- numEntries returns a 0 sentinel instead, which Lua has no use for).
check_raises("zip.method_after_close", function() z:numEntries() end)
check_raises("zip.statIndex_after_close", function() z:statIndex(0) end)

-- display.getBackBuffer()
local fb = disp.getBackBuffer()
check("fb.width", fb:width(), 320)
check("fb.height", fb:height(), 320)
check_true("fb.singleton", disp.getBackBuffer() == fb)

-- host-order RGB565, little-endian on the wire: 0xF800 -> 0x00 0xF8
local RED = string.char(0x00, 0xF8)
local block = string.rep(RED, 8 * 4)
check_true("fb.setPixels", fb:setPixels(block, 100, 100, 8, 4))
check("fb.setPixels.via_getPixel", disp.getPixel(100, 100), 0xF800)
check("fb.setPixels.far_corner", disp.getPixel(107, 103), 0xF800)
check("fb.getPixels.length", #fb:getPixels(100, 100, 8, 4), 64)
check("fb.getPixels.bytes", fb:getPixels(100, 100, 8, 4), block)
check("fb.setPixels.outside_block", disp.getPixel(99, 100), 0)

-- writes respect the clip rect, reads do not
disp.setClipRect(0, 0, 4, 4)
local before = disp.getPixel(7, 3)
check_true("fb.setPixels.partly_clipped", fb:setPixels(block, 0, 0, 8, 4))
check("fb.setPixels.clipped_in", disp.getPixel(1, 1), 0xF800)
check("fb.setPixels.clipped_out", disp.getPixel(7, 3), before)
check("fb.setPixels.wholly_outside", fb:setPixels(block, 200, 200, 8, 4), false)
disp.clearClipRect()

check_raises("fb.getPixels.off_screen", function() fb:getPixels(318, 0, 8, 1) end)
check_raises("fb.getPixels.zero_w", function() fb:getPixels(0, 0, 0, 4) end)
check_raises("fb.setPixels.short", function() fb:setPixels("\1\2", 0, 0, 8, 4) end)
check_raises("fb.setPixels.long", function() fb:setPixels(string.rep("\0", 200), 0, 0, 8, 4) end)
check_raises("fb.no_single_pixel_api", function() fb:getPixel(1, 1) end)
-- Metatables are locked SDK-wide: getmetatable() hands back the __metatable
-- value (a boolean) instead of the table, so the methods table is unreachable.
check("fb.metatable_locked", type(getmetatable(fb)), "boolean")

pc.sys.log("IA DONE")
"""


def _lines(sim):
    out = []
    for line in sim.get_log_buffer()["lines"]:
        out.append(line if isinstance(line, str) else line.get("text", ""))
    return out


def _run(simulator, test_sd_card):
    stage_lua_app(test_sd_card, APP, LUA,
                  files={"bundle.zip": _bundle_bytes()})
    simulator.launch_app(APP)
    try:
        simulator.wait_for_log("IA DONE", timeout=30.0)
    except TimeoutError:
        pytest.fail("probe never finished:\n" + "\n".join(_lines(simulator)[-30:]))

    results = {}
    for line in _lines(simulator):
        m = re.search(r"IA (ok|FAIL) (\S+)", line)
        if m:
            results[m.group(2)] = (m.group(1) == "ok", line)
    return results


@pytest.mark.sd(fixtures=[], reserve=1)
def test_index_addressed_api(simulator, test_sd_card):
    results = _run(simulator, test_sd_card)

    failures = [line for passed, line in results.values() if not passed]
    assert failures == [], "these contracts broke:\n" + "\n".join(failures)

    # Guard the harness itself: a probe that silently stopped checking would
    # otherwise pass on an empty result set.
    assert len(results) >= 35, f"only {len(results)} checks ran; the probe changed?"


@pytest.mark.sd(fixtures=[], reserve=1)
def test_framebuffer_handle_is_not_a_pointer(simulator, test_sd_card):
    """The Lua shape must stay a handle: the metatable is locked (as for every
    SDK object), so an app cannot walk it, and there is no single-pixel mirror
    (display.getPixel is that call)."""
    _run(simulator, test_sd_card)
    joined = "\n".join(_lines(simulator))
    assert "IA ok fb.metatable_locked" in joined
    assert "IA ok fb.no_single_pixel_api" in joined
    assert "IA ok fb.singleton" in joined
