"""picocalc.gamepad.getAxis: analog sticks and triggers (issue #68).

The fixture writes every change of the six axes to a file (the device's serial
capture drops log lines). The `pad` dev command sets the axes as a stick would
report them, so the dead zone applies: `pad lx=-0.6` reads about -0.575. The
exact shape, the combine rule and the decode of each pad's reports are covered
by tests/unit/test_pad_source.c and test_hid_pad.c; these check the whole path,
on the simulator and on the device.
"""

import shutil
import subprocess
import time
from pathlib import Path

import pytest

from helpers import stage_lua_app
from test_bluetooth import PAD, _keys, _pair, bt, wait_status

APP_ID = "com.test.axis_target"
AXIS_APP = """
local pc = picocalc
local sys, gp, d = pc.sys, pc.gamepad, pc.display
local AXES = {gp.AXIS_LX, gp.AXIS_LY, gp.AXIS_RX, gp.AXIS_RY, gp.AXIS_LT,
              gp.AXIS_RT}
local rows, last = {}, nil
local function save()
    local f = pc.fs.open(pc.fs.appPath("axes.txt"), "w")
    pc.fs.write(f, table.concat(rows, "\\n") .. "\\n")
    pc.fs.close(f)
end
d.clear(d.BLUE)
d.flush()
local deadline = sys.getTimeMs() + 60000
while sys.getTimeMs() < deadline do
    sys.resetIdleTimer()  -- never dim: a press that wakes the screen is swallowed
    pc.input.update()
    local v = {}
    for i, a in ipairs(AXES) do v[i] = string.format("%.3f", gp.getAxis(a)) end
    local s = table.concat(v, " ")
    if s ~= last then
        last = s
        rows[#rows + 1] = s
        save()
    end
    sys.sleep(16)
end
"""

REST = (0.0, 0.0, 0.0, 0.0, 0.0, 0.0)


def _rows(target):
    try:
        text = target.read_file(f"/data/{APP_ID}/axes.txt").decode()
    except FileNotFoundError:
        return []
    rows = []
    for ln in text.splitlines():
        try:
            row = tuple(float(v) for v in ln.split())
        except ValueError:  # a line caught half written
            continue
        if len(row) == 6:
            rows.append(row)
    return rows


def _close(row, want):
    return all(abs(a - b) <= 0.02 for a, b in zip(row, want))


def _wait_row(target, want, timeout=10.0):
    """Until the fixture's latest row reads `want` (0.02 either way)."""
    deadline = time.monotonic() + timeout
    while True:
        rows = _rows(target)
        if rows and _close(rows[-1], want):
            return rows
        if time.monotonic() >= deadline:
            raise AssertionError(f"axes read {rows[-5:]}, want {want}")
        time.sleep(0.05)


def _start(target):
    target.stage_lua_app("axis_target", AXIS_APP, id=APP_ID)
    target.delete_file(f"/data/{APP_ID}/axes.txt")
    assert target.launch_app("axis_target")["launched"]
    _wait_row(target, REST)


@pytest.mark.both
def test_axes_on_target(target):
    """Sticks and triggers from the `pad` command; the D-pad fallback, from
    the pad's buttons and from a key; a stick in use over the D-pad."""
    _start(target)
    try:
        target.pad("lx=-1+rt=1")
        _wait_row(target, (-1, 0, 0, 0, 0, 1))
        # Right and down positive; the right stick's corner keeps its direction
        # with its length clamped.
        target.pad("ly=1+rx=1+ry=-1+lt=1")
        _wait_row(target, (0, 1, 0.707, -0.707, 1, 0))

        # A stick past half way also sets the D-pad bit: it reads the stick,
        # not 1.
        target.pad("left+lx=-0.6")
        _wait_row(target, (-0.575, 0, 0, 0, 0, 0))
        # No stick: the D-pad reads -1, 0 or +1, inside the dead zone too.
        target.pad("left")
        _wait_row(target, (-1, 0, 0, 0, 0, 0))
        target.pad("right+lx=0.05")
        _wait_row(target, (1, 0, 0, 0, 0, 0))
        target.pad("up+rx=1")  # the right stick does not hide the D-pad
        _wait_row(target, (0, -1, 1, 0, 0, 0))
        target.pad("down")
        _wait_row(target, (0, 1, 0, 0, 0, 0))
        target.pad("none")
        _wait_row(target, REST)

        # A disconnect takes the axes with it.
        target.pad("lx=1+rt=0.5")
        _wait_row(target, (1, 0, 0, 0, 0, 0.5))
        target.pad("off")
        _wait_row(target, REST)

        # The keyboard's aliases read through the same fallback: a game that
        # calls only getAxis works on the keyboard.
        before = len(_rows(target))
        target.keypress("left")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            seen = _rows(target)[before:]
            if any(_close(r, (-1, 0, 0, 0, 0, 0)) for r in seen) and \
                    _close(seen[-1], REST):
                break
            time.sleep(0.05)
        else:
            raise AssertionError(f"left arrow read {_rows(target)[before:]}")
    finally:
        # The hardware session outlives this test: never leave the test pad
        # connected (header icon, launcher navigation) for the next one.
        target.pad("off")
        target.exit_app()
        target.wait_for_exit(timeout=20)


