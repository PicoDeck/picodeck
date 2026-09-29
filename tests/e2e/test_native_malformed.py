"""Malformed native ELFs are refused by the simulator's loader, cleanly.

specs/test-audit-2026-09-24.md §3.5 (b), R9. Each case builds a tiny ELF32
ARM PIE with elfgen.build_elf(), breaks one field, stages it as
/apps/badelf_<case>/main.elf and launches it. The simulator's Unicorn loader
(simulator/unicorn_runner.c) validates with the same src/os/elf_plan.c as the
firmware's native_loader.c, so these cases pin the shared validation through
a real launch:

- the launch finishes with app.exited result "load_failed" and error equal
  to the exact elf_strerror() text of the check the case targets (a case
  that trips an earlier check would be testing the wrong thing). The
  reason travels with the RPC outcome (unicorn_runner.c load_refused()), so
  there is no race against the separately drained stderr pipe;
- /system/error.log gains the firmware loader's "--- NATIVE ERROR [<app>]
  ---" entry with that reason, and nothing else on the SD card changes (no
  file created, modified or removed);
- nothing ran: no "Starting emulation" line;
- stderr also carries "[UNICORN] ELF rejected: <reason>" (secondary, polled
  with a bound since stderr is drained by its own thread);
- the simulator is still alive and answers ping, and the conftest health
  hook sees no crash log and no sanitizer report.

The host unit tests (tests/unit/test_elf_plan.c) and the fuzzer cover
elf_plan itself; this module proves the simulator's launch path uses it and
survives every refusal.
"""

from __future__ import annotations

import json
import re
import time
from pathlib import Path

import pytest

import elfgen
from elfgen import (DT_DEBUG, DT_REL, DT_RELENT, DT_RELSZ, IMAGE_SIZE,
                    R_ARM_RELATIVE, build_elf, default_dyn)

pytestmark = pytest.mark.native

# case id -> (build_elf knobs, expected elf_strerror() text)
CASES = {
    # §3.5 (b), in the audit's order
    "truncated_20_bytes": (dict(truncate=20), "ELF: file too small"),
    "bad_magic": (dict(magic=b"\x7fELG"), "ELF: bad magic"),
    "et_exec": (dict(e_type=elfgen.ET_EXEC), "ELF: must be PIE (ET_DYN)"),
    "phnum_0": (dict(e_phnum=0), "ELF: bad phdr count"),
    "phnum_ffff": (dict(e_phnum=0xFFFF), "ELF: bad phdr count"),
    "phentsize_8": (dict(e_phentsize=8), "ELF: bad phdr entry size"),
    "phoff_wraps": (dict(e_phoff=0xFFFFFFF0), "ELF: phdr table out of bounds"),
    "seg_offset_filesz_wrap": (
        dict(load=dict(p_offset=0xFFFFFF00, p_filesz=0x200, p_memsz=0x200)),
        "ELF: segment data out of bounds"),
    "seg_filesz_gt_memsz": (
        dict(load=dict(p_memsz=IMAGE_SIZE - 0x40)),
        "ELF: segment filesz > memsz"),
    "seg_vaddr_memsz_wrap": (
        dict(load=dict(p_vaddr=0xFFFFFF80)),
        "ELF: segment vaddr overflow"),
    "dynamic_outside_image": (
        dict(dynamic=dict(p_vaddr=0x10000)),
        "ELF: dynamic section out of bounds"),
    "dt_rel_outside_image": (
        dict(dyn_entries=default_dyn(DT_REL=0x10000)),
        "ELF: relocation table out of bounds"),
    "dt_relsz_huge": (
        dict(dyn_entries=default_dyn(DT_RELSZ=0xFFFFFFF8)),
        "ELF: relocation table out of bounds"),
    "r_offset_outside_image": (
        dict(rel_entries=[(0x10000, R_ARM_RELATIVE)]),
        "ELF: relocation target out of bounds"),
    "no_dt_null": (
        dict(dyn_entries=[(DT_REL, elfgen.REL_OFF), (DT_RELSZ, 8),
                          (DT_RELENT, 8), (DT_DEBUG, 0)]),
        "ELF: dynamic section unterminated"),
    # Beyond §3.5: the other elf_plan refusals a crafted image can reach.
    "elfclass64": (dict(ei_class=2), "ELF: must be 32-bit little-endian"),
    "big_endian": (dict(ei_data=2), "ELF: must be 32-bit little-endian"),
    "not_arm": (dict(e_machine=62), "ELF: must be ARM"),
    "entry_outside_image": (dict(e_entry=0x10001),
                            "ELF: entry point out of bounds"),
    "image_too_large": (  # > the simulator's 8 MB code region
        dict(load=dict(p_memsz=0x01000000)), "ELF: image too large"),
    "no_pt_load": (dict(load=dict(p_type=6)),  # PT_PHDR
                   "ELF: no PT_LOAD segments"),
    # R_ARM_PC24 (1): ABS32/GLOB_DAT/JUMP_SLOT are known (skipped but
    # bounds-checked), anything else is refused.
    "unknown_reloc_type": (dict(rel_entries=[(elfgen.DATA_OFF, 1)]),
                           "ELF: unsupported relocation type"),
    "bad_relent": (dict(dyn_entries=default_dyn(DT_RELENT=12)),
                   "ELF: relocation table out of bounds"),
}

LOAD_TIMEOUT = 15.0


