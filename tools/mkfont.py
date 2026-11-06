#!/usr/bin/env python3
"""mkfont.py - build PicoDeck .pfn bitmap fonts.

  mkfont.py SRC OUT.pfn [--size N] [--cell WxH] [--range FIRST-LAST]
                        [--proportional] [--spacing N] [--tight] [--dump]

SRC may be:
  *.bdf       - parsed directly; DWIDTH gives per-glyph advances
  *.ttf/*.otf - rasterised with Pillow at --size px (default 12), using
                FreeType's hinted 1-bit output
  *.png       - fixed grid sheet, --cell WxH required, glyphs left to right,
                top to bottom, starting at the range's first code

--proportional is for PNG sheets and fixed-cell BDFs: it trims each glyph to its
ink plus --spacing. A proportional BDF already has designed advances, so the
flag is ignored for one (with a warning). TTF/OTF sources ignore it too.

A BDF is fixed-cell when all its glyphs in --range share one advance, otherwise
proportional. Its height is:
  proportional - the span of the BBXes of the inked glyphs in --range, since
                 FONTBOUNDINGBOX also covers the glyphs outside the range
  fixed-cell   - FONTBOUNDINGBOX, the designed cell; --tight applies the
                 proportional rule instead

The renderer draws exactly `advance` columns of a glyph and the format has no
per-glyph x offset, so a glyph's ink has to lie inside its advance. Glyphs start
at FONTBOUNDINGBOX's xoff when that keeps all ink inside, and at their own pen
origin otherwise. Then, with a warning per glyph, ink that is still outside:
  proportional - widens that glyph's advance to cover it; ink left of the origin
                 moves the glyph right by as much
  fixed-cell   - moves the glyph inside its cell, so that columns stay aligned;
                 a glyph wider than the cell widens the cell of every glyph
TTF/OTF follow the same two rules, by whether their advances are all equal.

.pfn layout (little-endian):
  "PFNT" ver=1 flags first last height max_width stride reserved
  widths[count] (u8)  bitmaps[count * height * stride] (row-major, MSB left)

Fonts up to 64 px wide are supported: stride = (max_width + 7) // 8 bytes per
glyph row, emitted big-endian so the leftmost column is the MSB of byte 0.
"""
import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

MAX_DIM = 64


def stride_for(max_width):
    """Bytes per glyph row for a font this wide."""
    return (max_width + 7) // 8


def bit_for(col, stride):
    """Mask of column `col` in a row int that is 8*stride bits wide, MSB left."""
    return 1 << (8 * stride - 1 - col)


@dataclass
class Glyph:
    rows: list          # one int per row, bit (8*stride - 1 - col) set = pixel on
    advance: int
    name: str = field(default="")
    missing: bool = False   # not in the source: a blank stand-in


@dataclass
class RawGlyph:
    """A glyph as the source draws it, before it is placed in a cell."""
    name: str
    advance: int
    w: int              # the bitmap: `w` columns...
    xoff: int           # ...the first of them this far right of the pen...
    top: int            # ...and rows[0] this many rows above the baseline
    rows: list          # top-down, each an int of `w` bits, MSB left

    def ink(self):
        """Pen-relative (first, last) inked column, or None for a blank glyph."""
        bits = 0
        for r in self.rows:
            bits |= r
        if not bits:
            return None
        return (self.xoff + self.w - bits.bit_length(),
                self.xoff + self.w - (bits & -bits).bit_length())


def warn(message):
    print(f"warning: {message}", file=sys.stderr)


def parse_range(text):
    lo, hi = text.split("-")
    lo, hi = int(lo, 0), int(hi, 0)
    if not (0 <= lo <= hi <= 255):
        raise ValueError(f"bad range {text}")
    return lo, hi


def blank(height):
    return [0] * height


# ── BDF ─────────────────────────────────────────────────────────────────────

