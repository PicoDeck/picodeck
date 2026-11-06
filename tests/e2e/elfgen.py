"""Build tiny ELF32 ARM PIE images with struct.pack, valid or deliberately
malformed, for the native-loader E2E tests (test_native_malformed.py).

The valid image is the smallest thing both PicoDeck loaders accept and run
(src/os/elf_plan.c validates for the firmware's native_loader.c and the
simulator's unicorn_runner.c alike): ET_DYN, EM_ARM, one RWX PT_LOAD that
covers the whole file at vaddr 0, and a PT_DYNAMIC naming one DT_REL table
with a single R_ARM_RELATIVE entry. The code is `bx lr`, so picodeck_main()
returns at once and the launch outcome is "returned".

File layout (file offset == virtual address):

    0x00  ELF header (52 bytes)
    0x34  program headers: [0] PT_LOAD, [1] PT_DYNAMIC (2 x 32 bytes)
    0x80  code: bx lr; nop                                 <- e_entry | 1
    0x88  data word = 0x80 (relocated by +load base)       <- r_offset
    0x90  DT_REL table: one Elf32_Rel {0x88, R_ARM_RELATIVE}
    0xA0  dynamic: DT_REL, DT_RELSZ, DT_RELENT, DT_NULL
    0xC0  end of file / image

build_elf(**knobs) returns the bytes; each knob overrides one field, which
is how every malformation in specs/test-audit-2026-09-24.md §3.5 is made.
"""

from __future__ import annotations

import struct
from typing import Optional

# ELF constants
ET_EXEC, ET_DYN = 2, 3
EM_ARM = 40
PT_LOAD, PT_DYNAMIC = 1, 2
PF_RWX = 7
DT_NULL, DT_REL, DT_RELSZ, DT_RELENT, DT_DEBUG = 0, 17, 18, 19, 21
R_ARM_RELATIVE = 23

EHDR_SIZE = 52
PHDR_SIZE = 32

PHOFF = 0x34
CODE_OFF = 0x80
DATA_OFF = 0x88
REL_OFF = 0x90
DYN_OFF = 0xA0
IMAGE_SIZE = 0xC0

THUMB_BX_LR = 0x4770
THUMB_NOP = 0xBF00


def _u32(v: int) -> int:
    return v & 0xFFFFFFFF


def build_elf(*,
              magic: bytes = b"\x7fELF",
              ei_class: int = 1,          # ELFCLASS32
              ei_data: int = 1,           # little-endian
              e_type: int = ET_DYN,
              e_machine: int = EM_ARM,
              e_entry: int = CODE_OFF | 1,
              e_flags: int = 0x05000000,  # EABI5, no float-ABI flag
              e_phoff: int = PHOFF,
              e_phentsize: int = PHDR_SIZE,
              e_phnum: int = 2,
              load: Optional[dict] = None,
              dynamic: Optional[dict] = None,
              dyn_entries: Optional[list] = None,
              rel_entries: Optional[list] = None,
              truncate: Optional[int] = None) -> bytes:
    """Return an ELF image; every argument overrides one field of the valid
    baseline.

    load / dynamic: overrides for the PT_LOAD / PT_DYNAMIC program header
      (keys p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags,
      p_align).
    dyn_entries: [(d_tag, d_val), ...] for the dynamic section (at most 4
      entries fit; the default ends with DT_NULL).
    rel_entries: [(r_offset, r_info), ...] for the DT_REL table (1 fits).
    truncate: keep only the first N bytes of the file.
    """
    img = bytearray(IMAGE_SIZE)

    ident = bytearray(16)
    ident[0:4] = magic[:4].ljust(4, b"\0")
    ident[4] = ei_class
    ident[5] = ei_data
    ident[6] = 1  # EV_CURRENT
    struct.pack_into("<16sHHIIIIIHHHHHH", img, 0,
                     bytes(ident), e_type, e_machine, 1, _u32(e_entry),
                     _u32(e_phoff), 0, _u32(e_flags), EHDR_SIZE,
                     e_phentsize & 0xFFFF, e_phnum & 0xFFFF, 40, 0, 0)

    ph_load = dict(p_type=PT_LOAD, p_offset=0, p_vaddr=0, p_paddr=0,
                   p_filesz=IMAGE_SIZE, p_memsz=IMAGE_SIZE, p_flags=PF_RWX,
                   p_align=4)
    ph_load.update(load or {})
    ph_dyn = dict(p_type=PT_DYNAMIC, p_offset=DYN_OFF, p_vaddr=DYN_OFF,
                  p_paddr=DYN_OFF, p_filesz=IMAGE_SIZE - DYN_OFF,
                  p_memsz=IMAGE_SIZE - DYN_OFF, p_flags=6, p_align=4)
    ph_dyn.update(dynamic or {})
    for i, ph in enumerate((ph_load, ph_dyn)):
        struct.pack_into("<8I", img, PHOFF + i * PHDR_SIZE,
                         *(_u32(ph[k]) for k in (
                             "p_type", "p_offset", "p_vaddr", "p_paddr",
                             "p_filesz", "p_memsz", "p_flags", "p_align")))

    struct.pack_into("<HH", img, CODE_OFF, THUMB_BX_LR, THUMB_NOP)
    struct.pack_into("<I", img, DATA_OFF, CODE_OFF)

    if rel_entries is None:
        rel_entries = [(DATA_OFF, R_ARM_RELATIVE)]
    assert len(rel_entries) <= (DYN_OFF - REL_OFF) // 8
    for i, (off, info) in enumerate(rel_entries):
        struct.pack_into("<II", img, REL_OFF + 8 * i, _u32(off), _u32(info))

    if dyn_entries is None:
        dyn_entries = [(DT_REL, REL_OFF), (DT_RELSZ, 8 * len(rel_entries)),
                       (DT_RELENT, 8), (DT_NULL, 0)]
    assert len(dyn_entries) <= (IMAGE_SIZE - DYN_OFF) // 8
    for i, (tag, val) in enumerate(dyn_entries):
        struct.pack_into("<iI", img, DYN_OFF + 8 * i, tag, _u32(val))

    data = bytes(img)
    if truncate is not None:
        data = data[:truncate]
    return data


def default_dyn(**overrides) -> list:
    """The baseline dynamic entries with some values replaced, e.g.
    default_dyn(DT_REL=0x10000)."""
    vals = {"DT_REL": REL_OFF, "DT_RELSZ": 8, "DT_RELENT": 8}
    vals.update(overrides)
    return [(DT_REL, vals["DT_REL"]), (DT_RELSZ, vals["DT_RELSZ"]),
            (DT_RELENT, vals["DT_RELENT"]), (DT_NULL, 0)]
