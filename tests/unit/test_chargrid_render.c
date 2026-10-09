// Host unit tests for src/os/chargrid_render.c: the fast pair path and the
// per-pixel path against a reference that looks every pixel up from scratch.
#include "check.h"
#include "chargrid.h"

#include <string.h>

#define BW 64
#define BH 48
#define SENTINEL 0xDEADu

static uint32_t s_words[((BW + 1) * BH + 4) / 2];  // 4-byte aligned pixel buffer
static int s_stride = BW;                          // row stride in pixels (BW, or BW + 1: odd)
static uint32_t s_seed = 12345u;
static uint32_t rnd(void) {
  s_seed = s_seed * 1103515245u + 12345u;
  return s_seed >> 8;
}

static void fill_random(chargrid_t *g) {
  const size_t n = (size_t)g->cols * (size_t)g->rows;
  for (size_t i = 0; i < n; i++) {
    g->glyph[i] = (uint8_t)rnd();
    g->fg[i] = (uint8_t)rnd();
    g->bg[i] = (uint8_t)rnd();
  }
  for (size_t i = 0; i < (size_t)256 * (size_t)g->cell_h; i++)
    g->masks[i] = (uint16_t)(rnd() & ((1u << g->cell_w) - 1u));
  for (int i = 0; i < 256; i++) g->palette[i] = (uint16_t)rnd();
}

static uint16_t ref_pixel(const chargrid_t *g, const chargrid_target_t *t, int px, int py, int sx,
                          int sy, int w, int h, int x, int y) {
  if (x < t->clip_x0 || x > t->clip_x1 || y < t->clip_y0 || y > t->clip_y1) return SENTINEL;
  const long ox = (long)px - (long)sx * g->cell_w, oy = (long)py - (long)sy * g->cell_h;
  const long dx = x - ox, dy = y - oy;
  if (dx < 0 || dy < 0) return SENTINEL;
  const long cx = dx / g->cell_w, cy = dy / g->cell_h;
  if (cx < sx || cx >= (long)sx + w || cy < sy || cy >= (long)sy + h || cx >= g->cols ||
      cy >= g->rows)
    return SENTINEL;
  const size_t i = (size_t)cy * (size_t)g->cols + (size_t)cx;
  const int k = (int)(dx % g->cell_w), r = (int)(dy % g->cell_h);
  const bool on = (g->masks[(size_t)g->glyph[i] * g->cell_h + r] >> k) & 1u;
  const uint16_t c = g->palette[on ? g->fg[i] : g->bg[i]];
  return t->swap ? (uint16_t)((c >> 8) | (c << 8)) : c;
}

// Renders and compares every pixel of the buffer; returns the cells drawn.
static int check_case(int cw, int ch, int cols, int rows, uint16_t *fb, bool swap, int cx0,
                      int cy0, int cx1, int cy1, int px, int py, int sx, int sy, int w, int h) {
  chargrid_t *g = chargrid_create(cols, rows, cw, ch);
  fill_random(g);
  const chargrid_target_t t = {fb, s_stride, cx0, cy0, cx1, cy1, swap};
  for (int i = 0; i < s_stride * BH; i++) fb[i] = SENTINEL;
  const int n = chargrid_render(g, &t, px, py, sx, sy, w, h);
  int bad = 0;
  for (int y = 0; y < BH; y++)
    for (int x = 0; x < BW; x++)
      if (fb[y * s_stride + x] != ref_pixel(g, &t, px, py, sx, sy, w, h, x, y)) bad++;
  CHECK_EQ_INT(bad, 0);
  CHECK(!g->dirty);
  chargrid_free(g);
  return n;
}

static uint16_t *aligned(void) { return (uint16_t *)s_words; }

static void test_render_sizes(void) {
  static const int sizes[][2] = {{4, 4}, {5, 8}, {6, 8}, {8, 12}, {16, 3}, {1, 1}, {2, 2}, {10, 5},
                                 {3, 5}, {7, 9}, {9, 4}, {11, 7}, {13, 2}, {15, 6}, {1, 3}};
  for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
    const int cw = sizes[s][0], ch = sizes[s][1];
    const int cols = BW / cw + 1, rows = BH / ch + 1;
    for (int swap = 0; swap < 2; swap++) {
      check_case(cw, ch, cols, rows, aligned(), swap, 0, 0, BW - 1, BH - 1, 0, 0, 0, 0, cols, rows);
      check_case(cw, ch, cols, rows, aligned(), swap, 0, 0, BW - 1, BH - 1, 3, 1, 0, 0, cols, rows);
    }
  }
  CHECK_EQ_INT(check_case(6, 8, 8, 5, aligned(), false, 0, 0, BW - 1, BH - 1, 0, 0, 0, 0, 8, 5), 40);
}

