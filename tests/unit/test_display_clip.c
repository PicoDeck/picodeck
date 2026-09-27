// Host unit tests for src/drivers/display_clip.h — the clip-once rasterisers
// shared by the firmware display driver and the simulator.
//
// Each primitive is compared, pixel for pixel over a whole canvas, against a
// reference: the per-pixel loop the driver used before (same Bresenham, same
// triangle formulas, same sqrt disc) or, for the blitters, a direct model of
// the documented semantics. The "screen" sits in the middle of a larger
// canvas, so a write outside the clip lands in a guard band the comparison
// sees (ASan would not: it is still inside the allocation).
#include "check.h"
#include "display_clip.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

#define CW 160  // canvas
#define CH 140
#define OX 50   // screen origin inside the canvas
#define OY 50
#define SW 48   // screen size (the default clip)
#define SH 36

static uint16_t s_got[CW * CH], s_want[CW * CH];

static uint16_t *screen(uint16_t *canvas) { return canvas + OY * CW + OX; }

static void reset(uint16_t fill) {
  for (int i = 0; i < CW * CH; i++) s_got[i] = s_want[i] = fill;
}

static int s_first_diff_logged;
static bool same(const char *what) {
  for (int i = 0; i < CW * CH; i++) {
    if (s_got[i] != s_want[i]) {
      if (s_first_diff_logged++ < 12)
        printf("  %s: first diff at screen (%d,%d): got 0x%04x want 0x%04x\n",
               what, i % CW - OX, i / CW - OY, s_got[i], s_want[i]);
      return false;
    }
  }
  return true;
}

static uint32_t s_rng = 12345;
static int rnd(int lo, int hi) {  // inclusive
  s_rng = s_rng * 1103515245u + 12345u;
  return (int)(lo + (int64_t)((s_rng >> 8) % (uint64_t)((int64_t)hi - lo + 1)));
}

static disp_clip_t random_clip(void) {
  if (rnd(0, 2) == 0) return (disp_clip_t){0, 0, SW - 1, SH - 1};
  int x0 = rnd(0, SW - 1), x1 = rnd(x0, SW - 1);
  int y0 = rnd(0, SH - 1), y1 = rnd(y0, SH - 1);
  return (disp_clip_t){x0, y0, x1, y1};
}

// ── references (the drivers' pre-clip loops) ────────────────────────────────

static void ref_plot(uint16_t *fb, const disp_clip_t *c, int x, int y,
                     uint16_t v) {
  if (x >= c->x0 && x <= c->x1 && y >= c->y0 && y <= c->y1) fb[y * CW + x] = v;
}

static void ref_line(uint16_t *fb, const disp_clip_t *c, int x0, int y0,
                     int x1, int y1, uint16_t v) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  while (1) {
    ref_plot(fb, c, x0, y0, v);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void ref_triangle(uint16_t *fb, const disp_clip_t *c, int x0, int y0,
                         int x1, int y1, int x2, int y2, uint16_t v) {
  int t;
  if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y0 > y2) { t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
  if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
  if (y0 == y2) return;
  float inv_dy02 = 1.0f / (float)(y2 - y0);
  if (y0 == y1) {
    for (int y = y0; y <= y2; y++) {
      float tt = (float)(y - y0) * inv_dy02;
      ref_line(fb, c, x0 + (int)((x2 - x0) * tt), y,
               x1 + (int)((x2 - x1) * tt), y, v);
    }
  } else if (y1 == y2) {
    float inv_dy01 = 1.0f / (float)(y1 - y0);
    for (int y = y0; y <= y1; y++) {
      float tt = (float)(y - y0) * inv_dy01;
      ref_line(fb, c, x0 + (int)((x1 - x0) * tt), y,
               x0 + (int)((x2 - x0) * tt), y, v);
    }
  } else {
    float inv_dy01 = 1.0f / (float)(y1 - y0);
    float inv_dy12 = 1.0f / (float)(y2 - y1);
    for (int y = y0; y <= y1; y++) {
      float ts = (float)(y - y0) * inv_dy01, tl = (float)(y - y0) * inv_dy02;
      ref_line(fb, c, x0 + (int)((x1 - x0) * ts), y,
               x0 + (int)((x2 - x0) * tl), y, v);
    }
    for (int y = y1; y <= y2; y++) {
      float ts = (float)(y - y1) * inv_dy12, tl = (float)(y - y0) * inv_dy02;
      ref_line(fb, c, x1 + (int)((x2 - x1) * ts), y,
               x0 + (int)((x2 - x0) * tl), y, v);
    }
  }
}

// The simulator's disc: one line per row, half-width (int)sqrt(r^2 - dy^2).
static void ref_fill_circle(uint16_t *fb, const disp_clip_t *c, int x, int y,
                            int r, uint16_t v) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrt((double)(r * r - dy * dy));
    ref_line(fb, c, x - dx, y + dy, x + dx, y + dy, v);
  }
}

