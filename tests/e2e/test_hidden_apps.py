"""Hidden apps: /apps/.test and /apps/.dev.

The launcher scan skips dot names, so an app under either root is never
listed (and never counts against MAX_APPS), but the dev `launch` and the
`launch_app` RPC still start it (src/os/launcher.c: the hidden lookup after
the listed apps, `.test` first). `list all` prints them, tagged with their
root; `mv` moves a directory (dev_ops.c).
"""

import json
import re
from pathlib import Path

import pytest

from helpers import launcher_app_cap, stage_lua_app
from hw_target import SimTarget

# Logs where it runs and what the sandbox lets it see.
PROBE = """
local fs = picocalc.fs
picocalc.sys.log("HID:DIR " .. APP_DIR)
picocalc.sys.log("HID:ID " .. APP_ID)
picocalc.sys.log("HID:OWN " .. tostring(fs.readFile(APP_DIR .. "/main.lua") ~= nil))
picocalc.sys.log("HID:SIBLING " .. tostring(fs.readFile("/apps/.test/hid_sibling/main.lua") ~= nil))
picocalc.sys.log("HID:SIBLING_DEV " .. tostring(fs.readFile("/apps/.dev/hid_dev/main.lua") ~= nil))
picocalc.sys.log("HID:HELLO " .. tostring(fs.readFile("/apps/hello/main.lua") ~= nil))
local data = "/data/" .. APP_ID
fs.mkdir(data)
local f = fs.open(data .. "/x", "w")
if f then fs.write(f, "mine") fs.close(f) end
picocalc.sys.log("HID:DATA " .. tostring(f ~= nil))
"""


@pytest.fixture
def hid(sim_factory, test_sd_card):
    sd = test_sd_card
    stage_lua_app(sd, "hid_test", PROBE, id="com.test.hid_test")
    stage_lua_app(sd, "hid_sibling", 'picocalc.sys.log("HID:SIB_RAN")\n',
                  id="com.test.hid_sibling")
    app = sd / "apps" / ".dev" / "hid_dev"
    app.mkdir(parents=True)
    (app / "app.json").write_text(json.dumps(
        {"id": "com.dev.hid_dev", "name": "Hidden Dev"}))
    (app / "main.lua").write_text('picocalc.sys.log("HID:DEV_RAN " .. APP_DIR)\n')
    sim = sim_factory(sd)
    return sim, SimTarget(sim), sd


def _launch(sim, name, timeout=15.0):
    sim.launch_app(name, hidden=False)   # the name as given: the launcher's order
    return sim.wait_for_exit(timeout=timeout)


def _texts(sim, since=0):
    return [e["text"] for e in sim.get_log_lines(since)]


def _listing(sim, cmd):
    """The lines `list` / `list all` print (the simulator logs them)."""
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    reply = sim.call("dev_command", {"cmd": cmd, "timeout_ms": 5000},
                     timeout=10.0)
    assert reply.get("ok") is not False, reply
    return [t for t in _texts(sim, seq) if t.startswith("  ") or t.startswith("[DEV]")]


# ── Not listed, but in `list all` ───────────────────────────────────────────


def test_hidden_apps_are_not_listed(hid):
    sim, _, _ = hid
    lines = _listing(sim, "list")
    assert any("hello" in t for t in lines), lines
    assert not any("hid_" in t or "hid_dev" in t for t in lines), lines
    assert not any("Hidden apps" in t for t in lines), lines


def test_list_all_shows_hidden_apps_with_their_root(hid):
    sim, _, _ = hid
    lines = _listing(sim, "list all")
    joined = "\n".join(lines)
    assert re.search(r"hid_test\s+\(com\.test\.hid_test\)\s+\[\.test\]", joined), joined
    assert re.search(r"hid_sibling\s+\(com\.test\.hid_sibling\)\s+\[\.test\]", joined), joined
    assert re.search(r"Hidden Dev\s+\(com\.dev\.hid_dev\)\s+\[\.dev\]", joined), joined
    # The listed apps come first, then the hidden section, then the total.
    assert joined.index("Available apps") < joined.index("Hidden apps") \
        < joined.index("Total:")
    assert re.search(r"Total: \d+ apps, 3 hidden", joined), joined


def test_hidden_apps_do_not_count_in_the_launcher(hid):
    """The launcher's table (`list`'s Total) holds the listed apps only."""
    sim, _, sd = hid
    listed = sum(1 for a in (sd / "apps").iterdir()
                 if a.is_dir() and not a.name.startswith("."))
    total = [t for t in _listing(sim, "list") if t.startswith("[DEV] Total:")]
    assert total == [f"[DEV] Total: {listed} apps"], total


# ── Launching ───────────────────────────────────────────────────────────────


