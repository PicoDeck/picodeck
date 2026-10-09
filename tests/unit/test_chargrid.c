// Host unit tests for the chargrid core (src/os/chargrid*.c).
#include "check.h"
#include "chargrid.h"
#include "font_6x8.h"

#include <stdlib.h>
#include <string.h>

static void test_sizes(void) {
  CHECK(chargrid_check_size(1, 1, 1, 1) == NULL);
  CHECK(chargrid_check_size(1024, 64, 16, 32) == NULL);
  CHECK(chargrid_check_size(256, 256, 6, 8) == NULL);  // exactly 65536 cells
  CHECK(chargrid_check_size(0, 1, 6, 8) != NULL);
  CHECK(chargrid_check_size(1, 1025, 6, 8) != NULL);
  CHECK(chargrid_check_size(257, 256, 6, 8) != NULL);
  CHECK(chargrid_check_size(10, 10, 0, 8) != NULL);
  CHECK(chargrid_check_size(10, 10, 17, 8) != NULL);
  CHECK(chargrid_check_size(10, 10, 6, 33) != NULL);
  CHECK(chargrid_create(0, 1, 6, 8) == NULL);
}

static void test_create_defaults(void) {
  chargrid_t *g = chargrid_create(5, 3, 6, 8);
  CHECK(g != NULL);
  CHECK_EQ_INT(g->cols, 5);
  CHECK_EQ_INT(g->rows, 3);
  CHECK_EQ_INT(g->cell_w, 6);
  CHECK_EQ_INT(g->cell_h, 8);
  for (int i = 0; i < 15; i++) {
    CHECK_EQ_INT(g->glyph[i], 0);
    CHECK_EQ_INT(g->fg[i], CG_DEFAULT_FG);
    CHECK_EQ_INT(g->bg[i], CG_DEFAULT_BG);
  }
  CHECK(g->dirty);
  CHECK_EQ_INT(g->ramp_n, 0);
  CHECK_EQ_INT(g->cramp_n, 0);
  CHECK_EQ_U32(g->palette[9], 0xF800);
  chargrid_free(g);
  chargrid_free(NULL);
}

static void test_palette(void) {
  uint16_t p[256];
  chargrid_default_palette(p);
  CHECK_EQ_U32(p[0], 0x0000);
  CHECK_EQ_U32(p[1], 0x8000);    // maroon 800000
  CHECK_EQ_U32(p[7], 0xC618);    // silver c0c0c0
  CHECK_EQ_U32(p[9], 0xF800);    // red
  CHECK_EQ_U32(p[15], 0xFFFF);   // white
  CHECK_EQ_U32(p[16], 0x0000);   // cube (0,0,0)
  CHECK_EQ_U32(p[21], 0x001F);   // cube (0,0,5) = 0000ff
  CHECK_EQ_U32(p[196], 0xF800);  // cube (5,0,0)
  CHECK_EQ_U32(p[231], 0xFFFF);  // cube (5,5,5)
  CHECK_EQ_U32(p[232], 0x0841);  // grey 080808
  CHECK_EQ_U32(p[255], 0xEF7D);  // grey eeeeee
}

