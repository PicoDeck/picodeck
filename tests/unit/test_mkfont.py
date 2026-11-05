"""Unit tests for tools/mkfont.py. Run: python3 -m pytest tests/unit/test_mkfont.py -v"""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mkfont  # noqa: E402

TINY_BDF = """STARTFONT 2.1
FONT tiny
SIZE 4 75 75
FONTBOUNDINGBOX 3 4 0 0
CHARS 2
STARTCHAR A
ENCODING 65
DWIDTH 3 0
BBX 3 4 0 0
BITMAP
E0
A0
E0
00
ENDCHAR
STARTCHAR B
ENCODING 66
DWIDTH 2 0
BBX 2 4 0 0
BITMAP
C0
80
C0
00
ENDCHAR
ENDFONT
"""


def parse_header(data: bytes):
    magic, ver, flags, first, last, h, w, stride, res = struct.unpack("<4sBBBBBBBB", data[:12])
    return dict(magic=magic, ver=ver, flags=flags, first=first, last=last,
                height=h, max_width=w, stride=stride, reserved=res)


def test_bdf_roundtrip_is_proportional():
    glyphs, first, height, max_w = mkfont.load_bdf(TINY_BDF.splitlines(), (65, 66))
    assert (first, height, max_w) == (65, 4, 3)
    data = mkfont.build_pfn(glyphs, first, height, max_w, proportional=True)
    hdr = parse_header(data)
    assert hdr["magic"] == b"PFNT" and hdr["ver"] == 1 and hdr["reserved"] == 0
    assert hdr["flags"] & 1
    assert (hdr["first"], hdr["last"], hdr["stride"]) == (65, 66, 1)
    count = 2
    assert len(data) == 12 + count + count * 4 * 1
    assert list(data[12:14]) == [3, 2]                       # advances from DWIDTH
    assert list(data[14:18]) == [0xE0, 0xA0, 0xE0, 0x00]     # 'A' rows


def test_range_fills_missing_glyphs_blank():
    glyphs, first, height, max_w = mkfont.load_bdf(TINY_BDF.splitlines(), (64, 67))
    assert first == 64 and len(glyphs) == 4
    assert glyphs[0].rows == [0, 0, 0, 0] and glyphs[0].advance == max_w
    assert glyphs[3].rows == [0, 0, 0, 0]


def test_proportional_trim_from_mono_grid():
    # 4x2 cells: 'A' has ink in cols 2..3 (centred, blank on the left), 'B' is blank (space-like)
    a = mkfont.Glyph(rows=[0b00110000, 0b00100000], advance=4)
    b = mkfont.Glyph(rows=[0, 0], advance=4)
    out = mkfont.trim_proportional([a, b], max_width=4, spacing=1, first=65)
    assert out[0].rows == [0b11000000, 0b10000000]  # shifted left to drop blank cols
    assert out[0].advance == 3          # ink width 2 + spacing 1
    assert out[1].advance == 2          # blank keeps max(cell/2, 2)

    # 8x2 cell: ink only in column 7 (far right)
    c = mkfont.Glyph(rows=[0b00000001, 0], advance=8)
    out2 = mkfont.trim_proportional([c], max_width=8, spacing=1, first=65)
    assert out2[0].rows == [0b10000000, 0]
    assert out2[0].advance == 2         # ink width 1 + spacing 1


def test_png_grid_input(tmp_path):
    from PIL import Image
    # 2 cells of 3x2, first='A': A = all white, B = left column only
    img = Image.new("L", (6, 2), 0)
    for x in range(3):
        img.putpixel((x, 0), 255); img.putpixel((x, 1), 255)
    img.putpixel((3, 0), 255); img.putpixel((3, 1), 255)
    p = tmp_path / "sheet.png"
    img.save(p)
    glyphs, first, height, max_w = mkfont.load_png(p, cell=(3, 2), rng=(65, 66))
    assert (first, height, max_w) == (65, 2, 3)
    assert glyphs[0].rows == [0xE0, 0xE0]
    assert glyphs[1].rows == [0x80, 0x80]


