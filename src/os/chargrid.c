// The chargrid model (chargrid.h). Pure C; built at -O2 with
// -ffp-contract=off on the device.
#include "chargrid.h"

#include <string.h>

#ifndef CG_MALLOC
#include "umm_malloc.h"
#define CG_MALLOC umm_malloc
#define CG_FREE umm_free
#else
#include <stdlib.h>  // host builds define CG_MALLOC=malloc
#endif

const char *chargrid_check_size(int cols, int rows, int cell_w, int cell_h) {
  if (cols < 1 || rows < 1 || cols > CG_MAX_DIM || rows > CG_MAX_DIM)
    return "cols and rows must be 1-1024";
  if ((long)cols * (long)rows > CG_MAX_CELLS) return "cols * rows must be at most 65536";
  if (cell_w < 1 || cell_w > CG_MAX_CELL_W) return "cellW must be 1-16";
  if (cell_h < 1 || cell_h > CG_MAX_CELL_H) return "cellH must be 1-32";
  return NULL;
}

static uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void chargrid_default_palette(uint16_t pal[256]) {
  static const uint32_t k_base[16] = {
      0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
      0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
  static const uint8_t k_level[6] = {0, 95, 135, 175, 215, 255};
  for (int i = 0; i < 16; i++)
    pal[i] = rgb565((int)(k_base[i] >> 16), (int)(k_base[i] >> 8) & 0xFF,
                    (int)k_base[i] & 0xFF);
  for (int r = 0; r < 6; r++)
    for (int g = 0; g < 6; g++)
      for (int b = 0; b < 6; b++)
        pal[16 + 36 * r + 6 * g + b] = rgb565(k_level[r], k_level[g], k_level[b]);
  for (int i = 0; i < 24; i++) pal[232 + i] = rgb565(8 + 10 * i, 8 + 10 * i, 8 + 10 * i);
}

chargrid_t *chargrid_create(int cols, int rows, int cell_w, int cell_h) {
  if (chargrid_check_size(cols, rows, cell_w, cell_h)) return NULL;
  const size_t n = (size_t)cols * (size_t)rows;
  const size_t masks = (size_t)256 * (size_t)cell_h * sizeof(uint16_t);
  const size_t bytes = sizeof(chargrid_t) + masks + 3 * n;
  uint8_t *blk = (uint8_t *)CG_MALLOC(bytes);
  if (!blk) return NULL;
  memset(blk, 0, bytes);
  chargrid_t *g = (chargrid_t *)blk;
  g->cols = cols;
  g->rows = rows;
  g->cell_w = cell_w;
  g->cell_h = cell_h;
  g->masks = (uint16_t *)(blk + sizeof(chargrid_t));
  g->glyph = blk + sizeof(chargrid_t) + masks;
  g->fg = g->glyph + n;
  g->bg = g->fg + n;
  memset(g->fg, CG_DEFAULT_FG, n);
  chargrid_default_palette(g->palette);
  chargrid_set_font(g, NULL);
  g->dirty = true;
  return g;
}

void chargrid_free(chargrid_t *g) {
  if (!g) return;
  if (g->nearest) CG_FREE(g->nearest);
  CG_FREE(g);
}

static inline bool in_grid(const chargrid_t *g, int x, int y) {
  return x >= 0 && y >= 0 && x < g->cols && y < g->rows;
}

static inline size_t cell_index(const chargrid_t *g, int x, int y) {
  return (size_t)y * (size_t)g->cols + (size_t)x;
}

void chargrid_set(chargrid_t *g, int x, int y, int glyph, int fg, int bg) {
  if (!in_grid(g, x, y)) return;
  const size_t i = cell_index(g, x, y);
  if (glyph >= 0) g->glyph[i] = (uint8_t)glyph;
  if (fg >= 0) g->fg[i] = (uint8_t)fg;
  if (bg >= 0) g->bg[i] = (uint8_t)bg;
  g->dirty = true;
}

bool chargrid_get(const chargrid_t *g, int x, int y, uint8_t *glyph, uint8_t *fg, uint8_t *bg) {
  if (!in_grid(g, x, y)) {
    *glyph = *fg = *bg = 0;
    return false;
  }
  const size_t i = cell_index(g, x, y);
  *glyph = g->glyph[i];
  *fg = g->fg[i];
  *bg = g->bg[i];
  return true;
}

int chargrid_clip_span(const chargrid_t *g, int x, int y, int n, int *first) {
  *first = 0;
  if (y < 0 || y >= g->rows || n <= 0) return 0;
  const int64_t lo = x < 0 ? 0 : x;
  int64_t hi = (int64_t)x + n;
  if (hi > g->cols) hi = g->cols;
  if (hi <= lo) return 0;
  *first = (int)(lo - x);
  return (int)(hi - lo);
}

void chargrid_fill(chargrid_t *g, int x, int y, int w, int h, int glyph, int fg, int bg) {
  if (w <= 0 || h <= 0) return;
  const int64_t y0 = y < 0 ? 0 : y;
  int64_t y1 = (int64_t)y + h;
  if (y1 > g->rows) y1 = g->rows;
  for (int64_t r = y0; r < y1; r++) {
    int first;
    const int n = chargrid_clip_span(g, x, (int)r, w, &first);
    if (n <= 0) return;  // the same columns on every row
    const size_t i = cell_index(g, x + first, (int)r);
    if (glyph >= 0) memset(g->glyph + i, glyph, (size_t)n);
    if (fg >= 0) memset(g->fg + i, fg, (size_t)n);
    if (bg >= 0) memset(g->bg + i, bg, (size_t)n);
  }
  g->dirty = true;
}

// Glyph `code` of font f into rows[0..ch-1], centred (cropped when the font
// is larger than the cell). Codes outside the font are its hollow box, as
// font_render draws them.
static void mask_from_font(const pc_font_t *f, int code, int cw, int ch, uint16_t *rows) {
  for (int r = 0; r < ch; r++) rows[r] = 0;
  const int fw = f->max_width, fh = f->height;
  const int ox = (cw - fw) / 2, oy = (ch - fh) / 2;
  const bool have = code >= f->first && code <= f->last;
  const uint8_t *bm =
      have ? f->bitmaps + (size_t)(code - f->first) * (size_t)fh * (size_t)f->stride : NULL;
  for (int gr = 0; gr < fh; gr++) {
    const int r = gr + oy;
    if (r < 0 || r >= ch) continue;
    for (int gc = 0; gc < fw; gc++) {
      const int c = gc + ox;
      if (c < 0 || c >= cw) continue;
      bool on;
      if (bm)
        on = (bm[(size_t)gr * f->stride + (size_t)(gc >> 3)] & (0x80 >> (gc & 7))) != 0;
      else
        on = gr < fh - 1 && gc < fw - 1 && (gr == 0 || gr == fh - 2 || gc == 0 || gc == fw - 2);
      if (on) rows[r] |= (uint16_t)(1u << c);
    }
  }
}

static inline bool is_defined(const chargrid_t *g, int code) {
  return (g->defined[code >> 3] >> (code & 7)) & 1u;
}

void chargrid_set_font(chargrid_t *g, const pc_font_t *f) {
  for (int code = 0; code < 256; code++) {
    if (is_defined(g, code)) continue;
    uint16_t *rows = g->masks + (size_t)code * (size_t)g->cell_h;
    if (code >= CG_DOT1 && code <= CG_SHAPE_LAST)
      chargrid_shape(code, g->cell_w, g->cell_h, rows);
    else if (code >= 0x20 && f)
      mask_from_font(f, code, g->cell_w, g->cell_h, rows);
    else
      for (int r = 0; r < g->cell_h; r++) rows[r] = 0;
  }
  g->dirty = true;
}

void chargrid_define_glyph(chargrid_t *g, int code, const uint16_t *rows) {
  if (code < 1 || code > 255) return;
  const int cw = g->cell_w;
  uint16_t *dst = g->masks + (size_t)code * (size_t)g->cell_h;
  for (int r = 0; r < g->cell_h; r++) {
    uint16_t m = 0;
    for (int c = 0; c < cw; c++)
      if (rows[r] & (1u << (cw - 1 - c))) m |= (uint16_t)(1u << c);
    dst[r] = m;
  }
  g->defined[code >> 3] |= (uint8_t)(1u << (code & 7));
  g->dirty = true;
}

const float chargrid_bayer_off[16] = {
    0 / 16.0f - 0.47f,  8 / 16.0f - 0.47f,  2 / 16.0f - 0.47f,  10 / 16.0f - 0.47f,
    12 / 16.0f - 0.47f, 4 / 16.0f - 0.47f,  14 / 16.0f - 0.47f, 6 / 16.0f - 0.47f,
    3 / 16.0f - 0.47f,  11 / 16.0f - 0.47f, 1 / 16.0f - 0.47f,  9 / 16.0f - 0.47f,
    15 / 16.0f - 0.47f, 7 / 16.0f - 0.47f,  13 / 16.0f - 0.47f, 5 / 16.0f - 0.47f};

static void set_any_ramp(uint8_t *dst, uint16_t *n_out, bool *d_out, const uint8_t *src, int n,
                         bool dither) {
  if (n < 0 || !src) n = 0;
  if (n > CG_RAMP_MAX) n = CG_RAMP_MAX;
  if (n) memcpy(dst, src, (size_t)n);
  *n_out = (uint16_t)n;
  *d_out = dither;
}

void chargrid_set_ramp(chargrid_t *g, const uint8_t *codes, int n, bool dither) {
  set_any_ramp(g->ramp, &g->ramp_n, &g->ramp_dither, codes, n, dither);
}

void chargrid_set_color_ramp(chargrid_t *g, const uint8_t *indices, int n, bool dither) {
  set_any_ramp(g->cramp, &g->cramp_n, &g->cramp_dither, indices, n, dither);
}

int chargrid_ramp_step(float v, int n, bool dither, int x, int y) {
  if (n <= 1) return 0;
  return chargrid_ramp_index(
      v, (float)(n - 1), n - 1,
      dither ? chargrid_bayer_off[((unsigned)y & 3u) * 4u + ((unsigned)x & 3u)] : 0.0f);
}

void chargrid_shade(chargrid_t *g, int x, int y, float level, bool has_level, float tone,
                    bool has_tone) {
  if (!in_grid(g, x, y)) return;
  const size_t i = cell_index(g, x, y);
  if (has_level && g->ramp_n)
    g->glyph[i] = g->ramp[chargrid_ramp_step(level, g->ramp_n, g->ramp_dither, x, y)];
  if (has_tone && g->cramp_n)
    g->fg[i] = g->cramp[chargrid_ramp_step(tone, g->cramp_n, g->cramp_dither, x, y)];
  g->dirty = true;
}

void chargrid_blit(chargrid_t *dst, const chargrid_t *src, int sx, int sy, int w, int h,
                   int dx, int dy, int transparent) {
  if (w <= 0 || h <= 0) return;
  int64_t x0 = sx, y0 = sy, x1 = (int64_t)sx + w, y1 = (int64_t)sy + h;  // source, half-open
  const int64_t ox = (int64_t)dx - sx, oy = (int64_t)dy - sy;            // dst = src + o
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > src->cols) x1 = src->cols;
  if (y1 > src->rows) y1 = src->rows;
  if (x0 + ox < 0) x0 = -ox;
  if (y0 + oy < 0) y0 = -oy;
  if (x1 + ox > dst->cols) x1 = dst->cols - ox;
  if (y1 + oy > dst->rows) y1 = dst->rows - oy;
  if (x1 <= x0 || y1 <= y0) return;
  const size_t n = (size_t)(x1 - x0);
  const bool same = dst == src;
  const bool bottom_up = same && oy > 0;               // rows not yet read stay intact
  const bool right_to_left = same && oy == 0 && ox > 0;
  for (int64_t k = 0; k < y1 - y0; k++) {
    const int64_t yy = bottom_up ? y1 - 1 - k : y0 + k;
    const size_t si = (size_t)yy * (size_t)src->cols + (size_t)x0;
    const size_t di = (size_t)(yy + oy) * (size_t)dst->cols + (size_t)(x0 + ox);
    if (transparent < 0) {
      memmove(dst->glyph + di, src->glyph + si, n);
      memmove(dst->fg + di, src->fg + si, n);
      memmove(dst->bg + di, src->bg + si, n);
      continue;
    }
    for (size_t j = 0; j < n; j++) {
      const size_t c = right_to_left ? n - 1 - j : j;
      if (src->glyph[si + c] == (uint8_t)transparent) continue;
      dst->glyph[di + c] = src->glyph[si + c];
      dst->fg[di + c] = src->fg[si + c];
      dst->bg[di + c] = src->bg[si + c];
    }
  }
  dst->dirty = true;
}

