"""tools/check_hot_text.py fails the build when a pinned gfx3d function slips. Run: python3 -m pytest tests/unit/test_check_hot_text.py -v

The checker reads the firmware's symbols with nm; here it gets a symbol table made from its
own lists, laid out as src/hot_text.ld and src/hot_gfx.ld lay them out, so it must pass, and
then the same table with one thing wrong, so it must fail and say what.
"""
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_hot_text as cht  # noqa: E402

BASE = 0x100A0000


def good_map() -> dict:
    """Every group on its 8 KB boundary holding its functions, .cold_libgcc after them, .hot_gfx
    on the next boundary with HOT_GFX inside and every PINNED_GFX function at its symbol."""
    syms, at = {}, BASE
    for lo, hi, funcs in cht.GROUPS.values():  # each group ends where the next starts
        syms[lo] = at
        for k, f in enumerate(funcs):
            syms[f] = at + 64 * (k + 1)
        at += 4 * cht.SET_PERIOD
        syms[hi] = at
    syms["__cold_libgcc_start__"] = at
    for k, f in enumerate(cht.COLD_LIBGCC):
        syms[f] = at + 16 * (k + 1)
    syms["__cold_libgcc_end__"] = at + 1024
    gfx = at + cht.SET_PERIOD
    syms["__hot_gfx_start__"] = gfx
    for k, (f, mark) in enumerate(cht.PINNED_GFX):
        syms.setdefault(mark, gfx + cht.SET_PERIOD * k)
        syms[f] = syms[mark]
    for k, f in enumerate(cht.HOT_GFX):
        syms.setdefault(f, gfx + 8 * cht.SET_PERIOD + 128 * k)
    syms["__hot_gfx_end__"] = gfx + 12 * cht.SET_PERIOD
    return syms


def run(monkeypatch, capsys, syms):
    monkeypatch.setattr(cht, "symbols", lambda nm, elf: dict(syms))
    monkeypatch.setattr(sys, "argv", ["check_hot_text.py", "picodeck.elf"])
    rc = cht.main()
    out = capsys.readouterr()
    return rc, out.out + out.err


def test_the_map_as_linked_passes(monkeypatch, capsys):
    rc, out = run(monkeypatch, capsys, good_map())
    assert rc == 0 and "check_hot_text: ok" in out, out


@pytest.mark.parametrize("func,mark", cht.PINNED_GFX)
def test_a_slipped_pin_fails(monkeypatch, capsys, func, mark):
    syms = good_map()
    syms[func] = syms[mark] + 8  # one line on: another cache set
    rc, out = run(monkeypatch, capsys, syms)
    assert rc == 1 and f"gfx: {func} at" in out and "its pin slipped" in out, out


def test_a_clone_of_a_pinned_function_counts_as_it(monkeypatch, capsys):
    syms = good_map()
    func, mark = cht.PINNED_GFX[-1]
    syms[func + ".constprop.0"] = syms.pop(func)  # GCC renamed it, at the same place
    rc, out = run(monkeypatch, capsys, syms)
    assert rc == 0, out
    syms[func + ".constprop.0"] += 2
    rc, out = run(monkeypatch, capsys, syms)
    assert rc == 1 and "its pin slipped" in out, out


def test_a_lost_pin_symbol_fails(monkeypatch, capsys):
    syms = good_map()
    func, mark = cht.PINNED_GFX[1]
    del syms[mark]
    rc, out = run(monkeypatch, capsys, syms)
    assert rc == 1 and f"no symbol {mark}" in out, out
    syms = good_map()
    del syms[func]
    rc, out = run(monkeypatch, capsys, syms)
    assert rc == 1 and f"no symbol {func}" in out, out