def test_png_grid_input_wide(tmp_path):
    """A 12 px cell needs two bytes per row; ink past column 7 must survive."""
    from PIL import Image
    # 2 cells of 12x3, first='A': A has ink in columns 0 and 11, B in column 8.
    img = Image.new("L", (24, 3), 0)
    for y in range(3):
        img.putpixel((0, y), 255)
        img.putpixel((11, y), 255)
        img.putpixel((12 + 8, y), 255)
    p = tmp_path / "wide.png"
    img.save(p)
    glyphs, first, height, max_w = mkfont.load_png(p, cell=(12, 3), rng=(65, 66))
    assert (first, height, max_w) == (65, 3, 12)
    data = mkfont.build_pfn(glyphs, first, height, max_w, proportional=False)
    hdr = parse_header(data)
    assert hdr["stride"] == 2 and hdr["max_width"] == 12
    count, off = 2, 12 + 2
    a = data[off:off + 3 * 2]
    b = data[off + 3 * 2:off + count * 3 * 2]
    assert list(a) == [0x80, 0x10] * 3        # columns 0 and 11
    assert list(b) == [0x00, 0x80] * 3        # column 8
    assert len(data) == 12 + count + count * height * 2


def test_build_pfn_stride_2_length():
    height, count = 5, 3
    glyphs = [mkfont.Glyph(rows=[0] * height, advance=9) for _ in range(count)]
    data = mkfont.build_pfn(glyphs, 65, height, 9, proportional=False)
    assert parse_header(data)["stride"] == 2
    assert len(data) == 12 + count + count * height * 2


def test_build_rejects_oversize():
    g = mkfont.Glyph(rows=[0] * 65, advance=1)
    with pytest.raises(ValueError):
        mkfont.build_pfn([g], 65, 65, 1, proportional=False)


# ── BDF import: real fonts' metrics (issue #74) ─────────────────────────────

REPO = Path(__file__).resolve().parents[2]


def bdf(box, *chars):
    """BDF lines. box: FONTBOUNDINGBOX (w, h, xoff, yoff); each char:
    (code, dwidth, (bbx w, h, xoff, yoff), [hex rows])."""
    out = ["STARTFONT 2.1", "FONT test", "SIZE 8 75 75",
           "FONTBOUNDINGBOX " + " ".join(map(str, box)), f"CHARS {len(chars)}"]
    for code, dw, bbx, rows in chars:
        out += [f"STARTCHAR U+{code:04X}", f"ENCODING {code}", f"DWIDTH {dw} 0",
                "BBX " + " ".join(map(str, bbx)), "BITMAP", *rows, "ENDCHAR"]
    return out + ["ENDFONT"]


def write_bdf(tmp_path, lines):
    p = tmp_path / "font.bdf"
    p.write_text("\n".join(lines) + "\n")
    return p


# A proportional font in the shape of helvR12: FONTBOUNDINGBOX is 20 rows tall
# and starts left of the pen because of an accented capital outside the range.
TALL_BOX = (14, 20, -1, -5)
ACCENTED = (0xC1, 9, (9, 20, -1, -5), ["FF80"] * 20)
A = (0x41, 3, (3, 3, 0, 0), ["E0", "A0", "E0"])            # on the baseline
G = (0x67, 5, (3, 4, 1, -2), ["E0", "A0", "E0", "20"])     # descends 2 rows


def test_proportional_height_comes_from_glyphs_in_range():
    glyphs, first, height, max_w = mkfont.load_bdf(bdf(TALL_BOX, A, G, ACCENTED), (0x41, 0x67))
    assert height == 5          # 3 rows above the baseline, 2 below
    a, g = glyphs[0], glyphs[0x67 - 0x41]
    assert a.rows == [0xE0, 0xA0, 0xE0, 0, 0]
    assert g.rows == [0, 0x70, 0x50, 0x70, 0x10]    # baseline shared with 'A'


def test_blank_glyph_does_not_set_the_height():
    space = (0x20, 3, (1, 16, 0, -5), ["00"] * 16)
    _, _, height, _ = mkfont.load_bdf(bdf(TALL_BOX, space, A, G), (0x20, 0x67))
    assert height == 5


def test_fixed_cell_keeps_the_font_box():
    # Spleen, Terminus and friends: the box is the designed cell, tight BBXes
    # (or a range without descenders) must not shrink it.
    box = (4, 8, 0, -2)
    chars = [(0x41, 4, (3, 3, 0, 0), ["E0", "A0", "E0"]),
             (0x42, 4, (3, 2, 1, 1), ["E0", "A0"])]
    glyphs, _, height, max_w = mkfont.load_bdf(bdf(box, *chars), (0x41, 0x42))
    assert (height, max_w) == (8, 4)
    assert glyphs[0].rows == [0, 0, 0, 0xE0, 0xA0, 0xE0, 0, 0]
    assert glyphs[1].rows == [0, 0, 0, 0x70, 0x50, 0, 0, 0]
    assert [g.advance for g in glyphs] == [4, 4]