def test_timed_hold_ends_for_the_axes(simulator):
    target = _sim_target(simulator)
    _start(target)
    assert simulator.pad("lx=1", hold_ms=100) == "Pad: lx=1.000 for 100 ms"
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        rows = _rows(target)
        if any(_close(r, (1, 0, 0, 0, 0, 0)) for r in rows) and \
                _close(rows[-1], REST):
            break
        time.sleep(0.05)
    else:
        raise AssertionError(f"read {_rows(target)}")
    simulator.exit_app()
    simulator.wait_for_exit(timeout=15)


def _sim_target(sim):
    import hw_target
    return hw_target.SimTarget(sim)


def test_arguments_and_constants(simulator):
    sim = simulator
    stage_lua_app(Path(sim.sd_card_path), "axis_args", """
local gp, log = picocalc.gamepad, picocalc.sys.log
log(string.format("AX:CONST %d %d %d %d %d %d", gp.AXIS_LX, gp.AXIS_LY,
    gp.AXIS_RX, gp.AXIS_RY, gp.AXIS_LT, gp.AXIS_RT))
log("AX:BAD " .. tostring(pcall(gp.getAxis)) ..
    " " .. tostring(pcall(gp.getAxis, -1)) ..
    " " .. tostring(pcall(gp.getAxis, 6)) ..
    " " .. tostring(pcall(gp.getAxis, 0.5)) ..
    " " .. tostring(pcall(gp.getAxis, "lx")))
log("AX:TYPE " .. math.type(gp.getAxis(gp.AXIS_LX)))
log("AX:DONE")
""")
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("axis_args")
    sim.wait_for_log(r"^AX:DONE$", timeout=10, since_seq=seq)
    texts = [ln.get("text", "") for ln in sim.get_log_lines(seq)]
    assert "AX:CONST 0 1 2 3 4 5" in texts, texts
    assert "AX:BAD false false false false false" in texts, texts
    assert "AX:TYPE float" in texts, texts
    sim.wait_for_exit(timeout=10)


def test_pad_command_axes_and_errors(simulator):
    sim = simulator
    assert sim.pad("left+lx=-0.5+rt=1") == "Pad: left+lx=-0.500+rt=1.000"
    assert sim.pad("LY=.25") == "Pad: ly=0.250"
    assert sim.pad("RX=1+ry=-1.0+lt=0") == "Pad: rx=1.000+ry=-1.000"
    assert sim.pad("a+up+home+lx=0.125") == "Pad: up+a+home+lx=0.125"
    assert sim.pad("none") == "Pad: none"
    for cmd, want in (("pad lx=2", "Error: bad pad axis value: lx=2"),
                      ("pad lx=-1.01", "Error: bad pad axis value: lx=-1.01"),
                      ("pad rt=-0.5", "Error: bad pad axis value: rt=-0.5"),
                      ("pad lt=1.5", "Error: bad pad axis value: lt=1.5"),
                      ("pad lx=", "Error: bad pad axis value: lx="),
                      ("pad lx=abc", "Error: bad pad axis value: lx=abc"),
                      ("pad lx=1.2.3", "Error: bad pad axis value: lx=1.2.3"),
                      ("pad lx=-", "Error: bad pad axis value: lx=-"),
                      ("pad zz=1", "Error: unknown pad axis: zz=1"),
                      ("pad a+zz=1", "Error: unknown pad axis: zz=1"),
                      ("pad lx=1+zz", "Error: unknown pad button: zz")):
        r = sim.call("dev_command", {"cmd": cmd})
        assert not r["ok"] and r["output"].startswith(want), (cmd, r)
    assert sim.pad("off") == "Pad: off"