// ── tests ───────────────────────────────────────────────────────────────────

static void test_clip_rect(void) {
  disp_clip_t c = {0, 0, 319, 319};
  disp_span_t s;
  CHECK(disp_clip_rect(&c, -3, -5, 10, 10, &s));
  CHECK_EQ_INT(s.x, 0); CHECK_EQ_INT(s.y, 0);
  CHECK_EQ_INT(s.w, 7); CHECK_EQ_INT(s.h, 5);
  CHECK_EQ_INT(s.skip_x, 3); CHECK_EQ_INT(s.skip_y, 5);
  CHECK(!disp_clip_rect(&c, 320, 0, 5, 5, &s));
  CHECK(!disp_clip_rect(&c, 0, 0, 0, 5, &s));
  CHECK(!disp_clip_rect(&c, -10, 0, 10, 5, &s));
  // x + w would overflow int: still clipped correctly.
  CHECK(disp_clip_rect(&c, 10, 10, (int64_t)INT_MAX, INT_MAX, &s));
  CHECK_EQ_INT(s.w, 310); CHECK_EQ_INT(s.h, 310);
  CHECK(disp_clip_rect(&c, INT_MIN, INT_MIN, (int64_t)UINT32_MAX, UINT32_MAX, &s));
  CHECK_EQ_INT(s.x, 0); CHECK_EQ_INT(s.w, 320);
  CHECK_EQ_INT(s.skip_x, -(int64_t)INT_MIN);
  disp_clip_t empty = {0, 0, -1, -1};
  CHECK(!disp_clip_rect(&empty, 0, 0, 5, 5, &s));
}

static void test_lines_match_reference(void) {
  int bad = 0;
  for (int i = 0; i < 20000; i++) {
    disp_clip_t c = random_clip();
    int span = (i % 10 == 0) ? 6000 : 90;  // some long lines: long skip-ahead
    int x0 = rnd(-span, span), y0 = rnd(-span, span);
    int x1 = rnd(-span, span), y1 = rnd(-span, span);
    if (i % 7 == 0) y1 = y0;  // horizontal
    if (i % 11 == 0) x1 = x0; // vertical
    if (i % 13 == 0) { x1 = x0; y1 = y0; }  // a point
    reset(0);
    disp_line(screen(s_got), CW, &c, x0, y0, x1, y1, 0xABCD);
    ref_line(screen(s_want), &c, x0, y0, x1, y1, 0xABCD);
    if (!same("line")) {
      if (bad++ < 5)
        printf("  line (%d,%d)-(%d,%d) clip %d,%d-%d,%d\n", x0, y0, x1, y1,
               c.x0, c.y0, c.x1, c.y1);
    }
  }
  CHECK_EQ_INT(bad, 0);
}

