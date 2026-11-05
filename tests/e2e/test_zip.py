"""E2E tests for the hardened ZIP engine (zip_util) and archive handles.

The old zip path had NO traversal guard on the native side and a substring
".." check on the Lua side; the engine now validates entry names component-
wise and enforces entry-count / size caps. These tests feed it deliberately
hostile archives (crafted with python's zipfile below) and assert both the
Lua-visible behaviour (via "ZE " marker logs from the zip_test fixture app)
and the filesystem outcome (nothing escapes the destination).
"""
import io
import re
import zipfile
from pathlib import Path

import pytest


def _lines(sim):
    out = []
    for line in sim.get_log_buffer()["lines"]:
        out.append(line if isinstance(line, str) else line.get("text", ""))
    return out


def _wait_for(sim, marker, timeout=20.0):
    """Wait (event-driven) for a log line containing `marker`; returns the
    whole log joined."""
    try:
        sim.wait_for_log(re.escape(marker), timeout=timeout)
    except TimeoutError:
        pytest.fail(f"marker {marker!r} not seen within {timeout}s:\n"
                    + "\n".join(_lines(sim)[-30:]))
    return "\n".join(_lines(sim))


def _build_archives(app_dir: Path):
    """Craft the four archives zip_test/main.lua expects."""
    # hostile.zip: two good entries + five invalid names the engine must skip.
    with zipfile.ZipFile(app_dir / "hostile.zip", "w",
                         zipfile.ZIP_DEFLATED) as z:
        z.writestr("good.txt", "good content")
        z.writestr("dir/nested.txt", "nested content")
        z.writestr("../evil.txt", "escape attempt")
        z.writestr("/abs.txt", "absolute path")
        z.writestr("back\\slash.txt", "backslash")
        z.writestr("a/./b.txt", "dot component")
        z.writestr("ctrl\x01name.txt", "control byte")

    # caps.zip: a single 5MB member — over the 4MB in-memory read cap but
    # fine to stream to disk.
    with zipfile.ZipFile(app_dir / "caps.zip", "w",
                         zipfile.ZIP_DEFLATED) as z:
        z.writestr("big.bin", b"\x00" * (5 * 1024 * 1024))

    # manyfiles.zip: over the 8192-entry cap (stored, empty — stays small).
    with zipfile.ZipFile(app_dir / "manyfiles.zip", "w",
                         zipfile.ZIP_STORED) as z:
        for i in range(8300):
            z.writestr(f"f{i}.t", "")

    # truncated.zip: a valid archive cut in half.
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("whole.txt", "x" * 4096)
    data = buf.getvalue()
    (app_dir / "truncated.zip").write_bytes(data[: len(data) // 2])


def test_zip_hardening_and_handles(simulator, test_sd_card):
    app_dir = test_sd_card / "apps" / "zip_test"
    _build_archives(app_dir)

    simulator.launch_app("zip_test")
    joined = _wait_for(simulator, "ZE DONE", timeout=40.0)

    # extractAll succeeds while skipping every unsafe name
    assert "ZE HOSTILE ok=true" in joined
    assert "ZE HOSTILE_GOOD good content" in joined
    assert "ZE HOSTILE_NESTED nested content" in joined

    # handle API
    assert "ZE OPEN true" in joined
    assert "ZE EXISTS true false" in joined
    assert "ZE READ good content" in joined
    assert "ZE READMISS no such entry" in joined

    # 4MB in-memory read cap enforced; streaming extract of the same entry OK
    assert "ZE BIGREAD data=false err=size cap" in joined
    assert "ZE BIGX ok=true size=5242880" in joined

    # entry-count cap fails fast
    assert "ZE MANY ok=false" in joined

    # truncated archive: clean open failure
    assert "ZE TRUNC nil=true" in joined

    # open budget + lifecycle
    assert "ZE FIFTH nil=true" in joined
    assert "ZE CLOSED_RAISES true" in joined
    assert "ZE GC4 true" in joined

    # A destination that cannot be opened is named (issue #65).
    blocked = "ok=false err=cannot open /data/com.test.ziptest/out3/good.txt"
    assert f"ZE BLOCKED {blocked}" in joined
    assert f"ZE BLOCKED1 {blocked}" in joined

    # Filesystem outcome: none of the hostile names escaped the destination.
    out_dir = test_sd_card / "data" / "com.test.ziptest" / "out"
    assert (out_dir / "good.txt").exists()
    assert not list(test_sd_card.rglob("evil.txt")), "traversal escaped!"
    assert not (test_sd_card / "abs.txt").exists()
    assert not list(test_sd_card.rglob("*slash.txt"))
    # manyfiles destination must not have been created at all (fail-fast)
    out2 = test_sd_card / "data" / "com.test.ziptest" / "out2"
    assert not any(out2.rglob("*")) if out2.exists() else True
