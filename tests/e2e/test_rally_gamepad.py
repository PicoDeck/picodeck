"""Rally reads the gamepad (issue #25, PicoDeck/rally `feature/gamepad`).

Stages the real Rally bundle (tools/bundle.py of the Rally repo) and drives it
with the keys the gamepad binds (F4 = A throttle, F5 = B brake, Backspace = Y
handbrake), asserting on the log lines Rally writes: `RALLY: state N`
(0 intro, 1 countdown, 2 racing, 3 finish) and `RALLY: input thr= brk= hb=`.
The sim does not show Rally's viewport reliably, so nothing here reads pixels.

    PICODECK_RALLY_DIR=/path/to/rally   (default ~/Projects/PicoDeck/rally-gamepad;
                                         main.elf must be built there: `make`)
"""
import io
import json
import os
import re
import sys
import time
import zipfile
from pathlib import Path

import pytest

from helpers import build_sd_card, new_simulator, stop_and_check

RALLY = Path(os.environ.get("PICODECK_RALLY_DIR",
                            Path.home() / "Projects" / "PicoDeck" / "rally-gamepad"))


def _bundle_dir(tmp_path):
    if not (RALLY / "tools" / "bundle.py").exists() or not (RALLY / "main.elf").exists():
        pytest.skip(f"{RALLY} is not a built Rally checkout (PICODECK_RALLY_DIR)")
    sys.path.insert(0, str(RALLY / "tools"))
    import bundle
    out = tmp_path / "bundle"
    zipfile.ZipFile(io.BytesIO(bundle.build_zip_bytes(str(RALLY)))).extractall(out)
    return out


def make_sim(request, simulator_binary, tmp_path, rebind=None):
    app = _bundle_dir(tmp_path)
    extra = [(app, "apps/rally")]
    if rebind:
        g = tmp_path / "gamepad.json"
        g.write_text(json.dumps(rebind))
        extra.append((g, "data/net.picodeck.rally/gamepad.json"))
    sd = build_sd_card(tmp_path / "sd_card", extra=extra)
    sim = new_simulator(request.config, simulator_binary, sd,
                        tmp_path / "crash.log", test_mode=False)
    sim.log_seq = 0
    return sim


def new_logs(sim):
    """Rally log lines since the last call."""
    out = []
    while True:
        r = sim.get_log_buffer(since_seq=sim.log_seq)
        out += [l["text"] for l in r["lines"] if "RALLY:" in l["text"]]
        sim.log_seq = r["next_seq"]
        if not r.get("more"):
            return out


def wait_log(sim, pattern, secs):
    """Poll the log until a line matches; returns every line read meanwhile."""
    seen, end = [], time.time() + secs
    while time.time() < end:
        seen += new_logs(sim)
        if any(re.search(pattern, l) for l in seen):
            return seen
        time.sleep(0.2)
    return seen


def hold(sim, keys, secs):
    for k in keys:
        sim.call("inject_button", {"button": k, "action": "press"})
    time.sleep(secs)
    seen = new_logs(sim)
    for k in keys:
        sim.call("inject_button", {"button": k, "action": "release"})
    time.sleep(0.5)
    return seen + new_logs(sim)


def start_and_race(sim, start_key):
    sim.launch_app("rally")
    assert wait_log(sim, r"state 0", 20), "Rally did not reach the intro"
    sim.keypress(start_key)
    assert wait_log(sim, r"state 1", 5), f"{start_key} did not start the countdown"
    assert wait_log(sim, r"state 2", 10), "the countdown never ended"


def test_default_bindings_drive_the_car(request, simulator_binary, tmp_path):
    sim = make_sim(request, simulator_binary, tmp_path)
    try:
        start_and_race(sim, "f5")      # B (brake) also starts the stage
        new_logs(sim)
        a = hold(sim, ["f4"], 1.5)     # A: throttle only
        assert any("thr=1 brk=0" in l for l in a), a
        b = hold(sim, ["f5"], 1.5)     # B: brake only
        assert any("thr=0 brk=1" in l for l in b), b
        y = hold(sim, ["backspace"], 1.5)   # Y: handbrake
        assert any("hb=1" in l for l in y), y
    finally:
        stop_and_check(sim)


def test_default_bindings_keep_pace_and_autopilot(request, simulator_binary, tmp_path):
    """L=F2 and R=F3 are not buttons Rally reads, so F3 still toggles the
    autopilot (the device-gate procedure) on default bindings."""
    sim = make_sim(request, simulator_binary, tmp_path)
    try:
        start_and_race(sim, "f4")
        new_logs(sim)
        sim.keypress("f3")
        assert wait_log(sim, r"autopilot 1", 5), "F3 did not toggle the autopilot"
    finally:
        stop_and_check(sim)


def test_rebound_a_and_b_follow_the_new_keys(request, simulator_binary, tmp_path):
    sim = make_sim(request, simulator_binary, tmp_path,
                   rebind={"a": ["F3"], "b": ["F2"]})
    try:
        sim.launch_app("rally")
        assert wait_log(sim, r"state 0", 20)
        sim.keypress("f4")
        sim.keypress("f5")
        time.sleep(3)
        assert not any("state 1" in l for l in new_logs(sim)), \
            "unbound F4/F5 started the race"
        sim.keypress("f3")             # A is F3 now
        assert wait_log(sim, r"state 1", 5), "F3 (A) did not start the countdown"
        assert wait_log(sim, r"state 2", 10)
        new_logs(sim)
        assert not any("thr=1" in l for l in hold(sim, ["f4"], 1.5)), \
            "unbound F4 throttled"
        held = hold(sim, ["f3"], 1.5)
        assert any("thr=1 brk=0" in l for l in held), held
        # F3 is a pad button here, not the autopilot shortcut
        assert not any("autopilot" in l for l in held + new_logs(sim)), held
        braked = hold(sim, ["f2"], 1.5)
        assert any("brk=1" in l for l in braked), braked
    finally:
        stop_and_check(sim)


def test_esc_quits(request, simulator_binary, tmp_path):
    sim = make_sim(request, simulator_binary, tmp_path)
    try:
        sim.launch_app("rally")
        assert wait_log(sim, r"state 0", 20)
        assert sim.call("get_running_app").get("running")
        sim.keypress("esc")
        time.sleep(3)
        assert not sim.call("get_running_app").get("running"), "Esc did not quit Rally"
    finally:
        stop_and_check(sim)