static void test_huge_lines(void) {
  disp_clip_t c = {0, 0, SW - 1, SH - 1};
  // (0,0) -> (1e9,1e9) is the diagonal, returned without walking 1e9 steps.
  reset(0);
  disp_line(screen(s_got), CW, &c, 0, 0, 1000000000, 1000000000, 7);
  for (int k = 0; k < SH; k++) screen(s_want)[k * CW + k] = 7;
  CHECK(same("diagonal to 1e9"));
  // Full-range endpoints (|dx| = 2^32 - 1: no int overflow).
  reset(0);
  disp_line(screen(s_got), CW, &c, INT_MIN, 5, INT_MAX, 5, 7);
  for (int k = 0; k < SW; k++) screen(s_want)[5 * CW + k] = 7;
  CHECK(same("row 5 from INT_MIN to INT_MAX"));
  reset(0);
  disp_line(screen(s_got), CW, &c, 3, INT_MAX, 3, INT_MIN, 7);
  for (int k = 0; k < SH; k++) screen(s_want)[k * CW + 3] = 7;
  CHECK(same("column 3 from INT_MAX to INT_MIN"));
  // Entirely off-screen: nothing, including in the guard band.
  reset(0);
  disp_line(screen(s_got), CW, &c, 1000000000, -1000000000, 1000000001,
            1000000000, 7);
  CHECK(same("off-screen line"));
  // A long shallow line that crosses the screen: same pixels as walking it.
  reset(0);
  disp_line(screen(s_got), CW, &c, -30000, -10, 30000, 40, 7);
  ref_line(screen(s_want), &c, -30000, -10, 30000, 40, 7);
  CHECK(same("long shallow line"));
}

static void test_triangles_match_reference(void) {
  int bad = 0;
  for (int i = 0; i < 20000; i++) {
    disp_clip_t c = random_clip();
    int r = (i % 10 == 0) ? 3000 : 70;
    int x0 = rnd(-r, r), y0 = rnd(-r, r), x1 = rnd(-r, r), y1 = rnd(-r, r);
    int x2 = rnd(-r, r), y2 = rnd(-r, r);
    if (i % 5 == 0) y1 = y0;  // flat top
    if (i % 7 == 0) y2 = y1;  // flat bottom
    reset(0);
    disp_fill_triangle(screen(s_got), CW, &c, x0, y0, x1, y1, x2, y2, 0x1234);
    ref_triangle(screen(s_want), &c, x0, y0, x1, y1, x2, y2, 0x1234);
    if (!same("triangle")) {
      if (bad++ < 5)
        printf("  tri (%d,%d) (%d,%d) (%d,%d) clip %d,%d-%d,%d\n", x0, y0, x1,
               y1, x2, y2, c.x0, c.y0, c.x1, c.y1);
    }
  }
  CHECK_EQ_INT(bad, 0);
  // Huge: rows >= 10 fully covered, nothing above.
  disp_clip_t c = {0, 0, SW - 1, SH - 1};
  reset(0);
  disp_fill_triangle(screen(s_got), CW, &c, 0, 10, 1000000000, 10, 0,
                     1000000000, 9);
  for (int y = 10; y < SH; y++)
    for (int x = 0; x < SW; x++) screen(s_want)[y * CW + x] = 9;
  CHECK(same("huge triangle"));
}

static void test_isqrt(void) {
  CHECK_EQ_INT(disp_isqrt(0), 0);
  CHECK_EQ_INT(disp_isqrt(1), 1);
  CHECK_EQ_INT(disp_isqrt(15), 3);
  CHECK_EQ_INT(disp_isqrt(16), 4);
  CHECK_EQ_INT(disp_isqrt_round(12), 3);  // 3.46
  CHECK_EQ_INT(disp_isqrt_round(13), 4);  // 3.61
  CHECK_EQ_INT(disp_isqrt_round(20), 4);  // 4.47
  CHECK_EQ_INT(disp_isqrt_round(21), 5);  // 4.58
  uint64_t r = 3037000499u;  // floor(sqrt(2^63))
  CHECK_EQ_INT(disp_isqrt(r * r), r);
  CHECK_EQ_INT(disp_isqrt(r * r - 1), r - 1);
  int bad = 0;
  for (int i = 0; i < 200000; i++) {
    uint64_t n = ((uint64_t)(uint32_t)rnd(0, INT_MAX) << 31) ^ (uint32_t)rnd(0, INT_MAX);
    n >>= rnd(0, 60);
    uint64_t s = disp_isqrt(n);
    unsigned __int128 lo = (unsigned __int128)s * s;
    unsigned __int128 hi = (unsigned __int128)(s + 1) * (s + 1);
    if (!(lo <= n && n < hi)) bad++;
  }
  CHECK_EQ_INT(bad, 0);
}

