// Host unit tests for src/fonts/font.c. Build + run: make test-unit
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

// A 3-glyph, 4x3 font covering 'A'..'C'. Bit rows, MSB = leftmost.
//   A = full block 4 wide, B = left column only, C = empty
static const uint8_t k_bits[3 * 3] = {
  0xF0, 0xF0, 0xF0,   // 'A'
  0x80, 0x80, 0x80,   // 'B'
  0x00, 0x00, 0x00,   // 'C'
};
static const uint8_t k_widths[3] = { 4, 2, 1 };

static pc_font_t mono_font(void) {
  pc_font_t f = { 'A', 'C', 3, 4, 1, NULL, k_bits, NULL };
  return f;
}
static pc_font_t prop_font(void) {
  pc_font_t f = mono_font();
  f.widths = k_widths;
  return f;
}

static void test_widths(void) {
  pc_font_t m = mono_font(), p = prop_font();
  CHECK(font_glyph_width(&m, 'A') == 4);
  CHECK(font_glyph_width(&m, 'Z') == 4);        // out of range -> max_width
  CHECK(font_glyph_width(&p, 'A') == 4);
  CHECK(font_glyph_width(&p, 'B') == 2);
  CHECK(font_glyph_width(&p, 'C') == 1);
  CHECK(font_text_width(&m, "ABC") == 12);
  CHECK(font_text_width(&p, "ABC") == 7);
  CHECK(font_text_width(&p, "") == 0);
}

static void test_wrap(void) {
  pc_font_t p = prop_font();
  // "AB CC" = 4+2+space(4, out of range)+1+1 = 12
  CHECK(font_wrap_line(&p, "AB CC", 100) == 5);     // fits entirely
  CHECK(font_wrap_line(&p, "AB CC", 11) == 2);      // overflow -> break at space
  CHECK(font_wrap_line(&p, "AB\nCC", 100) == 2);    // stops before newline
  CHECK(font_wrap_line(&p, "\nCC", 100) == 0);      // leading newline consumes nothing
  CHECK(font_wrap_line(&p, "AAAA", 5) == 1);        // no space: hard break
  CHECK(font_wrap_line(&p, "A", 1) == 1);           // over-wide glyph still consumed
  CHECK(font_wrap_line(&p, "", 10) == 0);
}

static void test_render_mono_and_clip(void) {
  pc_font_t m = mono_font();
  uint16_t buf[8 * 4];
  memset(buf, 0x11, sizeof buf);
  // Draw "AB" at (0,0) into an 8-wide, 4-tall buffer, full clip.
  int adv = font_render(&m, buf, 8, 0, 0, 7, 3, 0, 0, "AB", 0xFFFF, 0x0000, false);
  CHECK(adv == 8);
  CHECK(buf[0] == 0xFFFF && buf[3] == 0xFFFF);      // 'A' row 0 cols 0..3 on
  CHECK(buf[4] == 0xFFFF && buf[5] == 0x0000);      // 'B' col 0 on, col 1 bg
  CHECK(buf[3 * 8 + 0] == 0x1111);                  // row 3 untouched (height 3)
  // Transparent: bg pixels keep their old value
  memset(buf, 0x22, sizeof buf);
  font_render(&m, buf, 8, 0, 0, 7, 3, 0, 0, "B", 0xFFFF, 0x0000, true);
  CHECK(buf[0] == 0xFFFF && buf[1] == 0x2222);
  // Clip: only column 5 may be written
  memset(buf, 0x33, sizeof buf);
  font_render(&m, buf, 8, 5, 0, 5, 3, 0, 0, "AB", 0xFFFF, 0x0000, false);
  CHECK(buf[4] == 0x3333 && buf[5] == 0x0000 && buf[6] == 0x3333);
  // Negative x: glyph partly off the left edge, nothing written before col 0
  memset(buf, 0x44, sizeof buf);
  adv = font_render(&m, buf, 8, 0, 0, 7, 3, -2, 0, "A", 0xFFFF, 0x0000, false);
  CHECK(adv == 4);
  CHECK(buf[0] == 0xFFFF && buf[1] == 0xFFFF && buf[2] == 0x4444);
  // Wholly off-clip lines and pens past the right edge draw nothing but still
  // return the full advance (the renderer clips once and stops early).
  memset(buf, 0x55, sizeof buf);
  CHECK(font_render(&m, buf, 8, 0, 0, 7, 3, 0, 4, "AB", 0xFFFF, 0, false) == 8);
  CHECK(font_render(&m, buf, 8, 0, 0, 7, 3, 0, -3, "AB", 0xFFFF, 0, false) == 8);
  CHECK(font_render(&m, buf, 8, 0, 0, 7, 3, 8, 0, "ABA", 0xFFFF, 0, false) == 12);
  CHECK(font_render(&m, buf, 8, 0, 0, 7, 3, 2147483640, 0, "AAAA", 0xFFFF, 0,
                    false) == 16);  // pen past INT_MAX: no overflow
  for (int i = 0; i < 8 * 4; i++) CHECK(buf[i] == 0x5555);
  // Right of the clip after the first glyph: the first is drawn, rest measured.
  adv = font_render(&m, buf, 8, 0, 0, 3, 3, 0, 0, "ABAB", 0xFFFF, 0, false);
  CHECK(adv == 16);
  CHECK(buf[0] == 0xFFFF && buf[4] == 0x5555);
}