def test_launch_hidden_by_dir_name_runs_with_its_own_dir_and_data(hid):
    sim, _, sd = hid
    out = _launch(sim, "hid_test")
    assert out["found"] is True and out["result"] == "returned", out
    assert out["id"] == "com.test.hid_test"
    t = _texts(sim)
    assert "HID:DIR /apps/.test/hid_test" in t, t
    assert "HID:ID com.test.hid_test" in t
    assert "HID:OWN true" in t
    assert "HID:DATA true" in t
    assert (sd / "data" / "com.test.hid_test" / "x").read_text() == "mine"


def test_launch_hidden_by_id(hid):
    sim, _, _ = hid
    out = _launch(sim, "com.test.hid_test")
    assert out["found"] is True and out["result"] == "returned", out
    assert "HID:DIR /apps/.test/hid_test" in _texts(sim)


def test_launch_dev_app_by_dir_name_and_by_id(hid):
    sim, _, _ = hid
    out = _launch(sim, "hid_dev")
    assert out["found"] is True and out["result"] == "returned", out
    assert "HID:DEV_RAN /apps/.dev/hid_dev" in _texts(sim)
    out = _launch(sim, "com.dev.hid_dev")
    assert out["found"] is True and out["result"] == "returned", out


def test_hidden_app_is_sandboxed_to_its_own_dir(hid):
    sim, _, _ = hid
    _launch(sim, "hid_test")
    t = _texts(sim)
    assert "HID:OWN true" in t
    for line in ("HID:SIBLING false", "HID:SIBLING_DEV false", "HID:HELLO false"):
        assert line in t, (line, t)


def test_test_root_wins_over_dev_root(hid):
    sim, _, sd = hid
    clash = sd / "apps" / ".dev" / "hid_test"
    clash.mkdir(parents=True)
    (clash / "app.json").write_text(json.dumps({"id": "com.dev.clash"}))
    (clash / "main.lua").write_text('picocalc.sys.log("HID:CLASH_DEV")\n')
    out = _launch(sim, "hid_test")
    assert out["id"] == "com.test.hid_test", out
    assert "HID:CLASH_DEV" not in _texts(sim)


def test_listed_app_wins_over_hidden_one(hid):
    sim, _, sd = hid
    listed = sd / "apps" / "hid_test"
    listed.mkdir()
    (listed / "app.json").write_text(json.dumps(
        {"id": "com.listed.hid_test", "name": "Listed"}))
    (listed / "main.lua").write_text('picocalc.sys.log("HID:LISTED_RAN " .. APP_DIR)\n')
    # The launcher lists what it scanned: a listed app staged after boot
    # needs the rescan (a hidden one never does).
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.rescan_apps()
    sim.wait_for_log(r"apps rescanned", timeout=5.0, src="os", since_seq=seq)
    out = _launch(sim, "hid_test")
    assert out["result"] == "returned", out
    t = _texts(sim)
    assert "HID:LISTED_RAN /apps/hid_test" in t, t
    assert "HID:DIR /apps/.test/hid_test" not in t


# ── Refusals ────────────────────────────────────────────────────────────────


@pytest.mark.parametrize("bad", ["../x", "a/b", ".x", ".test", "..", "a/.dev/hid_dev",
                                 "x" * 64])
def test_bad_hidden_names_are_not_found(hid, bad):
    sim, _, sd = hid
    # Something a traversal could reach: /apps/.test/../hello.
    out = _launch(sim, bad)
    assert out["found"] is False and out["result"] == "load_failed", out
    assert "not found" in out["error"], out
    assert not any(t.startswith("HID:") for t in _texts(sim)), _texts(sim)


def test_missing_app_is_still_not_found(hid):
    sim, _, _ = hid
    out = _launch(sim, "no_such_app_xyz")
    assert out["found"] is False and out["result"] == "load_failed", out
    assert "not found" in out["error"], out


def test_hidden_app_without_main_is_not_found(hid):
    sim, _, sd = hid
    (sd / "apps" / ".test" / "hid_empty").mkdir()
    out = _launch(sim, "hid_empty")
    assert out["found"] is False, out


# ── The cap ─────────────────────────────────────────────────────────────────