def test_tight_shrinks_a_fixed_cell_font():
    box = (4, 8, 0, -2)
    chars = [(0x41, 4, (3, 3, 0, 0), ["E0", "A0", "E0"]),
             (0x42, 4, (3, 2, 1, 1), ["E0", "A0"])]
    glyphs, _, height, _ = mkfont.load_bdf(bdf(box, *chars), (0x41, 0x42), tight=True)
    assert height == 3
    assert glyphs[0].rows == [0xE0, 0xA0, 0xE0]
    assert glyphs[1].rows == [0x70, 0x50, 0]


def test_right_overhang_widens_the_advance(capsys):
    a = (0x41, 4, (3, 2, 0, 0), ["E0", "A0"])
    f = (0x66, 3, (4, 2, 0, 0), ["F0", "90"])       # helvR12's 'f': 4 px in 3
    glyphs, _, _, max_w = mkfont.load_bdf(bdf((4, 2, 0, 0), a, f), (0x41, 0x66))
    g = glyphs[0x66 - 0x41]
    assert g.advance == 4 and max_w == 4
    assert g.rows == [0xF0, 0x90]
    assert glyphs[0].advance == 4                   # 'A' keeps its own
    err = capsys.readouterr().err
    assert "0x66" in err and "advance widened to 4" in err
    assert "0x41" not in err


def test_left_overhang_moves_the_glyph_and_widens_it(capsys):
    # The box's xoff of -1 is j's; moving the whole font right by it would cut
    # the last column of 'H' (no side bearing), so only 'j' moves.
    j = (0x6A, 2, (3, 2, -1, 0), ["E0", "20"])      # ink from column -1
    h = (0x48, 3, (3, 2, 0, 0), ["A0", "E0"])
    glyphs, _, _, _ = mkfont.load_bdf(bdf((4, 2, -1, 0), h, j), (0x48, 0x6A))
    assert glyphs[0].advance == 3 and glyphs[0].rows == [0xA0, 0xE0]
    g = glyphs[0x6A - 0x48]
    assert g.advance == 3                           # 2 + the 1 px moved
    assert g.rows == [0xE0, 0x20]
    err = capsys.readouterr().err
    assert "0x6A" in err and "moved right 1 px" in err and "0x48" not in err


def test_blank_bbx_columns_are_not_ink(capsys):
    # BBX runs past the advance and left of the pen, the bitmap's ink does not.
    c = (0x41, 3, (6, 1, -1, 0), ["70"])            # ink in BBX columns 1..3
    n = (0x42, 4, (3, 1, 0, 0), ["E0"])
    glyphs, _, _, _ = mkfont.load_bdf(bdf((6, 1, -1, 0), c, n), (0x41, 0x42))
    assert capsys.readouterr().err == ""
    assert glyphs[0].advance == 3


def test_font_box_origin_is_kept_when_all_ink_fits(capsys):
    # The designer's spacing, exactly: j overhangs but every glyph has room for
    # the box's 1 px shift, so nothing widens and the old placement stands.
    j = (0x6A, 3, (2, 1, -1, 0), ["C0"])
    h = (0x48, 4, (2, 1, 1, 0), ["C0"])
    glyphs, _, _, _ = mkfont.load_bdf(bdf((4, 1, -1, 0), h, j), (0x48, 0x6A))
    assert capsys.readouterr().err == ""
    assert glyphs[0].advance == 4 and glyphs[0].rows == [0x30]
    g = glyphs[0x6A - 0x48]
    assert g.advance == 3 and g.rows == [0xC0]


def test_fixed_cell_shift_that_clips_is_dropped():
    # X.Org Courier: the box starts 2 px left of the pen for glyphs outside the
    # range, and every cell would lose its last 2 columns to that shift.
    box = (11, 3, -2, 0)
    chars = [(0x41, 7, (6, 1, 1, 0), ["FC"]), (0x42, 7, (7, 1, 0, 0), ["FE"])]
    glyphs, _, _, max_w = mkfont.load_bdf(bdf(box, *chars), (0x41, 0x42))
    assert max_w == 7 and [g.advance for g in glyphs] == [7, 7]
    assert glyphs[0].rows[2] == 0x7E
    assert glyphs[1].rows[2] == 0xFE