static void test_render_proportional_and_fallback(void) {
  pc_font_t p = prop_font();
  uint16_t buf[8 * 4];
  memset(buf, 0x55, sizeof buf);
  // "BB": each advances 2, second B's column lands at x=2
  int adv = font_render(&p, buf, 8, 0, 0, 7, 3, 0, 0, "BB", 0xFFFF, 0x0000, false);
  CHECK(adv == 4);
  CHECK(buf[0] == 0xFFFF && buf[1] == 0x0000 && buf[2] == 0xFFFF && buf[3] == 0x0000);
  CHECK(buf[4] == 0x5555);                          // nothing beyond advance
  // Fallback box for out-of-range 'Z': 4x3 cell, hollow 3x2 outline
  memset(buf, 0x66, sizeof buf);
  adv = font_render(&p, buf, 8, 0, 0, 7, 3, 0, 0, "Z", 0xFFFF, 0x0000, false);
  CHECK(adv == 4);
  CHECK(buf[0] == 0xFFFF && buf[1] == 0xFFFF && buf[2] == 0xFFFF); // top edge
  CHECK(buf[3] == 0x0000);                          // last column is bg
  CHECK(buf[8 + 0] == 0xFFFF && buf[8 + 2] == 0xFFFF);            // bottom edge (row h-2)
  CHECK(buf[2 * 8 + 0] == 0x0000);                  // row h-1 is bg
}

// A 12-wide font needs 2 bytes per row; columns past 7 live in the second.
static void test_render_stride2(void) {
  static const uint8_t bits[2 * 2] = {
    0x80, 0x10,   // row 0: columns 0 and 11
    0x00, 0x80,   // row 1: column 8
  };
  pc_font_t f = { 'A', 'A', 2, 12, 2, NULL, bits, NULL };
  uint16_t buf[16 * 2];
  memset(buf, 0x77, sizeof buf);
  int adv = font_render(&f, buf, 16, 0, 0, 15, 1, 0, 0, "A", 0xFFFF, 0x0000, false);
  CHECK(adv == 12);
  for (int c = 0; c < 12; c++) {
    bool want_on = (c == 0 || c == 11);
    CHECK(buf[c] == (want_on ? 0xFFFF : 0x0000));
  }
  for (int c = 0; c < 12; c++)
    CHECK(buf[16 + c] == (c == 8 ? 0xFFFF : 0x0000));
  CHECK(buf[12] == 0x7777 && buf[16 + 12] == 0x7777);   // nothing past the cell
}

