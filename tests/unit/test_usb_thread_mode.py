"""TinyUSB's CDC reads and writes reach it from thread mode only through the SDK's
stdio_usb driver, which holds stdio_usb_mutex for them. Called without it, TinyUSB can
be interrupted holding one of its own mutexes, and its background task (an interrupt on
Core 0) then waits for that mutex on the core that holds it, for good (src/console_gate.h,
which holds the rule for the write side; `put` broke it on the read side).

dev_commands.c and console_gate.c need the SDK's USB stack, so this cannot run them on
the host: tests/e2e/test_screenshot_integrity.py::test_binary_put_under_log_noise does on
the device. This keeps the direct calls from coming back.
Run: python3 -m pytest tests/unit/test_usb_thread_mode.py -v"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "src"
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
DIRECT = re.compile(r"\btud_cdc_(?:n_)?(?:read|read_char|write|write_char|write_flush|write_str)\s*\(")


def test_no_direct_cdc_transfers():
    found = []
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in (".c", ".cpp", ".h"):
            continue
        text = COMMENT.sub("", path.read_text(errors="replace"))
        found += [f"{path.relative_to(SRC)}: {m.group(0)}" for m in DIRECT.finditer(text)]
    assert not found, found
