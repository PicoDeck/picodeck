// Renders a window of a chargrid into a 16-bit buffer (chargrid.h). Every
// per-cell value stays in registers: called from Lua this runs on the app's
// PSRAM stack, where a per-cell table cost 33-35% in P0
// (specs/2026-10-09-chargrid-p0-findings.md). Built at -O2.
#include "chargrid.h"

typedef uint32_t __attribute__((may_alias)) cg_word_t;

// Two pixels per 32-bit store. Mask bits b0 (left) and b1 (right) choose fg or
// bg in each half; the left pixel is the low half (little-endian).
static const uint32_t k_pairmask[4] = {0x00000000u, 0x0000FFFFu, 0xFFFF0000u, 0xFFFFFFFFu};

static inline uint16_t cg_color(const chargrid_t *g, uint8_t idx, bool swap) {
  const uint16_t c = g->palette[idx];
  return swap ? (uint16_t)((c >> 8) | (c << 8)) : c;
}

static inline int64_t floordiv(int64_t a, int64_t b) {  // b > 0
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}

#define PAIR(p) (bb ^ (xr & k_pairmask[(bits >> (2 * (p))) & 3u]))

// Cells c0..c1 of the row whose first plane index is `base`, all wholly
// inside the clip, starting at the 4-byte-aligned pixel dst.
#define DEFINE_FAST_ROW(NAME, W, STORES)                                          \
  static void NAME(const chargrid_t *g, size_t base, int c0, int c1, uint16_t *dst, \
                   int stride, bool swap) {                                       \
    const int ch = g->cell_h;                                                     \
    for (int c = c0; c <= c1; c++, dst += (W)) {                                  \
      const size_t i = base + (size_t)c;                                          \
      const uint32_t f = cg_color(g, g->fg[i], swap), b = cg_color(g, g->bg[i], swap); \
      const uint32_t bb = b | (b << 16), xr = (f ^ b) * 0x00010001u;              \
      const uint16_t *m = g->masks + (size_t)g->glyph[i] * (size_t)ch;            \
      cg_word_t *d = (cg_word_t *)dst;                                            \
      for (int r = 0; r < ch; r++, d += stride / 2) {                             \
        const uint32_t bits = m[r];                                               \
        STORES                                                                    \
      }                                                                           \
    }                                                                             \
  }

DEFINE_FAST_ROW(row_w4, 4, d[0] = PAIR(0); d[1] = PAIR(1);)
DEFINE_FAST_ROW(row_w6, 6, d[0] = PAIR(0); d[1] = PAIR(1); d[2] = PAIR(2);)
DEFINE_FAST_ROW(row_w8, 8, d[0] = PAIR(0); d[1] = PAIR(1); d[2] = PAIR(2); d[3] = PAIR(3);)

static void row_even(const chargrid_t *g, size_t base, int c0, int c1, uint16_t *dst, int stride,
                     bool swap) {
  const int cw = g->cell_w, ch = g->cell_h, pairs = cw / 2;
  for (int c = c0; c <= c1; c++, dst += cw) {
    const size_t i = base + (size_t)c;
    const uint32_t f = cg_color(g, g->fg[i], swap), b = cg_color(g, g->bg[i], swap);
    const uint32_t bb = b | (b << 16), xr = (f ^ b) * 0x00010001u;
    const uint16_t *m = g->masks + (size_t)g->glyph[i] * (size_t)ch;
    cg_word_t *d = (cg_word_t *)dst;
    for (int r = 0; r < ch; r++, d += stride / 2) {
      const uint32_t bits = m[r];
      for (int p = 0; p < pairs; p++) d[p] = PAIR(p);
    }
  }
}

typedef void (*fast_row_fn)(const chargrid_t *, size_t, int, int, uint16_t *, int, bool);

