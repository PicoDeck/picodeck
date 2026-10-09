// Host unit tests for the chargrid core (src/os/chargrid*.c).
#include "check.h"
#include "chargrid.h"
#include "font_6x8.h"

#include <math.h>
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

static int enc_utf8(uint32_t cp, char *o) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static void test_utf8_map(void) {
  static const struct { uint32_t cp; int code; } k[] = {
      {0x00B7, CG_DOT1}, {0x2022, CG_DOT2}, {0x25CF, CG_DOT3}, {0x2B24, CG_DOT4},
      {0x2591, CG_SHADE1}, {0x2592, CG_SHADE2}, {0x2593, CG_SHADE3}, {0x2588, CG_SHADE4},
      {0x2580, CG_HALF_TOP}, {0x2584, CG_HALF_BOTTOM}, {0x258C, CG_HALF_LEFT}, {0x2590, CG_HALF_RIGHT},
      {0x2500, CG_BOX_H}, {0x2501, CG_BOX_H}, {0x2550, CG_BOX_H},
      {0x2502, CG_BOX_V}, {0x2503, CG_BOX_V}, {0x2551, CG_BOX_V},
      {0x250C, CG_BOX_TL}, {0x250F, CG_BOX_TL}, {0x2554, CG_BOX_TL}, {0x256D, CG_BOX_TL},
      {0x2510, CG_BOX_TR}, {0x2513, CG_BOX_TR}, {0x2557, CG_BOX_TR}, {0x256E, CG_BOX_TR},
      {0x2514, CG_BOX_BL}, {0x2517, CG_BOX_BL}, {0x255A, CG_BOX_BL}, {0x2570, CG_BOX_BL},
      {0x2518, CG_BOX_BR}, {0x251B, CG_BOX_BR}, {0x255D, CG_BOX_BR}, {0x256F, CG_BOX_BR},
      {0x251C, CG_BOX_LT}, {0x2523, CG_BOX_LT}, {0x2560, CG_BOX_LT},
      {0x2524, CG_BOX_RT}, {0x252B, CG_BOX_RT}, {0x2563, CG_BOX_RT},
      {0x252C, CG_BOX_TT}, {0x2533, CG_BOX_TT}, {0x2566, CG_BOX_TT},
      {0x2534, CG_BOX_BT}, {0x253B, CG_BOX_BT}, {0x2569, CG_BOX_BT},
      {0x253C, CG_BOX_X}, {0x254B, CG_BOX_X}, {0x256C, CG_BOX_X},
      {0x25B2, CG_TRI_UP}, {0x25B6, CG_TRI_RIGHT}, {0x25BC, CG_TRI_DOWN}, {0x25C0, CG_TRI_LEFT},
      {'A', 'A'}, {'\n', '\n'}, {0x00A0, 0xA0}, {0x00E9, 0xE9}, {0x00FF, 0xFF},
      {0x0080, '?'}, {0x2603, '?'}, {0x1F600, '?'},
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
    char buf[4];
    const int n = enc_utf8(k[i].cp, buf);
    uint8_t code = 0;
    CHECK_EQ_INT(chargrid_utf8_code(buf, (size_t)n, &code), n);
    CHECK_EQ_INT(code, k[i].code);
  }
  uint8_t code = 7;
  CHECK_EQ_INT(chargrid_utf8_code("", 0, &code), 0);
}

static void test_utf8_invalid(void) {
  static const struct { const char *s; size_t len; int code; } k[] = {
      {"\x80", 1, 0x80},                  // a lone continuation byte
      {"\xC3", 1, 0xC3},                  // truncated
      {"\xC3\x28", 2, 0xC3},              // bad continuation
      {"\xC0\xAF", 2, 0xC0},              // overlong '/'
      {"\xE0\x80\xAF", 3, 0xE0},          // overlong, 3 bytes
      {"\xED\xA0\x80", 3, 0xED},          // a surrogate
      {"\xF4\x90\x80\x80", 4, 0xF4},      // above U+10FFFF
      {"\xF5\x80\x80\x80", 4, 0xF5},      // never a lead byte
      {"\xE2\x96", 2, 0xE2},              // cut short by len
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
    uint8_t code = 0;
    CHECK_EQ_INT(chargrid_utf8_code(k[i].s, k[i].len, &code), 1);
    CHECK_EQ_INT(code, k[i].code);
  }
}

