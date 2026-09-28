"""Native app exit gives back everything the app took through the API.

specs/code-review-2026-09-24.md, Core Medium "Native app exit only cleans up
audio and menu items...": files, images, players, terminals and PSRAM a
native app never freed stayed allocated (and FatFS's 16 open-file slots
stayed taken) until a reboot.

tests/e2e/apps/native_leaky (source tests/e2e/native/leaky.c) opens 12
files, makes 8 160x160 images, leaves a sample player playing, plus a
sampleless player, a file player, a terminal, a MOD player and a qmiAlloc
block, then returns. tests/e2e/apps/mem_report (Lua) logs the PSRAM heap and
how many of 16 simultaneous opens succeed; it runs before and after.

--real-umm: the firmware's umm_malloc on a device-sized arena, so
psram_largest_block means what it means on hardware.
"""

from __future__ import annotations

import re

import pytest

from helpers import log_texts, write_wav

pytestmark = pytest.mark.native

# The mem_report baseline and the after-run differ by allocator noise only
# (a Lua VM of the same app, the same fixed-size log lines); the leak is
# 400 KB of images alone.
TOLERANCE = 16 * 1024


def _cursor(sim) -> int:
    lines = sim.get_log_lines()
    return lines[-1]["seq"] + 1 if lines else 0


def _run(sim, name: str, pattern: str) -> tuple[dict, list[str]]:
    start = _cursor(sim)
    sim.launch_app(name)
    outcome = sim.wait_for_exit(timeout=20)
    texts = [re.sub(r"^\[APP\] ", "", t)
             for t in log_texts(sim.get_log_lines(since_seq=start))]
    return outcome, [t for t in texts if re.search(pattern, t)]


def _mem(sim) -> dict:
    outcome, lines = _run(sim, "mem_report", r"^MEM ")
    assert outcome.get("result") == "returned", outcome
    assert lines, "mem_report logged nothing"
    m = re.fullmatch(r"MEM free=(\d+) largest=(\d+) files=(\d+)", lines[-1])
    assert m, lines
    return dict(zip(("free", "largest", "files"), map(int, m.groups())))


@pytest.fixture(scope="module")
def leaky(sim_module_factory):
    def setup(sd):
        # Long enough to still be playing when the app has returned.
        write_wav(sd / "apps" / "native_leaky" / "beep.wav", seconds=2.0)

    sim = sim_module_factory(setup=setup, extra_args=["--real-umm"])
    # One run of each first: the first launch of an app kind makes one-time
    # OS allocations that are never freed (the MOD engine's buffers, the
    # file player's WAV buffer, ...), so they must predate the baseline.
    _run(sim, "native_leaky", r"^LEAKY ")
    _mem(sim)
    before = _mem(sim)
    outcome, lines = _run(sim, "native_leaky", r"^LEAKY ")
    audio = sim.call("get_audio_state")
    after = _mem(sim)
    return {"sim": sim, "before": before, "after": after, "outcome": outcome,
            "lines": lines, "audio": audio}


def test_leaky_app_ran_and_leaked(leaky):
    """Precondition: the fixture really took every handle it leaks."""
    assert leaky["outcome"].get("result") == "returned", leaky["outcome"]
    lines = leaky["lines"]
    assert lines and lines[-1] == "LEAKY done", lines
    assert lines[:2] == ["LEAKY files=12 images=8 sample=1 player=1 idle=1",
                         "LEAKY fp=1 term=1 mod=1 qmi=1"], lines


def test_files_are_closed_on_exit(leaky):
    """All 16 FatFS open-file slots are free again after the native app."""
    assert leaky["before"]["files"] == 16, leaky["before"]
    assert leaky["after"]["files"] == 16, (
        f"only {leaky['after']['files']} of 16 files open after the native "
        "app exited: its handles were never closed")


def test_psram_is_returned_on_exit(leaky):
    """psram_free and the largest free block are back to the baseline."""
    b, a = leaky["before"], leaky["after"]
    assert b["free"] - a["free"] <= TOLERANCE, (
        f"{(b['free'] - a['free']) // 1024} KB of PSRAM still held after the "
        f"native app exited (before {b}, after {a})")
    assert b["largest"] - a["largest"] <= TOLERANCE, (
        f"largest free block shrank by {(b['largest'] - a['largest']) // 1024}"
        f" KB (before {b}, after {a})")


def test_players_are_stopped_on_exit(leaky):
    """The sample player the app left playing does not play on into the
    launcher (it held a mixer slot until the next Lua app's sound_init)."""
    assert leaky["audio"]["sound_players_active"] == 0, leaky["audio"]


def test_master_volume_is_reset_on_exit(leaky):
    """The app left the master volume at 20: the exit puts it back to 100,
    so the next app (and its MP3) is not quiet until a reboot."""
    assert leaky["audio"]["master_volume"] == 100, leaky["audio"]


def test_appconfig_binds_only_own_id(leaky):
    """Native gate: appconfig->load(<another app's id>) is refused and leaves
    the store unbound (Lua apps always bind to their own id)."""
    line = next(t for t in leaky["lines"] if t.startswith("LEAKY appconfig"))
    assert line == "LEAKY appconfig other=0 bound=(none)", line

