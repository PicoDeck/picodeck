// The boot splash's frames (src/os/splash_render.c): the timeline's
// checkpoints, time clamping, determinism, byte order, the still frame and
// the status text. SPLASH_DUMP=1 also writes splash_t*.ppm for a look.
#include "check.h"
#include "splash_logo_cells.h"
#include "splash_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 320
#define STILL_CRC 0x926efab2u  // pinned in Step 4 from the verified still frame
#define LOGO_Y0 (SPLASH_LOGO_ROW0 * 2)
#define LOGO_Y1 ((SPLASH_LOGO_ROW0 + SPLASH_LOGO_ROWS) * 2)  // exclusive

static uint16_t fb_a[W * H], fb_b[W * H];

static void render(uint16_t *fb, float t, const char *status, const char *subtext) {
  for (int i = 0; i < W * H; i++) fb[i] = 0xA5A5;  // every pixel must be drawn
  splash_render(fb, W, false, t, status, subtext);
}

static uint32_t crc32(const uint16_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    for (int b = 0; b < 2; b++) {
      c ^= (uint32_t)((p[i] >> (8 * b)) & 0xFF);
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

static int lit(uint16_t c) { return c != splash_palette[0]; }
static int outside_logo(int y) { return y < LOGO_Y0 || y >= LOGO_Y1; }

// The palette entry a pixel shows (the highest, if two entries share it).
static int entry_of(uint16_t c) {
  for (int i = SPLASH_PALETTE_SIZE - 1; i >= 0; i--)
    if (splash_palette[i] == c) return i;
  return -1;
}

static void dump(const char *name, const uint16_t *fb) {
  FILE *f = fopen(name, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) {
    const uint16_t c = fb[i];
    const unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31),
                                  (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                  (unsigned char)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static void test_dark_at_zero(void) {
  render(fb_a, 0.0f, NULL, NULL);
  int black = 0;
  for (int i = 0; i < W * H; i++) black += fb_a[i] == splash_palette[0];
  CHECK_EQ_INT(black, W * H);
  CHECK_EQ_U32(splash_palette[0], 0x0000u);
}

static void test_time_is_clamped(void) {
  render(fb_a, 0.0f, NULL, NULL);
  render(fb_b, -1.0f, NULL, NULL);
  CHECK(memcmp(fb_a, fb_b, sizeof fb_a) == 0);
  render(fb_b, NAN, NULL, NULL);
  CHECK(memcmp(fb_a, fb_b, sizeof fb_a) == 0);
  render(fb_a, SPLASH_T_END, NULL, NULL);
  const float later[] = {5.0f, 100.0f, INFINITY};
  for (int i = 0; i < 3; i++) {
    render(fb_b, later[i], NULL, NULL);
    CHECK(memcmp(fb_a, fb_b, sizeof fb_a) == 0);
  }
}

// t = 0.2: the dot. Nothing is lit more than 12 px from the centre.
static void test_dot(void) {
  render(fb_a, 0.2f, NULL, NULL);
  int n = 0, far = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (lit(fb_a[y * W + x])) {
        n++;
        far += fabsf((float)x + 0.5f - 160.0f) > 12.0f || fabsf((float)y + 0.5f - 160.0f) > 12.0f;
      }
  CHECK(n > 0);
  CHECK_EQ_INT(far, 0);
}

static int lit_columns(const uint16_t *fb) {
  int n = 0;
  for (int x = 0; x < W; x++) {
    int any = 0;
    for (int y = 0; y < H && !any; y++) any = lit(fb[y * W + x]);
    n += any;
  }
  return n;
}

// t = 0.4: the line, within 24 px of the middle row; it widens from 0.32 to 0.45.
static void test_line(void) {
  render(fb_a, 0.4f, NULL, NULL);
  int n = 0, far = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (lit(fb_a[y * W + x])) {
        n++;
        far += fabsf((float)y + 0.5f - 160.0f) > 24.0f;
      }
  CHECK(n > 0);
  CHECK_EQ_INT(far, 0);
  render(fb_a, 0.32f, NULL, NULL);
  render(fb_b, 0.45f, NULL, NULL);
  const int w0 = lit_columns(fb_a), w1 = lit_columns(fb_b);
  CHECK(w0 > 0);
  CHECK(w1 > w0);
}

// t = 0.7: the raster is open and washed out: even cell rows white, odd
// (scanline) rows entry 41 or brighter.
static void test_open_flash(void) {
  render(fb_a, 0.7f, NULL, NULL);
  int bad = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const uint16_t c = fb_a[y * W + x];
      bad += ((y / 2) % 2 == 0) ? c != 0xFFFF : entry_of(c) < 41;
    }
  CHECK_EQ_INT(bad, 0);
}

static int row_uniform(const uint16_t *fb, int y) {
  for (int x = 1; x < W; x++)
    if (fb[y * W + x] != fb[y * W]) return 0;
  return 1;
}

// t = 1.2: snow breaks the rows outside the logo; by t = 1.75 snow and
// flash are gone and only the logo is lit.
static void test_snow_then_clear(void) {
  render(fb_a, 1.2f, NULL, NULL);
  int mixed = 0;
  for (int y = 0; y < H; y++)
    if (outside_logo(y)) mixed += !row_uniform(fb_a, y);
  CHECK(mixed > 0);
  render(fb_a, 1.75f, NULL, NULL);
  int lit_out = 0;
  for (int y = 0; y < H; y++)
    if (outside_logo(y))
      for (int x = 0; x < W; x++) lit_out += lit(fb_a[y * W + x]);
  CHECK_EQ_INT(lit_out, 0);
}

// The glint changes only the logo's rows, and is gone by SPLASH_T_END.
static void test_glint(void) {
  render(fb_a, 2.0f, NULL, NULL);
  render(fb_b, 2.4f, NULL, NULL);
  int changed = 0, outside = 0;
  for (int i = 0; i < W * H; i++)
    if (fb_a[i] != fb_b[i]) {
      changed++;
      outside += outside_logo(i / W);
    }
  CHECK(changed > 0);
  CHECK_EQ_INT(outside, 0);
  render(fb_a, SPLASH_T_END, NULL, NULL);
  CHECK(memcmp(fb_a, fb_b, sizeof fb_a) != 0);
  render(fb_b, 2.0f, NULL, NULL);
  CHECK(memcmp(fb_a, fb_b, sizeof fb_a) == 0);
}

static void test_deterministic_and_swapped(void) {
  render(fb_a, 1.234f, "Loading...", NULL);
  render(fb_b, 1.234f, "Loading...", NULL);
  CHECK(memcmp(fb_a, fb_b, sizeof fb_a) == 0);
  for (int i = 0; i < W * H; i++) fb_b[i] = 0xA5A5;
  splash_render(fb_b, W, true, 1.234f, "Loading...", NULL);
  int bad = 0;
  for (int i = 0; i < W * H; i++) bad += fb_b[i] != (uint16_t)((fb_a[i] >> 8) | (fb_a[i] << 8));
  CHECK_EQ_INT(bad, 0);
}

static void test_still_frame(void) {
  render(fb_a, SPLASH_T_END, NULL, NULL);
  const uint32_t crc = crc32(fb_a, W * H);
  if (crc != STILL_CRC) printf("still frame crc32 0x%08lx\n", (unsigned long)crc);
  CHECK_EQ_U32(crc, STILL_CRC);
}

static int count_colour(const uint16_t *fb, int y0, int y1, uint16_t c) {
  int n = 0;
  for (int y = y0; y < y1; y++)
    for (int x = 0; x < W; x++) n += fb[y * W + x] == c;
  return n;
}

static void test_status_text(void) {
  const uint16_t tc = splash_palette[38];
  render(fb_a, SPLASH_T_END, NULL, NULL);
  render(fb_b, SPLASH_T_END, "Loading...", "Please wait");
  CHECK(count_colour(fb_b, SPLASH_STATUS_Y, SPLASH_STATUS_Y + 8, tc) > 0);
  CHECK(count_colour(fb_b, SPLASH_SUBTEXT_Y, SPLASH_SUBTEXT_Y + 8, tc) > 0);
  int other = 0;
  for (int i = 0; i < W * H; i++) {
    const int y = i / W;
    const int text_row = (y >= SPLASH_STATUS_Y && y < SPLASH_STATUS_Y + 8) ||
                         (y >= SPLASH_SUBTEXT_Y && y < SPLASH_SUBTEXT_Y + 8);
    other += fb_a[i] != fb_b[i] && !text_row;
  }
  CHECK_EQ_INT(other, 0);
  render(fb_b, SPLASH_T_END, "", "");  // empty strings draw nothing
  CHECK(memcmp(fb_a, fb_b, sizeof fb_a) == 0);
  render(fb_a, 2.1f, NULL, NULL);  // before SPLASH_T_TEXT: no text
  render(fb_b, 2.1f, "Loading...", "Please wait");
  CHECK(memcmp(fb_a, fb_b, sizeof fb_a) == 0);
}

// A status wider than the screen is clipped: guard columns right of the
// 320 px and guard rows below it stay untouched.
#define GW (W + 8)
#define GH (H + 4)
static uint16_t big[GW * GH];
static void test_long_status_is_clipped(void) {
  char s[201];
  memset(s, 'W', 200);
  s[200] = '\0';
  for (int i = 0; i < GW * GH; i++) big[i] = 0x1234;
  splash_render(big, GW, false, SPLASH_T_END, s, s);
  int bad = 0, text = 0;
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++) {
      const uint16_t c = big[y * GW + x];
      if (x >= W || y >= H) bad += c != 0x1234;
      else if (y >= SPLASH_STATUS_Y && y < SPLASH_STATUS_Y + 8) text += c == splash_palette[38];
    }
  CHECK_EQ_INT(bad, 0);
  CHECK(text > 0);
}

int main(void) {
  test_dark_at_zero();
  test_time_is_clamped();
  test_dot();
  test_line();
  test_open_flash();
  test_snow_then_clear();
  test_glint();
  test_deterministic_and_swapped();
  test_still_frame();
  test_status_text();
  test_long_status_is_clipped();
  if (getenv("SPLASH_DUMP")) {
    const float ts[] = {0.2f, 0.4f, 0.7f, 1.2f, 2.4f, 2.8f};
    for (int i = 0; i < 6; i++) {
      char name[32];
      snprintf(name, sizeof name, "splash_t%.2f.ppm", (double)ts[i]);
      render(fb_a, ts[i], "Loading...", NULL);
      dump(name, fb_a);
    }
  }
  return check_report("test_splash_render");
}
