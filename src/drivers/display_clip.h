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

// Extra attributes for the hot loops: the image blitters, disp_fill_px and the
// bulk pixel copies.
// The firmware (built -Os) defines it as optimize("O2") before including
// this header; elsewhere empty.
#ifndef DISP_HOT
#define DISP_HOT
#endif
// The section of the out-of-line blit only gfx3d's sprites call (disp_blit_scaled_rect_lut):
// gfx3d.c defines it before including this header, to keep it in its sprite fill's slot
// (src/hot_gfx.ld); elsewhere empty.
#ifndef DISP_SPRITE_AUX
#define DISP_SPRITE_AUX
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

// The 16-bit pixel buffer seen as 32-bit words, for paired stores. may_alias:
// the buffer is uint16_t, and these stores must not be reordered past 16-bit
// accesses to the same pixels.
typedef uint32_t __attribute__((may_alias)) disp_u32a_t;

// Set n consecutive pixels from p to v: a 16-bit store when p is not 4-byte
// aligned, then pairs as 32-bit stores eight at a time, then a 16-bit store
// for an odd last pixel. On the device the plain -Os loops this replaced cost
// a compare and a branch per store: ~5 cycles a word for display_clear, ~10
// for a full-width fillRect, one 16-bit store per pixel for any other rect.
__attribute__((always_inline)) static inline void disp_fill_px_inl(uint16_t *p, size_t n,
                                                                   uint16_t v) {
  if (n && ((uintptr_t)p & 2)) { *p++ = v; n--; }
  disp_u32a_t *q = (disp_u32a_t *)(void *)p;
  const uint32_t v2 = ((uint32_t)v << 16) | v;
  size_t words = n / 2;
  for (; words >= 8; words -= 8, q += 8) {
    q[0] = v2; q[1] = v2; q[2] = v2; q[3] = v2;
    q[4] = v2; q[5] = v2; q[6] = v2; q[7] = v2;
  }
  while (words--) *q++ = v2;
  if (n & 1) *(uint16_t *)(void *)q = v;
}

DISP_HOT static inline void disp_fill_px(uint16_t *p, size_t n, uint16_t v) {
  disp_fill_px_inl(p, n, v);
}