static void test_render_clip(void) {
  for (int cw = 1; cw <= 9; cw++) {  // cells cut on every side by the clip rect
    check_case(cw, 8, 20, 10, aligned(), true, 5, 3, 40, 30, 2, 1, 0, 0, 20, 10);
    check_case(cw, 8, 20, 10, aligned(), false, 7, 9, 7, 9, 0, 0, 0, 0, 20, 10);  // one pixel
  }
  CHECK_EQ_INT(check_case(6, 8, 4, 4, aligned(), false, 10, 10, 9, 20, 0, 0, 0, 0, 4, 4), 0);
}

static void test_render_window(void) {
  check_case(6, 8, 12, 9, aligned(), false, 0, 0, BW - 1, BH - 1, 4, 2, 2, 1, 3, 2);
  check_case(6, 8, 12, 9, aligned(), true, 0, 0, BW - 1, BH - 1, 0, 0, -2, -1, 100, 100);
  check_case(4, 4, 30, 30, aligned(), false, 0, 0, BW - 1, BH - 1, -7, -5, 0, 0, 30, 30);
  CHECK_EQ_INT(check_case(6, 8, 4, 4, aligned(), false, 0, 0, BW - 1, BH - 1, 0, 0, 0, 0, 0, 4), 0);
}

static void test_render_far(void) {  // far off-screen: nothing drawn, no overflow
  CHECK_EQ_INT(check_case(6, 8, 50, 50, aligned(), false, 0, 0, BW - 1, BH - 1, 1000000, 0, 0, 0, 50, 50), 0);
  CHECK_EQ_INT(check_case(6, 8, 50, 50, aligned(), false, 0, 0, BW - 1, BH - 1, -1000000, -1000000, 0, 0, 50, 50), 0);
  CHECK_EQ_INT(check_case(4, 4, 1024, 64, aligned(), false, 0, 0, BW - 1, BH - 1, -16000000, 0, 0, 0, 1024, 64), 0);
}

static void test_render_misaligned(void) {  // a 2-byte aligned buffer: unaligned pair stores
  uint16_t *odd = (uint16_t *)s_words + 1;
  check_case(6, 8, 12, 7, odd, false, 0, 0, BW - 1, BH - 2, 0, 0, 0, 0, 12, 7);
  check_case(4, 4, 16, 12, odd, true, 0, 0, BW - 1, BH - 2, 2, 2, 0, 0, 16, 12);
  check_case(6, 8, 12, 5, aligned(), false, 0, 0, BW - 1, BH - 1, 1, 1, 0, 0, 12, 5);  // odd x
  check_case(5, 8, 12, 5, odd, false, 0, 0, BW - 1, BH - 2, 0, 0, 0, 0, 12, 5);
  check_case(7, 3, 9, 15, odd, true, 0, 0, BW - 1, BH - 2, 1, 2, 0, 0, 9, 15);
  check_case(5, 8, 12, 5, aligned(), false, 0, 0, BW - 1, BH - 1, 1, 1, 0, 0, 12, 5);  // odd x
}

static void test_render_odd_stride(void) {  // an odd row stride: rows start on odd halfwords
  s_stride = BW + 1;
  check_case(6, 8, 10, 5, aligned(), false, 0, 0, BW - 1, BH - 1, 0, 0, 0, 0, 10, 5);
  check_case(4, 4, 16, 12, aligned(), true, 0, 0, BW - 1, BH - 1, 2, 1, 0, 0, 16, 12);
  check_case(8, 12, 8, 4, (uint16_t *)s_words + 1, false, 0, 0, BW - 1, BH - 1, 1, 0, 0, 0, 8, 4);
  check_case(5, 8, 12, 5, aligned(), false, 0, 0, BW - 1, BH - 1, 0, 0, 0, 0, 12, 5);
  check_case(3, 4, 20, 12, (uint16_t *)s_words + 1, true, 0, 0, BW - 1, BH - 1, 2, 1, 0, 0, 20, 12);
  s_stride = BW;
}

int main(void) {
  test_render_sizes();
  test_render_clip();
  test_render_window();
  test_render_far();
  test_render_misaligned();
  test_render_odd_stride();
  return check_report("test_chargrid_render");
}