def test_two_sources_combine_and_a_lost_link_lets_go(simulator):
    """Largest magnitude across sources; a Bluetooth pad's link loss zeroes its
    axes and leaves the other source's."""
    sim = simulator
    target = _sim_target(sim)
    _pair(sim)
    _keys(sim, ["esc", "esc", "esc"])
    _start(target)
    sim.pad("lx=-0.5")
    _wait_row(target, (-0.46, 0, 0, 0, 0, 0))
    assert bt(sim, "sim press lx=1+rt=1") == "BT sim: report"
    _wait_row(target, (1, 0, 0, 0, 0, 1))
    assert bt(sim, "sim press rt=1") == "BT sim: report"
    _wait_row(target, (-0.46, 0, 0, 0, 0, 1))
    bt(sim, "sim drop")
    wait_status(sim, "link", "none")
    _wait_row(target, (-0.46, 0, 0, 0, 0, 0))
    bt(sim, f"forget {PAD}")
    sim.pad("off")
    _wait_row(target, REST)
    sim.exit_app()
    sim.wait_for_exit(timeout=15)


# The host's game controllers: hal_pad.c's real path over a fake SDL controller.
SHIM = Path(__file__).parent / "fixtures" / "sdl_pad_axes_shim.c"


@pytest.fixture
def axes_shim(tmp_path, monkeypatch):
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc or not shutil.which("pkg-config"):
        pytest.skip("needs a C compiler and pkg-config")
    flags = subprocess.run(["pkg-config", "--cflags", "sdl2"],
                           capture_output=True, text=True)
    if flags.returncode != 0:
        pytest.skip("needs the SDL2 headers")
    so = tmp_path / "sdl_pad_axes_shim.so"
    r = subprocess.run([cc, "-shared", "-fPIC", "-o", str(so), str(SHIM),
                        *flags.stdout.split(), "-ldl"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        # A toolchain that is present but cannot build the shim is a bug in
        # the shim, not a missing tool: fail, so it cannot hide as a skip.
        pytest.fail(f"the shim did not compile: {r.stderr[-300:]}")
    values = tmp_path / "axes"
    monkeypatch.setenv("LD_PRELOAD", str(so))
    monkeypatch.setenv("PADAXES_FILE", str(values))
    return values


def test_host_controller_axes(axes_shim, sim_factory, test_sd_card):
    """SDL's raw values reach getAxis, past the dead zone: full deflection in
    both directions, -32768 included, and the trigger's 0..32767."""
    sim = sim_factory(test_sd_card, extra_args=["--gamepad"])
    target = _sim_target(sim)
    _start(target)

    def report(*values):
        tmp = axes_shim.with_suffix(".new")
        tmp.write_text(" ".join(str(v) for v in values))
        tmp.replace(axes_shim)  # never a half-written file

    report(0, 0, 0, 0, 0, 0)  # plugged in, at rest
    _wait_row(target, REST)
    report(-32768, 0, 32767, 0, 32767, 16384)
    _wait_row(target, (-1, 0, 1, 0, 1, 0.5))
    assert "[PAD] Connected: Shim Pad" in sim.get_output()["stdout"]
    # Radial: past the dead zone a small component keeps its small value.
    report(1000, -32768, 0, 32767, 0, 0)
    _wait_row(target, (0.031, -1, 0, 1, 0, 0))
    report(3000, 0, 2000, 0, 300, 300)  # inside every dead zone
    _wait_row(target, REST)
    sim.exit_app()
    sim.wait_for_exit(timeout=15)