static void test_from_blob(void) {
  // Build a valid 2-glyph 3x2 font: count=2, height=2, max_width=3, stride=1
  uint8_t img[PFNT_HEADER_SIZE + 2 + 2 * 2 * 1];
  memcpy(img, "PFNT", 4);
  img[4] = PFNT_VERSION; img[5] = PFNT_FLAG_PROPORTIONAL;
  img[6] = 'a'; img[7] = 'b'; img[8] = 2; img[9] = 3; img[10] = 1; img[11] = 0;
  img[12] = 3; img[13] = 2;                 // widths
  img[14] = 0xE0; img[15] = 0xE0; img[16] = 0x80; img[17] = 0x80;
  pc_font_t f;
  CHECK(font_from_blob(&f, img, sizeof img));
  CHECK(f.first == 'a' && f.last == 'b' && f.height == 2 && f.max_width == 3);
  CHECK(f.stride == 1 && f.widths == img + 12 && f.bitmaps == img + 14);
  CHECK(f.blob == NULL);
  CHECK(font_text_width(&f, "ab") == 5);

  CHECK(!font_from_blob(&f, img, sizeof img - 1));   // short
  CHECK(!font_from_blob(&f, img, sizeof img + 1));   // long
  img[4] = 2; CHECK(!font_from_blob(&f, img, sizeof img)); img[4] = 1;
  img[0] = 'X'; CHECK(!font_from_blob(&f, img, sizeof img)); img[0] = 'P';
  img[10] = 2; CHECK(!font_from_blob(&f, img, sizeof img)); img[10] = 1;
  img[7] = 'Z' - 1; CHECK(!font_from_blob(&f, img, sizeof img)); img[7] = 'b';  // last < first
  img[13] = 4; CHECK(!font_from_blob(&f, img, sizeof img)); img[13] = 2;  // width > max
  img[13] = 0; CHECK(!font_from_blob(&f, img, sizeof img)); img[13] = 2;  // width 0
  img[11] = 1; CHECK(!font_from_blob(&f, img, sizeof img)); img[11] = 0;  // reserved
  img[5] = 0x02; CHECK(!font_from_blob(&f, img, sizeof img));             // unknown flag bit
  img[5] = PFNT_FLAG_PROPORTIONAL;
  img[8] = 65; CHECK(!font_from_blob(&f, img, sizeof img)); img[8] = 2;   // height > 64
  CHECK(!font_from_blob(&f, NULL, 0));
}

static void test_demo_pfn_file(void) {
  FILE *fp = fopen("apps/fonttest/fonts/demo_prop.pfn", "rb");
  if (!fp) { printf("SKIP demo_prop.pfn not present\n"); return; }
  fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
  unsigned char *img = malloc((size_t)n);
  CHECK(fread(img, 1, (size_t)n, fp) == (size_t)n);
  fclose(fp);
  pc_font_t f;
  CHECK(font_from_blob(&f, img, (size_t)n));
  CHECK(f.first == 0x20 && f.last == 0x7E && f.height == 12 && f.max_width == 8);
  CHECK(font_glyph_width(&f, 'i') < font_glyph_width(&f, 'W'));
  CHECK(font_text_width(&f, "iii") < font_text_width(&f, "WWW"));
  free(img);
}


// ── Reference renderer: font_render as it was before the byte-at-a-time
// glyph path (a verbatim copy), so the fast path can be checked pixel for
// pixel against it.
#include "font_6x8.h"
#include "font_8x12.h"

static void ref_blit_cell(uint16_t *buf, int buf_w, int cx0, int cy0, int cx1,
                          int cy1, long long x, long long y, int w, int h,
                          const uint8_t *rows, int stride, uint16_t fg,
                          uint16_t bg, bool transparent) {
  long long r0 = y < cy0 ? cy0 - y : 0, r1 = y + h - 1 > cy1 ? cy1 - y : h - 1;
  long long c0 = x < cx0 ? cx0 - x : 0, c1 = x + w - 1 > cx1 ? cx1 - x : w - 1;
  if (r0 > r1 || c0 > c1) return;
  for (int row = (int)r0; row <= (int)r1; row++) {
    uint16_t *dst = buf + (size_t)(y + row) * buf_w + (size_t)(x + c0) - c0;
    const uint8_t *bits = rows ? rows + row * stride : NULL;
    for (int col = (int)c0; col <= (int)c1; col++) {
      bool on;
      if (bits) {
        on = (bits[col >> 3] & (0x80 >> (col & 7))) != 0;
      } else {
        on = row < h - 1 && col < w - 1 &&
             (row == 0 || row == h - 2 || col == 0 || col == w - 2);
      }
      if (on) dst[col] = fg;
      else if (!transparent) dst[col] = bg;
    }
  }
}

