// display_clip.h — clip-once rasterisers shared by the firmware display driver
// (src/drivers/display.c) and the simulator's display (simulator/stubs/
// driver_stubs.c), and host-tested by tests/unit/test_display_clip.c.
//
// Everything here is pure: it draws into a caller-supplied 16-bit buffer
// (`fb`, row pitch `stride` pixels) inside an inclusive clip rect, and knows
// nothing about the panel. The firmware framebuffer holds byte-SWAPPED RGB565
// (the panel wants big-endian over the 8-bit PIO SPI DMA) and the simulator's
// holds host-order RGB565, so the blitters take a `swap` flag. Callers pass a
// literal true/false; every function is static inline, so each call site gets
// its own loop with the swap folded away.
//
// Contract, for every primitive:
//   * The destination rect is clipped ONCE, before any pixel loop, so the
//     loops run only over visible pixels with no per-pixel bounds test.
//   * Coordinates and sizes are full-range ints: the clip math is done in
//     int64, so x + w, src_w * scale, a line to (1e9, 1e9) or a circle of
//     radius 1e9 neither overflow nor iterate over off-screen pixels.
//   * Output is pixel-identical to the per-pixel reference loops these
//     replaced (the unit test checks this exhaustively on small cases).
#ifndef DISPLAY_CLIP_H
#define DISPLAY_CLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

// Extra attributes for the image blitters' loops. The firmware (built -Os)
// defines it as optimize("O2") before including this header; elsewhere empty.
#ifndef DISP_HOT
#define DISP_HOT
#endif

// Inclusive clip rect. Empty when x1 < x0 or y1 < y0.
typedef struct {
  int x0, y0, x1, y1;
} disp_clip_t;

// A destination rect after clipping: its visible part (x, y, w, h) and how
// many columns/rows of the unclipped rect were cut off on the left/top.
typedef struct {
  int x, y, w, h;
  int64_t skip_x, skip_y;
} disp_span_t;

// Clip the w x h rect at (x, y). Returns false when nothing is visible.
static inline bool disp_clip_rect(const disp_clip_t *c, int64_t x, int64_t y,
                                  int64_t w, int64_t h, disp_span_t *o) {
  if (w <= 0 || h <= 0) return false;
  int64_t xa = x, ya = y, xb = x + w - 1, yb = y + h - 1;
  if (xa < c->x0) xa = c->x0;
  if (ya < c->y0) ya = c->y0;
  if (xb > c->x1) xb = c->x1;
  if (yb > c->y1) yb = c->y1;
  if (xa > xb || ya > yb) return false;
  o->x = (int)xa;
  o->y = (int)ya;
  o->w = (int)(xb - xa + 1);
  o->h = (int)(yb - ya + 1);
  o->skip_x = xa - x;
  o->skip_y = ya - y;
  return true;
}

static inline uint16_t disp_px(uint16_t c, bool swap) {
  return swap ? (uint16_t)((c >> 8) | (c << 8)) : c;
}

// ── Solid fills ─────────────────────────────────────────────────────────────

// Fill a clipped rect with an already-converted pixel value.
static inline void disp_fill(uint16_t *fb, int stride, const disp_clip_t *c,
                             int64_t x, int64_t y, int64_t w, int64_t h,
                             uint16_t v) {
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, w, h, &s)) return;
  uint16_t *row = fb + (size_t)s.y * stride + s.x;
  if (s.w == stride && s.x == 0 && ((uintptr_t)row & 3) == 0 &&
      ((size_t)s.w * s.h) % 2 == 0) {
    // Full-width band: 32-bit stores over the whole block.
    uint32_t v2 = ((uint32_t)v << 16) | v;
    uint32_t *p = (uint32_t *)(void *)row;
    size_t n = (size_t)s.w * s.h / 2;
    for (size_t i = 0; i < n; i++) p[i] = v2;
    return;
  }
  for (int r = 0; r < s.h; r++, row += stride)
    for (int i = 0; i < s.w; i++) row[i] = v;
}