// One cell pixel by pixel, clipped: edge cells, odd widths, odd x.
static void cell_slow(const chargrid_t *g, size_t i, const chargrid_target_t *t, int64_t x,
                      int64_t y) {
  const int cw = g->cell_w, ch = g->cell_h;
  const uint16_t f = cg_color(g, g->fg[i], t->swap), b = cg_color(g, g->bg[i], t->swap);
  const uint16_t *m = g->masks + (size_t)g->glyph[i] * (size_t)ch;
  const int r0 = y < t->clip_y0 ? (int)(t->clip_y0 - y) : 0;
  const int r1 = y + ch - 1 > t->clip_y1 ? (int)(t->clip_y1 - y) : ch - 1;
  const int k0 = x < t->clip_x0 ? (int)(t->clip_x0 - x) : 0;
  const int k1 = x + cw - 1 > t->clip_x1 ? (int)(t->clip_x1 - x) : cw - 1;
  for (int r = r0; r <= r1; r++) {
    uint16_t *d = t->fb + (ptrdiff_t)(y + r) * t->stride + (ptrdiff_t)(x + k0);
    const uint32_t bits = m[r];
    for (int k = k0; k <= k1; k++) *d++ = ((bits >> k) & 1u) ? f : b;
  }
}

int chargrid_render(chargrid_t *g, const chargrid_target_t *t, int px, int py, int sx, int sy,
                    int w, int h) {
  g->dirty = false;
  if (w <= 0 || h <= 0 || t->clip_x1 < t->clip_x0 || t->clip_y1 < t->clip_y0) return 0;
  const int cw = g->cell_w, ch = g->cell_h;
  // Pixel of cell (0, 0): cell (sx, sy) lands on (px, py).
  const int64_t ox = (int64_t)px - (int64_t)sx * cw, oy = (int64_t)py - (int64_t)sy * ch;
  // The window, clipped to the grid and to the cells touching the clip rect.
  int64_t c0 = sx > 0 ? sx : 0, c1 = (int64_t)sx + w - 1;
  int64_t r0 = sy > 0 ? sy : 0, r1 = (int64_t)sy + h - 1;
  if (c1 > g->cols - 1) c1 = g->cols - 1;
  if (r1 > g->rows - 1) r1 = g->rows - 1;
  const int64_t kc0 = floordiv(t->clip_x0 - ox, cw), kc1 = floordiv(t->clip_x1 - ox, cw);
  const int64_t kr0 = floordiv(t->clip_y0 - oy, ch), kr1 = floordiv(t->clip_y1 - oy, ch);
  if (c0 < kc0) c0 = kc0;
  if (c1 > kc1) c1 = kc1;
  if (r0 < kr0) r0 = kr0;
  if (r1 > kr1) r1 = kr1;
  if (c1 < c0 || r1 < r0) return 0;
  // Cells wholly inside the clip horizontally take the pair path when the
  // width is even and their first pixel is 4-byte aligned.
  int64_t f0 = floordiv(t->clip_x0 - ox + cw - 1, cw), f1 = floordiv(t->clip_x1 - ox + 1, cw) - 1;
  if (f0 < c0) f0 = c0;
  if (f1 > c1) f1 = c1;
  const bool pairs = (cw & 1) == 0 && (t->stride & 1) == 0 && ((uintptr_t)t->fb & 3u) == 0 &&
                     ((ox + f0 * cw) & 1) == 0 && f0 <= f1;
  const fast_row_fn fast = cw == 4 ? row_w4 : cw == 6 ? row_w6 : cw == 8 ? row_w8 : row_even;
  for (int64_t r = r0; r <= r1; r++) {
    const int64_t y = oy + r * ch;
    const size_t base = (size_t)r * (size_t)g->cols;
    int64_t c = c0;
    if (pairs && y >= t->clip_y0 && y + ch - 1 <= t->clip_y1) {
      for (; c < f0; c++) cell_slow(g, base + (size_t)c, t, ox + c * cw, y);
      fast(g, base, (int)f0, (int)f1, t->fb + (ptrdiff_t)y * t->stride + (ptrdiff_t)(ox + f0 * cw),
           t->stride, t->swap);
      c = f1 + 1;
    }
    for (; c <= c1; c++) cell_slow(g, base + (size_t)c, t, ox + c * cw, y);
  }
  return (int)((c1 - c0 + 1) * (r1 - r0 + 1));
}
