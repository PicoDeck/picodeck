#!/usr/bin/env python3
"""Build src/os/splash_logo_cells.{h,c}: the boot splash's "PicoDeck" logo
as cells, and its colours (src/os/splash_render.c;
specs/2026-10-10-boot-splash-design.md).

The lettering is drawn from signed-distance shapes at 4x supersampling and
given a phosphor glow: letter pixels at level 0.86 on the blue-white ramp,
a Gaussian glow around them. The screen is 80 x 160 cells of 4x2 pixels;
each cell keeps two levels (the higher is the foreground) and a glyph, its
pixel pattern: row 0 in bits 7-4, row 1 in bits 3-0, the leftmost pixel in
the highest bit of each nibble. Only the rows with something in them (and
one spare row each side) are written. Ported from the crt-splash prototype
app's make_logo.py (geometric lettering, glow look), whose cells this
reproduces.

Needs numpy and scipy.
    python3 tools/make_splash_logo.py          # writes src/os/splash_logo_cells.{h,c}
    python3 tools/make_splash_logo.py DIR      # writes them into DIR instead
"""
import sys
from pathlib import Path

import numpy as np
from scipy import ndimage

ROOT = Path(__file__).resolve().parent.parent
SS = 4                       # supersamples per pixel
SCR = 320
COLS, CELL_W, CELL_H = 80, 4, 2
LETTER = 0.86                # the letters' level on the ramp
NPAL = 64
# (level, RGB) stops: black, the UI's navy #101439, select blue #2951a4 and
# focus blue #638ede, then white.
RAMP = [(0.0, (0x00, 0x00, 0x00)), (0.10, (0x10, 0x14, 0x39)), (0.25, (0x1a, 0x2a, 0x6a)),
        (0.50, (0x29, 0x51, 0xa4)), (0.72, (0x63, 0x8e, 0xde)), (0.86, (0xb4, 0xc8, 0xf0)),
        (1.0, (0xff, 0xff, 0xff))]

BASE = 168                   # the baseline row
CAP, XH = 54, 40             # cap and x height
ST = 10                      # stroke width
R = 11                       # outer corner radius
GAP = 6                      # letter spacing
TOP, XTOP = BASE - CAP, BASE - XH


# ── lettering: signed distance shapes ───────────────────────────────────────

def sd_box(X, Y, x0, y0, x1, y1, r=0.0):
    cx, cy, hw, hh = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    qx, qy = np.abs(X - cx) - (hw - r), np.abs(Y - cy) - (hh - r)
    return (np.hypot(np.maximum(qx, 0), np.maximum(qy, 0))
            + np.minimum(np.maximum(qx, qy), 0) - r)


def sd_seg(X, Y, ax, ay, bx, by, t):
    pax, pay, bax, bay = X - ax, Y - ay, bx - ax, by - ay
    h = np.clip((pax * bax + pay * bay) / (bax * bax + bay * bay), 0, 1)
    return np.hypot(pax - bax * h, pay - bay * h) - t


def ring(X, Y, x0, y0, x1, y1, r):
    inner = sd_box(X, Y, x0 + ST, y0 + ST, x1 - ST, y1 - ST, max(r - ST, 1.5))
    return np.maximum(sd_box(X, Y, x0, y0, x1, y1, r), -inner)


def cut(d, X, Y, x0, y0, x1, y1):
    return np.maximum(d, -sd_box(X, Y, x0, y0, x1, y1))


def g_P(X, Y, x):
    w = 36
    return np.minimum(sd_box(X, Y, x, TOP, x + ST, BASE), ring(X, Y, x, TOP, x + w, TOP + 31, R)), w


def g_i(X, Y, x):
    return np.minimum(sd_box(X, Y, x, XTOP, x + ST, BASE),
                      sd_box(X, Y, x, TOP, x + ST, TOP + 10, 2)), ST


def g_c(X, Y, x):
    w = 31
    return cut(ring(X, Y, x, XTOP, x + w, BASE, R), X, Y,
               x + w - ST - 1, XTOP + ST + 1, x + w + 1, BASE - ST - 1), w


