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
  g->dirty = true;
  return g;
}

void chargrid_free(chargrid_t *g) {
  if (g) CG_FREE(g);
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