def bdf_glyph(code, name, dw, bbx, bitmap):
    """Decode one STARTCHAR block's values."""
    bw, bh, bxo, byo = bbx
    rows = []
    stray = 0
    for text in bitmap[:bh]:
        # A row's length says how many bits it holds: real BDFs pad past
        # ceil(width / 8) bytes (Charter has 8 px glyphs with 16-bit rows).
        bits = len(text) * 4
        val = int(text, 16)
        if bits >= bw:
            stray |= val & ((1 << (bits - bw)) - 1)
            rows.append(val >> (bits - bw))
        else:
            rows.append(val << (bw - bits))
    if stray:
        warn(f"glyph 0x{code:02X} {name!r}: bitmap bits past its BBX width ignored")
    return RawGlyph(name, dw if dw is not None else bw, bw, bxo, byo + bh, rows)


def load_bdf(lines, rng, tight=False):
    """Return (glyphs, first, height, max_width) for codes rng[0]..rng[1]."""
    first, last = rng
    fbb = None
    chars = {}
    it = iter(lines)
    for line in it:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "FONTBOUNDINGBOX":
            fbb = tuple(int(v) for v in parts[1:5])  # w h xoff yoff
        elif parts[0] == "STARTCHAR":
            name = parts[1] if len(parts) > 1 else ""
            enc = None; dw = None; bbx = None; bitmap = []
            for l in it:
                p = l.split()
                if not p:
                    continue
                if p[0] == "ENCODING":
                    enc = int(p[1])
                elif p[0] == "DWIDTH":
                    dw = int(p[1])
                elif p[0] == "BBX":
                    bbx = tuple(int(v) for v in p[1:5])
                elif p[0] == "BITMAP":
                    for b in it:
                        if b.strip() == "ENDCHAR":
                            break
                        bitmap.append(b.strip())
                    break
                elif p[0] == "ENDCHAR":
                    break
            if enc is not None and first <= enc <= last:
                chars[enc] = bdf_glyph(enc, name, dw, bbx, bitmap)
    if fbb is None:
        raise ValueError("BDF has no FONTBOUNDINGBOX")
    fw, fh, fxo, fyo = fbb
    fixed_cell = len({c.advance for c in chars.values()}) <= 1
    # A blank glyph's BBX says nothing about the text's height.
    inked = [(c.top, c.top - len(c.rows)) for c in chars.values() if c.ink()]
    if inked and (tight or not fixed_cell):
        box_top = max(top for top, _ in inked)
        height = box_top - min(bottom for _, bottom in inked)
    else:
        box_top, height = fh + fyo, fh
    placed = place_glyphs(chars, fxo, fixed_cell)
    return assemble(chars, placed, first, last, box_top, height)


def place_glyphs(chars, origin, fixed_cell):
    """Return {code: (column of the bitmap in its cell, advance)}; see the module
    docstring. `origin` is the column FONTBOUNDINGBOX puts at the cell's left."""
    inked = {code: c.ink() for code, c in chars.items() if c.ink()}
    if all(lo - origin >= 0 and hi - origin < chars[code].advance
           for code, (lo, hi) in inked.items()):
        return {code: (c.xoff - origin, c.advance) for code, c in chars.items()}
    cell = None
    if fixed_cell:
        advance = next(iter(chars.values())).advance
        cell = max([advance] + [hi - lo + 1 for lo, hi in inked.values()])
        if cell > advance:
            warn(f"a glyph is wider than the {advance} px cell: "
                 f"the cell of every glyph widened to {cell}")
    placed = {}
    for code, c in chars.items():
        if code not in inked:
            placed[code] = (c.xoff, cell or c.advance)
            continue
        lo, hi = inked[code]
        if fixed_cell:
            move = -lo if lo < 0 else min(0, cell - 1 - hi)
            advance = cell
        else:
            move = max(0, -lo)
            advance = max(c.advance + move, hi + move + 1)
        if lo < 0 or hi >= c.advance:
            done = [f"moved {'right' if move > 0 else 'left'} {abs(move)} px"] if move else []
            if advance != c.advance:
                done.append(f"advance widened to {advance}")
            warn(f"glyph 0x{code:02X} {c.name!r}: ink in columns {lo}..{hi} is outside "
                 f"its {c.advance} px advance: {', '.join(done)}")
        placed[code] = (c.xoff + move, advance)
    return placed