static int ref_render(const pc_font_t *f, uint16_t *buf, int buf_w, int cx0,
                      int cy0, int cx1, int cy1, int x, int y, const char *text,
                      uint16_t fg, uint16_t bg, bool transparent) {
  int h = f->height;
  if ((long long)y + h - 1 < cy0 || y > cy1 || cx1 < cx0)
    return font_text_width(f, text);
  long long xx = x;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    if (xx > cx1) return (int)(xx - x) + font_text_width(f, (const char *)p);
    unsigned char c = *p;
    if (c < f->first || c > f->last) {
      int adv = f->max_width;
      ref_blit_cell(buf, buf_w, cx0, cy0, cx1, cy1, xx, y, adv, h, NULL, 0, fg,
                    bg, transparent);
      xx += adv;
      continue;
    }
    int gi = c - f->first;
    int adv = f->widths ? f->widths[gi] : f->max_width;
    const uint8_t *glyph = f->bitmaps + (size_t)gi * h * f->stride;
    ref_blit_cell(buf, buf_w, cx0, cy0, cx1, cy1, xx, y, adv, h, glyph,
                  f->stride, fg, bg, transparent);
    xx += adv;
  }
  return (int)(xx - x);
}

// Every font shape (6x8 and 8x12 built-ins, the proportional test font, a
// 12-wide font whose rows span two bytes) at every offset, clip and mode
// renders exactly as the reference.
static void test_render_matches_reference(void) {
  static uint8_t wide_bits[5 * 10 * 2];
  uint32_t seed = 12345;
  for (size_t i = 0; i < sizeof wide_bits; i++) {
    seed = seed * 1103515245u + 12345u;
    wide_bits[i] = (uint8_t)(seed >> 16);
  }
  pc_font_t fonts[4] = {
    {0x20, 0x7E, 8, 6, 1, NULL, &font_6x8[0][0], NULL},
    {0x20, 0x7E, 12, 8, 1, NULL, &font_8x12[0][0], NULL},
    prop_font(),
    {'A', 'E', 10, 12, 2, NULL, wide_bits, NULL},
  };
  const char *texts[4] = {"Nova Rail 0:12.345 ~\x7f", "LAP 1/3 TIME\x01", "ABCA BZ",
                          "ABCDE AZE"};
  const int xs[] = {-7, -3, 0, 5}, ys[] = {-9, -2, 0, 3};
  const int clips[3][4] = {{0, 0, 63, 23}, {2, 1, 40, 20}, {7, 5, 7, 5}};
  static uint16_t got[64 * 24], want[64 * 24];
  for (int fi = 0; fi < 4; fi++)
    for (int xi = 0; xi < 4; xi++)
      for (int yi = 0; yi < 4; yi++)
        for (int ci = 0; ci < 3; ci++)
          for (int tr = 0; tr < 2; tr++) {
            for (int i = 0; i < 64 * 24; i++) got[i] = want[i] = 0x1234;
            const int *c = clips[ci];
            int a = font_render(&fonts[fi], got, 64, c[0], c[1], c[2], c[3], xs[xi],
                                ys[yi], texts[fi], 0xFFFF, 0x0841, tr);
            int b = ref_render(&fonts[fi], want, 64, c[0], c[1], c[2], c[3], xs[xi],
                               ys[yi], texts[fi], 0xFFFF, 0x0841, tr);
            CHECK(a == b);
            CHECK(memcmp(got, want, sizeof got) == 0);
          }
}

int main(void) {
  test_widths();
  test_wrap();
  test_render_mono_and_clip();
  test_render_proportional_and_fallback();
  test_render_stride2();
  test_render_matches_reference();
  test_from_blob();
  test_demo_pfn_file();
  return check_report("test_font");
}
