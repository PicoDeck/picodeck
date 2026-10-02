"""C-Dogs reads the gamepad (issue #25, PicoDeck/cdogs `feature/gamepad`).

The shim logs every SDL key edge as `KEYEDGE down|up <scancode>` when it is
built with -DPICODECK_KEY_TRACE, and these tests assert on those lines. They
need that trace build of main.elf:

    PICODECK_CDOGS_TRACE_ELF=/path/to/main.elf   (the trace build)
    PICODECK_CDOGS_DIR=/path/to/cdogs            (default ~/Projects/PicoDeck/cdogs;
                                                  app.json and data/ come from it)

Both cases drive C-Dogs into a live mission with the pad's fire key (F4, or F3
once rebound), then check the pad's presses. Scancodes: X 27, Z 29, S 22, A 4,
D 7, Backspace 42, Right 79, Left 80.
"""
import json
import os
import re
import time
from pathlib import Path

import pytest

from helpers import build_sd_card, new_simulator, stop_and_check
import test_cdogs_memory as M

TRACE_ELF = os.environ.get("PICODECK_CDOGS_TRACE_ELF")
CDOGS = M.CDOGS_SRC
X, Z, S, A, D, BKSP, RIGHT, LEFT = 27, 29, 22, 4, 7, 42, 79, 80

# fire, switch weapon, gamepad override file
CASES = {
    "default": ("f4", "f5", None),
    "rebound": ("f3", "f2", {"a": ["F3"], "b": ["F2"], "right": ["D"], "left": ["A"], "x": ["S"]}),
}


@pytest.fixture(scope="module", params=list(CASES))
def sim(request, simulator_binary, tmp_path_factory):
    if not TRACE_ELF or not Path(TRACE_ELF).exists():
        pytest.skip("PICODECK_CDOGS_TRACE_ELF not set: needs a cdogs main.elf built "
                    "with -DPICODECK_KEY_TRACE")
    if not (CDOGS / "data" / "graphics").exists():
        pytest.skip(f"{CDOGS}/data not prepared (PICODECK_CDOGS_DIR)")
    fire, sw, rebind = CASES[request.param]
    base = tmp_path_factory.mktemp("cdogs_pad")
    extra = [(Path(TRACE_ELF), "apps/cdogs/main.elf"),
             (CDOGS / "app.json", "apps/cdogs/app.json"),
             (CDOGS / "data", "apps/cdogs/data")]
    if rebind:
        g = base / "gamepad.json"
        g.write_text(json.dumps(rebind))
        extra.append((g, "data/net.picodeck.cdogs/gamepad.json"))
    sd = build_sd_card(base / "sd_card", extra=extra)
    s = new_simulator(request.config, simulator_binary, sd, base / "crash.log",
                      test_mode=False)
    s.case, s.fire, s.sw = request.param, fire, sw
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            if s.call("ping", timeout=2.0).get("uptime_ms", 0) >= 1100:
                break
        except Exception:
            pass
        time.sleep(0.2)
    yield s
    stop_and_check(s)


def _edges_since(sim, seq):
    out = []
    while True:
        r = sim.get_log_buffer(since_seq=seq)
        for line in r["lines"]:
            m = re.search(r"KEYEDGE (down|up) (\d+)", line["text"])
            if m:
                out.append((m.group(1), int(m.group(2))))
        if not r.get("more"):
            return out
        seq = r["next_seq"]


def _tap(sim, key, want=None):
    """Tap `key`; return the key edges it produced. A tap shorter than one
    emulated frame can be missed, so retry (up to 4 times) until want(edges)."""
    for _ in range(4):
        seq = sim.get_log_buffer(tail=1)["next_seq"]
        sim.keypress(key)
        time.sleep(1.0)
        e = _edges_since(sim, seq)
        if want is None or want(e):
            return e
    return e