static void test_set_get_fill(void) {
  chargrid_t *g = chargrid_create(4, 3, 6, 8);
  uint8_t c, f, b;
  g->dirty = false;
  chargrid_set(g, 1, 2, 'A', 3, 4);
  CHECK(g->dirty);
  CHECK(chargrid_get(g, 1, 2, &c, &f, &b));
  CHECK_EQ_INT(c, 'A');
  CHECK_EQ_INT(f, 3);
  CHECK_EQ_INT(b, 4);
  chargrid_set(g, 1, 2, 'B', CG_KEEP, CG_KEEP);
  chargrid_get(g, 1, 2, &c, &f, &b);
  CHECK_EQ_INT(c, 'B');
  CHECK_EQ_INT(f, 3);
  CHECK_EQ_INT(b, 4);
  chargrid_set(g, -1, 0, 'X', 1, 1);  // outside: dropped
  chargrid_set(g, 4, 0, 'X', 1, 1);
  chargrid_set(g, 0, 3, 'X', 1, 1);
  CHECK(!chargrid_get(g, 4, 0, &c, &f, &b));
  CHECK_EQ_INT(c, 0);
  CHECK_EQ_INT(f, 0);
  CHECK_EQ_INT(b, 0);
  chargrid_fill(g, -2, -2, 4, 4, '#', CG_KEEP, 9);  // lands on (0..1, 0..1)
  chargrid_get(g, 0, 0, &c, &f, &b);
  CHECK_EQ_INT(c, '#');
  CHECK_EQ_INT(f, CG_DEFAULT_FG);
  CHECK_EQ_INT(b, 9);
  chargrid_get(g, 1, 1, &c, &f, &b);
  CHECK_EQ_INT(c, '#');
  chargrid_get(g, 2, 0, &c, &f, &b);
  CHECK_EQ_INT(c, 0);
  chargrid_fill(g, 0, 0, 1 << 24, 1 << 24, '.', 1, 2);  // huge: the whole grid
  for (int i = 0; i < 12; i++) {
    CHECK_EQ_INT(g->glyph[i], '.');
    CHECK_EQ_INT(g->fg[i], 1);
    CHECK_EQ_INT(g->bg[i], 2);
  }
  chargrid_fill(g, 0, 0, 0, 3, 'Z', 0, 0);   // empty
  chargrid_fill(g, 0, 0, -5, 3, 'Z', 0, 0);  // negative
  chargrid_fill(g, -2000000000, -2000000000, 5, 5, 'Z', 0, 0);  // far away
  CHECK_EQ_INT(g->glyph[0], '.');
  chargrid_free(g);
}

static void test_clip_span(void) {
  chargrid_t *g = chargrid_create(10, 2, 6, 8);
  int first = -1;
  CHECK_EQ_INT(chargrid_clip_span(g, 0, 0, 10, &first), 10);
  CHECK_EQ_INT(first, 0);
  CHECK_EQ_INT(chargrid_clip_span(g, -3, 1, 5, &first), 2);
  CHECK_EQ_INT(first, 3);
  CHECK_EQ_INT(chargrid_clip_span(g, 8, 0, 5, &first), 2);
  CHECK_EQ_INT(first, 0);
  CHECK_EQ_INT(chargrid_clip_span(g, 0, 2, 5, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, 0, -1, 5, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, 10, 0, 5, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, -5, 0, 5, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, 0, 0, 0, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, -2000000000, 0, 2000000010, &first), 10);
  CHECK_EQ_INT(first, 2000000000);
  chargrid_free(g);
}

static const pc_font_t k_font6x8 = {0x20, 0x7E, 8, 6, 1, NULL, &font_6x8[0][0], NULL};

static int popcount16(uint32_t v) {
  int n = 0;
  for (; v; v >>= 1) n += (int)(v & 1u);
  return n;
}

static int shape_pixels(int code, int cw, int ch) {
  uint16_t r[CG_MAX_CELL_H];
  chargrid_shape(code, cw, ch, r);
  int n = 0;
  for (int i = 0; i < ch; i++) n += popcount16(r[i]);
  return n;
}

