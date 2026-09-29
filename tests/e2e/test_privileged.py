"""Privileged system calls are gated by app.json requirements (review:
Lua Critical "qmiPsram*/applyUpdate/sysconfig open to every app", Network
Medium "wifi_pass readable", Audio/storage Medium "PIO PSRAM has no bounds").

- `picocalc.sysconfig` exists only with the "sysconfig" requirement, and even
  then `wifi_pass` is write-only.
- `sys.applyUpdate` exists only with "system-update" AND an OS app id
  (the updater/store), refuses an image without a valid .sha256 and ECDSA
  signature (the TEST update key in the simulator), and asks for confirmation
  before flashing.
- `sys.pioPsramRead/Write` refuse the OS-reserved range (MP3 ring + video
  pool below PIO_PSRAM_APP_BASE) and anything past the chip.
- `sys.qmiPsram*` handles are bounds-checked userdata, not raw pointers.
- `fs.browse(start)` cannot open a directory the app may not read.

Each group is an inline picotest app staged onto one module simulator (the
simulator rescans /apps when a launch misses).
"""

import hashlib
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

from helpers import run_lua_app, stage_lua_app

APP_BASE = 288 * 1024  # PIO_PSRAM_APP_BASE (src/drivers/pio_psram.h)

ROOT = Path(__file__).resolve().parents[2]
SIGN_TOOL = ROOT / "tools" / "sign_update.py"
TEST_KEY = ROOT / "tests" / "keys" / "picodeck-update-TEST-private.pem"
OTHER_KEY = ROOT / "tests" / "keys" / "picodeck-update-TEST-other-private.pem"


def fake_firmware(size=4096, fill=0):
    """An image that passes the vector-table check (SP in SRAM, reset vector
    in flash); the simulator never flashes it."""
    return struct.pack("<II", 0x20082000, 0x10000101) + bytes([fill]) * (size - 8)


def sign(data: bytes, key: Path, tmp: Path) -> bytes:
    """DER ECDSA-P256 signature via tools/sign_update.py (openssl CLI)."""
    img, out = tmp / "img.bin", tmp / "img.sig"
    img.write_bytes(data)
    subprocess.run([sys.executable, str(SIGN_TOOL), "sign", str(img),
                    "--key", str(key), "--out", str(out)],
                   check=True, capture_output=True)
    return out.read_bytes()


def stage_update_files(sd, image: bytes, tmp: Path, *, sha: "bytes|None" = b"",
                       sig: "bytes|None" = b""):
    """Write /system/update.sha256 and /system/update.sig for `image`.
    sha/sig b"" = correct value, None = absent, other bytes = verbatim."""
    system = Path(sd) / "system"
    for name in ("update.sha256", "update.sig"):
        (system / name).unlink(missing_ok=True)
    if sha is not None:
        (system / "update.sha256").write_bytes(
            sha or (hashlib.sha256(image).hexdigest() + "\n").encode())
    if sig is not None:
        (system / "update.sig").write_bytes(sig or sign(image, TEST_KEY, tmp))