def _launch_to_menu(sim):
    """Launch C-Dogs once per simulator and wait for its main menu."""
    if getattr(sim, "launched", False):
        return
    sim.launch_app("cdogs")
    deadline = time.time() + 120
    while time.time() < deadline:
        txt = M._combined_output(sim)
        if M.MENU_READY_MARKER in txt:
            break
        time.sleep(0.2)
    assert M.MENU_READY_MARKER in M._combined_output(sim)
    time.sleep(2)
    sim.launched = True


def _check_pad_keys(sim):
    rebound = sim.case == "rebound"
    assert ("down", X) in _tap(sim, sim.fire, lambda e: ("down", X) in e), \
        "pad A did not press fire (X)"
    assert ("down", Z) in _tap(sim, sim.sw, lambda e: ("down", Z) in e), \
        "pad B did not press switch weapon (Z)"
    e = _tap(sim, "right", None if rebound else lambda e: bool(e))
    if rebound:
        assert not e, e          # the Right key is unbound: RIGHT is on D
    else:
        assert ("down", RIGHT) in e and ("up", RIGHT) in e, e

    if not rebound:
        e = _tap(sim, "del", lambda e: ("down", S) in e)
        assert ("down", S) in e, e              # pad X: grenade
        e = _tap(sim, "backspace", lambda e: ("down", A) in e)
        assert ("down", A) in e, e              # pad Y: map
        # (Backspace also stays a typed char on firmware; the sim's button
        # injection sends no char, so that half is covered by the shim harness.)
    else:
        # right is bound to D: the typed 'd' is the pad's RIGHT. (A D press
        # also goes through: D is not one of player 1's keys, so it is inert.)
        e = _tap(sim, "d", lambda e: bool(e))
        assert ("down", RIGHT) in e and ("up", RIGHT) in e, e
        # left is bound to A, which IS C-Dogs' map key: the typed 'a' must be
        # the pad's LEFT only, never an A (map) press
        e = _tap(sim, "a", lambda e: ("down", LEFT) in e)
        assert ("down", LEFT) in e and ("up", LEFT) in e, e
        assert not any(sc == A for _, sc in e), e
        # x is bound to S: the typed 's' is the grenade via the pad, once
        e = _tap(sim, "s", lambda e: ("down", S) in e)
        assert e.count(("down", S)) == 1, e


@pytest.mark.flaky(reason="C-Dogs mission drive: menu navigation in the sim "
                          "misses a step about 1 run in 4")
@pytest.mark.timeout(600)
def test_pad_presses_cdogs_keys_in_a_live_mission(sim):
    _launch_to_menu(sim)
    seen, stats = {"GFXSTAT": set()}, {"GFXSTAT": []}

    def poll():
        text = M._combined_output(sim)
        M._accumulate(text, M.parse_gfxstats, seen["GFXSTAT"], stats["GFXSTAT"])
        return text

    # Menu walk with the pad's own fire key as the confirm/join key.
    steps = [("enter", "")] * 5 + [(sim.fire, "")] + [("enter", "")] + \
            [(sim.fire, "")] + [("enter", "")] * 3 + [(sim.fire, "")] * 2
    M._advance_through_screens(sim, poll, steps)
    deadline = time.time() + 120
    while time.time() < deadline and not M._mission_started(stats["GFXSTAT"]):
        sim.keypress(sim.fire)
        end = time.time() + 12
        while time.time() < end and not M._mission_started(stats["GFXSTAT"]):
            poll()
            time.sleep(0.05)
    assert M._mission_started(stats["GFXSTAT"]), "never reached a live mission"
    time.sleep(3)
    _check_pad_keys(sim)


@pytest.mark.timeout(300)
def test_pad_presses_cdogs_keys_after_the_mission(sim):
    """The shim's event pump emits the pad's key edges on every screen, so this
    check does not depend on the mission drive above succeeding: if that one
    fails (flaky), the app is still at some menu and the edges are asserted
    there. Runs second because pressing keys at the menu moves its cursor."""
    _launch_to_menu(sim)
    _check_pad_keys(sim)