def stage_native_app(sd: Path, name: str, elf: bytes) -> Path:
    """Write /apps/<name>/{app.json,main.elf} onto the SD card `sd`."""
    app_dir = Path(sd) / "apps" / name
    app_dir.mkdir(parents=True, exist_ok=True)
    (app_dir / "app.json").write_text(json.dumps({
        "id": f"com.test.{name}", "name": name,
        "description": "E2E native test app", "version": "1.0",
        "author": "PicoDeck E2E"}, indent=2))
    (app_dir / "main.elf").write_bytes(elf)
    return app_dir


def sd_snapshot(sd: Path) -> dict:
    """{relative path: (size, mtime_ns)} for every file and dir on the card."""
    out = {}
    for p in Path(sd).rglob("*"):
        st = p.stat()
        out[str(p.relative_to(sd))] = (p.is_dir(), st.st_size if p.is_file() else 0,
                                       st.st_mtime_ns if p.is_file() else 0)
    return out


ERROR_LOG = "system/error.log"


def stderr_since(sim, mark: int) -> str:
    return "\n".join(sim.get_output()["stderr"].splitlines()[mark:])


def wait_stderr(sim, mark: int, pattern: str, timeout: float = 5.0) -> str:
    """stderr after `mark` once it matches `pattern` (the pipe is drained by
    a background thread, so a line can trail the RPC notification)."""
    deadline = time.monotonic() + timeout
    while True:
        err = stderr_since(sim, mark)
        if re.search(pattern, err) or time.monotonic() > deadline:
            return err
        time.sleep(0.05)


def expected_error(reason: str, case: str) -> str:
    # image_too_large carries the simulator's code-region size as detail
    return (reason + ": > 8388608 byte code region"
            if case == "image_too_large" else reason)


def test_baseline_image_loads_and_returns(simulator, test_sd_card):
    """The unmutated elfgen image is accepted and runs (bx lr), so every
    refusal below is caused by its one mutation, not by the generator."""
    stage_native_app(test_sd_card, "goodelf", build_elf())
    simulator.launch_app("goodelf")
    outcome = simulator.wait_for_exit(timeout=LOAD_TIMEOUT)
    out = simulator.get_output()
    assert outcome.get("found"), outcome
    assert outcome.get("result") == "returned", (outcome, out["stderr"][-2000:])
    assert "ELF rejected" not in out["stderr"]
    assert "[UNICORN] Starting emulation" in out["stdout"], out["stdout"][-2000:]
    assert "[UNICORN] Normal exit: app returned from picodeck_main()" in out["stdout"]


@pytest.mark.parametrize("case", list(CASES))
def test_malformed_elf_refused(simulator, test_sd_card, case):
    knobs, reason = CASES[case]
    name = f"badelf_{case}"
    stage_native_app(test_sd_card, name, build_elf(**knobs))
    before = sd_snapshot(test_sd_card)
    mark = len(simulator.get_output()["stderr"].splitlines())

    log_path = Path(test_sd_card) / ERROR_LOG
    log_before = log_path.read_text() if log_path.exists() else ""

    simulator.launch_app(name)
    outcome = simulator.wait_for_exit(timeout=LOAD_TIMEOUT)
    want = expected_error(reason, case)

    # Primary: the RPC-delivered outcome.
    assert outcome.get("found"), outcome
    assert outcome.get("result") == "load_failed", outcome
    assert outcome.get("error") == want, outcome

    # The firmware's error.log record, and no other SD change.
    added = (log_path.read_text() if log_path.exists() else "")[len(log_before):]
    assert f"--- NATIVE ERROR [{name}] ---\n{reason}\n" in added, added
    after = sd_snapshot(test_sd_card)
    after.pop(ERROR_LOG, None)
    before.pop(ERROR_LOG, None)
    assert after == before, "the refused launch changed the SD card"

    assert simulator.ping(), "simulator stopped answering after the refusal"

    # Secondary: the stderr line (bounded poll: separate pipe and thread).
    err = wait_stderr(simulator, mark, r"\[UNICORN\] ELF rejected: ")
    rejected = re.findall(r"\[UNICORN\] ELF rejected: ([^\n]*)", err)
    assert rejected and rejected[0] == want, (rejected, err)
    out = simulator.get_output()["stdout"]
    assert "Starting emulation" not in out.split(f"Loading native app: /apps/{name}")[-1]


@pytest.mark.sd(fixtures=[], reserve=len(CASES) + 1)  # see README, "App cap"
def test_sim_survives_every_refusal_in_a_row(simulator, test_sd_card):
    """All the malformed images back to back in one simulator, then a valid
    one: a refusal leaves no state behind that breaks the next launch."""
    for case, (knobs, _) in CASES.items():
        stage_native_app(test_sd_card, f"badelf_{case}", build_elf(**knobs))
    stage_native_app(test_sd_card, "goodelf", build_elf())
    for case in CASES:
        simulator.launch_app(f"badelf_{case}")
        outcome = simulator.wait_for_exit(timeout=LOAD_TIMEOUT)
        assert outcome.get("result") == "load_failed", (case, outcome)
        assert outcome.get("error") == expected_error(CASES[case][1], case), (case, outcome)
    simulator.launch_app("goodelf")
    assert simulator.wait_for_exit(timeout=LOAD_TIMEOUT).get("result") == "returned"
    assert simulator.ping()