def test_fixed_cell_overhang_moves_the_glyph_and_keeps_the_cell(capsys):
    box = (5, 1, 0, 0)
    chars = [(0x41, 4, (3, 1, 2, 0), ["E0"]),       # ink in columns 2..4
             (0x42, 4, (3, 1, -1, 0), ["E0"]),      # ink in columns -1..1
             (0x43, 4, (4, 1, 0, 0), ["F0"])]
    glyphs, _, _, max_w = mkfont.load_bdf(bdf(box, *chars), (0x41, 0x43))
    assert max_w == 4 and [g.advance for g in glyphs] == [4, 4, 4]
    assert glyphs[0].rows == [0x70]
    assert glyphs[1].rows == [0xE0]
    assert glyphs[2].rows == [0xF0]
    err = capsys.readouterr().err
    assert "0x41" in err and "moved left 1 px" in err
    assert "0x42" in err and "moved right 1 px" in err
    assert "0x43" not in err


def test_glyph_wider_than_the_cell_widens_every_cell(capsys):
    box = (5, 1, 0, 0)
    chars = [(0x41, 4, (5, 1, 0, 0), ["F8"]), (0x42, 4, (2, 1, 0, 0), ["C0"])]
    glyphs, _, _, max_w = mkfont.load_bdf(bdf(box, *chars), (0x41, 0x42))
    assert max_w == 5 and [g.advance for g in glyphs] == [5, 5]
    assert glyphs[0].rows == [0xF8]
    assert "widened to 5" in capsys.readouterr().err


def test_ink_outside_a_fixed_cell_box_warns(capsys):
    box = (3, 2, 0, 0)                              # no room for the 3rd row
    chars = [(0x41, 3, (3, 3, 0, 0), ["E0", "A0", "E0"])]
    glyphs, _, height, _ = mkfont.load_bdf(bdf(box, *chars), (0x41, 0x41))
    assert height == 2
    assert "0x41" in capsys.readouterr().err


def test_rows_padded_past_the_bbx_width():
    # charR12: 'ampersand' has BBX width 8 and the row 3000.
    amp = (0x26, 8, (8, 2, 0, 0), ["3000", "FF00"])
    other = (0x27, 3, (2, 2, 0, 0), ["C0", "80"])
    glyphs, _, _, _ = mkfont.load_bdf(bdf((8, 2, 0, 0), amp, other), (0x26, 0x27))
    assert glyphs[0].rows == [0x30, 0xFF]


def test_rows_padded_in_a_wide_font():
    wide = (0x41, 12, (12, 1, 0, 0), ["FFF00000"])
    other = (0x42, 3, (2, 1, 0, 0), ["C0"])
    glyphs, _, _, max_w = mkfont.load_bdf(bdf((12, 1, 0, 0), wide, other), (0x41, 0x42))
    assert max_w == 12 and glyphs[0].rows == [0xFFF0]


def test_row_shorter_than_the_bbx_width_is_msb_left():
    c = (0x41, 8, (8, 1, 0, 0), ["F"])
    d = (0x42, 3, (2, 1, 0, 0), ["C0"])
    glyphs, _, _, _ = mkfont.load_bdf(bdf((8, 1, 0, 0), c, d), (0x41, 0x42))
    assert glyphs[0].rows == [0xF0]


def test_nonzero_bits_past_the_bbx_width_warn_and_do_not_draw(capsys):
    c = (0x41, 8, (5, 1, 0, 0), ["FF"])
    d = (0x42, 3, (2, 1, 0, 0), ["C0"])
    glyphs, _, _, _ = mkfont.load_bdf(bdf((8, 1, 0, 0), c, d), (0x41, 0x42))
    assert glyphs[0].rows == [0xF8]
    err = capsys.readouterr().err
    assert "0x41" in err and "0x42" not in err


def test_proportional_flag_keeps_a_bdf_s_designed_advances(tmp_path, capsys):
    lines = bdf(TALL_BOX, A, G, ACCENTED)
    src = write_bdf(tmp_path, lines)
    plain, flagged = tmp_path / "plain.pfn", tmp_path / "flagged.pfn"
    mkfont.main([str(src), str(plain), "--range", "0x41-0x67"])
    assert capsys.readouterr().err == ""
    mkfont.main([str(src), str(flagged), "--range", "0x41-0x67", "--proportional"])
    assert flagged.read_bytes() == plain.read_bytes()
    assert "--proportional ignored" in capsys.readouterr().err