// Horizontal span xa..xb (either order) on row y.
static inline void disp_hspan(uint16_t *fb, int stride, const disp_clip_t *c,
                              int64_t y, int64_t xa, int64_t xb, uint16_t v) {
  if (xa > xb) { int64_t t = xa; xa = xb; xb = t; }
  disp_fill(fb, stride, c, xa, y, xb - xa + 1, 1, v);
}

static inline void disp_plot(uint16_t *fb, int stride, const disp_clip_t *c,
                             int64_t x, int64_t y, uint16_t v) {
  if (x >= c->x0 && x <= c->x1 && y >= c->y0 && y <= c->y1)
    fb[(size_t)y * stride + (size_t)x] = v;
}

// ── Lines ───────────────────────────────────────────────────────────────────
// The drivers' Bresenham (err = dx + dy, e2 = 2*err) has a closed form: after
// k steps along the major axis it has taken n = floor((2*k*minor + major) /
// (2*major)) minor-axis steps, and err = |dx| - |dy| - xsteps*|dy| +
// ysteps*|dx|. So the loop can start at the first step whose major coordinate
// is inside the clip and stop after the last: a line runs at most clip width
// (or height) + 1 iterations however long it is, and plots exactly the pixels
// the unclipped loop would have (the unit test checks this against the loop).
static inline void disp_line(uint16_t *fb, int stride, const disp_clip_t *c,
                             int x0, int y0, int x1, int y1, uint16_t v) {
  if (c->x1 < c->x0 || c->y1 < c->y0) return;
  // Trivial reject: bounding box misses the clip.
  if ((x0 < c->x0 && x1 < c->x0) || (x0 > c->x1 && x1 > c->x1) ||
      (y0 < c->y0 && y1 < c->y0) || (y0 > c->y1 && y1 > c->y1))
    return;
  const int64_t a = x1 > x0 ? (int64_t)x1 - x0 : (int64_t)x0 - x1;  // |dx|
  const int64_t b = y1 > y0 ? (int64_t)y1 - y0 : (int64_t)y0 - y1;  // |dy|
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  const bool xmajor = a >= b;
  const int64_t major = xmajor ? a : b;
  // Major-axis steps whose coordinate lies inside the clip.
  const int64_t m0 = xmajor ? x0 : y0;
  const int msgn = xmajor ? sx : sy;
  const int64_t lo = xmajor ? c->x0 : c->y0, hi = xmajor ? c->x1 : c->y1;
  int64_t k_lo = msgn > 0 ? lo - m0 : m0 - hi;
  int64_t k_hi = msgn > 0 ? hi - m0 : m0 - lo;
  if (k_lo < 0) k_lo = 0;
  if (k_hi > major) k_hi = major;
  if (k_lo > k_hi) return;
  // State at step k_lo. P = k*minor fits uint64 (k <= major < 2^33).
  int64_t x, y, err;
  if (major == 0) {
    x = x0; y = y0; err = a - b;
  } else {
    const uint64_t minor = (uint64_t)(xmajor ? b : a);
    const uint64_t P = (uint64_t)k_lo * minor;
    const uint64_t q = P / (uint64_t)major, r = P % (uint64_t)major;
    const uint64_t d = (2 * r >= (uint64_t)major) ? 1 : 0;
    const int64_t n = (int64_t)(q + d);  // minor-axis steps taken
    if (xmajor) {
      x = (int64_t)x0 + sx * k_lo;
      y = (int64_t)y0 + sy * n;
      err = a - b + (int64_t)d * a - (int64_t)r;
    } else {
      x = (int64_t)x0 + sx * n;
      y = (int64_t)y0 + sy * k_lo;
      err = a - b + (int64_t)r - (int64_t)d * b;
    }
  }
  for (int64_t k = k_lo;; k++) {
    disp_plot(fb, stride, c, x, y, v);
    if (k == k_hi) break;
    const int64_t e2 = 2 * err;
    if (e2 >= -b) { err -= b; x += sx; }
    if (e2 <= a) { err += a; y += sy; }
  }
}