def assemble(chars, placed, first, last, box_top, height):
    """Build the (glyphs, first, height, max_width) a loader returns."""
    # The row ints are aligned to 8*stride bits, so the widest advance (and the
    # widest bitmap) has to be known before any row is built.
    max_w = max([1] + [max(placed[code][1], c.w) for code, c in chars.items()])
    row_bits = 8 * stride_for(max_w)
    row_mask = (1 << row_bits) - 1
    glyphs = []
    for code in range(first, last + 1):
        if code not in chars:
            glyphs.append(Glyph(blank(height), max_w, "", missing=True))
            continue
        c = chars[code]
        x, adv = placed[code]
        rows = blank(height)
        shift = row_bits - c.w - x      # the bitmap's last column -> bit `shift`
        for i, val in enumerate(c.rows):
            r = box_top - c.top + i
            if 0 <= r < height:
                rows[r] = (val << shift if shift >= 0 else val >> -shift) & row_mask
            elif val:
                warn(f"glyph 0x{code:02X} {c.name!r}: ink above or below the font box dropped")
        glyphs.append(Glyph(rows, max(adv, 1), c.name))
    return glyphs, first, height, max_w


# ── TTF via Pillow ──────────────────────────────────────────────────────────

def load_ttf(path, size, rng):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(str(path), size)
    first, last = rng
    ascent, descent = font.getmetrics()
    height = ascent + descent
    chars = {}
    for code in range(first, last + 1):
        ch = chr(code)
        # FreeType's hinted 1-bit output, with its own advance: thresholding the
        # anti-aliased output loses stems at small sizes.
        adv = max(1, int(round(font.getlength(ch, mode="1"))))
        # A size-px margin each side keeps the ink that overhangs the advance.
        img = Image.new("L", (adv + 2 * size, height), 0)
        draw = ImageDraw.Draw(img)
        draw.fontmode = "1"
        draw.text((size, 0), ch, font=font, fill=255)
        box = img.getbbox()
        left, right = (box[0], box[2]) if box else (size, size)
        rows = []
        for y in range(height):
            v = 0
            for x in range(left, right):
                v = v << 1 | (img.getpixel((x, y)) >= 128)
            rows.append(v)
        chars[code] = RawGlyph(ch, adv, right - left, left - size, height, rows)
    placed = place_glyphs(chars, 0, len({c.advance for c in chars.values()}) == 1)
    return assemble(chars, placed, first, last, height, height)


# ── PNG grid sheet ──────────────────────────────────────────────────────────

