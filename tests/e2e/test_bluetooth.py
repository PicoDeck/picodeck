"""Bluetooth gamepads in the simulator (issue #26): the system menu's
Settings -> Bluetooth page, the bt_enabled setting, the bonding file and the
PAD_SOURCE_BT path, over the simulator's scripted radio (simulator/sim_bt.c).

The radio is a script, not BTstack: devices "in range" come from the
`bt sim add` dev command, and the controller, a search and a connection
complete after short delays. What runs for real is the menu page
(src/os/system_menu.c), the bonding store and its file
(src/drivers/bt_pad_store.c, /system/bluetooth.dat), the setting
(/system/config.json) and the pad source a connected pad publishes on. The
firmware's BTstack side is covered on the device (test_bt_hw.py) and, for
reports, by tests/unit/test_hid_pad.c.
"""

import json
import struct
import time
from pathlib import Path

PAD = "11:22:33:44:55:66"
PAD_NAME = "Wireless Controller"
PHONE = "AA:BB:CC:DD:EE:01"

# Main page at the launcher: Battery, Settings, ...  Settings: Brightness,
# Battery %, Show FPS, Controls, Bluetooth, ...
TO_BLUETOOTH = ["down", "enter", "down", "down", "down", "down", "enter"]


def bt(sim, args="", ok=True):
    r = sim.call("dev_command", {"cmd": ("bt " + args).strip()})
    if ok:
        assert r.get("ok"), r
    return r.get("output", "")


def status(sim) -> dict:
    out = bt(sim)
    assert out.startswith("BT: "), out
    st = dict(kv.split("=", 1) for kv in out[4:].split(" note=")[0].split())
    st["note"] = out.split(' note="', 1)[1].rstrip('"')
    return st


def wait_status(sim, key, want, timeout=10.0) -> dict:
    deadline = time.time() + timeout
    while True:
        st = status(sim)
        if st.get(key) == want:
            return st
        if time.time() >= deadline:
            raise AssertionError(f"bt {key}={st.get(key)!r}, want {want!r}: {st}")
        time.sleep(0.1)


def _mark(sim):
    return sim.get_log_buffer(tail=1).get("next_seq", 0)


def _keys(sim, keys, delay=0.2):
    for k in keys:
        sim.keypress(k)
        time.sleep(delay)


def _config(sim) -> dict:
    p = Path(sim.sd_card_path) / "system" / "config.json"
    return json.loads(p.read_text()) if p.exists() else {}


def _bonds(sim) -> dict:
    """/system/bluetooth.dat as {tag: value bytes} (bt_pad_store.h)."""
    p = Path(sim.sd_card_path) / "system" / "bluetooth.dat"
    if not p.exists():
        return {}
    b = p.read_bytes()
    assert b[:4] == b"PDBT" and b[4] == 1, b[:8]
    out, at = {}, 6
    for _ in range(b[5]):
        tag, n = struct.unpack_from("<IB", b, at)
        out[tag] = b[at + 5:at + 5 + n]
        at += 5 + n
    assert at == len(b)
    return out


def _paired_names(sim) -> list:
    return [v[10:].split(b"\0")[0].decode() for t, v in _bonds(sim).items()
            if t >> 8 == 0x504450]  # 'PDP' + slot


def _open_page(sim):
    sim.keypress("menu")
    time.sleep(0.3)
    _keys(sim, TO_BLUETOOTH)


def _pair(sim):
    """Bluetooth on, a pad and a phone in range, a search, then the pad
    chosen on the page. Leaves the page open on the pad's row."""
    bt(sim, "on")
    wait_status(sim, "power", "on")
    bt(sim, f"sim add {PAD} 002508 {PAD_NAME}")
    bt(sim, f"sim add {PHONE} 5a020c Phone")
    since = _mark(sim)
    _open_page(sim)
    _keys(sim, ["down", "enter"])  # "Search for controllers"
    sim.wait_for_log(r"^\[BT\] Search done", timeout=10, since_seq=since)
    time.sleep(0.4)  # the page polls every 250 ms
    _keys(sim, ["down", "enter"])  # the first found row: the pad (pads first)
    sim.wait_for_log(rf"^\[BT\] Connected: {PAD_NAME}$", timeout=10,
                     since_seq=since)
    sim.wait_for_log(r"^\[BT\] bonds saved", timeout=10, since_seq=since)