// ── Circles ─────────────────────────────────────────────────────────────────
// Radii up to DISP_CIRCLE_MIDPOINT_MAX keep the drivers' own midpoint loops
// (cheap, and their exact pixels are what apps and goldens were drawn with).
// Beyond that the midpoint loop is O(r) — a radius of 1e9 spins for minutes —
// so larger circles are rasterised per visible row/column from an integer
// square root instead: O(clip size), and the same pixels within +-1 of the
// true circle.
#define DISP_CIRCLE_MIDPOINT_MAX 4096

static inline uint64_t disp_isqrt(uint64_t n) {  // floor(sqrt(n))
  uint64_t r = 0, bit = (uint64_t)1 << 62;
  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
    else r >>= 1;
    bit >>= 2;
  }
  return r;
}

static inline uint64_t disp_isqrt_round(uint64_t n) {
  uint64_t s = disp_isqrt(n);
  return (n - s * s > s) ? s + 1 : s;  // n >= s^2 + s + 1 rounds up
}

// True when the circle's bounding box misses the clip entirely.
static inline bool disp_circle_rejected(const disp_clip_t *c, int64_t cx,
                                        int64_t cy, int64_t r) {
  return r < 0 || c->x1 < c->x0 || c->y1 < c->y0 || cx + r < c->x0 ||
         cx - r > c->x1 || cy + r < c->y0 || cy - r > c->y1;
}

// Outline for large radii: two points per visible row, two per visible
// column (so flat stretches of the arc have no gaps).
static inline void disp_circle_big(uint16_t *fb, int stride,
                                   const disp_clip_t *c, int64_t cx,
                                   int64_t cy, int64_t r, uint16_t v) {
  const uint64_t rr = (uint64_t)r * (uint64_t)r;
  for (int64_t yy = c->y0; yy <= c->y1; yy++) {
    int64_t d = yy - cy;
    if (d < -r || d > r) continue;
    int64_t h = (int64_t)disp_isqrt_round(rr - (uint64_t)(d * d));
    disp_plot(fb, stride, c, cx - h, yy, v);
    disp_plot(fb, stride, c, cx + h, yy, v);
  }
  for (int64_t xx = c->x0; xx <= c->x1; xx++) {
    int64_t d = xx - cx;
    if (d < -r || d > r) continue;
    int64_t h = (int64_t)disp_isqrt_round(rr - (uint64_t)(d * d));
    disp_plot(fb, stride, c, xx, cy - h, v);
    disp_plot(fb, stride, c, xx, cy + h, v);
  }
}

// Filled disc as one span per visible row: half-width floor(sqrt(r^2 - dy^2)).
static inline void disp_fill_circle_rows(uint16_t *fb, int stride,
                                         const disp_clip_t *c, int64_t cx,
                                         int64_t cy, int64_t r, uint16_t v) {
  const uint64_t rr = (uint64_t)r * (uint64_t)r;
  int64_t ya = cy - r < c->y0 ? c->y0 : cy - r;
  int64_t yb = cy + r > c->y1 ? c->y1 : cy + r;
  for (int64_t yy = ya; yy <= yb; yy++) {
    int64_t d = yy - cy;
    int64_t h = (int64_t)disp_isqrt(rr - (uint64_t)(d * d));
    disp_fill(fb, stride, c, cx - h, yy, 2 * h + 1, 1, v);
  }
}

// ── Triangles ───────────────────────────────────────────────────────────────
// The drivers' scanline triangle (same float edge formulas and the same row
// split, including the shared middle row painted from both halves), with the
// row loop clipped to the clip rect and each row a span fill instead of a
// Bresenham line (a horizontal Bresenham line is exactly that span). Edge x
// values are clamped to one pixel outside the clip, which cannot change the
// visible span.
static inline int64_t disp_tri_x(int64_t base, float delta, float t,
                                 const disp_clip_t *c) {
  // |delta * t| <= |delta| < 2^33, so the int64 conversion is defined; within
  // int range it truncates exactly like the original (int) cast.
  int64_t x = base + (int64_t)(delta * t);
  if (x < (int64_t)c->x0 - 1) x = (int64_t)c->x0 - 1;
  if (x > (int64_t)c->x1 + 1) x = (int64_t)c->x1 + 1;
  return x;
}