def load_png(path, cell, rng):
    from PIL import Image
    cw, ch = cell
    first, last = rng
    img = Image.open(path).convert("L")
    per_row = img.width // cw
    if per_row == 0:
        raise ValueError("cell wider than image")
    stride = stride_for(cw)
    glyphs = []
    for i, code in enumerate(range(first, last + 1)):
        gx, gy = (i % per_row) * cw, (i // per_row) * ch
        rows = []
        for y in range(ch):
            v = 0
            for x in range(cw):
                px, py = gx + x, gy + y
                if px < img.width and py < img.height and img.getpixel((px, py)) >= 128:
                    v |= bit_for(x, stride)
            rows.append(v)
        glyphs.append(Glyph(rows, cw, chr(code)))
    return glyphs, first, ch, cw


# ── Transforms and output ───────────────────────────────────────────────────

def trim_proportional(glyphs, max_width, spacing, first):
    stride = stride_for(max_width)
    row_mask = (1 << (8 * stride)) - 1
    out = []
    for i, g in enumerate(glyphs):
        lo = None
        hi = None
        for r in g.rows:
            for col in range(max_width):
                if r & bit_for(col, stride):
                    if lo is None or col < lo:
                        lo = col
                    if hi is None or col > hi:
                        hi = col
        if lo is None:
            adv = max(max_width // 2, 2)
            rows = list(g.rows)
        else:
            adv = min(max_width, (hi - lo + 1) + spacing)
            rows = [(r << lo) & row_mask for r in g.rows]
        out.append(Glyph(rows, adv, g.name))
    return out


def build_pfn(glyphs, first, height, max_width, proportional):
    count = len(glyphs)
    last = first + count - 1
    if not (1 <= height <= MAX_DIM and 1 <= max_width <= MAX_DIM):
        raise ValueError("height/width must be 1..64")
    if not (0 <= first <= last <= 255):
        raise ValueError("code range must fit in 0..255")
    stride = stride_for(max_width)
    hdr = struct.pack("<4sBBBBBBBB", b"PFNT", 1, 1 if proportional else 0,
                      first, last, height, max_width, stride, 0)
    widths = bytes(min(max(g.advance, 1), max_width) for g in glyphs)
    row_mask = (1 << (8 * stride)) - 1
    bitmaps = bytearray()
    for g in glyphs:
        if len(g.rows) != height:
            raise ValueError(f"glyph {g.name!r} has {len(g.rows)} rows, expected {height}")
        for r in g.rows:
            bitmaps.extend((r & row_mask).to_bytes(stride, "big"))
    return hdr + widths + bytes(bitmaps)


def dump(glyphs, first, max_width):
    stride = stride_for(max_width)
    for i, g in enumerate(glyphs):
        print(f"--- 0x{first + i:02X} {g.name!r} advance={g.advance}")
        for r in g.rows:
            print("".join("#" if r & bit_for(c, stride) else "." for c in range(max_width)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src"); ap.add_argument("out")
    ap.add_argument("--size", type=int, default=12)
    ap.add_argument("--cell", help="WxH for PNG sheets")
    ap.add_argument("--range", default="0x20-0x7E")
    ap.add_argument("--proportional", action="store_true",
                    help="PNG sheets and fixed-cell BDFs: trim each glyph to its ink plus --spacing "
                         "(ignored, with a warning, for a proportional BDF)")
    ap.add_argument("--spacing", type=int, default=1)
    ap.add_argument("--tight", action="store_true",
                    help="fixed-cell BDFs: height from the glyphs in --range, as for a proportional "
                         "BDF, not from FONTBOUNDINGBOX")
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args(argv)
    rng = parse_range(a.range)
    src = Path(a.src)
    ext = src.suffix.lower()
    if ext == ".bdf":
        glyphs, first, height, max_w = load_bdf(src.read_text().splitlines(), rng, a.tight)
        proportional = a.proportional or len({g.advance for g in glyphs}) > 1
        # Trimming would throw away the designer's advances and side bearings.
        trim = a.proportional and len({g.advance for g in glyphs if not g.missing}) <= 1
        if a.proportional and not trim:
            warn("--proportional ignored: this BDF already has per-glyph advances")
    elif ext in (".ttf", ".otf"):
        glyphs, first, height, max_w = load_ttf(src, a.size, rng)
        proportional = True
        trim = False
    elif ext == ".png":
        if not a.cell:
            sys.exit("--cell WxH is required for PNG sheets")
        cw, ch = (int(v) for v in a.cell.lower().split("x"))
        glyphs, first, height, max_w = load_png(src, (cw, ch), rng)
        proportional = trim = a.proportional
    else:
        sys.exit(f"unsupported source type {ext}")
    if trim:
        glyphs = trim_proportional(glyphs, max_w, a.spacing, first)
        proportional = True
    data = build_pfn(glyphs, first, height, max_w, proportional)
    Path(a.out).write_bytes(data)
    if a.dump:
        dump(glyphs, first, max_w)
    print(f"wrote {a.out}: {len(glyphs)} glyphs, {max_w}x{height}, "
          f"{'proportional' if proportional else 'monospace'}, {len(data)} bytes")


if __name__ == "__main__":
    main()