def g_o(X, Y, x):
    w = 33
    return ring(X, Y, x, XTOP, x + w, BASE, R), w


def g_D(X, Y, x):
    w = 40
    return np.minimum(sd_box(X, Y, x, TOP, x + ST, BASE), ring(X, Y, x, TOP, x + w, BASE, 17)), w


def g_e(X, Y, x):
    w, mid = 33, BASE - XH / 2
    d = np.minimum(ring(X, Y, x, XTOP, x + w, BASE, R), sd_box(X, Y, x, mid - ST / 2, x + w, mid + ST / 2))
    # open the lower right as the c is open: the right stroke goes, the
    # rounded bottom corner stays, so the two bottoms match
    return cut(d, X, Y, x + w - ST - 1, mid + ST / 2, x + w + 1, BASE - ST - 1), w


def g_k(X, Y, x):
    w, t = 32, ST * 0.55
    arm = sd_seg(X, Y, x + ST - 1, BASE - XH * 0.40, x + w + 4, XTOP - 6, t)
    leg = sd_seg(X, Y, x + ST + 4, BASE - XH * 0.52, x + w + 4, BASE + 6, t)
    diag = np.maximum(np.minimum(arm, leg), sd_box(X, Y, x, XTOP, x + w, BASE))
    return np.minimum(sd_box(X, Y, x, TOP, x + ST, BASE), diag), w


def lettering():
    """Mask (SCR x SCR, bool): pixels at least half covered by "PicoDeck"."""
    letters = [g_P, g_i, g_c, g_o, g_D, g_e, g_c, g_k]
    probe_y, probe_x = np.mgrid[0:1, 0:1].astype(np.float32)
    width = sum(f(probe_x, probe_y, 0)[1] for f in letters) + GAP * (len(letters) - 1)
    ys, xs = np.mgrid[0:SCR * SS, 0:SCR * SS].astype(np.float32)
    X, Y = (xs + 0.5) / SS, (ys + 0.5) / SS
    d = np.full(X.shape, np.inf, np.float32)
    x = (SCR - width) // 2
    for f in letters:
        dl, w = f(X, Y, x)
        d = np.minimum(d, dl)
        x += w + GAP
    covered = (d < 0).reshape(SCR, SS, SCR, SS).mean(axis=(1, 3))
    return covered >= 0.5


# ── the glow, and the cells ─────────────────────────────────────────────────

def glow_levels(mask):
    glow = np.minimum(ndimage.gaussian_filter(mask.astype(np.float32), 7) * 0.9, 0.42)
    return np.where(mask, LETTER, glow)


LETTER_LEVELS = {round(LETTER * 255)}


def _two_levels(px):
    """Collapse a cell to two levels: the letter level stays, the glow levels
    become their mean, then the rarest of three merges into its nearest."""
    px = px.copy()
    glow_px = ~np.isin(px, list(LETTER_LEVELS))
    if glow_px.any():
        px[glow_px] = int(round(px[glow_px].mean()))
    vals, counts = np.unique(px, return_counts=True)
    while len(vals) > 2:
        i = int(np.argmin(counts))
        others = [v for j, v in enumerate(vals) if j != i]
        px[px == vals[i]] = min(others, key=lambda v: abs(int(v) - int(vals[i])))
        vals, counts = np.unique(px, return_counts=True)
    return px


