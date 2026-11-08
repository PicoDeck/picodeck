"""The perf rig links gfx3d.c as the firmware does. Run: python3 -m pytest tests/unit/test_hot_gfx_slots.py -v

src/hot_gfx.ld pins gfx3d.c's hot sections at fixed offsets (process_mesh first, then a
sprite's fill in a slot of fixed parts), and tools/perf_rig/guest/guest.ld repeats that
layout so the rig's estimates fall on the firmware's cache sets. The two are kept by hand:
this holds them to the same sections, in the same order, at the same offsets.
"""
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / "src" / "hot_gfx.ld"
RIG = ROOT / "tools" / "perf_rig" / "guest" / "guest.ld"


def bare(text: str) -> str:
    """The script without its comments."""
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def slot_symbol(text: str) -> str:
    """The symbol the script sets just before .gfx3d_sprite_text: where the sprite slot starts."""
    start = re.search(r"(\w+)\s*=\s*\.\s*;\s*(?:KEEP\()?\*\S*gfx3d\.c?\.?o\(\.gfx3d_sprite_text\)",
                      bare(text))
    assert start, "no sprite slot: the symbol set just before .gfx3d_sprite_text"
    return start.group(1)


def layout(text: str) -> list:
    """gfx3d.c's own sections and the sprite slot's offsets, in link order: ("slot",) where the
    slot starts, ("sec", name) for a section, ("at", n) for `. = <slot> + n`."""
    text = bare(text)
    slot = slot_symbol(text)
    token = re.compile(r"\(\s*(\.gfx3d_\w+)\s*\)|\.\s*=\s*" + re.escape(slot) +
                       r"\s*\+\s*(\d+)\s*;|\b" + re.escape(slot) + r"\s*=\s*\.\s*;")
    out = []
    for m in token.finditer(text):
        if m.group(1):
            out.append(("sec", m.group(1)))
        elif m.group(2):
            out.append(("at", int(m.group(2))))
        else:
            out.append(("slot",))
    # Every `. = <slot> ...` must have been read: one written another way (hex, a sum, no
    # spaces where they are expected) would drop out of both lists alike and pass unseen.
    assigns = len(re.findall(r"\.\s*=\s*" + re.escape(slot) + r"\b", text))
    read = sum(1 for t in out if t[0] == "at")
    assert assigns == read, (f"{assigns} assignments `. = {slot} ...`, {read} read: "
                             f"write each as `. = {slot} + <decimal>;`")
    return out


def test_the_rig_links_gfx3d_as_the_firmware_does():
    fw, rig = layout(FIRMWARE.read_text()), layout(RIG.read_text())
    assert fw == rig


def test_the_layout_is_what_the_firmware_pins():
    fw = layout(FIRMWARE.read_text())
    assert fw[0] == ("sec", ".gfx3d_mesh_text") and fw[1] == ("slot",)
    assert fw[-1] == ("sec", ".gfx3d_tex_text"), "textured code last"
    offsets = [t[1] for t in fw if t[0] == "at"]
    assert offsets == sorted(offsets) and len(offsets) >= 2, offsets


def test_a_slot_size_that_differs_is_seen():
    rig = RIG.read_text()
    first = re.search(r"__sprite_slot \+ (\d+);", rig)
    moved = rig.replace(first.group(0), f"__sprite_slot + {int(first.group(1)) + 8};", 1)
    assert layout(moved) != layout(FIRMWARE.read_text())


def test_an_offset_written_another_way_fails():
    # Both scripts with the same offset in hex would parse alike without the guard. The one
    # rewritten is a slot assignment, `. = <slot> + <decimal>;` for the symbol the script sets
    # as the slot, whatever other sums the script holds before it.
    for path in (FIRMWARE, RIG):
        text = bare(path.read_text())
        slot = slot_symbol(text)
        m = re.search(r"\.\s*=\s*" + re.escape(slot) + r"\s*\+\s*(\d+)\s*;", text)
        assert m, f"{path.name}: no `. = {slot} + <decimal>;`"
        layout(text)  # as written, it reads
        hexed = text[:m.start(1)] + hex(int(m.group(1))) + text[m.end(1):]
        with pytest.raises(AssertionError, match="assignments"):
            layout(hexed)


def test_a_section_out_of_order_is_seen():
    rig = RIG.read_text()
    a, b = "KEEP(*gfx3d.o(.gfx3d_sprite_aux_text))", "KEEP(*gfx3d.o(.gfx3d_sprite_runs_text))"
    assert a in rig and b in rig
    swapped = rig.replace(a, "@@").replace(b, a).replace("@@", b)
    assert layout(swapped) != layout(FIRMWARE.read_text())
