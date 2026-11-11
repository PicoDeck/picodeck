"""Stage the in-repo apps (apps/) and drive them from the test gamepad.

The test pad (`sim.pad`, the firmware's `pad` dev command) is a pad source,
the path a Bluetooth controller takes: it lights picocalc.gamepad and never
the keyboard's masks, so it reaches only an app that reads the gamepad.
Each test stages the real app from apps/ onto its own card (the module asks
for room: pytest.mark.sd(fixtures=[], reserve=1)); staging hello replaces
the copy the card already has (simulator/assets/sd_card).
The waits assume the wall clock (no --virtual-time).
Used by tests/e2e/test_pad_*.py
(docs/superpowers/plans/2026-11-10-gamepad-apps.md).
"""

from __future__ import annotations

import json
import shutil
import time
from pathlib import Path

from helpers import app_id_of

APPS = Path(__file__).resolve().parents[2] / "apps"
# An app's clean exits: returning from main.lua, or picocalc.sys.exit().
EXITED = ("returned", "exit_sentinel")
# ui.confirm ignores input for 400 ms after it opens (src/os/ui.c).
CONFIRM_GRACE = 0.7


def stage(sim, name: str) -> Path:
    """Copy apps/<name> onto the simulator's card as /apps/<name>."""
    dest = Path(sim.sd_card_path) / "apps" / name
    shutil.copytree(APPS / name, dest, dirs_exist_ok=True)
    return dest


def rebind(sim, name: str, mapping: dict) -> None:
    """Write the app's own bindings (/data/<id>/gamepad.json), read at its
    next launch. `name` must be staged already."""
    sd = Path(sim.sd_card_path)
    path = sd / "data" / app_id_of(sd, name) / "gamepad.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(mapping))


def mark(sim) -> int:
    """The sequence number the next log entry will get."""
    return sim.get_log_buffer(tail=1).get("next_seq", 0)


def set_wifi(sim, status: str) -> None:
    """Pin what the network reports (simulator/CLAUDE.md, set_wifi_state):
    "connecting" keeps an app waiting for WiFi, "not_available" makes WiFi
    unavailable, "connected" restores the real state."""
    sim.call("set_wifi_state", {"status": status})


def wait_until(check, timeout: float = 10.0, step: float = 0.05):
    """Poll check() until it returns something truthy, and return that."""
    deadline = time.monotonic() + timeout
    while True:
        value = check()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise TimeoutError(f"not met within {timeout} s: {check}")
        time.sleep(step)


def launch(sim, name: str, settle: float = 0.8) -> None:
    """Stage and launch apps/<name>, wait until it runs, then `settle` more
    seconds for its first frames. The test pad is connected first, with
    nothing held: a button held as an app starts stays hidden until it is
    released, and the header's gamepad icon is up before any screenshot."""
    stage(sim, name)
    sim.pad("none")
    sim.launch_app(name)
    wait_until(lambda: (sim.call("get_running_app") or {}).get("name"))
    time.sleep(settle)


def press(sim, button: str, gap: float = 0.25) -> None:
    """One press of a test-pad button ("a", "down", "select", ...): held
    100 ms from the first poll that sees it, then `gap` s for the release."""
    sim.pad(button, 100)
    time.sleep(gap)


def tap(sim, key: str, gap: float = 0.25) -> None:
    """One keyboard key (sim.keypress), read by the OS, then `gap` s."""
    r = sim.keypress(key)
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)
    time.sleep(gap)


def answer_confirm(sim, button: str = "a") -> None:
    """Answer the ui.confirm the app just opened: A is yes, B is no."""
    time.sleep(CONFIRM_GRACE)
    press(sim, button)


def _exited(sim) -> dict:
    outcome = sim.wait_for_exit(timeout=10)
    assert outcome.get("result") in EXITED, outcome
    return outcome


def finish(sim) -> dict:
    """Press B on the test pad and require the app to exit cleanly."""
    sim.pad("b", 100)
    return _exited(sim)


def finish_key(sim, key: str) -> dict:
    """Press a keyboard key and require the app to exit cleanly."""
    sim.keypress(key)
    return _exited(sim)


def region(sim, box: tuple) -> bytes:
    """The screen's RGB bytes inside box = (left, top, right, bottom)."""
    return sim.screenshot_pil().convert("RGB").crop(box).tobytes()


def wait_region(sim, box: tuple, pred, timeout: float = 5.0) -> bytes:
    """Wait until pred(region(sim, box)) holds; return that region."""
    def check():
        now = region(sim, box)
        return now if pred(now) else None
    return wait_until(check, timeout)


def count_pixels(sim, box: tuple, pred) -> int:
    """How many pixels inside box satisfy pred((r, g, b))."""
    raw = region(sim, box)
    return sum(1 for i in range(0, len(raw), 3) if pred(tuple(raw[i:i + 3])))