void chargrid_scroll(chargrid_t *g, int dx, int dy, int glyph, int fg, int bg) {
  if (dx >= g->cols || dx <= -g->cols || dy >= g->rows || dy <= -g->rows) {
    chargrid_fill(g, 0, 0, g->cols, g->rows, glyph, fg, bg);
    return;
  }
  chargrid_blit(g, g, 0, 0, g->cols, g->rows, dx, dy, CG_KEEP);
  if (dx > 0) chargrid_fill(g, 0, 0, dx, g->rows, glyph, fg, bg);
  if (dx < 0) chargrid_fill(g, g->cols + dx, 0, -dx, g->rows, glyph, fg, bg);
  if (dy > 0) chargrid_fill(g, 0, 0, g->cols, dy, glyph, fg, bg);
  if (dy < 0) chargrid_fill(g, 0, g->rows + dy, g->cols, -dy, glyph, fg, bg);
  g->dirty = true;
}

// ── nearest palette entry ────────────────────────────────────────────────────
#define NEAR_LEVELS (1 << CG_NEAREST_BITS)
#define NEAR_N (NEAR_LEVELS * NEAR_LEVELS * NEAR_LEVELS)

static int near_level(float v) {
  if (!(v > 0.0f)) return 0;  // NaN too
  if (v >= 1.0f) return NEAR_LEVELS - 1;
  return (int)(v * (float)NEAR_LEVELS);
}