@pytest.mark.sd(fixtures=[])
def test_hidden_apps_launch_with_a_full_launcher(sim_factory, test_sd_card):
    """A card holding MAX_APPS listed apps cannot list one more, but a hidden
    app, staged after boot, still launches (no reboot, no rescan)."""
    sd = test_sd_card
    cap = launcher_app_cap()
    have = sum(1 for a in (sd / "apps").iterdir() if a.is_dir())
    for i in range(cap - have):
        stage_lua_app(sd, f"full_{i:03d}", "return\n", hidden=False)
    sim = sim_factory(sd)
    stage_lua_app(sd, "after_boot", 'picocalc.sys.log("HID:AFTER_BOOT")\n')
    stage_lua_app(sd, "after_boot_2", 'picocalc.sys.log("HID:AFTER_BOOT_2")\n')
    # One listed app past the cap is dropped: only a hidden one is reachable.
    stage_lua_app(sd, "over_cap", 'picocalc.sys.log("HID:OVER")\n', hidden=False)
    for name, mark in (("after_boot", "HID:AFTER_BOOT"),
                       ("after_boot_2", "HID:AFTER_BOOT_2")):
        out = _launch(sim, name)
        assert out["found"] is True and out["result"] == "returned", (name, out)
        assert mark in _texts(sim)


# ── mv ──────────────────────────────────────────────────────────────────────


def test_mv_moves_an_app_into_the_dev_root(hid):
    sim, target, sd = hid
    app = sd / "apps" / "hid_move"
    app.mkdir()
    (app / "app.json").write_text(json.dumps({"id": "com.test.hid_move"}))
    (app / "main.lua").write_text('picocalc.sys.log("HID:MOVED_RAN " .. APP_DIR)\n')
    out = target.command("mv /apps/hid_move /apps/.dev/hid_move")
    assert out == ["[DEV] Moved: /apps/hid_move -> /apps/.dev/hid_move"], out
    assert not app.exists()
    assert (sd / "apps" / ".dev" / "hid_move" / "main.lua").exists()
    res = _launch(sim, "hid_move")
    assert res["found"] is True and res["result"] == "returned", res
    assert "HID:MOVED_RAN /apps/.dev/hid_move" in _texts(sim)


def test_mv_creates_the_missing_parent(hid):
    _, target, sd = hid
    (sd / "data" / "mvsrc").mkdir()
    out = target.command("mv /data/mvsrc /data/deep/er/mvdst")
    assert out[0].startswith("[DEV] Moved:"), out
    assert (sd / "data" / "deep" / "er" / "mvdst").is_dir()


def test_mv_refuses_to_overwrite(hid):
    _, target, sd = hid
    (sd / "data" / "mva").mkdir()
    (sd / "data" / "mvb").mkdir()
    (sd / "data" / "mva" / "f").write_text("a")
    out = target.command("mv /data/mva /data/mvb")
    assert "destination exists" in out[0], out
    assert (sd / "data" / "mva" / "f").exists()
    assert not (sd / "data" / "mvb" / "f").exists()


def test_mv_refuses_system_source_and_target(hid):
    _, target, sd = hid
    (sd / "data" / "mvc").mkdir()
    for cmd in ("mv /system/lib /data/lib2", "mv /data/mvc /system/mvc",
                "mv /SYSTEM/lib /data/lib3", "mv /system /data/sys"):
        out = target.command(cmd)
        assert out[0].startswith("[DEV] Error: mv"), (cmd, out)
        assert "off limits" in out[0] or "top-level" in out[0], (cmd, out)
    assert (sd / "system" / "lib").is_dir()
    assert (sd / "data" / "mvc").is_dir()


def test_mv_refuses_bad_arguments(hid):
    _, target, sd = hid
    (sd / "data" / "mvd").mkdir()
    cases = {
        "mv /data/missing /data/x": "no such file",
        "mv /data/mvd /data/mvd/inner": "into itself",
        "mv /data/mvd": "Usage",
        "mv data/mvd /data/y": "Usage",
        "mv /data/mvd /data/../y": "bad path",
    }
    for cmd, frag in cases.items():
        out = target.command(cmd)
        assert frag in out[0], (cmd, out)
    assert (sd / "data" / "mvd").is_dir()


# ── The .test/<name> form ───────────────────────────────────────────────────


def test_root_form_launches_only_from_that_root(hid):
    sim, _, sd = hid
    # A listed twin (same dir name and id) of the hidden app: plain `launch`
    # prefers it, `.test/<name>` cannot be shadowed by it.
    twin = sd / "apps" / "hid_test"
    twin.mkdir()
    (twin / "app.json").write_text(json.dumps(
        {"id": "com.test.hid_test", "name": "Listed Twin"}))
    (twin / "main.lua").write_text('picocalc.sys.log("HID:TWIN_RAN " .. APP_DIR)\n')
    sim.rescan_apps()
    sim.wait_for_log(r"apps rescanned", timeout=5.0, src="os")
    assert _launch(sim, "hid_test")["result"] == "returned"
    assert "HID:TWIN_RAN /apps/hid_test" in _texts(sim)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    out = _launch(sim, ".test/hid_test")
    assert out["found"] is True and out["result"] == "returned", out
    assert "HID:DIR /apps/.test/hid_test" in _texts(sim, seq)
    # By id, and the other root.
    assert _launch(sim, ".test/com.test.hid_test")["found"] is True
    assert _launch(sim, ".dev/hid_dev")["found"] is True
    # Only that root: a .test app is not in .dev, and the reverse.
    assert _launch(sim, ".dev/hid_test")["found"] is False
    assert _launch(sim, ".test/hid_dev")["found"] is False
    assert _launch(sim, ".test/../hello")["found"] is False
    assert _launch(sim, ".test/")["found"] is False
    assert _launch(sim, ".other/hid_test")["found"] is False