static void test_shapes(void) {
  uint16_t r[CG_MAX_CELL_H];
  CHECK(!chargrid_shape(0x00, 6, 8, r));
  CHECK(!chargrid_shape(0x1C, 6, 8, r));
  CHECK(!chargrid_shape('A', 6, 8, r));
  CHECK(!chargrid_shape(CG_DOT1, 17, 8, r));
  CHECK_EQ_U32(r[0], 0);
  // dots at 4x4: 1 pixel, 2x2, a plus, a round square
  CHECK(chargrid_shape(CG_DOT1, 4, 4, r));
  CHECK_EQ_U32(r[0], 0); CHECK_EQ_U32(r[1], 0x2); CHECK_EQ_U32(r[2], 0); CHECK_EQ_U32(r[3], 0);
  chargrid_shape(CG_DOT2, 4, 4, r);
  CHECK_EQ_U32(r[0], 0); CHECK_EQ_U32(r[1], 0x6); CHECK_EQ_U32(r[2], 0x6); CHECK_EQ_U32(r[3], 0);
  chargrid_shape(CG_DOT3, 4, 4, r);
  CHECK_EQ_U32(r[0], 0x2); CHECK_EQ_U32(r[1], 0x7); CHECK_EQ_U32(r[2], 0x2); CHECK_EQ_U32(r[3], 0);
  chargrid_shape(CG_DOT4, 4, 4, r);
  CHECK_EQ_U32(r[0], 0x6); CHECK_EQ_U32(r[1], 0xF); CHECK_EQ_U32(r[2], 0xF); CHECK_EQ_U32(r[3], 0x6);
  static const int sizes[][2] = {{4, 4}, {5, 10}, {6, 8}, {8, 12}, {16, 32}};
  for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
    const int cw = sizes[s][0], ch = sizes[s][1];
    const uint32_t full = (1u << cw) - 1u;
    int prev = 0;
    for (int code = CG_DOT1; code <= CG_DOT4; code++) {  // dots always grow
      const int n = shape_pixels(code, cw, ch);
      CHECK(n > prev);
      prev = n;
    }
    uint16_t t[CG_MAX_CELL_H], b[CG_MAX_CELL_H], l[CG_MAX_CELL_H], rt[CG_MAX_CELL_H];
    chargrid_shape(CG_HALF_TOP, cw, ch, t);
    chargrid_shape(CG_HALF_BOTTOM, cw, ch, b);
    chargrid_shape(CG_HALF_LEFT, cw, ch, l);
    chargrid_shape(CG_HALF_RIGHT, cw, ch, rt);
    for (int i = 0; i < ch; i++) {  // the halves partition the cell
      CHECK_EQ_U32((uint32_t)(t[i] | b[i]), full);
      CHECK_EQ_U32((uint32_t)(t[i] & b[i]), 0);
      CHECK_EQ_U32((uint32_t)(l[i] | rt[i]), full);
      CHECK_EQ_U32((uint32_t)(l[i] & rt[i]), 0);
    }
    CHECK_EQ_INT(shape_pixels(CG_SHADE4, cw, ch), cw * ch);
  }
  // shades: exactly 25/50/75% of a cell whose sides are multiples of 4
  CHECK_EQ_INT(shape_pixels(CG_SHADE1, 4, 4), 4);
  CHECK_EQ_INT(shape_pixels(CG_SHADE2, 4, 4), 8);
  CHECK_EQ_INT(shape_pixels(CG_SHADE3, 4, 4), 12);
  CHECK_EQ_INT(shape_pixels(CG_SHADE2, 8, 8), 32);
  // box drawing at 6x8: centre column 2, centre row 3
  chargrid_shape(CG_BOX_H, 6, 8, r);
  for (int i = 0; i < 8; i++) CHECK_EQ_U32(r[i], i == 3 ? 0x3F : 0);
  chargrid_shape(CG_BOX_V, 6, 8, r);
  for (int i = 0; i < 8; i++) CHECK_EQ_U32(r[i], 0x04);
  chargrid_shape(CG_BOX_TL, 6, 8, r);
  for (int i = 0; i < 8; i++) CHECK_EQ_U32(r[i], i < 3 ? 0 : i == 3 ? 0x3C : 0x04);
  chargrid_shape(CG_BOX_BR, 6, 8, r);
  for (int i = 0; i < 8; i++) CHECK_EQ_U32(r[i], i < 3 ? 0x04 : i == 3 ? 0x07 : 0);
  chargrid_shape(CG_BOX_X, 6, 8, r);
  for (int i = 0; i < 8; i++) CHECK_EQ_U32(r[i], i == 3 ? 0x3F : 0x04);
  // triangles at 6x8
  chargrid_shape(CG_TRI_UP, 6, 8, r);
  CHECK_EQ_U32(r[0], 0x04); CHECK_EQ_U32(r[7], 0x3F);
  chargrid_shape(CG_TRI_DOWN, 6, 8, r);
  CHECK_EQ_U32(r[0], 0x3F); CHECK_EQ_U32(r[7], 0x04);
  chargrid_shape(CG_TRI_RIGHT, 6, 8, r);
  for (int i = 0; i < 8; i++) CHECK(r[i] & 0x01);
  CHECK(r[3] & 0x20); CHECK(!(r[0] & 0x20));
  chargrid_shape(CG_TRI_LEFT, 6, 8, r);
  for (int i = 0; i < 8; i++) CHECK(r[i] & 0x20);
  CHECK(r[3] & 0x01); CHECK(!(r[0] & 0x01));
}