PLAIN_APP = f"""
local T = picocalc.sys.loadlib("picotest")
local sys = picocalc.sys
local APP_BASE = {APP_BASE}

T.case("sysconfig_absent", function()
    T.eq(picocalc.sysconfig, nil, "picocalc.sysconfig without the requirement")
end)

T.case("applyUpdate_absent", function()
    T.eq(sys.applyUpdate, nil, "sys.applyUpdate without system-update")
end)

T.case("pio_write_mp3_ring_errors", function()
    T.raises(function() sys.pioPsramWrite(0, "x") end, "reserved")
end)

T.case("pio_write_video_pool_errors", function()
    T.raises(function() sys.pioPsramWrite(0x8000, "x") end, "reserved")
end)

T.case("pio_write_straddling_base_errors", function()
    T.raises(function() sys.pioPsramWrite(APP_BASE - 1, "xy") end, "reserved")
end)

T.case("pio_read_reserved_errors", function()
    T.raises(function() sys.pioPsramRead(0x100, 16) end, "reserved")
end)

T.case("pio_negative_errors", function()
    T.raises(function() sys.pioPsramWrite(-1, "x") end, "range")
    T.raises(function() sys.pioPsramRead(APP_BASE, -5) end, "range")
end)

T.case("pio_past_chip_errors", function()
    -- 64-bit arithmetic: addr + len must not wrap into the app region.
    T.raises(function() sys.pioPsramRead(0xFFFFFFFF, 2) end, "range")
    T.raises(function() sys.pioPsramRead(APP_BASE, 0x7FFFFFFF) end, "range")
end)

T.case("pio_app_region_allowed", function()
    local ok, err = pcall(sys.pioPsramWrite, APP_BASE, "x")
    T.ok(ok, "write at PIO_PSRAM_APP_BASE: " .. tostring(err))
end)

T.case("qmi_handle_bounds", function()
    local h = T.ok(sys.qmiPsramAlloc(64), "qmiPsramAlloc(64)")
    T.eq(sys.qmiPsramWrite(h, 60, "abcd"), 4)
    T.eq(sys.qmiPsramRead(h, 60, 4), "abcd")
    T.raises(function() sys.qmiPsramWrite(h, 64, "x") end, "range")
    T.raises(function() sys.qmiPsramWrite(h, -1, "x") end, "range")
    T.raises(function() sys.qmiPsramRead(h, 60, 8) end, "range")
    sys.qmiPsramFree(h)
    T.raises(function() sys.qmiPsramWrite(h, 0, "x") end, "freed")
    sys.qmiPsramFree(h)  -- double free is a no-op, not a heap corruption
end)

T.case("qmi_rejects_foreign_values", function()
    T.raises(function() sys.qmiPsramFree("not a handle") end)
    T.raises(function() sys.qmiPsramWrite({{}}, 0, "x") end)
    T.raises(function() sys.qmiPsramRead(picocalc.json, 0, 1) end)
end)

T.done()
"""

SYSCONFIG_APP = """
local T = picocalc.sys.loadlib("picotest")

T.case("sysconfig_present", function()
    T.ok(picocalc.sysconfig, "picocalc.sysconfig with the requirement")
end)

T.case("wifi_pass_not_readable", function()
    local sc = picocalc.sysconfig
    T.ok(sc.load(), "sysconfig.load()")
    T.eq(sc.get("wifi_ssid"), "HomeNet")
    T.eq(sc.get("wifi_pass"), nil, "wifi_pass after load")
end)

T.case("wifi_pass_write_only", function()
    local sc = picocalc.sysconfig
    sc.set("wifi_pass", "changed-by-app")
    T.eq(sc.get("wifi_pass"), nil, "wifi_pass after set")
    T.ok(sc.save(), "sysconfig.save()")
end)

T.case("other_keys_read_write", function()
    local sc = picocalc.sysconfig
    sc.set("t7_key", "v")
    T.eq(sc.get("t7_key"), "v")
end)

T.done()
"""

UPDATE_PRESENCE_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("applyUpdate_absent", function()
    T.eq(picocalc.sys.applyUpdate, nil,
         "applyUpdate for a non-OS app id with system-update")
end)
T.done()
"""

UPDATE_CONFIRM_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("applyUpdate_present", function()
    T.eq(type(picocalc.sys.applyUpdate), "function")
end)
T.case("applyUpdate_asks_first", function()
    picocalc.sys.log("T7:CONFIRM_NEXT")
    local ok, err = picocalc.sys.applyUpdate(APP_DIR .. "/fw.bin")
    T.eq(ok, false)
    T.eq(err, "cancelled")
end)
T.done()
"""


@pytest.fixture(scope="module")
def sim(sim_module_factory):
    def setup(sd):
        (sd / "system" / "config.json").write_text(json.dumps(
            {"wifi_ssid": "HomeNet", "wifi_pass": "hunter2"}))
    # Every app here is staged by the tests (about 20): none of the fixture
    # apps is used, so the card carries none and leaves the cap to them.
    return sim_module_factory(setup=setup, fixtures=[], reserve=30)


def _passed(run):
    run.assert_all_passed()


def _press_until_exit(sim, key, tries=10):
    """Press `key` until the app launched last exits (a modal is up)."""
    for _ in range(tries):
        time.sleep(0.3)
        sim.keypress(key)
        try:
            return sim.wait_for_exit(timeout=0.5)
        except TimeoutError:
            pass
    return sim.wait_for_exit(timeout=5)


def test_app_without_requirements_is_unprivileged(sim):
    stage_lua_app(sim.sd_card_path, "priv_plain", PLAIN_APP)
    _passed(run_lua_app(sim, "priv_plain"))


