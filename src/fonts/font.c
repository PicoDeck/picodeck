#include "font.h"
#include <string.h>

int font_glyph_width(const pc_font_t *f, unsigned char c) {
  if (c < f->first || c > f->last) return f->max_width;
  return f->widths ? f->widths[c - f->first] : f->max_width;
}

int font_text_width(const pc_font_t *f, const char *text) {
  int w = 0;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    w += font_glyph_width(f, *p);
  return w;
}

int font_wrap_line(const pc_font_t *f, const char *text, int max_w) {
  const unsigned char *p = (const unsigned char *)text;
  int n = 0, w = 0, last_space = -1;
  while (p[n] && p[n] != '\n') {
    if (p[n] == ' ') last_space = n;
    int gw = font_glyph_width(f, p[n]);
    if (w + gw > max_w) break;
    w += gw;
    n++;
  }
  bool overflowed = p[n] && p[n] != '\n';
  if (overflowed && last_space > 0) n = last_space;
  if (n == 0 && p[0] && p[0] != '\n') n = 1;
  return n;
}

// Blit one glyph cell. `on_at(row, col)` semantics are inlined for the two
// shapes we have (bitmap glyph, fallback box) so the clip and color logic
// exists exactly once. The cell is clipped once up front (in 64-bit, so a cell
// near INT_MAX cannot overflow); the loops then touch only visible pixels.
static void blit_cell(uint16_t *buf, int buf_w,
                      int cx0, int cy0, int cx1, int cy1,
                      long long x, long long y, int w, int h,
                      const uint8_t *rows, int stride,   // NULL => fallback box
                      uint16_t fg, uint16_t bg, bool transparent) {
  long long r0 = y < cy0 ? cy0 - y : 0, r1 = y + h - 1 > cy1 ? cy1 - y : h - 1;
  long long c0 = x < cx0 ? cx0 - x : 0, c1 = x + w - 1 > cx1 ? cx1 - x : w - 1;
  if (r0 > r1 || c0 > c1) return;
  const int cs = (int)c0, ce = (int)c1;
  for (int row = (int)r0; row <= (int)r1; row++) {
    // Offset so that dst[col] is cell column col (x + c0 >= cx0 >= 0).
    uint16_t *dst = buf + (size_t)(y + row) * buf_w + (size_t)(x + c0) - c0;
    if (!rows) {
      // Hollow box occupying rows 0..h-2 and cols 0..w-2 of the cell.
      for (int col = cs; col <= ce; col++) {
        bool on = row < h - 1 && col < w - 1 &&
                  (row == 0 || row == h - 2 || col == 0 || col == w - 2);
        if (on) dst[col] = fg;
        else if (!transparent) dst[col] = bg;
      }
      continue;
    }
    // A glyph row, a byte of bits at a time: b holds the current byte
    // shifted so that bit 7 is column col.
    const uint8_t *bits = rows + row * stride;
    unsigned b = (unsigned)bits[cs >> 3] << (cs & 7);
    for (int col = cs; col <= ce; col++, b <<= 1) {
      if ((col & 7) == 0) b = bits[col >> 3];
      if (b & 0x80) dst[col] = fg;
      else if (!transparent) dst[col] = bg;
    }
  }
}

// The first n (1..8) pixels of one glyph byte b (bit 7 = d[0]): a case per
// pixel, falling through, so each pixel is a bit test and a select (opaque)
// or a conditional store (transparent), with no shift or loop count per
// pixel. Font 0 is 6 wide, so most bytes are partial.
#define GLYPH_PX(i, bit) \
  case (i) + 1:          \
    if (transparent) {   \
      if (b & (bit)) d[i] = fg; \
    } else {             \
      d[i] = (b & (bit)) ? fg : bg; \
    }                    \
    __attribute__((fallthrough));
static inline void blit_glyph_byte(uint16_t *d, unsigned b, int n,
                                   uint16_t fg, uint16_t bg,
                                   bool transparent) {
  switch (n) {
    GLYPH_PX(7, 0x01)
    GLYPH_PX(6, 0x02)
    GLYPH_PX(5, 0x04)
    GLYPH_PX(4, 0x08)
    GLYPH_PX(3, 0x10)
    GLYPH_PX(2, 0x20)
    GLYPH_PX(1, 0x40)
    GLYPH_PX(0, 0x80)
    default:
      break;
  }
}
#undef GLYPH_PX