static void row_str(const chargrid_t *g, int y, char *out) {  // 0 shows as '_'
  for (int x = 0; x < g->cols; x++) {
    const uint8_t c = g->glyph[(size_t)y * g->cols + x];
    out[x] = c ? (char)c : '_';
  }
  out[g->cols] = 0;
}

static void test_print_wrap(void) {
  chargrid_t *g = chargrid_create(10, 6, 6, 8);
  char s[16];
  CHECK_EQ_INT(chargrid_print(g, 1, 0, "Hi", 2, 4, CG_KEEP), 3);
  CHECK_EQ_INT(g->fg[1], 4);
  CHECK_EQ_INT(g->bg[1], CG_DEFAULT_BG);
  CHECK_EQ_INT(chargrid_print(g, 0, 1, "a\nbc", 4, CG_KEEP, CG_KEEP), 2);
  row_str(g, 2, s); CHECK_STR(s, "bc________");
  CHECK_EQ_INT(chargrid_print(g, 0, 3, "a\tb\x7f" "c", 5, CG_KEEP, CG_KEEP), 3);
  row_str(g, 3, s); CHECK_STR(s, "abc_______");
  CHECK_EQ_INT(chargrid_print(g, 8, 4, "xyz", 3, CG_KEEP, CG_KEEP), 11);  // clipped, still advances
  row_str(g, 4, s); CHECK_STR(s, "________xy");
  chargrid_fill(g, 0, 0, 10, 6, CG_EMPTY, CG_KEEP, CG_KEEP);
  const char *t = "hello world again";
  CHECK_EQ_INT(chargrid_print_wrapped(g, 0, 0, 10, 0, t, strlen(t), CG_KEEP, CG_KEEP), 3);
  row_str(g, 0, s); CHECK_STR(s, "hello ____");
  row_str(g, 1, s); CHECK_STR(s, "world ____");
  row_str(g, 2, s); CHECK_STR(s, "again_____");
  CHECK_EQ_INT(chargrid_print_wrapped(g, 0, 0, 10, 2, t, strlen(t), CG_KEEP, CG_KEEP), 2);
  chargrid_fill(g, 0, 0, 10, 6, CG_EMPTY, CG_KEEP, CG_KEEP);
  CHECK_EQ_INT(chargrid_print_wrapped(g, 0, 0, 4, 0, "ab\n\ncd\n", 7, CG_KEEP, CG_KEEP), 3);
  row_str(g, 0, s); CHECK_STR(s, "ab________");
  row_str(g, 1, s); CHECK_STR(s, "__________");
  row_str(g, 2, s); CHECK_STR(s, "cd________");
  // multi-byte characters take one cell each
  chargrid_fill(g, 0, 0, 10, 6, CG_EMPTY, CG_KEEP, CG_KEEP);
  const char *d = "\xC2\xB7\xC2\xB7\xC2\xB7 \xC2\xB7\xC2\xB7\xC2\xB7";  // "··· ···"
  CHECK_EQ_INT(chargrid_print_wrapped(g, 0, 0, 4, 0, d, strlen(d), CG_KEEP, CG_KEEP), 2);
  CHECK_EQ_INT(g->glyph[0], CG_DOT1);
  CHECK_EQ_INT(g->glyph[3], ' ');
  CHECK_EQ_INT(g->glyph[10 + 2], CG_DOT1);
  CHECK_EQ_INT(chargrid_print_wrapped(g, 0, 0, 0, 0, "x", 1, CG_KEEP, CG_KEEP), 0);
  CHECK_EQ_INT(chargrid_print_wrapped(g, 0, 0, 5, 0, "", 0, CG_KEEP, CG_KEEP), 0);
  chargrid_free(g);
}