def test_sysconfig_requirement_hides_wifi_pass(sim):
    stage_lua_app(sim.sd_card_path, "priv_sysconfig", SYSCONFIG_APP,
                  requirements=["sysconfig"])
    _passed(run_lua_app(sim, "priv_sysconfig"))
    # set() is allowed (write-only), and save() persisted it.
    cfg = json.loads((Path(sim.sd_card_path) / "system" / "config.json").read_text())
    assert cfg.get("wifi_pass") == "changed-by-app", cfg


def test_system_update_needs_an_os_app_id(sim):
    stage_lua_app(sim.sd_card_path, "priv_upd_plain", UPDATE_PRESENCE_APP,
                  requirements=["system-update"], id="com.test.upd")
    _passed(run_lua_app(sim, "priv_upd_plain"))


def test_os_updater_without_requirement_has_no_apply_update(sim):
    stage_lua_app(sim.sd_card_path, "priv_upd_noreq", UPDATE_PRESENCE_APP,
                  id="net.picodeck.store")
    _passed(run_lua_app(sim, "priv_upd_noreq"))


def test_apply_update_asks_before_flashing(sim, tmp_path):
    """The OS updater id with system-update gets applyUpdate, and for an image
    that is correctly signed it shows a confirmation naming the file; Esc
    cancels without flashing."""
    image = fake_firmware()
    stage_update_files(sim.sd_card_path, image, tmp_path)
    stage_lua_app(sim.sd_card_path, "priv_upd_ok", UPDATE_CONFIRM_APP,
                  requirements=["system-update", "root-filesystem"],
                  id="net.picodeck.updater", files={"fw.bin": image})
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("priv_upd_ok")
    sim.wait_for_log("T7:CONFIRM_NEXT", timeout=15, since_seq=seq)
    outcome = _press_until_exit(sim, "esc")
    assert outcome.get("result") == "returned", outcome
    res = json.loads((Path(sim.sd_card_path) / "data" / "net.picodeck.updater" /
                      "test_results.json").read_text())
    cases = {c["name"]: c for c in res["cases"]}
    assert res["done"], res
    for name in ("applyUpdate_present", "applyUpdate_asks_first"):
        assert cases[name]["status"] == "PASS", cases[name]


QUEUED_KEYS_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("queued_keys_do_not_confirm", function()
    picocalc.sys.log("T7:QUEUE_NOW")
    picocalc.sys.sleep(1500)   -- does not poll: injected keys stay queued
    local ok, err = picocalc.sys.applyUpdate(APP_DIR .. "/fw.bin")
    T.eq(ok, false)
    T.eq(err, "cancelled")
end)
T.done()
"""


def test_confirm_ignores_keys_queued_before_it(sim, tmp_path):
    """A "yes" typed (queued) before the confirm dialog appears must not
    answer it: an app could show "press Enter", sleep, then applyUpdate."""
    image = fake_firmware()
    stage_update_files(sim.sd_card_path, image, tmp_path)
    stage_lua_app(sim.sd_card_path, "priv_upd_queued", QUEUED_KEYS_APP,
                  requirements=["system-update", "root-filesystem"],
                  id="net.picodeck.store", files={"fw.bin": image})
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("priv_upd_queued")
    sim.wait_for_log("T7:QUEUE_NOW", timeout=15, since_seq=seq)
    sim.keypress("y")
    sim.keypress("enter")
    sim.keypress("Y")
    # Sleep ends, the dialog shows and its grace period passes: still open.
    with pytest.raises(TimeoutError):
        sim.wait_for_exit(timeout=3.0)
    outcome = _press_until_exit(sim, "n")
    assert outcome.get("result") == "returned", outcome
    res = json.loads((Path(sim.sd_card_path) / "data" / "net.picodeck.store" /
                      "test_results.json").read_text())
    case = {c["name"]: c for c in res["cases"]}["queued_keys_do_not_confirm"]
    assert case["status"] == "PASS", case


# ── OTA signature (review: Network Critical "applyUpdate flashes unsigned
#    firmware") ─────────────────────────────────────────────────────────────
# The simulator runs the firmware's own ota_verify.c against the TEST update
# key, so these refusals are the device's.  A refused image never reaches the
# confirm dialog: applyUpdate returns false at once.

UPDATE_REFUSED_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("refused", function()
    local ok, err = picocalc.sys.applyUpdate(APP_DIR .. "/fw.bin")
    picocalc.sys.log("T8:ERR " .. tostring(err))
    T.eq(ok, false)
    T.ok(type(err) == "string" and err:find(EXPECT, 1, true),
         "error names the problem: " .. tostring(err))
end)
T.done()
"""