def test_lookup_is_quiet_and_picks_by_id_across_roots(hid):
    sim, _, sd = hid
    (sd / "apps" / ".test" / "hid_nomain").mkdir()   # not an app: no warning
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    out = _launch(sim, "com.dev.hid_dev")
    assert out["found"] is True, out
    out = sim.get_output()["stdout"]
    assert "failed to read" not in out and "both main.lua" not in out, out[-600:]


# ── mv: the spellings FatFS reads as the same directory ─────────────────────


@pytest.mark.parametrize("cmd", [
    "mv //system /data/sys", "mv /./system /data/sys", "mv /system. /data/sys",
    "mv /\\system /data/sys", "mv /SYSTEM/lib /data/lib3",
    "mv /system./lib /data/lib4", "mv /data/mvx /system./data",
    "mv /data/mvx //system/data", "mv /data/mvx /SYSTEM/x/y",
    "mv /apps /data/apps2", "mv /data /x", "mv //apps/ /y",
])
def test_mv_spellings_cannot_reach_system_or_the_roots(hid, cmd):
    _, target, sd = hid
    (sd / "data" / "mvx").mkdir()
    out = target.command(cmd)
    assert out[0].startswith("[DEV] Error: mv"), (cmd, out)
    assert (sd / "system" / "lib").is_dir() and (sd / "apps").is_dir()
    assert not (sd / "data" / "sys").exists() and not (sd / "system" / "data").exists()


@pytest.mark.parametrize("dst", [
    "/data/mvi/bar", "/data//mvi/bar", "/data/./mvi/bar", "/data/mvi./bar",
    "/data\\mvi\\bar", "/data/mvi/a/b/c",
])
def test_mv_into_itself_is_refused(hid, dst):
    _, target, sd = hid
    (sd / "data" / "mvi").mkdir()
    (sd / "data" / "mvi" / "keep").write_text("k")
    out = target.command(f"mv /data/mvi {dst}")
    assert out[0].startswith("[DEV] Error: mv"), (dst, out)
    assert (sd / "data" / "mvi" / "keep").read_text() == "k"
    assert not (sd / "data" / "mvi" / "bar").exists()


def test_mv_a_case_only_rename_says_why(hid):
    _, target, sd = hid
    (sd / "data" / "mvcase").mkdir()
    out = target.command("mv /data/mvcase /data/mvcase")
    assert "destination exists" in out[0], out


def test_mv_refuses_the_running_apps_directory(hid):
    sim, target, sd = hid
    app = sd / "apps" / ".test" / "hid_run"
    app.mkdir()
    (app / "app.json").write_text(json.dumps({"id": "com.test.hid_run"}))
    (app / "main.lua").write_text("picocalc.sys.sleep(60000)\n")
    sim.launch_app(".test/hid_run")
    sim.wait_for_log(r"start hid_run", timeout=5.0, src="os")
    out = target.command("mv /apps/.test/hid_run /apps/.dev/hid_run")
    assert "running app" in out[0], out
    assert (app / "main.lua").exists()
    sim.exit_app()
    sim.wait_for_exit(timeout=10)
    out = target.command("mv /apps/.test/hid_run /apps/.dev/hid_run")
    assert out[0].startswith("[DEV] Moved:"), out


def test_list_all_while_a_hidden_app_runs_leaves_its_entry_alone(hid):
    """The simulator serves `list all` mid-app (firmware only at the
    launcher): it must not use the running hidden app's entry as scratch."""
    sim, target, sd = hid
    app = sd / "apps" / ".test" / "hid_busy"
    app.mkdir()
    (app / "app.json").write_text(json.dumps(
        {"id": "com.test.hid_busy", "name": "Busy"}))
    (app / "main.lua").write_text("picocalc.sys.sleep(60000)\n")
    sim.launch_app("hid_busy")
    sim.wait_for_log(r"start Busy", timeout=5.0, src="os")
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    for _ in range(3):
        target.command("list all")
    texts = _texts(sim, seq)
    assert any("Hidden Dev" in t and "[.dev]" in t for t in texts), texts
    running = sim.call("get_running_app")
    assert running.get("running") and running.get("name") == "Busy", running
    sim.exit_app()
    assert sim.wait_for_exit(timeout=10)["result"] in ("returned", "exit_sentinel")