static uint8_t nearest_search(const chargrid_t *g, float r, float gr, float b) {
  int best = 0;
  float bd = 0.0f;
  for (int i = 0; i < 256; i++) {
    const uint16_t c = g->palette[i];
    const float dr = (float)(c >> 11) * (1.0f / 31.0f) - r;
    const float dg = (float)((c >> 5) & 63u) * (1.0f / 63.0f) - gr;
    const float db = (float)(c & 31u) * (1.0f / 31.0f) - b;
    const float d = 0.3f * dr * dr + 0.5f * dg * dg + 0.2f * db * db;
    if (i == 0 || d < bd) {
      bd = d;
      best = i;
    }
  }
  return (uint8_t)best;
}

uint8_t chargrid_nearest(chargrid_t *g, float r, float gr, float b) {
  const int qr = near_level(r), qg = near_level(gr), qb = near_level(b);
  const int key = (qr << (2 * CG_NEAREST_BITS)) | (qg << CG_NEAREST_BITS) | qb;
  if (!g->nearest && !g->nearest_failed) {
    g->nearest = (uint8_t *)CG_MALLOC(NEAR_N + NEAR_N / 8);
    if (g->nearest)
      memset(g->nearest + NEAR_N, 0, NEAR_N / 8);
    else
      g->nearest_failed = true;  // full searches, without a CG_MALLOC per cell
  }
  uint8_t *t = g->nearest;
  const uint8_t bit = (uint8_t)(1u << (key & 7));
  if (t && (t[NEAR_N + (key >> 3)] & bit)) return t[key];
  const float inv = 1.0f / (float)NEAR_LEVELS;  // search for the bucket's centre
  const uint8_t idx = nearest_search(g, ((float)qr + 0.5f) * inv, ((float)qg + 0.5f) * inv,
                                     ((float)qb + 0.5f) * inv);
  if (t) {
    t[key] = idx;
    t[NEAR_N + (key >> 3)] |= bit;
  }
  return idx;
}

void chargrid_palette_changed(chargrid_t *g) {
  if (g->nearest) memset(g->nearest + NEAR_N, 0, NEAR_N / 8);
  g->nearest_failed = false;  // the next chargrid_nearest tries to allocate again
  g->dirty = true;
}