def _tampered(image: bytes) -> bytes:
    b = bytearray(image)
    b[2000] ^= 0x01
    return bytes(b)


@pytest.mark.parametrize("case,expect", [
    ("unsigned", "Missing signature file"),
    ("empty_sig", "Missing signature file"),
    ("wrong_key", "Bad signature"),
    ("tampered", "Bad signature"),
    ("truncated_sig", "Bad signature"),
    ("no_checksum", "Missing checksum file"),
    ("sha256sum_format", "Malformed checksum file"),
])
def test_apply_update_refuses_unsigned_or_bad_image(sim, tmp_path, case, expect):
    good = fake_firmware()
    image = good
    sha, sig = b"", b""
    if case == "unsigned":
        sig = None
    elif case == "empty_sig":
        sig = None  # written as a 0-byte file below
    elif case == "wrong_key":
        sig = sign(good, OTHER_KEY, tmp_path)
    elif case == "tampered":
        # Attacker controls the image AND its .sha256, but not the key.
        image = _tampered(good)
        sig = sign(good, TEST_KEY, tmp_path)
    elif case == "truncated_sig":
        sig = sign(good, TEST_KEY, tmp_path)[:-3]
    elif case == "no_checksum":
        sha = None
    elif case == "sha256sum_format":
        sha = (hashlib.sha256(good).hexdigest() + "  picodeck.bin\n").encode()
    stage_update_files(sim.sd_card_path, image, tmp_path, sha=sha, sig=sig)
    if case == "empty_sig":
        (Path(sim.sd_card_path) / "system" / "update.sig").write_bytes(b"")
    name = f"priv_upd_{case}"
    stage_lua_app(sim.sd_card_path, name,
                  UPDATE_REFUSED_APP.replace("EXPECT", json.dumps(expect)),
                  requirements=["system-update", "root-filesystem"],
                  id="net.picodeck.updater", files={"fw.bin": image})
    # No dialog: the app must finish on its own.
    _passed(run_lua_app(sim, name, timeout=20))


# ── fs.browse start path (sandbox residual a) ───────────────────────────────

BROWSE_APP = """
picocalc.sys.log("T7:BROWSING")
local p = picocalc.fs.browse(START)
picocalc.sys.log("T7:BROWSE " .. tostring(p))
"""


def _browse(sim, name, start, requirements=()):
    app_id = f"com.test.{name}"
    data = Path(sim.sd_card_path) / "data" / app_id
    data.mkdir(parents=True, exist_ok=True)
    (data / "mine.txt").write_text("mine")
    stage_lua_app(sim.sd_card_path, name,
                  BROWSE_APP.replace("START", json.dumps(start)),
                  requirements=requirements)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app(name)
    sim.wait_for_log("T7:BROWSING", timeout=15, since_seq=seq)
    # Enter walks into directories until it selects a file.
    out = _press_until_exit(sim, "enter")
    assert out.get("result") == "returned", out
    texts = [e["text"] for e in sim.get_log_lines(seq)]
    hit = [t for t in texts if t.startswith("T7:BROWSE ")]
    assert hit, texts
    return hit[0][len("T7:BROWSE "):], app_id


@pytest.mark.parametrize("start", ["/system", "/data/com.other", "/apps/.."])
def test_browse_cannot_start_outside_the_sandbox(sim, start):
    (Path(sim.sd_card_path) / "data" / "com.other").mkdir(parents=True, exist_ok=True)
    (Path(sim.sd_card_path) / "data" / "com.other" / "secret.txt").write_text("s")
    name = "browse_" + "".join(c for c in start if c.isalnum())
    picked, app_id = _browse(sim, name, start)
    assert picked.startswith(f"/data/{app_id}/"), picked


def test_browse_root_filesystem_app_may_start_anywhere(sim):
    picked, _ = _browse(sim, "browse_root", "/system",
                        requirements=["root-filesystem"])
    assert picked.startswith("/system/"), picked