def test_proportional_flag_still_trims_a_fixed_cell_bdf(tmp_path, capsys):
    box = (4, 1, 0, 0)
    chars = [(0x41, 4, (4, 1, 0, 0), ["30"]), (0x42, 4, (4, 1, 0, 0), ["F0"])]
    src = write_bdf(tmp_path, bdf(box, *chars))
    out = tmp_path / "out.pfn"
    mkfont.main([str(src), str(out), "--range", "0x41-0x42", "--proportional", "--spacing", "1"])
    assert capsys.readouterr().err == ""
    data = out.read_bytes()
    assert parse_header(data)["flags"] & 1
    assert list(data[12:14]) == [3, 4]          # ink 2 + 1 and ink 4 (cell cap)
    assert list(data[14:16]) == [0xC0, 0xF0]    # moved to the left of the cell


def test_spleen_stays_byte_identical(tmp_path):
    """The committed font is built from this BDF by the command in its README."""
    out = tmp_path / "spleen.pfn"
    mkfont.main([str(REPO / "sdk/fonts/spleen/spleen-5x8.bdf"), str(out), "--range", "0x20-0x7E"])
    assert out.read_bytes() == (REPO / "system/lib/fonts/spleen-5x8.pfn").read_bytes()


def test_demo_sheet_stays_byte_identical(tmp_path):
    out = tmp_path / "demo.pfn"
    mkfont.main([str(REPO / "apps/fonttest/fonts/demo_prop.png"), str(out),
                 "--cell", "8x12", "--proportional"])
    assert out.read_bytes() == (REPO / "apps/fonttest/fonts/demo_prop.pfn").read_bytes()


# ── TTF ─────────────────────────────────────────────────────────────────────

def ttf_file(tmp_path):
    """Pillow's embedded Aileron as a file: no system font needed."""
    from PIL import ImageFont
    try:
        data = ImageFont.load_default(12).font_bytes    # Pillow 10.1+, with FreeType
    except (AttributeError, TypeError):
        pytest.skip("this Pillow has no embedded TrueType font")
    p = tmp_path / "aileron.ttf"
    p.write_bytes(data)
    return p


def mono_ink(font, ch):
    """FreeType's hinted 1-bit rendering of `ch`, cropped to its ink columns."""
    from PIL import Image, ImageDraw
    img = Image.new("L", (64, 32), 0)
    draw = ImageDraw.Draw(img)
    draw.fontmode = "1"
    draw.text((16, 0), ch, font=font, fill=255)
    box = img.getbbox()
    if not box:
        return []
    return ["".join("#" if img.getpixel((x, y)) else "." for x in range(box[0], box[2]))
            for y in range(box[1], box[3])]


def glyph_ink(g, max_w):
    """The glyph as the renderer draws it (its `advance` columns), cropped to its ink."""
    stride = mkfont.stride_for(max_w)
    rows = ["".join("#" if r & mkfont.bit_for(c, stride) else "." for c in range(g.advance))
            for r in g.rows]
    inked = [y for y, r in enumerate(rows) if "#" in r]
    if not inked:
        return []
    rows = rows[inked[0]:inked[-1] + 1]
    lo = min(r.index("#") for r in rows if "#" in r)
    hi = max(r.rindex("#") for r in rows if "#" in r)
    return [r[lo:hi + 1] for r in rows]


def test_ttf_is_freetype_1bit_with_all_its_ink(tmp_path, capsys):
    from PIL import ImageFont
    path = ttf_file(tmp_path)
    font = ImageFont.truetype(str(path), 12)
    glyphs, first, height, max_w = mkfont.load_ttf(path, 12, (0x20, 0x7E))
    for i, g in enumerate(glyphs):
        assert glyph_ink(g, max_w) == mono_ink(font, chr(first + i)), repr(chr(first + i))
    assert "advance widened" in capsys.readouterr().err    # Aileron's '/' and ','


def test_ttf_rows_are_aligned_to_a_wide_stride(tmp_path):
    """Rows stay MSB-left when a large size needs a second byte per row."""
    from PIL import ImageFont
    path = ttf_file(tmp_path)
    font = ImageFont.truetype(str(path), 24)
    glyphs, first, height, max_w = mkfont.load_ttf(path, 24, (0x41, 0x5A))
    assert mkfont.stride_for(max_w) >= 2
    data = mkfont.build_pfn(glyphs, first, height, max_w, proportional=True)
    assert parse_header(data)["stride"] == mkfont.stride_for(max_w)
    w = glyphs[ord("W") - first]
    assert glyph_ink(w, max_w) == mono_ink(font, "W")