static inline void disp_fill_triangle(uint16_t *fb, int stride,
                                      const disp_clip_t *c, int x0, int y0,
                                      int x1, int y1, int x2, int y2,
                                      uint16_t v) {
  int t;
  if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y0 > y2) { t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
  if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
  if (y0 == y2) return;  // degenerate: all on one row
  if (c->x1 < c->x0 || c->y1 < c->y0 || y2 < c->y0 || y0 > c->y1) return;

  const float d20 = (float)((int64_t)x2 - x0), d10 = (float)((int64_t)x1 - x0),
              d21 = (float)((int64_t)x2 - x1);
  const float inv_dy02 = 1.0f / (float)((int64_t)y2 - y0);
  // Clip a row range [ya, yb] to the clip rect.
#define DISP_TRI_ROWS(ya, yb, lo, hi)                                        \
  int64_t lo = (ya) < c->y0 ? c->y0 : (ya), hi = (yb) > c->y1 ? c->y1 : (yb)

  if (y0 == y1) {
    DISP_TRI_ROWS(y0, y2, ra, rb);
    for (int64_t y = ra; y <= rb; y++) {
      float tt = (float)(y - y0) * inv_dy02;
      disp_hspan(fb, stride, c, y, disp_tri_x(x0, d20, tt, c),
                 disp_tri_x(x1, d21, tt, c), v);
    }
  } else if (y1 == y2) {
    const float inv_dy01 = 1.0f / (float)((int64_t)y1 - y0);
    DISP_TRI_ROWS(y0, y1, ra, rb);
    for (int64_t y = ra; y <= rb; y++) {
      float tt = (float)(y - y0) * inv_dy01;
      disp_hspan(fb, stride, c, y, disp_tri_x(x0, d10, tt, c),
                 disp_tri_x(x0, d20, tt, c), v);
    }
  } else {
    const float inv_dy01 = 1.0f / (float)((int64_t)y1 - y0);
    const float inv_dy12 = 1.0f / (float)((int64_t)y2 - y1);
    {
      DISP_TRI_ROWS(y0, y1, ra, rb);
      for (int64_t y = ra; y <= rb; y++) {
        float ts = (float)(y - y0) * inv_dy01;
        float tl = (float)(y - y0) * inv_dy02;
        disp_hspan(fb, stride, c, y, disp_tri_x(x0, d10, ts, c),
                   disp_tri_x(x0, d20, tl, c), v);
      }
    }
    {
      DISP_TRI_ROWS(y1, y2, ra, rb);
      for (int64_t y = ra; y <= rb; y++) {
        float ts = (float)(y - y1) * inv_dy12;
        float tl = (float)(y - y0) * inv_dy02;
        disp_hspan(fb, stride, c, y, disp_tri_x(x1, d21, ts, c),
                   disp_tri_x(x0, d20, tl, c), v);
      }
    }
  }
#undef DISP_TRI_ROWS
}

// ── Span fill and the float-vertex triangle (gfx3d) ─────────────────────────

// Fill row[x0..x1] (inclusive, already clipped) with v; 32-bit stores for the
// aligned middle.
static inline void disp_span16(uint16_t *row, int x0, int x1, uint16_t v) {
  if (x1 < x0) return;
  uint16_t *p = row + x0;
  int n = x1 - x0 + 1;
  if ((uintptr_t)p & 2u) {
    *p++ = v;
    n--;
  }
  uint32_t v2 = ((uint32_t)v << 16) | v;
  uint32_t *q = (uint32_t *)(void *)p;
  for (; n >= 2; n -= 2) *q++ = v2;
  if (n) *(uint16_t *)(void *)q = v;
}

// Smallest integer >= v, clamped to [lo, hi] (NaN gives lo).
static inline int disp_ceil_px(float v, int lo, int hi) {
  if (!(v > (float)lo)) return lo;
  if (v >= (float)hi) return hi;
  return (int)ceilf(v);
}

// x of the edge from its upper end (xt, yt) to (xb, yb) at height yc.
static inline float disp_edge_x(float xt, float yt, float xb, float yb,
                                float yc) {
  return xt + (yc - yt) * ((xb - xt) / (yb - yt));
}