static void test_fill_circle_rows(void) {
  int bad = 0;
  for (int i = 0; i < 5000; i++) {
    disp_clip_t c = random_clip();
    int cx = rnd(-40, 90), cy = rnd(-40, 80), r = rnd(0, 60);
    reset(0);
    disp_fill_circle_rows(screen(s_got), CW, &c, cx, cy, r, 5);
    ref_fill_circle(screen(s_want), &c, cx, cy, r, 5);
    if (!same("disc") && bad++ < 5)
      printf("  disc (%d,%d) r=%d\n", cx, cy, r);
  }
  CHECK_EQ_INT(bad, 0);
}

static void test_huge_circles(void) {
  disp_clip_t c = {0, 0, SW - 1, SH - 1};
  // Outline of radius 1e9 whose top grazes row 6: exactly row 6.
  reset(0);
  CHECK(!disp_circle_rejected(&c, 20, 1000000006, 1000000000));
  disp_circle_big(screen(s_got), CW, &c, 20, 1000000006, 1000000000, 3);
  for (int x = 0; x < SW; x++) screen(s_want)[6 * CW + x] = 3;
  CHECK(same("huge outline"));
  // Disc of radius 1e9 whose top is row 20: apex pixel, then full rows.
  reset(0);
  disp_fill_circle_rows(screen(s_got), CW, &c, 20, 1000000020, 1000000000, 3);
  screen(s_want)[20 * CW + 20] = 3;
  for (int y = 21; y < SH; y++)
    for (int x = 0; x < SW; x++) screen(s_want)[y * CW + x] = 3;
  CHECK(same("huge disc"));
  // Rejections.
  CHECK(disp_circle_rejected(&c, INT_MIN, INT_MIN, 5));
  CHECK(disp_circle_rejected(&c, 10, 10, -1));
  CHECK(disp_circle_rejected(&c, -100, 10, 99));
  CHECK(!disp_circle_rejected(&c, -100, 10, 100));
  CHECK(!disp_circle_rejected(&c, INT_MAX, INT_MAX, INT_MAX));
  // Full-range radius and centre: defined, bounded, no guard writes.
  reset(0);
  disp_circle_big(screen(s_got), CW, &c, INT_MAX, INT_MAX, INT_MAX, 3);
  disp_fill_circle_rows(screen(s_got), CW, &c, INT_MIN, INT_MIN, INT_MAX, 3);
  for (int i = 0; i < CW * CH; i++) {
    int x = i % CW - OX, y = i / CW - OY;
    if (x < 0 || x >= SW || y < 0 || y >= SH) CHECK_EQ_U32(s_got[i], 0);
  }
}

// Model of the integer nearest-neighbour blit: dest pixel (x+c, y+r) takes
// source (c/scale, r/scale), if inside the clip.
static void ref_nn(uint16_t *fb, const disp_clip_t *cl, int x, int y,
                   const uint16_t *src, int sw, int sh, int scale, bool swap) {
  for (int r = 0; r < sh * scale; r++)
    for (int c = 0; c < sw * scale; c++)
      ref_plot(fb, cl, x + c, y + r,
               disp_px(src[(r / scale) * sw + c / scale], swap));
}

static void test_blit_nn(void) {
  uint16_t src[8 * 8];
  for (int i = 0; i < 64; i++) src[i] = (uint16_t)(0x100 + i * 7);
  int bad = 0;
  for (int i = 0; i < 20000; i++) {
    disp_clip_t c = random_clip();
    int sw = rnd(1, 8), sh = rnd(1, 8), scale = rnd(1, 5);
    int x = rnd(-45, SW + 5), y = rnd(-45, SH + 5);
    bool swap = rnd(0, 1);
    reset(0);
    disp_blit_nn(screen(s_got), CW, &c, x, y, src, sw, sh, scale, swap);
    ref_nn(screen(s_want), &c, x, y, src, sw, sh, scale, swap);
    if (!same("drawImageNN") && bad++ < 5)
      printf("  nn at (%d,%d) %dx%d scale %d clip %d,%d-%d,%d\n", x, y, sw,
             sh, scale, c.x0, c.y0, c.x1, c.y1);
  }
  CHECK_EQ_INT(bad, 0);
  // The code-review case: y = -3, scale 2 must not write row -1.
  disp_clip_t c = {0, 0, SW - 1, SH - 1};
  reset(0);
  disp_blit_nn(screen(s_got), CW, &c, -3, -3, src, 4, 4, 2, false);
  ref_nn(screen(s_want), &c, -3, -3, src, 4, 4, 2, false);
  CHECK(same("drawImageNN at (-3,-3) scale 2"));
}