static void test_box_stamp(void) {
  chargrid_t *g = chargrid_create(5, 4, 6, 8);
  char s[8];
  chargrid_fill(g, 0, 0, 5, 4, 'x', CG_KEEP, CG_KEEP);
  chargrid_box(g, 0, 0, 4, 3, CG_STYLE_SINGLE, 2, CG_KEEP);
  CHECK_EQ_INT(g->glyph[0], CG_BOX_TL);
  CHECK_EQ_INT(g->glyph[1], CG_BOX_H);
  CHECK_EQ_INT(g->glyph[3], CG_BOX_TR);
  CHECK_EQ_INT(g->glyph[5], CG_BOX_V);
  CHECK_EQ_INT(g->glyph[6], 'x');  // inside untouched
  CHECK_EQ_INT(g->glyph[10], CG_BOX_BL);
  CHECK_EQ_INT(g->glyph[13], CG_BOX_BR);
  CHECK_EQ_INT(g->fg[1], 2);
  chargrid_box(g, 0, 0, 3, 2, CG_STYLE_ASCII, CG_KEEP, CG_KEEP);
  CHECK_EQ_INT(g->glyph[0], '+');
  CHECK_EQ_INT(g->glyph[1], '-');
  CHECK_EQ_INT(g->glyph[2], '+');
  CHECK_EQ_INT(g->glyph[5], '+');  // a 2-row box: its bottom row is corners and edge too
  CHECK_EQ_INT(g->glyph[6], '-');
  chargrid_box(g, 0, 0, 2, 2, CG_STYLE_BLOCK, CG_KEEP, CG_KEEP);
  CHECK_EQ_INT(g->glyph[0], CG_SHADE4);
  CHECK_EQ_INT(g->glyph[6], CG_SHADE4);
  chargrid_box(g, 4, 0, 1, 4, CG_STYLE_SINGLE, CG_KEEP, CG_KEEP);  // too thin: nothing
  CHECK_EQ_INT(g->glyph[4], 'x');
  chargrid_box(g, -5, -5, 1 << 24, 1 << 24, CG_STYLE_SINGLE, CG_KEEP, CG_KEEP);  // huge: fast, clipped
  CHECK_EQ_INT(g->glyph[16], 'x');  // row 3: no box has touched it
  CHECK_EQ_INT(g->glyph[0], CG_SHADE4);
  chargrid_fill(g, 0, 0, 5, 4, '.', CG_DEFAULT_FG, CG_KEEP);  // also resets the box's fg=2
  const char *art = " /\\\n/__\\";
  chargrid_stamp(g, 0, 0, art, strlen(art), 3, CG_KEEP, ' ');
  row_str(g, 0, s); CHECK_STR(s, "./\\..");
  row_str(g, 1, s); CHECK_STR(s, "/__\\.");
  CHECK_EQ_INT(g->fg[0], CG_DEFAULT_FG);  // transparent cell: nothing written
  CHECK_EQ_INT(g->fg[1], 3);
  chargrid_free(g);
}

static void test_ramps(void) {
  CHECK_EQ_INT(chargrid_ramp_step(0.0f, 4, false, 0, 0), 0);
  CHECK_EQ_INT(chargrid_ramp_step(0.34f, 4, false, 0, 0), 1);
  CHECK_EQ_INT(chargrid_ramp_step(0.67f, 4, false, 0, 0), 2);
  CHECK_EQ_INT(chargrid_ramp_step(1.0f, 4, false, 0, 0), 3);
  CHECK_EQ_INT(chargrid_ramp_step(-3.0f, 4, false, 0, 0), 0);
  CHECK_EQ_INT(chargrid_ramp_step(7.0f, 4, false, 0, 0), 3);
  CHECK_EQ_INT(chargrid_ramp_step(NAN, 4, false, 0, 0), 0);
  CHECK_EQ_INT(chargrid_ramp_step(0.9f, 1, true, 3, 3), 0);
  int on = 0;  // dither: 0.5 in a 2-entry ramp lights exactly half of a 4x4 block
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) on += chargrid_ramp_step(0.5f, 2, true, x, y);
  CHECK_EQ_INT(on, 8);
  for (int y = 0; y < 4; y++)  // dither never leaves the ends
    for (int x = 0; x < 4; x++) {
      CHECK_EQ_INT(chargrid_ramp_step(0.0f, 4, true, x, y), 0);
      CHECK_EQ_INT(chargrid_ramp_step(1.0f, 4, true, x, y), 3);
    }
  chargrid_t *g = chargrid_create(4, 1, 6, 8);
  const uint8_t ramp[] = {' ', '.', ':', '#'}, cr[] = {10, 11};
  chargrid_shade(g, 0, 0, 1.0f, true, 0.0f, false);  // no ramp yet: unchanged
  CHECK_EQ_INT(g->glyph[0], 0);
  chargrid_set_ramp(g, ramp, 4, false);
  chargrid_set_color_ramp(g, cr, 2, false);
  CHECK_EQ_INT(g->ramp_n, 4);
  CHECK_EQ_INT(g->cramp_n, 2);
  chargrid_shade(g, 1, 0, 0.34f, true, 1.0f, true);
  CHECK_EQ_INT(g->glyph[1], '.');
  CHECK_EQ_INT(g->fg[1], 11);
  chargrid_shade(g, 2, 0, 1.0f, true, 0.0f, false);  // tone off: fg kept
  CHECK_EQ_INT(g->glyph[2], '#');
  CHECK_EQ_INT(g->fg[2], CG_DEFAULT_FG);
  chargrid_shade(g, 9, 0, 1.0f, true, 1.0f, true);   // outside: ignored
  chargrid_set_ramp(g, NULL, 0, false);
  CHECK_EQ_INT(g->ramp_n, 0);
  chargrid_free(g);
}

