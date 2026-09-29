"""Native (ELF) apps through the simulator's Unicorn runner.

specs/test-audit-2026-09-24.md R9 (b):

- smoke: the committed apps/hello_c/main.elf launches, draws its text, and
  returns to the launcher on a key press;
- API layout: tests/e2e/apps/native_api_probe (source tests/e2e/native/
  api_probe.c, built against sdk/native/os.h) logs api->version and the
  sub-table layout it sees. The expected values come from the sources, not
  from constants in this file: g_api.version from src/main.c, the layout
  from src/os/os.h (parsed by tools/check_native_abi.py, which CI's
  native-sdk job also runs against the compiled headers).

Malformed images are in test_native_malformed.py.
"""

from __future__ import annotations

import importlib.util
import re

import pytest

from helpers import PROJECT_ROOT, known_bug, log_texts

pytestmark = pytest.mark.native

_spec = importlib.util.spec_from_file_location(
    "check_native_abi", PROJECT_ROOT / "tools" / "check_native_abi.py")
abi = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(abi)


@pytest.mark.sd(extra=[("apps/hello_c", "apps/hello_c")])
def test_hello_c_smoke(simulator):
    simulator.launch_app("hello_c")
    simulator.wait_for_log(r"\[LAUNCHER\] start Hello C \(com\.example\.hello\)",
                           timeout=10)
    simulator.wait_frames(3, timeout=10)

    # "Hello from C!" is drawn in white on black: the app really ran.
    img = simulator.screenshot_pil().convert("RGB")
    colors = dict((c, n) for n, c in img.getcolors(maxcolors=1 << 17))
    white = colors.get((255, 255, 255), 0)
    assert white > 50, f"no white text on screen ({white} white pixels)"
    out = simulator.get_output()
    assert "[NATIVE] Loading 'Hello C' via Unicorn Engine" in out["stdout"]
    assert "ELF rejected" not in out["stderr"]

    seq = simulator.keypress("a")["input_seq"]
    simulator.wait_input_consumed(seq, timeout=10)
    outcome = simulator.wait_for_exit(timeout=10)
    assert outcome.get("result") == "returned", outcome
    assert simulator.ping()


TABLES = [field for field, _typ, _size in abi.api_layout()["tables"]]

# The simulator's terminal table carries 5 trailing SLOT_TERM_PAD slots
# (simulator/unicorn_trampolines.c) "to maintain compatibility with os.h struct
# size", but os.h's picocalc_terminal_t has 42 members and the table is 47.
# Harmless while the pads trail the table (every sub-table pointer is written
# explicitly), but it is exactly the drift this test guards against.
KNOWN_TABLE_DRIFT = {
    "terminal": "task-24 finding: simulator terminal table has 5 stale "
                "SLOT_TERM_PAD slots (47 vs os.h's 42)",
}


@pytest.fixture(scope="module")
def probe(sim_module):
    """The native_api_probe's PROBE lines from one run."""
    sim = sim_module
    sim.launch_app("native_api_probe")
    outcome = sim.wait_for_exit(timeout=15)
    assert outcome.get("result") == "returned", outcome
    lines = [re.sub(r"^\[APP\] ", "", t) for t in log_texts(sim.get_log_lines())]
    lines = [t for t in lines if t.startswith("PROBE ")]
    assert lines and lines[-1] == "PROBE done", (
        "the probe did not log its layout:\n" + "\n".join(lines))
    table_lines = [t for t in lines[2:-1] if t.startswith("PROBE table ")]
    tables = {}
    for t in table_lines:
        m = re.fullmatch(r"PROBE table (\w+) size=(\d+) gap=(\d+)", t)
        assert m, lines
        tables[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    return {"lines": lines, "tables": tables,
            "order": [re.match(r"PROBE table (\w+)", t).group(1)
                      for t in table_lines]}


def test_api_version_matches_main_c(probe):
    """api->version as a native app reads it == g_api.version in src/main.c.
    A version read from the wrong offset (a layout drift) is garbage."""
    m = re.fullmatch(r"PROBE version=(\d+)", probe["lines"][0])
    assert m, probe["lines"]
    assert int(m.group(1)) == abi.api_version(), (
        f"native app sees api->version={m.group(1)}, src/main.c sets "
        f"g_api.version = {abi.api_version()}")


def test_api_struct_matches_os_h(probe):
    """The probe's compiled PicoCalcAPI (sdk/native/os.h) == src/os/os.h's."""
    layout = abi.api_layout()
    m = re.fullmatch(r"PROBE api size=(\d+) version_off=(\d+) tables=(\d+)",
                     probe["lines"][1])
    assert m, probe["lines"]
    assert tuple(map(int, m.groups())) == (
        layout["size"], layout["version_off"], len(layout["tables"])), (
        "the committed probe was built against a different PicoCalcAPI than "
        "src/os/os.h: rebuild it (make -C tests/e2e/native) and check "
        "sdk/native/os.h (python3 tools/check_native_abi.py)")
    assert probe["order"] == TABLES, probe["order"]


def test_api_gamepad_table(probe):
    """API version 9 appends the gamepad table after `version`: a native app
    that checks api->version >= 9 reaches it, nothing is held at launch, and
    getLabel reports the default binding of A (F4, alternate unbound). The
    label is a permanent string, as the device's flash one: kept from
    startup, it still reads F4 after 10000 more getLabel calls."""
    assert abi.api_version() >= 9
    lines = [t for t in probe["lines"] if t.startswith("PROBE gamepad ")]
    assert lines == ["PROBE gamepad buttons=0 pressed=0 label_a=F4 "
                     "label_a_alt=-",
                     "PROBE gamepad kept label_a=F4"], probe["lines"]


@pytest.mark.parametrize("table", [
    pytest.param(t, marks=[known_bug(KNOWN_TABLE_DRIFT[t])]
                 if t in KNOWN_TABLE_DRIFT else []) for t in TABLES])
def test_api_table_matches_os_h(probe, table):
    """Each sub-table: the probe's sizeof (sdk/native/os.h) == src/os/os.h's,
    and the simulator's trampoline table is exactly that long (tables are laid
    out back to back, so the gap to the next one is its slot count; the last
    table has no successor and is only size-checked)."""
    want = {f: (typ, size) for f, typ, size in abi.api_layout()["tables"]}
    typ, size = want[table]
    got, gap = probe["tables"][table]
    assert got == size, (f"{table}: probe sizeof({typ}) {got}, src/os/os.h "
                         f"{size}: rebuild the probe / check sdk/native/os.h")
    if table != TABLES[-1]:
        assert gap == got, (f"{table}: simulator table is {gap} bytes, "
                            f"sizeof({typ}) is {got}: trampoline slot count "
                            f"drifted (simulator/unicorn_trampolines.c)")