// Fill a clipped rect with an already-converted pixel value.
static inline void disp_fill(uint16_t *fb, int stride, const disp_clip_t *c,
                             int64_t x, int64_t y, int64_t w, int64_t h,
                             uint16_t v) {
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, w, h, &s)) return;
  uint16_t *row = fb + (size_t)s.y * stride + s.x;
  if (s.w == stride) {
    // Full-width band: one contiguous run.
    disp_fill_px(row, (size_t)s.w * s.h, v);
  } else if (s.w == 1) {
    // A column (a vertical line, a rect's side).
    for (int r = 0; r < s.h; r++, row += stride) *row = v;
  } else {
    for (int r = 0; r < s.h; r++, row += stride) disp_fill_px(row, s.w, v);
  }
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
//
// A horizontal or vertical line is a fill: the walk would set every pixel of
// that row or column span anyway, one plot and its clip test at a time (on
// the device a full-width line took 189 us that way and takes ~16 as a fill,
// a 280-pixel column 165 against 20).
static inline void disp_line(uint16_t *fb, int stride, const disp_clip_t *c,
                             int x0, int y0, int x1, int y1, uint16_t v) {
  if (y0 == y1) {
    disp_hspan(fb, stride, c, y0, x0, x1, v);
    return;
  }
  if (x0 == x1) {
    int64_t ya = y0 < y1 ? y0 : y1, yb = y0 < y1 ? y1 : y0;
    disp_fill(fb, stride, c, x0, ya, 1, yb - ya + 1, v);
    return;
  }
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
  if (major == 0 || k_lo == 0) {  // starts at (x0, y0): no 64-bit division
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
  // Any line a Lua app can draw (|v| <= 2^24), and any sane native one: the walk
  // in int32 (|err| <= 2a + b, so e2 fits), with the clip test as two unsigned
  // compares. In int64 a pixel costs ~118 cycles on the device (a full-width
  // Bresenham line took 189 us).
  if (a < (1 << 28) && b < (1 << 28)) {
    int xi = (int)x, yi = (int)y, ei = (int)err, left = (int)(k_hi - k_lo);
    const int ai = (int)a, bi = (int)b;
    const int cx0 = c->x0, cy0 = c->y0;
    const unsigned cw = (unsigned)(c->x1 - cx0), ch = (unsigned)(c->y1 - cy0);
    for (;;) {
      if ((unsigned)(xi - cx0) <= cw && (unsigned)(yi - cy0) <= ch)
        fb[(size_t)yi * (size_t)stride + (size_t)xi] = v;
      if (left-- == 0) return;
      const int e2 = 2 * ei;
      if (e2 >= -bi) { ei -= bi; xi += sx; }
      if (e2 <= ai) { ei += ai; yi += sy; }
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

// Filled disc by the midpoint walk (the firmware's pixels up to
// DISP_CIRCLE_MIDPOINT_MAX). All spans are one colour, so a row needs only its
// widest span: the rows cy +- y get theirs once each, and the rows cy +- x, which
// repeat with a growing width while x stays put, are drawn only as x is about to
// change or the walk ends. The union of the spans is unchanged.
static inline void disp_fill_circle_mid(uint16_t *fb, int stride,
                                        const disp_clip_t *c, int cx, int cy,
                                        int r, uint16_t v) {
  int x = r, y = 0, d = 1 - r;
  while (x >= y) {
    disp_fill(fb, stride, c, cx - x, cy + y, 2 * x + 1, 1, v);
    if (y) disp_fill(fb, stride, c, cx - x, cy - y, 2 * x + 1, 1, v);
    const int cap_w = 2 * y + 1, cap_x = cx - y, cap_row = x;
    y++;
    const bool step = d > 0;
    if (step) {
      x--;
      d += 2 * (y - x) + 1;
    } else {
      d += 2 * y + 1;
    }
    if (step || x < y) {
      disp_fill(fb, stride, c, cap_x, cy + cap_row, cap_w, 1, v);
      if (cap_row) disp_fill(fb, stride, c, cap_x, cy - cap_row, cap_w, 1, v);
    }
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
  // In int32 range the cast is one VCVT; the int64 one is a libgcc call per edge
  // per row. Both truncate.
  const float m = delta * t;
  int64_t x = base + ((m > -1e9f && m < 1e9f) ? (int32_t)m : (int64_t)m);
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

// Fill row[x0..x1] (inclusive, already clipped) with v, through disp_fill_px's
// eight-at-a-time stores: gfx3d's sky alone is ~38 K word stores a frame.
static inline void disp_span16(uint16_t *row, int x0, int x1, uint16_t v) {
  if (x1 < x0) return;
  disp_fill_px(row + x0, (size_t)(x1 - x0 + 1), v);
}

// Smallest integer >= v, clamped to [lo, hi] (NaN gives lo).
static inline int disp_ceil_px(float v, int lo, int hi) {
  if (!(v > (float)lo)) return lo;
  if (v >= (float)hi) return hi;
  return (int)ceilf(v);
}

// One row of a float-vertex triangle, its two edge x values given in either
// order. flo/fhi are (float)cx0 and (float)(cx1 + 1), the clip's x range.
__attribute__((always_inline)) static inline void disp_tri_row(
    uint16_t *row, float xl, float xr, int cx0, int cxe, float flo, float fhi,
    uint16_t v) {
  // A sub-triangle with zero height on this side (e.g. x1==x2 with y1==y2,
  // dividing 0/0 for its slope) yields NaN: nothing to fill.
  if (xl != xl || xr != xr) return;
  if (xr < xl) {
    const float t = xl;
    xl = xr;
    xr = t;
  }
  xl -= 0.5f;
  xr -= 0.5f;
  // disp_ceil_px(xl, cx0, cxe), disp_ceil_px(xr, cx0, cxe)
  const int xa = !(xl > flo) ? cx0 : xl >= fhi ? cxe : (int)ceilf(xl);
  const int xe = !(xr > flo) ? cx0 : xr >= fhi ? cxe : (int)ceilf(xr);  // exclusive
  if (xe > xa) disp_fill_px_inl(row + xa, (size_t)(xe - xa), v);
}

// The first half of disp_fill_tri_f: false for a non-finite vertex, else sorts
// the vertices by (y, x), the order disp_fill_tri_sorted wants.
__attribute__((always_inline)) static inline bool disp_tri_sort(
    float *x0, float *y0, float *x1, float *y1, float *x2, float *y2) {
  // x - x is 0 when x is finite, else NaN, and NaN stays through the sum.
  const float fin = (*x0 - *x0) + (*y0 - *y0) + (*x1 - *x1) + (*y1 - *y1) +
                    (*x2 - *x2) + (*y2 - *y2);
  if (fin != fin) return false;
  float t;
#define DISP_TRI_SWAPF(xa, ya, xb, yb) \
  do { t = *xa; *xa = *xb; *xb = t; t = *ya; *ya = *yb; *yb = t; } while (0)
  if (*y1 < *y0 || (*y1 == *y0 && *x1 < *x0)) DISP_TRI_SWAPF(x0, y0, x1, y1);
  if (*y2 < *y0 || (*y2 == *y0 && *x2 < *x0)) DISP_TRI_SWAPF(x0, y0, x2, y2);
  if (*y2 < *y1 || (*y2 == *y1 && *x2 < *x1)) DISP_TRI_SWAPF(x1, y1, x2, y2);
#undef DISP_TRI_SWAPF
  return true;
}

// The second half: vertices from disp_tri_sort, the clip rect (not empty) by
// value, so a caller drawing many triangles keeps it in registers: a span store
// (disp_u32a_t, may_alias) could change anything a pointer reaches, and a clip
// behind one is read again for every row. Always inlined, so a caller placed in
// RAM stays there.
__attribute__((always_inline)) static inline void disp_fill_tri_sorted(
    uint16_t *fb, int stride, int cx0, int cy0, int cx1, int cy1, float x0,
    float y0, float x1, float y1, float x2, float y2, uint16_t v) {
  if (!(y2 > y0)) return;  // zero height
  const int ya = disp_ceil_px(y0 - 0.5f, cy0, cy1 + 1);
  const int yb = disp_ceil_px(y2 - 0.5f, cy0, cy1 + 1);  // exclusive
  if (ya >= yb) return;
  // An edge's x at height yc is xt + (yc - yt) * slope, from its upper end, on
  // every row and in every triangle that shares it. Each slope is one division
  // per triangle, done only if a row needs it. (xs, ys, ss) is the short side's
  // edge on this row: from the top vertex, then from the middle one. One row
  // loop, not one per side: this code is in SRAM on the device.
  const int cxe = cx1 + 1;
  const float flo = (float)cx0, fhi = (float)cxe;
  const float s02 = (x2 - x0) / (y2 - y0);
  uint16_t *row = fb + (size_t)ya * stride;
  int py = ya;
  float yc = (float)py + 0.5f;
  bool lower = !(yc < y1);
  float xs = lower ? x1 : x0, ys = lower ? y1 : y0;
  float ss = lower ? (x2 - x1) / (y2 - y1) : (x1 - x0) / (y1 - y0);
  for (;;) {
    disp_tri_row(row, x0 + (yc - y0) * s02, xs + (yc - ys) * ss, cx0, cxe, flo,
                 fhi, v);
    if (++py >= yb) break;
    row += stride;
    yc = (float)py + 0.5f;
    if (!lower && !(yc < y1)) {
      lower = true;
      xs = x1;
      ys = y1;
      ss = (x2 - x1) / (y2 - y1);
    }
  }
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
  if (c->x1 < c->x0 || c->y1 < c->y0) return;
  if (!disp_tri_sort(&x0, &y0, &x1, &y1, &x2, &y2)) return;
  disp_fill_tri_sorted(fb, stride, c->x0, c->y0, c->x1, c->y1, x0, y0, x1, y1, x2,
                       y2, v);
}

// ── Blitters ────────────────────────────────────────────────────────────────

// Copy n pixels converting byte order. With swap, two pixels at a time (one
// REV16 per pair): the source is first brought to a word, so each load is one
// access inside one XIP cache line, and the store may be two bytes off a word,
// which the M33 and every host take as it is (a sprite at an odd x is that).
DISP_HOT static inline void disp_copy_row(uint16_t *d, const uint16_t *src, int n,
                                 bool swap) {
  if (!swap) {
    memcpy(d, src, (size_t)n * sizeof(uint16_t));
    return;
  }
  if (n >= 4) {
    if ((uintptr_t)src & 2) {
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
  // The source row being drawn, and how many rows it still covers: no 64-bit
  // division per row.
  const uint16_t *line = data + (size_t)(s.skip_y / scale) * src_w + col0;
  int rows_left = scale - (int)(s.skip_y % scale);
  uint16_t *row = fb + (size_t)s.y * stride + s.x;
  for (int r = 0; r < s.h; r++, row += stride) {
    if (r > 0 && --rows_left > 0) {  // same source row as the line above: copy it
      memcpy(row, row - stride, (size_t)s.w * sizeof(uint16_t));
      continue;
    }
    if (r > 0) {
      line += src_w;
      rows_left = scale;
    }
    const uint16_t *sp = line;
    if (scale == 1) {
      disp_copy_row(row, sp, s.w, swap);
      continue;
    }
    if (scale == 2 && lead == 2) {  // one word per source pixel
      int i = 0;
      for (; i + 1 < s.w; i += 2) {
        const uint32_t v = disp_px(*sp++, swap);
        const uint32_t v2 = (v << 16) | v;
        memcpy(row + i, &v2, 4);
      }
      if (i < s.w) row[i] = disp_px(*sp, swap);
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

// The same walking the source backwards from src: d[i] = src[-i]. One word holds
// src[-1] and src[0], so reversing its bytes both swaps and exchanges the pair.
DISP_HOT static inline void disp_copy_row_rev(uint16_t *d, const uint16_t *src,
                                              int n, bool swap) {
  if (n >= 4) {
    if (!((uintptr_t)src & 2)) {  // src[-1] is in the word before
      *d++ = disp_px(*src--, swap);
      n--;
    }
    for (; n >= 2; n -= 2, d += 2, src -= 2) {
      uint32_t v;
      memcpy(&v, src - 1, 4);
      v = swap ? __builtin_bswap32(v) : (v >> 16) | (v << 16);
      memcpy(d, &v, 4);
    }
  }
  while (n-- > 0) *d++ = disp_px(*src--, swap);
}

// Is either half of x zero? One test for the common case, both pixels drawn.
#define DISP_HAS_ZERO_HALF(x) \
  ((((x) - 0x00010001u) & ~(x) & 0x80008000u) != 0)

// A colour-keyed row, a pair of pixels per loaded word: both keyed (nothing to
// store), both drawn (one REV16 and one store) or one of each. On the device this
// halves the loads from the image in PSRAM and the stores to the framebuffer.
DISP_HOT static inline void disp_key_row(uint16_t *d, const uint16_t *src, int n,
                                         uint16_t key, bool swap) {
  if (n >= 4) {
    if ((uintptr_t)src & 2) {
      const uint16_t p = *src++;
      if (p != key) *d = disp_px(p, swap);
      d++;
      n--;
    }
    const uint32_t key2 = ((uint32_t)key << 16) | key;
    for (; n >= 2; n -= 2, d += 2, src += 2) {
      uint32_t v;
      memcpy(&v, src, 4);
      const uint32_t x = v ^ key2;
      if (!DISP_HAS_ZERO_HALF(x)) {
        if (swap) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
        memcpy(d, &v, 4);
      } else if (x & 0xFFFFu) {
        d[0] = disp_px((uint16_t)v, swap);
      } else if (x) {
        d[1] = disp_px((uint16_t)(v >> 16), swap);
      }
    }
  }
  for (; n > 0; n--, d++, src++) {
    const uint16_t p = *src;
    if (p != key) *d = disp_px(p, swap);
  }
}

// disp_key_row walking the source backwards: d[i] = src[-i].
DISP_HOT static inline void disp_key_row_rev(uint16_t *d, const uint16_t *src,
                                             int n, uint16_t key, bool swap) {
  if (n >= 4) {
    if (!((uintptr_t)src & 2)) {
      const uint16_t p = *src--;
      if (p != key) *d = disp_px(p, swap);
      d++;
      n--;
    }
    const uint32_t key2 = ((uint32_t)key << 16) | key;
    for (; n >= 2; n -= 2, d += 2, src -= 2) {
      uint32_t v;
      memcpy(&v, src - 1, 4);  // low half src[-1], high half src[0]
      const uint32_t x = v ^ key2;
      if (!DISP_HAS_ZERO_HALF(x)) {
        v = swap ? __builtin_bswap32(v) : (v >> 16) | (v << 16);
        memcpy(d, &v, 4);
      } else if (x >> 16) {
        d[0] = disp_px((uint16_t)(v >> 16), swap);
      } else if (x) {
        d[1] = disp_px((uint16_t)v, swap);
      }
    }
  }
  for (; n > 0; n--, d++, src--) {
    const uint16_t p = *src;
    if (p != key) *d = disp_px(p, swap);
  }
}

// Partial image blit: source rect (sx, sy, sw, sh) of an img_w x img_h image
// drawn with its top-left at (x, y), optionally flipped, skipping pixels equal
// to `key` (0 = opaque). The source rect is first clamped to the image (the
// destination origin stays at x, y, as it always has); then the destination
// rect is clipped once and one of four row loops runs — opaque/keyed x
// flipped/not — with no per-pixel bounds tests.
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
  // First visible source pixel, and the step from one row's to the next's.
  const int64_t col0 = flip_x ? sx_ + sw_ - 1 - s.skip_x : sx_ + s.skip_x;
  const int64_t row0 = flip_y ? sy_ + sh_ - 1 - s.skip_y : sy_ + s.skip_y;
  const ptrdiff_t step = flip_y ? -(ptrdiff_t)img_w : (ptrdiff_t)img_w;
  const ptrdiff_t first = (ptrdiff_t)row0 * img_w + (ptrdiff_t)col0;
  for (int r = 0; r < s.h; r++, row += stride) {
    const uint16_t *sp = data + first + (ptrdiff_t)r * step;
    if (!key) {
      if (!flip_x) disp_copy_row(row, sp, s.w, swap);
      else disp_copy_row_rev(row, sp, s.w, swap);
    } else if (!flip_x) {
      disp_key_row(row, sp, s.w, key, swap);
    } else {
      disp_key_row_rev(row, sp, s.w, key, swap);
    }
  }
}

// One host RGB565 pixel mixed `level`/31 (0..31) of the way to `fog`, per channel.
static inline uint16_t disp_fog_px(uint16_t v, uint16_t fog, unsigned level) {
  const unsigned k = 31u - level;
  const unsigned r = ((((unsigned)v >> 11) & 31u) * k + (((unsigned)fog >> 11) & 31u) * level + 15u) / 31u;
  const unsigned g = ((((unsigned)v >> 5) & 63u) * k + (((unsigned)fog >> 5) & 63u) * level + 15u) / 31u;
  const unsigned b = (((unsigned)v & 31u) * k + ((unsigned)fog & 31u) * level + 15u) / 31u;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Nearest-neighbour stretch of the source rect (sx, sy, sw, sh) of an
// img_w-wide image to dst_w x dst_h at (x, y), skipping `key` pixels (0 =
// opaque). Destination pixel (dx, dy) of the unclipped rect samples source
// (sx + dx*sw/dst_w, sy + dy*sh/dst_h) — exact integer division, stepped with
// a remainder DDA across each row. The source rect must lie inside the image
// (callers clamp). Returns the pixels of the clipped destination rect, whatever the key
// skipped: what the call covers, for a counter that wants it for free.
// A negative stride runs the rows upwards from fb, for a picture the other way up: every
// row of the clip must then be at fb + its y * stride.
DISP_HOT static inline uint32_t disp_blit_scaled_rect(
    uint16_t *fb, int stride, const disp_clip_t *c, int x, int y,
    const uint16_t *data, int img_w, int sx, int sy, int sw, int sh,
    int dst_w, int dst_h, uint16_t key, bool swap) {
  if (!data || img_w <= 0 || sw <= 0 || sh <= 0 || dst_w <= 0 || dst_h <= 0)
    return 0;
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, dst_w, dst_h, &s)) return 0;
  // The first visible source column and row, then a remainder DDA in 32 bits for
  // both: acc < dst_w and rm < dst_w, so acc + rm fits uint32. (In int64 each
  // pixel cost two add-with-carry pairs and a 64-bit compare, each row a 64-bit
  // division.)
  const int q = sw / dst_w, rq = sh / dst_h;
  const uint32_t rm = (uint32_t)(sw % dst_w), dw = (uint32_t)dst_w;
  const uint32_t rrm = (uint32_t)(sh % dst_h), dh = (uint32_t)dst_h;
  const int col_start = s.skip_x ? (int)(s.skip_x * sw / dst_w) : 0;
  const uint32_t acc_start = s.skip_x ? (uint32_t)(s.skip_x * sw % dst_w) : 0;
  int src_row = s.skip_y ? (int)(s.skip_y * sh / dst_h) : 0;
  uint32_t racc = s.skip_y ? (uint32_t)(s.skip_y * sh % dst_h) : 0;
  uint16_t *row = fb + (ptrdiff_t)s.y * stride + s.x;
  int prev = -1;
  for (int r = 0; r < s.h; r++, row += stride) {
    if (!key && src_row == prev) {  // opaque: an upscaled row repeats
      memcpy(row, row - stride, (size_t)s.w * sizeof(uint16_t));
    } else {
      prev = src_row;
      const uint16_t *sp = data + (size_t)(sy + src_row) * img_w + sx;
      int col = col_start;
      uint32_t acc = acc_start;
      if (key) {
        for (int i = 0; i < s.w; i++) {
          const uint16_t v = sp[col];
          if (v != key) row[i] = disp_px(v, swap);
          col += q;
          acc += rm;
          if (acc >= dw) { acc -= dw; col++; }
        }
      } else {
        for (int i = 0; i < s.w; i++) {
          row[i] = disp_px(sp[col], swap);
          col += q;
          acc += rm;
          if (acc >= dw) { acc -= dw; col++; }
        }
      }
    }
    src_row += rq;
    racc += rrm;
    if (racc >= dh) { racc -= dh; src_row++; }
  }
  return (uint32_t)s.w * (uint32_t)s.h;
}

// disp_blit_scaled_rect with fog: each pixel drawn (not a key one) is first mixed `level`/31
// of the way to the host RGB565 colour `fog`, per channel,
// (c * (31 - level) + fog * level + 15) / 31, so 0 is the image and 31 the fog. Same
// sampling, same return. gfx3d's sprites; a level of 0 is better sent to the plain blit.
// Nearest-neighbour stretch of the source rect (sx, sy, sw, sh) of an
// img_w-wide image to dst_w x dst_h at (x, y), skipping `key` pixels (0 =
// opaque). Destination pixel (dx, dy) of the unclipped rect samples source
// (sx + dx*sw/dst_w, sy + dy*sh/dst_h) — exact integer division, stepped with
// a remainder DDA across each row. The source rect must lie inside the image
// (callers clamp).
DISP_HOT static inline uint32_t disp_blit_scaled_rect_fog(
    uint16_t *fb, int stride, const disp_clip_t *c, int x, int y,
    const uint16_t *data, int img_w, int sx, int sy, int sw, int sh,
    int dst_w, int dst_h, uint16_t key, bool swap,
    uint16_t fog, unsigned level) {
  if (!data || img_w <= 0 || sw <= 0 || sh <= 0 || dst_w <= 0 || dst_h <= 0)
    return 0;
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, dst_w, dst_h, &s)) return 0;
  // The first visible source column and row, then a remainder DDA in 32 bits for
  // both: acc < dst_w and rm < dst_w, so acc + rm fits uint32. (In int64 each
  // pixel cost two add-with-carry pairs and a 64-bit compare, each row a 64-bit
  // division.)
  const int q = sw / dst_w, rq = sh / dst_h;
  const uint32_t rm = (uint32_t)(sw % dst_w), dw = (uint32_t)dst_w;
  const uint32_t rrm = (uint32_t)(sh % dst_h), dh = (uint32_t)dst_h;
  const int col_start = s.skip_x ? (int)(s.skip_x * sw / dst_w) : 0;
  const uint32_t acc_start = s.skip_x ? (uint32_t)(s.skip_x * sw % dst_w) : 0;
  int src_row = s.skip_y ? (int)(s.skip_y * sh / dst_h) : 0;
  uint32_t racc = s.skip_y ? (uint32_t)(s.skip_y * sh % dst_h) : 0;
  uint16_t *row = fb + (ptrdiff_t)s.y * stride + s.x;
  int prev = -1;
  for (int r = 0; r < s.h; r++, row += stride) {
    if (!key && src_row == prev) {  // opaque: an upscaled row repeats
      memcpy(row, row - stride, (size_t)s.w * sizeof(uint16_t));
    } else {
      prev = src_row;
      const uint16_t *sp = data + (size_t)(sy + src_row) * img_w + sx;
      int col = col_start;
      uint32_t acc = acc_start;
      if (key) {
        for (int i = 0; i < s.w; i++) {
          const uint16_t v = sp[col];
          if (v != key) row[i] = disp_px(disp_fog_px(v, fog, level), swap);
          col += q;
          acc += rm;
          if (acc >= dw) { acc -= dw; col++; }
        }
      } else {
        for (int i = 0; i < s.w; i++) {
          row[i] = disp_px(disp_fog_px(sp[col], fog, level), swap);
          col += q;
          acc += rm;
          if (acc >= dw) { acc -= dw; col++; }
        }
      }
    }
    src_row += rq;
    racc += rrm;
    if (racc >= dh) { racc -= dh; src_row++; }
  }
  return (uint32_t)s.w * (uint32_t)s.h;
}

// disp_fog_px for one fog colour and level, as three tables of DISP_FOG_LUT_LEN entries in all:
// disp_fog_lut_px(lut, v) is disp_px(disp_fog_px(v, fog, level), swap). Each entry is one channel
// mixed, already in its place and in the target's byte order (a swap only moves bits, so the
// channels can be swapped apart and ORed after), so a pixel is three loads and two ORs where the
// mix is three multiply-adds and three divisions by 31.
#define DISP_FOG_LUT_LEN 128  // red's 32, green's 64, blue's 32
static inline void disp_fog_lut(uint16_t *lut, uint16_t fog, unsigned level, bool swap) {
  const unsigned k = 31u - level;
  const unsigned fr = (((unsigned)fog >> 11) & 31u) * level + 15u;
  const unsigned fg = (((unsigned)fog >> 5) & 63u) * level + 15u;
  const unsigned fb = ((unsigned)fog & 31u) * level + 15u;
  for (unsigned v = 0; v < 32u; v++) {
    lut[v] = disp_px((uint16_t)(((v * k + fr) / 31u) << 11), swap);
    lut[96u + v] = disp_px((uint16_t)((v * k + fb) / 31u), swap);
  }
  for (unsigned v = 0; v < 64u; v++) lut[32u + v] = disp_px((uint16_t)(((v * k + fg) / 31u) << 5), swap);
}

static inline uint16_t disp_fog_lut_px(const uint16_t *lut, uint16_t v) {
  return (uint16_t)(lut[v >> 11] | lut[32u + ((v >> 5) & 63u)] | lut[96u + (v & 31u)]);
}

// disp_blit_scaled_rect_fog with the mix looked up in disp_fog_lut's tables: the same pixels and
// the same return. No swap: the tables are in the target's byte order. Out of line, so that no
// caller's state competes with its pixel loops for registers.
DISP_HOT DISP_SPRITE_AUX static __attribute__((noinline, unused)) uint32_t disp_blit_scaled_rect_lut(
    uint16_t *fb, int stride, const disp_clip_t *c, int x, int y,
    const uint16_t *data, int img_w, int sx, int sy, int sw, int sh,
    int dst_w, int dst_h, uint16_t key, const uint16_t *lut) {
  if (!data || img_w <= 0 || sw <= 0 || sh <= 0 || dst_w <= 0 || dst_h <= 0)
    return 0;
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, dst_w, dst_h, &s)) return 0;
  const int q = sw / dst_w, rq = sh / dst_h;
  const uint32_t rm = (uint32_t)(sw % dst_w), dw = (uint32_t)dst_w;
  const uint32_t rrm = (uint32_t)(sh % dst_h), dh = (uint32_t)dst_h;
  const int col_start = s.skip_x ? (int)(s.skip_x * sw / dst_w) : 0;
  const uint32_t acc_start = s.skip_x ? (uint32_t)(s.skip_x * sw % dst_w) : 0;
  int src_row = s.skip_y ? (int)(s.skip_y * sh / dst_h) : 0;
  uint32_t racc = s.skip_y ? (uint32_t)(s.skip_y * sh % dst_h) : 0;
  uint16_t *row = fb + (ptrdiff_t)s.y * stride + s.x;
  int prev = -1;
  for (int r = 0; r < s.h; r++, row += stride) {
    if (!key && src_row == prev) {  // opaque: an upscaled row repeats
      memcpy(row, row - stride, (size_t)s.w * sizeof(uint16_t));
    } else {
      prev = src_row;
      // The source pixel itself steps, not its column: one register fewer than the other
      // blits' loops, which on the M33 is what keeps the loop off the stack.
      const uint16_t *sp = data + (size_t)(sy + src_row) * img_w + sx + col_start;
      uint32_t acc = acc_start;
      uint16_t *d = row;
      uint16_t *const e = row + s.w;
      if (key) {
        for (; d < e; d++) {
          const uint16_t v = *sp;
          if (v != key) *d = disp_fog_lut_px(lut, v);
          sp += q;
          acc += rm;
          if (acc >= dw) { acc -= dw; sp++; }
        }
      } else {
        for (; d < e; d++) {
          *d = disp_fog_lut_px(lut, *sp);
          sp += q;
          acc += rm;
          if (acc >= dw) { acc -= dw; sp++; }
        }
      }
    }
    src_row += rq;
    racc += rrm;
    if (racc >= dh) { racc -= dh; src_row++; }
  }
  return (uint32_t)s.w * (uint32_t)s.h;
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

// Vertical gradient from `top` at y0 to `bottom` at y1 (swapped if y0 > y1),
// each RGB565 channel interpolated, clipped once. The ramp is that of the whole
// line: clipping shows part of it and never compresses it. A line too long for
// the channel product in int32 is shifted down, which loses nothing visible.
static inline void disp_fill_vline_gradient(uint16_t *fb, int stride,
                                            const disp_clip_t *c, int x, int y0,
                                            int y1, uint16_t top, uint16_t bottom,
                                            bool swap) {
  if (x < c->x0 || x > c->x1) return;
  if (y0 > y1) {
    int t = y0; y0 = y1; y1 = t;
    uint16_t tc = top; top = bottom; bottom = tc;
  }
  const int first = y0 < c->y0 ? c->y0 : y0;
  const int last = y1 > c->y1 ? c->y1 : y1;
  if (first > last) return;

  int64_t full = (int64_t)y1 - y0;
  int shift = 0;
  while ((full >> shift) > 0xFFFFFF) shift++;
  const int span = full ? (int)(full >> shift) : 1;

  const int r0 = (top >> 11) & 0x1F, g0 = (top >> 5) & 0x3F, b0 = top & 0x1F;
  const int r1 = (bottom >> 11) & 0x1F, g1 = (bottom >> 5) & 0x3F, b1 = bottom & 0x1F;

  uint16_t *p = fb + (size_t)first * stride + x;
  for (int y = first; y <= last; y++, p += stride) {
    const int t = (int)(((int64_t)y - y0) >> shift);
    const int r = r0 + (r1 - r0) * t / span;
    const int g = g0 + (g1 - g0) * t / span;
    const int b = b0 + (b1 - b0) * t / span;
    *p = disp_px((uint16_t)((r << 11) | (g << 5) | b), swap);
  }
}

// ── Bulk pixel blocks (Lua fb:setPixels / getPixels) ────────────────────────

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "disp_copy_px_bytes assumes a little-endian host (memcpy of host-order pixels)"
#endif

// Copy n 16-bit pixels between a pixel buffer and a byte string of
// little-endian RGB565, byte-swapping each pixel when `swap` (its own
// inverse, so one routine serves both directions). Either pointer may sit at
// any byte address: a Lua string's bytes are only byte-aligned in general,
// and a row starting at an odd x is 2 bytes off a word. Words move through
// memcpy, which the M33 (and every host) does as single loads and stores,
// unaligned or not; with swap each word gets one REV16 (two pixels). The
// per-byte loop this replaced cost ~0.12 us a pixel into PSRAM on the device.
DISP_HOT static inline void disp_copy_px_bytes(uint8_t *dst, const uint8_t *src,
                                               size_t n, bool swap) {
  if (!swap) {
    memcpy(dst, src, n * 2u);
    return;
  }
  uint32_t a, b, c, d;
  for (; n >= 8; n -= 8, src += 16, dst += 16) {
    memcpy(&a, src, 4); memcpy(&b, src + 4, 4);
    memcpy(&c, src + 8, 4); memcpy(&d, src + 12, 4);
    a = ((a & 0x00FF00FFu) << 8) | ((a >> 8) & 0x00FF00FFu);
    b = ((b & 0x00FF00FFu) << 8) | ((b >> 8) & 0x00FF00FFu);
    c = ((c & 0x00FF00FFu) << 8) | ((c >> 8) & 0x00FF00FFu);
    d = ((d & 0x00FF00FFu) << 8) | ((d >> 8) & 0x00FF00FFu);
    memcpy(dst, &a, 4); memcpy(dst + 4, &b, 4);
    memcpy(dst + 8, &c, 4); memcpy(dst + 12, &d, 4);
  }
  for (; n >= 2; n -= 2, src += 4, dst += 4) {
    memcpy(&a, src, 4);
    a = ((a & 0x00FF00FFu) << 8) | ((a >> 8) & 0x00FF00FFu);
    memcpy(dst, &a, 4);
  }
  if (n) {
    uint8_t lo = src[0];
    dst[0] = src[1];
    dst[1] = lo;
  }
}

// Bulk host-order RGB565 block copy (Lua fb:setPixels). `src` is w*h*2 bytes
// of little-endian host-order RGB565, row-major, top row first. The
// destination is clipped ONCE; each visible row is then copied from the
// source offset (skip_y + r) * w + skip_x, so the pixels land where they
// would unclipped. Returns false when nothing is visible.
DISP_HOT static inline bool disp_set_pixels(uint16_t *fb, int stride,
                                            const disp_clip_t *c, int x, int y,
                                            int w, int h, const uint8_t *src,
                                            bool swap) {
  disp_span_t s;
  if (!disp_clip_rect(c, x, y, w, h, &s)) return false;
  for (int r = 0; r < s.h; r++) {
    const uint8_t *sp =
        src + (((size_t)(s.skip_y + r) * (size_t)w) + (size_t)s.skip_x) * 2u;
    uint16_t *row = fb + (size_t)(s.y + r) * stride + s.x;
    disp_copy_px_bytes((uint8_t *)row, sp, (size_t)s.w, swap);
  }
  return true;
}

// The reverse (fb:getPixels): a w x h rect already known to lie inside the
// buffer, read out as host-order little-endian RGB565. Reads ignore the clip
// rect (as display_get_pixel does).
DISP_HOT static inline void disp_get_pixels(const uint16_t *fb, int stride,
                                            int x, int y, int w, int h,
                                            uint8_t *dst, bool swap) {
  const size_t row_bytes = (size_t)w * 2u;
  for (int r = 0; r < h; r++, dst += row_bytes) {
    const uint16_t *row = fb + (size_t)(y + r) * stride + x;
    disp_copy_px_bytes(dst, (const uint8_t *)row, (size_t)w, swap);
  }
}

#endif  // DISPLAY_CLIP_H