// Flat triangle with float vertices. A pixel is filled when its centre
// (px + 0.5, py + 0.5) is inside, or exactly on a left or top edge (top-left
// rule): two triangles that share an edge fill each pixel along it exactly
// once. Every edge is evaluated only from its upper end point (lower y, then
// lower x), so both triangles compute bit-identical x for a shared edge.
// Non-finite vertices draw nothing; rows and spans are clamped to the clip,
// so any coordinates cost O(visible pixels). `v` is in framebuffer order.
static inline void disp_fill_tri_f(uint16_t *fb, int stride,
                                   const disp_clip_t *c, float x0, float y0,
                                   float x1, float y1, float x2, float y2,
                                   uint16_t v) {
  if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
      !isfinite(x2) || !isfinite(y2))
    return;
  if (c->x1 < c->x0 || c->y1 < c->y0) return;
  float t;
#define DISP_TRI_SWAPF(xa, ya, xb, yb) \
  do { t = xa; xa = xb; xb = t; t = ya; ya = yb; yb = t; } while (0)
  if (y1 < y0 || (y1 == y0 && x1 < x0)) DISP_TRI_SWAPF(x0, y0, x1, y1);
  if (y2 < y0 || (y2 == y0 && x2 < x0)) DISP_TRI_SWAPF(x0, y0, x2, y2);
  if (y2 < y1 || (y2 == y1 && x2 < x1)) DISP_TRI_SWAPF(x1, y1, x2, y2);
#undef DISP_TRI_SWAPF
  if (!(y2 > y0)) return;  // zero height
  const int ya = disp_ceil_px(y0 - 0.5f, c->y0, c->y1 + 1);
  const int yb = disp_ceil_px(y2 - 0.5f, c->y0, c->y1 + 1);  // exclusive
  for (int py = ya; py < yb; py++) {
    const float yc = (float)py + 0.5f;
    float xl = disp_edge_x(x0, y0, x2, y2, yc);
    float xr = yc < y1 ? disp_edge_x(x0, y0, x1, y1, yc)
                       : disp_edge_x(x1, y1, x2, y2, yc);
    // A sub-triangle with zero height on this side (e.g. x1==x2 with
    // y1==y2, dividing 0/0 in disp_edge_x) yields NaN: nothing to fill.
    if (xl != xl || xr != xr) continue;
    if (xr < xl) {
      t = xl;
      xl = xr;
      xr = t;
    }
    const int xa = disp_ceil_px(xl - 0.5f, c->x0, c->x1 + 1);
    const int xe = disp_ceil_px(xr - 0.5f, c->x0, c->x1 + 1);  // exclusive
    if (xe > xa) disp_span16(fb + (size_t)py * stride, xa, xe - 1, v);
  }
}

// ── Blitters ────────────────────────────────────────────────────────────────

// Integer nearest-neighbour upscale: each source pixel becomes a scale x scale
// block. No colour key (the native drawImageNN contract). The destination rect
// is clipped once; a negative offset that is not a multiple of scale starts
// mid-block (the first visible row/column repeats only scale - skip % scale
// times) instead of being rounded to a block boundary, which used to write
// rows above the framebuffer (y = -3, scale 2 wrote row -1).
DISP_HOT static inline void disp_blit_nn(uint16_t *fb, int stride, const disp_clip_t *c,
                                int x, int y, const uint16_t *data, int src_w,
                                int src_h, int scale, bool swap) {
  if (!data || src_w <= 0 || src_h <= 0 || scale <= 0) return;
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, (int64_t)src_w * scale, (int64_t)src_h * scale,
                      &s))
    return;
  const int64_t col0 = s.skip_x / scale;
  const int lead = scale - (int)(s.skip_x % scale);
  uint16_t *row = fb + (size_t)s.y * stride + s.x;
  int64_t src_row = -1;
  for (int r = 0; r < s.h; r++, row += stride) {
    const int64_t sr = (s.skip_y + r) / scale;
    if (sr == src_row) {  // same source row as the line above: copy it
      memcpy(row, row - stride, (size_t)s.w * sizeof(uint16_t));
      continue;
    }
    src_row = sr;
    const uint16_t *sp = data + (size_t)sr * src_w + col0;
    if (scale == 1) {
      for (int i = 0; i < s.w; i++) row[i] = disp_px(sp[i], swap);
      continue;
    }
    int rep = lead;
    uint16_t v = disp_px(*sp, swap);
    for (int i = 0; i < s.w; i++) {
      row[i] = v;
      if (--rep == 0 && i + 1 < s.w) {
        rep = scale;
        v = disp_px(*++sp, swap);
      }
    }
  }
}