// One glyph cell with no column clipping (the common case: the whole cell is
// inside the clip's columns): n rows of it, from glyph row `rows` drawn at
// `dst`. A glyph row is drawn a byte (eight columns) at a time; transparent
// text skips empty bytes. blit_cell above covers everything else (a cell cut
// by the clip's left or right edge, the fallback box) and is what this must
// match pixel for pixel (tests/unit/test_font.c compares both against the
// original renderer).
static inline void blit_glyph_rows(uint16_t *dst, int buf_w, int n, int w,
                                   const uint8_t *rows, int stride,
                                   uint16_t fg, uint16_t bg, bool transparent) {
  if (stride == 1) {  // every built-in font: one byte per row
    for (; n > 0; n--, rows++, dst += buf_w) {
      unsigned b = *rows;
      if (transparent && !b) continue;
      blit_glyph_byte(dst, b, w, fg, bg, transparent);
    }
    return;
  }
  for (; n > 0; n--, rows += stride, dst += buf_w) {
    uint16_t *d = dst;
    const uint8_t *bits = rows;
    for (int left = w; left > 0; left -= 8, d += 8) {
      unsigned b = *bits++;
      if (transparent && !b) continue;
      blit_glyph_byte(d, b, left < 8 ? left : 8, fg, bg, transparent);
    }
  }
}

int font_render(const pc_font_t *f, uint16_t *buf, int buf_w,
                int cx0, int cy0, int cx1, int cy1,
                int x, int y, const char *text,
                uint16_t fg, uint16_t bg, bool transparent) {
  int h = f->height;
  // A line wholly above or below the clip draws nothing; only the advance
  // is still summed (the return value is the text width).
  if ((long long)y + h - 1 < cy0 || y > cy1 || cx1 < cx0)
    return font_text_width(f, text);
  // The rows every cell of this line keeps (the line is not wholly outside
  // the clip's rows, so this is a non-empty range inside 0..h-1).
  const int r0 = y < cy0 ? (int)((long long)cy0 - y) : 0;
  const int r1 = (long long)y + h - 1 > cy1 ? (int)((long long)cy1 - y) : h - 1;
  long long xx = x;  // 64-bit pen: a long string near INT_MAX cannot overflow
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    if (xx > cx1)  // the rest lies right of the clip: just measure it
      return (int)(xx - x) + font_text_width(f, (const char *)p);
    unsigned char c = *p;
    if (c < f->first || c > f->last) {
      int adv = f->max_width;
      blit_cell(buf, buf_w, cx0, cy0, cx1, cy1, xx, y, adv, h,
                NULL, 0, fg, bg, transparent);
      xx += adv;
      continue;
    }
    int gi = c - f->first;
    // Deliberately inlines font_glyph_width: the glyph index is needed anyway
    // for the bitmap pointer, so re-deriving it inside a call would be waste.
    int adv = f->widths ? f->widths[gi] : f->max_width;
    const uint8_t *glyph = f->bitmaps + (size_t)gi * h * f->stride;
    if (xx >= cx0 && xx + adv - 1 <= cx1) {
      // Whole cell inside the clip's columns, so xx is a valid column and
      // y + r0 a valid row of buf.
      blit_glyph_rows(buf + ((long long)y + r0) * buf_w + xx, buf_w,
                      r1 - r0 + 1, adv, glyph + (size_t)r0 * f->stride,
                      f->stride, fg, bg, transparent);
    } else {
      blit_cell(buf, buf_w, cx0, cy0, cx1, cy1, xx, y, adv, h,
                glyph, f->stride, fg, bg, transparent);
    }
    xx += adv;
  }
  return (int)(xx - x);
}

bool font_from_blob(pc_font_t *out, const void *blob, size_t len) {
  const uint8_t *b = (const uint8_t *)blob;
  if (!out || !b || len < PFNT_HEADER_SIZE) return false;
  if (memcmp(b, "PFNT", 4) != 0) return false;
  if (b[4] != PFNT_VERSION) return false;
  if (b[5] & ~PFNT_FLAG_PROPORTIONAL) return false;   // unknown flag bits
  uint8_t first = b[6], last = b[7], height = b[8], max_w = b[9], stride = b[10];
  if (last < first) return false;
  if (height < 1 || height > FONT_MAX_DIM) return false;
  if (max_w < 1 || max_w > FONT_MAX_DIM) return false;
  if (stride != (max_w + 7) / 8) return false;
  if (b[11] != 0) return false;
  size_t count = (size_t)last - first + 1;
  size_t expect = PFNT_HEADER_SIZE + count + count * height * stride;
  if (len != expect) return false;
  const uint8_t *widths = b + PFNT_HEADER_SIZE;
  for (size_t i = 0; i < count; i++)
    if (widths[i] < 1 || widths[i] > max_w) return false;
  out->first = first;
  out->last = last;
  out->height = height;
  out->max_width = max_w;
  out->stride = stride;
  out->widths = widths;
  out->bitmaps = widths + count;
  out->blob = NULL;
  return true;
}