static uint16_t font_row_mask(char c, int r) {  // MSB-left font byte -> bit c = column c
  uint16_t m = 0;
  const uint8_t bits = font_6x8[c - 0x20][r];
  for (int col = 0; col < 6; col++)
    if (bits & (0x80 >> col)) m |= (uint16_t)(1u << col);
  return m;
}

static void test_glyph_cache(void) {
  chargrid_t *g = chargrid_create(2, 2, 6, 8);
  uint16_t want[CG_MAX_CELL_H];
  chargrid_shape(CG_DOT3, 6, 8, want);  // shapes exist from create
  for (int r = 0; r < 8; r++) CHECK_EQ_U32(g->masks[CG_DOT3 * 8 + r], want[r]);
  for (int r = 0; r < 8; r++) {  // no font: 0x20+ are empty, as are 0 and 0x1C
    CHECK_EQ_U32(g->masks['A' * 8 + r], 0);
    CHECK_EQ_U32(g->masks[0x00 * 8 + r], 0);
    CHECK_EQ_U32(g->masks[0x1C * 8 + r], 0);
  }
  g->dirty = false;
  chargrid_set_font(g, &k_font6x8);
  CHECK(g->dirty);
  int lit = 0;
  for (int r = 0; r < 8; r++) {
    CHECK_EQ_U32(g->masks['A' * 8 + r], font_row_mask('A', r));
    lit += popcount16(g->masks['A' * 8 + r]);
  }
  CHECK(lit > 0);
  // a code the font lacks: the hollow box, rows 0..6 x cols 0..4
  CHECK_EQ_U32(g->masks[0x80 * 8 + 0], 0x1F);
  CHECK_EQ_U32(g->masks[0x80 * 8 + 3], 0x11);
  CHECK_EQ_U32(g->masks[0x80 * 8 + 6], 0x1F);
  CHECK_EQ_U32(g->masks[0x80 * 8 + 7], 0x00);
  chargrid_free(g);
  // centred in a larger cell, cropped in a smaller one
  chargrid_t *big = chargrid_create(1, 1, 8, 12), *small = chargrid_create(1, 1, 4, 4);
  chargrid_set_font(big, &k_font6x8);
  chargrid_set_font(small, &k_font6x8);
  for (int r = 0; r < 8; r++)
    CHECK_EQ_U32(big->masks['A' * 12 + r + 2], (uint32_t)(font_row_mask('A', r) << 1));
  CHECK_EQ_U32(big->masks['A' * 12 + 0], 0);
  for (int r = 0; r < 4; r++)
    CHECK_EQ_U32(small->masks['A' * 4 + r], (uint32_t)((font_row_mask('A', r + 2) >> 1) & 0xF));
  chargrid_free(big);
  chargrid_free(small);
}

static void test_define_glyph(void) {
  chargrid_t *g = chargrid_create(1, 1, 6, 8);
  const uint16_t rows[8] = {0x30, 0x3F, 0, 0, 0, 0, 0xFFC0, 0x01};  // bit 5 = leftmost
  chargrid_define_glyph(g, 'A', rows);
  CHECK_EQ_U32(g->masks['A' * 8 + 0], 0x03);  // the two leftmost pixels
  CHECK_EQ_U32(g->masks['A' * 8 + 1], 0x3F);
  CHECK_EQ_U32(g->masks['A' * 8 + 6], 0x00);  // bits beyond the cell are ignored
  CHECK_EQ_U32(g->masks['A' * 8 + 7], 0x20);  // the rightmost pixel
  chargrid_set_font(g, &k_font6x8);           // a defined code survives setFont
  CHECK_EQ_U32(g->masks['A' * 8 + 0], 0x03);
  CHECK_EQ_U32(g->masks['B' * 8 + 1], font_row_mask('B', 1));
  chargrid_define_glyph(g, 0, rows);          // out of range: ignored
  chargrid_define_glyph(g, 256, rows);
  CHECK_EQ_U32(g->masks[0], 0);
  chargrid_free(g);
}

int main(void) {
  test_sizes();
  test_create_defaults();
  test_palette();
  test_set_get_fill();
  test_clip_span();
  test_shapes();
  test_glyph_cache();
  test_define_glyph();
  return check_report("test_chargrid");
}