// Copy n pixels converting byte order. With swap, two pixels at a time where
// source and destination share word alignment (one REV16-style op per pair).
DISP_HOT static inline void disp_copy_row(uint16_t *d, const uint16_t *src, int n,
                                 bool swap) {
  if (!swap) {
    memcpy(d, src, (size_t)n * sizeof(uint16_t));
    return;
  }
  if (n >= 4 && (((uintptr_t)d ^ (uintptr_t)src) & 2) == 0) {
    if ((uintptr_t)d & 2) {
      *d++ = disp_px(*src++, true);
      n--;
    }
    for (; n >= 2; n -= 2, d += 2, src += 2) {
      uint32_t v;
      memcpy(&v, src, 4);
      v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      memcpy(d, &v, 4);
    }
  }
  while (n-- > 0) *d++ = disp_px(*src++, true);
}

// Partial image blit: source rect (sx, sy, sw, sh) of an img_w x img_h image
// drawn with its top-left at (x, y), optionally flipped, skipping pixels equal
// to `key` (0 = opaque). The source rect is first clamped to the image (the
// destination origin stays at x, y, as it always has); then the destination
// rect is clipped once and one of four inner loops runs — opaque/keyed x
// flipped/not — with no per-pixel bounds tests. Opaque unflipped rows are a
// memcpy (swap = false) or a paired byte swap (swap = true).
DISP_HOT static inline void disp_blit(uint16_t *fb, int stride, const disp_clip_t *c,
                             int x, int y, const uint16_t *data, int img_w,
                             int img_h, int sx, int sy, int sw, int sh,
                             bool flip_x, bool flip_y, uint16_t key,
                             bool swap) {
  if (!data || img_w <= 0 || img_h <= 0) return;
  int64_t sx_ = sx, sy_ = sy, sw_ = sw, sh_ = sh;
  if (sx_ < 0) { sw_ += sx_; sx_ = 0; }
  if (sy_ < 0) { sh_ += sy_; sy_ = 0; }
  if (sx_ + sw_ > img_w) sw_ = img_w - sx_;
  if (sy_ + sh_ > img_h) sh_ = img_h - sy_;
  if (sw_ <= 0 || sh_ <= 0) return;
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, sw_, sh_, &s)) return;

  uint16_t *row = fb + (size_t)s.y * stride + s.x;
  // First visible source column, and the step through the source row.
  const int64_t col0 = flip_x ? sx_ + sw_ - 1 - s.skip_x : sx_ + s.skip_x;
  for (int r = 0; r < s.h; r++, row += stride) {
    const int64_t R = s.skip_y + r;
    const int64_t src_row = flip_y ? sy_ + sh_ - 1 - R : sy_ + R;
    const uint16_t *sp = data + (size_t)src_row * img_w + (size_t)col0;
    if (!key) {
      if (!flip_x) {
        disp_copy_row(row, sp, s.w, swap);
      } else {
        for (int i = 0; i < s.w; i++) row[i] = disp_px(sp[-i], swap);
      }
    } else if (!flip_x) {
      for (int i = 0; i < s.w; i++) {
        uint16_t v = sp[i];
        if (v != key) row[i] = disp_px(v, swap);
      }
    } else {
      for (int i = 0; i < s.w; i++) {
        uint16_t v = sp[-i];
        if (v != key) row[i] = disp_px(v, swap);
      }
    }
  }
}