def test_off_by_default_and_the_setting(simulator):
    sim = simulator
    st = status(sim)
    assert st["available"] == "1" and st["enabled"] == "0", st
    assert st["power"] == "off" and st["radio_in_use"] == "0", st
    assert "bt_enabled" not in _config(sim)

    # Settings -> Bluetooth -> Enter on "Bluetooth: Off" turns it on.
    since = _mark(sim)
    _open_page(sim)
    _keys(sim, ["enter"])
    sim.wait_for_log(r"^\[BT\] On \(", timeout=10, since_seq=since)
    assert _config(sim).get("bt_enabled") == "1"
    st = status(sim)
    assert st["enabled"] == "1" and st["power"] == "on", st

    # ... and off again: the key goes.
    _keys(sim, ["enter"])
    sim.wait_for_log(r"^\[BT\] Off$", timeout=10, since_seq=since)
    assert "bt_enabled" not in _config(sim)
    _keys(sim, ["esc", "esc", "esc"])
    assert status(sim)["enabled"] == "0"


def test_search_pair_and_bond_file(simulator):
    sim = simulator
    _pair(sim)
    st = status(sim)
    assert st["link"] == "connected" and st["ready"] == "1", st
    assert st["peer"] == PAD and st["profile"] == "PlayStation", st
    assert _paired_names(sim) == [PAD_NAME]
    out = bt(sim, "paired")
    assert out == f"BT paired: n=1; {PAD} {PAD_NAME}", out
    # Search results list pads first.
    found = bt(sim, "found")
    assert found.index(PAD) < found.index(PHONE), found
    _keys(sim, ["esc", "esc", "esc"])


def test_pad_reaches_the_game_and_link_loss_releases(simulator):
    sim = simulator
    _pair(sim)
    _keys(sim, ["esc", "esc", "esc"])
    seq = _mark(sim)
    sim.launch_app("gamepad_test")
    sim.wait_for_log(r"^GP:READY$", timeout=10, since_seq=seq)
    mark = _mark(sim)
    bt(sim, "sim press a+right")
    sim.wait_for_log(r"^GP:PRESS ", timeout=10, since_seq=mark)
    # The link drops with both held: they release, nothing sticks.
    bt(sim, "sim drop")
    sim.wait_for_log(r"^GP:RELEASE ", timeout=10, since_seq=mark)
    texts = [l.get("text", "") for l in sim.get_log_lines(mark)]
    edges = [t.split(" held=")[0] for t in texts
             if t.startswith("GP:PRESS ") or t.startswith("GP:RELEASE ")]
    assert edges == ["GP:PRESS right,a", "GP:RELEASE right,a"], texts
    assert status(sim)["link"] == "none"
    # The bonded pad reconnects on its own and drives the game again.
    bt(sim, f"sim wake {PAD}")
    wait_status(sim, "link", "connected")
    mark = _mark(sim)
    bt(sim, "sim press b")
    sim.wait_for_log(r"^GP:PRESS b", timeout=10, since_seq=mark)
    bt(sim, "sim press none")
    sim.wait_for_log(r"^GP:RELEASE b", timeout=10, since_seq=mark)
    sim.keypress("esc")
    sim.wait_for_log(r"^GP:DONE$", timeout=10, since_seq=seq)
    sim.wait_for_exit(timeout=10)


def test_home_on_the_pad_opens_the_menu(simulator):
    sim = simulator
    _pair(sim)
    _keys(sim, ["esc", "esc", "esc"])
    seq = _mark(sim)
    sim.launch_app("gamepad_test")
    sim.wait_for_log(r"^GP:READY$", timeout=10, since_seq=seq)
    bt(sim, "sim press home")
    bt(sim, "sim press none")
    # The menu opens over the game: Up wraps to Exit App, A chooses it.
    time.sleep(0.5)
    bt(sim, "sim press up")
    bt(sim, "sim press none")
    time.sleep(0.3)
    bt(sim, "sim press a")
    bt(sim, "sim press none")
    out = sim.wait_for_exit(timeout=15)
    assert out["result"] == "exit_sentinel", out


def test_forget_and_unknown_devices(simulator):
    sim = simulator
    _pair(sim)
    # The page is still open, and its cursor stayed on the pad's row: the
    # pad moved from Found to Paired, which now holds that row.
    time.sleep(0.4)
    since = _mark(sim)
    _keys(sim, ["del"])
    st = status(sim)
    assert st["note"].startswith("Connected"), st  # one Del only arms it
    _keys(sim, ["del"])
    sim.wait_for_log(r"^\[BT\] Wireless Controller disconnected$", timeout=10,
                     since_seq=since)
    sim.wait_for_log(r"^\[BT\] bonds saved", timeout=10, since_seq=since)
    assert _paired_names(sim) == []
    assert status(sim)["paired"] == "0"
    _keys(sim, ["esc", "esc", "esc"])
    # Forgotten (or never paired): its reconnection is refused.
    assert "refused" in bt(sim, f"sim wake {PAD}", ok=False)
    assert status(sim)["link"] == "none"


def test_status_errors(simulator):
    sim = simulator
    assert bt(sim, "connect nonsense", ok=False).startswith("Usage:")
    assert bt(sim, "bogus", ok=False).startswith("Usage:")
    # Searching needs the controller on.
    assert "not on" in bt(sim, "scan", ok=False)