// The drivers' original per-pixel partial blit.
static void ref_blit(uint16_t *fb, const disp_clip_t *cl, int x, int y,
                     const uint16_t *data, int img_w, int img_h, int sx,
                     int sy, int sw, int sh, bool flip_x, bool flip_y,
                     uint16_t key, bool swap) {
  if (sx < 0) { sw += sx; sx = 0; }
  if (sy < 0) { sh += sy; sy = 0; }
  if (sx + sw > img_w) sw = img_w - sx;
  if (sy + sh > img_h) sh = img_h - sy;
  if (sw <= 0 || sh <= 0 || !data) return;
  for (int row = 0; row < sh; row++) {
    int src_row = flip_y ? (sy + sh - 1 - row) : (sy + row);
    for (int col = 0; col < sw; col++) {
      int src_col = flip_x ? (sx + sw - 1 - col) : (sx + col);
      uint16_t c = data[src_row * img_w + src_col];
      if (key != 0 && c == key) continue;
      ref_plot(fb, cl, x + col, y + row, disp_px(c, swap));
    }
  }
}

static void test_blit_partial(void) {
  uint16_t img[12 * 10];
  int bad = 0;
  for (int i = 0; i < 40000; i++) {
    disp_clip_t c = random_clip();
    int w = rnd(1, 12), h = rnd(1, 10);
    uint16_t key = rnd(0, 1) ? 0 : 0x0102;
    for (int k = 0; k < w * h; k++)
      img[k] = (rnd(0, 3) == 0) ? 0x0102 : (uint16_t)(0x2000 + k * 13 + i);
    int sx = rnd(-4, w + 2), sy = rnd(-4, h + 2);
    int sw = rnd(-2, w + 4), sh = rnd(-2, h + 4);
    int x = rnd(-16, SW + 4), y = rnd(-16, SH + 4);
    bool fx = rnd(0, 1), fy = rnd(0, 1), swap = rnd(0, 1);
    reset(0x5555);
    disp_blit(screen(s_got), CW, &c, x, y, img, w, h, sx, sy, sw, sh, fx, fy,
              key, swap);
    ref_blit(screen(s_want), &c, x, y, img, w, h, sx, sy, sw, sh, fx, fy, key,
             swap);
    if (!same("blit") && bad++ < 5)
      printf("  blit %dx%d src %d,%d %dx%d at (%d,%d) flip %d%d key %04x\n",
             w, h, sx, sy, sw, sh, x, y, fx, fy, key);
  }
  CHECK_EQ_INT(bad, 0);
  // Huge source rect / offsets: clamped, no overflow, nothing outside.
  disp_clip_t c = {0, 0, SW - 1, SH - 1};
  reset(0);
  disp_blit(screen(s_got), CW, &c, INT_MIN, INT_MAX, img, 4, 4, 0, 0,
            INT_MAX, INT_MAX, true, true, 0, true);
  disp_blit(screen(s_got), CW, &c, 2, 2, img, 4, 4, 1, 1, INT_MAX, INT_MAX,
            false, false, 0, true);
  reset(0);
  disp_blit(screen(s_want), CW, &c, 2, 2, img, 4, 4, 1, 1, 3, 3, false, false,
            0, true);
  disp_blit(screen(s_got), CW, &c, 2, 2, img, 4, 4, 1, 1, INT_MAX, INT_MAX,
            false, false, 0, true);
  CHECK(same("huge source rect clamps to the image"));
}

static void test_copy_row_alignments(void) {
  uint16_t src[40], dst[40], want[40];
  for (int i = 0; i < 40; i++) src[i] = (uint16_t)(0x1234 + i * 0x0101);
  for (int so = 0; so < 2; so++)
    for (int d0 = 0; d0 < 2; d0++)
      for (int n = 0; n <= 30; n++) {
        memset(dst, 0, sizeof dst);
        memset(want, 0, sizeof want);
        disp_copy_row(dst + d0, src + so, n, true);
        for (int i = 0; i < n; i++) want[d0 + i] = disp_px(src[so + i], true);
        CHECK(memcmp(dst, want, sizeof dst) == 0);
      }
}