// Nearest-neighbour stretch of the source rect (sx, sy, sw, sh) of an
// img_w-wide image to dst_w x dst_h at (x, y), skipping `key` pixels (0 =
// opaque). Destination pixel (dx, dy) of the unclipped rect samples source
// (sx + dx*sw/dst_w, sy + dy*sh/dst_h) — exact integer division, stepped with
// a remainder DDA across each row. The source rect must lie inside the image
// (callers clamp).
DISP_HOT static inline void disp_blit_scaled_rect(
    uint16_t *fb, int stride, const disp_clip_t *c, int x, int y,
    const uint16_t *data, int img_w, int sx, int sy, int sw, int sh,
    int dst_w, int dst_h, uint16_t key, bool swap) {
  if (!data || img_w <= 0 || sw <= 0 || sh <= 0 || dst_w <= 0 || dst_h <= 0)
    return;
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, dst_w, dst_h, &s)) return;
  const int64_t q = sw / dst_w, rm = sw % dst_w;
  const int64_t col_start = s.skip_x * sw / dst_w;
  const int64_t acc_start = s.skip_x * sw % dst_w;
  uint16_t *row = fb + (size_t)s.y * stride + s.x;
  int64_t prev = -1;
  for (int r = 0; r < s.h; r++, row += stride) {
    const int64_t src_row = (s.skip_y + r) * sh / dst_h;
    if (!key && src_row == prev) {  // opaque: an upscaled row repeats
      memcpy(row, row - stride, (size_t)s.w * sizeof(uint16_t));
      continue;
    }
    prev = src_row;
    const uint16_t *sp = data + (size_t)(sy + src_row) * img_w + sx;
    int64_t col = col_start, acc = acc_start;
    for (int i = 0; i < s.w; i++) {
      uint16_t v = sp[col];
      if (!key || v != key) row[i] = disp_px(v, swap);
      col += q;
      acc += rm;
      if (acc >= dst_w) { acc -= dst_w; col++; }
    }
  }
}

// Nearest-neighbour scale of a whole src_w x src_h image (see above).
DISP_HOT static inline void disp_blit_scaled(uint16_t *fb, int stride,
                                    const disp_clip_t *c, int x, int y,
                                    const uint16_t *data, int src_w, int src_h,
                                    int dst_w, int dst_h, uint16_t key,
                                    bool swap) {
  if (src_h <= 0) return;
  disp_blit_scaled_rect(fb, stride, c, x, y, data, src_w, 0, 0, src_w, src_h,
                        dst_w, dst_h, key, swap);
}

// Bulk host-order RGB565 block copy (Lua fb:setPixels / getPixels). `src` is
// w*h*2 bytes of little-endian host-order RGB565, row-major, top row first.
// The destination is clipped ONCE; each visible row is then copied from the
// source offset (skip_y + r) * w + skip_x, so the pixels land where they
// would unclipped. Returns false when nothing is visible.
static inline bool disp_set_pixels(uint16_t *fb, int stride,
                                   const disp_clip_t *c, int x, int y, int w,
                                   int h, const uint8_t *src, bool swap) {
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, w, h, &s)) return false;
  for (int r = 0; r < s.h; r++) {
    const uint8_t *sp =
        src + (((size_t)(s.skip_y + r) * (size_t)w) + (size_t)s.skip_x) * 2u;
    uint16_t *row = fb + (size_t)(s.y + r) * stride + s.x;
    for (int i = 0; i < s.w; i++, sp += 2)
      row[i] = disp_px((uint16_t)(sp[0] | ((uint16_t)sp[1] << 8)), swap);
  }
  return true;
}

// The reverse: a w x h rect already known to lie inside the buffer, read out
// as host-order little-endian RGB565. Reads ignore the clip rect (as
// display_get_pixel does).
static inline void disp_get_pixels(const uint16_t *fb, int stride, int x,
                                   int y, int w, int h, uint8_t *dst,
                                   bool swap) {
  for (int r = 0; r < h; r++) {
    const uint16_t *row = fb + (size_t)(y + r) * stride + x;
    for (int i = 0; i < w; i++, dst += 2) {
      uint16_t v = disp_px(row[i], swap);  // swap is its own inverse
      dst[0] = (uint8_t)(v & 0xFF);
      dst[1] = (uint8_t)(v >> 8);
    }
  }
}

#endif  // DISPLAY_CLIP_H