static void test_blit_scroll(void) {
  chargrid_t *a = chargrid_create(4, 2, 6, 8), *b = chargrid_create(4, 2, 6, 8);
  char s[8];
  chargrid_print(a, 0, 0, "abcd", 4, 1, CG_KEEP);
  chargrid_print(a, 0, 1, "e f ", 4, CG_KEEP, CG_KEEP);
  chargrid_fill(b, 0, 0, 4, 2, '.', CG_KEEP, CG_KEEP);
  chargrid_blit(b, a, 1, 0, 3, 2, 0, 0, ' ');  // spaces transparent
  row_str(b, 0, s); CHECK_STR(s, "bcd.");
  row_str(b, 1, s); CHECK_STR(s, ".f..");
  CHECK_EQ_INT(b->fg[0], 1);
  CHECK_EQ_INT(b->fg[3], CG_DEFAULT_FG);
  chargrid_blit(a, a, 0, 0, 3, 1, 1, 0, CG_KEEP);  // overlap to the right
  row_str(a, 0, s); CHECK_STR(s, "aabc");
  chargrid_blit(a, a, 1, 0, 3, 1, 0, 0, CG_KEEP);  // overlap to the left
  row_str(a, 0, s); CHECK_STR(s, "abcc");
  chargrid_blit(a, a, 0, 0, 4, 1, 0, 1, 'c');      // down, 'c' transparent
  row_str(a, 1, s); CHECK_STR(s, "abf ");
  chargrid_blit(b, a, -1, -1, 10, 10, -2, 0, CG_KEEP);  // clipped on both grids
  row_str(b, 1, s); CHECK_STR(s, "bcc.");
  chargrid_t *d = chargrid_create(3, 3, 6, 8);     // a diagonal self-overlap
  chargrid_print(d, 0, 0, "abc\ndef\nghi", 11, CG_KEEP, CG_KEEP);
  chargrid_blit(d, d, 0, 0, 2, 2, 1, 1, CG_KEEP);
  row_str(d, 0, s); CHECK_STR(s, "abc");
  row_str(d, 1, s); CHECK_STR(s, "dab");
  row_str(d, 2, s); CHECK_STR(s, "gde");
  chargrid_blit(d, d, 0, 0, 0, 3, 1, 1, CG_KEEP);  // empty: nothing
  chargrid_scroll(d, 0, -1, '-', CG_KEEP, CG_KEEP);
  row_str(d, 0, s); CHECK_STR(s, "dab");
  row_str(d, 1, s); CHECK_STR(s, "gde");
  row_str(d, 2, s); CHECK_STR(s, "---");
  chargrid_scroll(d, 2, 0, '#', CG_KEEP, CG_KEEP);
  row_str(d, 0, s); CHECK_STR(s, "##d");
  row_str(d, 2, s); CHECK_STR(s, "##-");
  chargrid_scroll(d, 0, 5, '*', CG_KEEP, CG_KEEP);
  row_str(d, 1, s); CHECK_STR(s, "***");
  chargrid_scroll(d, -2000000000, 0, '!', CG_KEEP, CG_KEEP);
  row_str(d, 2, s); CHECK_STR(s, "!!!");
  chargrid_free(a);
  chargrid_free(b);
  chargrid_free(d);
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
  test_utf8_map();
  test_utf8_invalid();
  test_print_wrap();
  test_box_stamp();
  test_ramps();
  test_blit_scroll();
  return check_report("test_chargrid");
}