// The simulator's scaled NN: dest (dx, dy) samples (dx*sw/dw, dy*sh/dh).
static void ref_scaled(uint16_t *fb, const disp_clip_t *cl, int x, int y,
                       const uint16_t *data, int sw, int sh, int dw, int dh,
                       uint16_t key, bool swap) {
  for (int dy = 0; dy < dh; dy++)
    for (int dx = 0; dx < dw; dx++) {
      uint16_t v = data[(dy * sh / dh) * sw + dx * sw / dw];
      if (key && v == key) continue;
      ref_plot(fb, cl, x + dx, y + dy, disp_px(v, swap));
    }
}

static void test_blit_scaled(void) {
  uint16_t img[9 * 9];
  int bad = 0;
  for (int i = 0; i < 30000; i++) {
    disp_clip_t c = random_clip();
    int sw = rnd(1, 9), sh = rnd(1, 9), dw = rnd(1, 60), dh = rnd(1, 50);
    for (int k = 0; k < sw * sh; k++)
      img[k] = (rnd(0, 3) == 0) ? 0x0303 : (uint16_t)(0x4000 + k * 31 + i);
    uint16_t key = rnd(0, 1) ? 0 : 0x0303;
    int x = rnd(-60, SW + 4), y = rnd(-50, SH + 4);
    bool swap = rnd(0, 1);
    reset(0x7777);
    disp_blit_scaled(screen(s_got), CW, &c, x, y, img, sw, sh, dw, dh, key,
                     swap);
    ref_scaled(screen(s_want), &c, x, y, img, sw, sh, dw, dh, key, swap);
    if (!same("scaled") && bad++ < 5)
      printf("  scaled %dx%d -> %dx%d at (%d,%d) key %04x\n", sw, sh, dw, dh,
             x, y, key);
  }
  CHECK_EQ_INT(bad, 0);
}

// Sub-rect stretch: destination (dx, dy) of the unclipped rect samples source
// (sx + dx*sw/dw, sy + dy*sh/dh) of an img_w-wide image.
static void test_blit_scaled_rect_matches_reference(void) {
  enum { IW = 23, IH = 17 };
  uint16_t img[IW * IH];
  for (int i = 0; i < IW * IH; i++) img[i] = (uint16_t)(1 + (i * 37) % 997);
  int bad = 0;
  for (int i = 0; i < 4000; i++) {
    disp_clip_t c = random_clip();
    int sx = rnd(0, IW - 1), sy = rnd(0, IH - 1);
    int sw = rnd(1, IW - sx), sh = rnd(1, IH - sy);
    int dw = rnd(1, 90), dh = rnd(1, 90), x = rnd(-60, 60), y = rnd(-60, 60);
    uint16_t key = (i % 3 == 0) ? img[(sy * IW) + sx] : 0;
    reset(0);
    disp_blit_scaled_rect(screen(s_got), CW, &c, x, y, img, IW, sx, sy, sw, sh,
                          dw, dh, key, false);
    uint16_t *w = screen(s_want);
    for (int dy = 0; dy < dh; dy++)
      for (int dx = 0; dx < dw; dx++) {
        int px = x + dx, py = y + dy;
        if (px < c.x0 || px > c.x1 || py < c.y0 || py > c.y1) continue;
        uint16_t v = img[(sy + dy * sh / dh) * IW + sx + dx * sw / dw];
        if (!key || v != key) w[py * CW + px] = v;
      }
    if (!same("scaled rect") && bad++ < 5)
      printf("  rect src %d,%d %dx%d dst %d,%d %dx%d\n", sx, sy, sw, sh, x, y, dw, dh);
  }
  CHECK_EQ_INT(bad, 0);
}

int main(void) {
  test_clip_rect();
  test_lines_match_reference();
  test_huge_lines();
  test_triangles_match_reference();
  test_isqrt();
  test_fill_circle_rows();
  test_huge_circles();
  test_blit_nn();
  test_blit_partial();
  test_copy_row_alignments();
  test_blit_scaled();
  test_blit_scaled_rect_matches_reference();
  return check_report("test_display_clip");
}