def to_cells(levels):
    """SCR x SCR levels -> glyph, fg, bg planes (uint8, rows x COLS)."""
    rows = SCR // CELL_H
    q = np.round(levels * 255).astype(np.int32)
    glyph = np.zeros((rows, COLS), np.uint8)
    fg = np.zeros((rows, COLS), np.uint8)
    bg = np.zeros((rows, COLS), np.uint8)
    for cy in range(rows):
        for cx in range(COLS):
            px = q[cy * CELL_H:(cy + 1) * CELL_H, cx * CELL_W:(cx + 1) * CELL_W]
            vals = np.unique(px)
            if len(vals) > 2:
                px = _two_levels(px)
                vals = np.unique(px)
            hi, lo = int(vals.max()), int(vals.min())
            fg[cy, cx], bg[cy, cx] = hi, lo
            if hi != lo:
                bits = (px == hi).astype(np.int32)
                code = 0
                for r in range(CELL_H):
                    for c in range(CELL_W):
                        code |= int(bits[r, c]) << (CELL_W * (CELL_H - 1 - r) + CELL_W - 1 - c)
                glyph[cy, cx] = code
    return glyph, fg, bg


def palette():
    """64 RGB565 entries: entry i interpolates the ramp at i/63, each channel
    rounded half up (as the prototype app's Lua did)."""
    out = []
    for i in range(NPAL):
        level = i / (NPAL - 1)
        for (l0, c0), (l1, c1) in zip(RAMP, RAMP[1:]):
            if level <= l1:
                f = (level - l0) / (l1 - l0)
                r, g, b = (int(np.floor(a + (z - a) * f + 0.5)) for a, z in zip(c0, c1))
                break
        out.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return out


# ── output ──────────────────────────────────────────────────────────────────

def c_array(ctype, name, size, values, fmt):
    lines = [f"const {ctype} {name}[{size}] = {{"]
    for i in range(0, len(values), 16):
        lines.append("    " + ", ".join(fmt.format(v) for v in values[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


HEADER = """\
// Generated by tools/make_splash_logo.py: do not edit; regenerate instead.
// The boot splash's logo as cells (src/os/splash_render.c) and its colours.
// Rows SPLASH_LOGO_ROW0.. of the 80 x 160 cell screen (4x2-pixel cells); per
// cell a glyph (its pixel pattern: row 0 in bits 7-4, row 1 in bits 3-0, the
// leftmost pixel in the highest bit of each nibble) and fg and bg levels
// 0-255 on the ramp. splash_palette is RGB565 in native order.
#pragma once
#include <stdint.h>

#define SPLASH_LOGO_ROW0 {row0}
#define SPLASH_LOGO_ROWS {rows}
#define SPLASH_COLS {cols}
#define SPLASH_PALETTE_SIZE {npal}

extern const uint8_t splash_logo_glyph[SPLASH_LOGO_ROWS * SPLASH_COLS];
extern const uint8_t splash_logo_fg[SPLASH_LOGO_ROWS * SPLASH_COLS];
extern const uint8_t splash_logo_bg[SPLASH_LOGO_ROWS * SPLASH_COLS];
extern const uint16_t splash_palette[SPLASH_PALETTE_SIZE];
"""


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "src" / "os"
    glyph, fg, bg = to_cells(glow_levels(lettering()))
    used = np.flatnonzero((glyph != 0).any(1) | (fg != 0).any(1) | (bg != 0).any(1))
    r0, r1 = int(used[0]) - 1, int(used[-1]) + 2
    (out_dir / "splash_logo_cells.h").write_text(
        HEADER.format(row0=r0, rows=r1 - r0, cols=COLS, npal=NPAL))
    size = "SPLASH_LOGO_ROWS * SPLASH_COLS"
    body = ["// Generated by tools/make_splash_logo.py: do not edit; regenerate instead.",
            '#include "splash_logo_cells.h"', ""]
    for name, plane in (("splash_logo_glyph", glyph), ("splash_logo_fg", fg), ("splash_logo_bg", bg)):
        body += [c_array("uint8_t", name, size, plane[r0:r1].ravel().tolist(), "0x{:02x}"), ""]
    body.append(c_array("uint16_t", "splash_palette", "SPLASH_PALETTE_SIZE", palette(), "0x{:04x}"))
    (out_dir / "splash_logo_cells.c").write_text("\n".join(body) + "\n")
    print(f"rows {r0}..{r1 - 1} -> {out_dir}")


if __name__ == "__main__":
    main()
