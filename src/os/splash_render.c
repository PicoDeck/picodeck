// The boot splash's frames (specs/2026-10-10-boot-splash-design.md),
// ported from the crt-splash prototype app (its TV power-on, phosphor-glow
// look, geometric lettering, scanlines): dark, a dot, a line, a white raster
// opening, the picture clearing through snow, the "PicoDeck" logo, one
// glint. The maths is the chargrid shader's (charshader_math.h, noise.h),
// libm-free, so the host, the simulator and the device draw the same pixels.
#include "splash_render.h"

#include "../fonts/font.h"
#include "../fonts/font_6x8.h"
#include "charshader_math.h"
#include "noise.h"
#include "splash_logo_cells.h"

#include <stddef.h>

#define ROWS 160
#define SCR 320
#define TEXT_ENTRY 38  // the status colour: tone 0.6

enum { PH_DARK, PH_DOT, PH_LINE, PH_OPEN, PH_SETTLE };

// The state of one frame: everything that is the same for every cell.
typedef struct {
  int phase;
  float p;          // progress through the dot or the opening
  float B, F, N;    // brightness, flash, snow
  float k;          // the beam glow's radius (dot, opening)
  float h, v;       // the line's half-width; the raster's half-height
  float gl, gs;     // the glint's position and strength (0: none)
  int32_t seed;     // the snow's
} frame_t;

static const pc_font_t s_font = {FONT_6X8_FIRST, FONT_6X8_LAST, FONT_6X8_HEIGHT,
                                 FONT_6X8_WIDTH, 1, NULL, &font_6x8[0][0], NULL};

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float ease_out(float p) {
  const float q = 1.0f - p;
  return 1.0f - q * q * q;
}

static float ease_back(float p) {
  const float q = p - 1.0f;
  return 1.0f + 2.2f * q * q * q + 1.2f * q * q;
}

// The colour-ramp step chargrid uses (chargrid_ramp_index): floor(v * 63 + 0.5).
static int ramp_step(float v) {
  if (!(v >= 0.0f)) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  return (int)(v * (float)(SPLASH_PALETTE_SIZE - 1) + 0.5f);
}

static void frame_at(float t, frame_t *f) {
  *f = (frame_t){.phase = PH_DARK, .B = 1.0f};
  if (t < 0.15f) {
    f->phase = PH_DARK;
  } else if (t < 0.30f) {  // a dot in the middle
    f->phase = PH_DOT;
    f->p = (t - 0.15f) / 0.15f;
    f->k = 2.0f + 3.0f * f->p;
  } else if (t < 0.50f) {  // stretching into a line
    f->phase = PH_LINE;
    f->h = 4.0f + 156.0f * ease_out((t - 0.30f) / 0.20f);
  } else if (t < 0.85f) {  // opening into a white raster
    f->phase = PH_OPEN;
    f->p = (t - 0.50f) / 0.35f;
    f->v = 160.0f * ease_back(f->p);
    f->F = 1.0f;
    f->k = 2.5f + 30.0f * f->p;
  } else {  // the picture comes out of the flash, through snow
    f->phase = PH_SETTLE;
    const float p = clamp01((t - 0.85f) / 1.05f), q = 1.0f - p, s = 1.0f - 1.6f * p;
    f->F = q * q * q;
    f->N = s > 0.0f ? 0.35f * s * s : 0.0f;
    f->B = 1.0f + 0.2f * q;
  }
  const float gp = clamp01((t - 2.0f) / 0.8f);
  // Gated on t itself: (2.8f - 2.0f) / 0.8f rounds to just under 1, which
  // would leave a faint glint on the final frame.
  if (t > 2.0f && t < SPLASH_T_END) {
    f->gl = -40.0f + 420.0f * gp;
    f->gs = 0.6f;
  }
  f->seed = (int32_t)(t * 60.0f);
}

// The raster window (cw) and the beam glow (cg) of each column.
static void columns(const frame_t *f, float *cw, float *cg) {
  for (int x = 0; x < SPLASH_COLS; x++) {
    const float px = cs_abs((float)(x * 4 + 2 - 160));
    switch (f->phase) {
    case PH_DOT:
      cw[x] = 0.0f;
      cg[x] = cs_exp(-(px * px) / (f->k * f->k));
      break;
    case PH_LINE: {
      const float d = px - f->h;
      cw[x] = 0.0f;
      cg[x] = d > 0.0f ? cs_exp(d * d * (-1.0f / 25.0f)) : 1.0f;
      break;
    }
    case PH_OPEN:
      cw[x] = clamp01((200.0f - px) / 3.0f + 0.5f);
      cg[x] = 1.0f;
      break;
    case PH_SETTLE:
      cw[x] = clamp01((200.0f - px) + 0.5f);
      cg[x] = 0.0f;
      break;
    default:
      cw[x] = 0.0f;
      cg[x] = 0.0f;
      break;
    }
  }
}

// The raster window (rw) and the beam glow (rg) of cell row y.
static void row(const frame_t *f, int y, float *rw, float *rg) {
  const float py = cs_abs((float)(y * 2 + 1 - 160)), py2 = py * py;
  switch (f->phase) {
  case PH_DOT:
    *rw = 0.0f;
    *rg = 1.6f * f->p * cs_exp(-py2 / (f->k * f->k));
    break;
  case PH_LINE:
    *rw = 0.0f;
    *rg = 1.6f * cs_exp(py2 * (-1.0f / 6.25f)) + 0.4f * cs_exp(py2 * (-1.0f / 81.0f));
    break;
  case PH_OPEN: {
    const float e = py - f->v;
    *rw = clamp01((f->v - py) / 3.0f + 0.5f);
    *rg = (1.0f - f->p) *
          (1.6f * cs_exp(-py2 / (f->k * f->k)) + 0.8f * cs_exp(e * e * (-1.0f / 16.0f)));
    break;
  }
  case PH_SETTLE:
    *rw = clamp01((200.0f - py) + 0.5f);
    *rg = 0.0f;
    break;
  default:
    *rw = 0.0f;
    *rg = 0.0f;
    break;
  }
}

static void draw_text(uint16_t *fb, int stride, const char *text, int y, uint16_t colour) {
  if (!text || !text[0]) return;
  const int x = (SCR - font_text_width(&s_font, text)) / 2;
  font_render(&s_font, fb, stride, 0, 0, SCR - 1, SCR - 1, x, y, text, colour, 0, true);
}

void splash_render(uint16_t *fb, int stride, bool swap, float t,
                   const char *status, const char *subtext) {
  if (!(t >= 0.0f)) t = 0.0f;  // NaN too
  if (t > SPLASH_T_END) t = SPLASH_T_END;
  frame_t f;
  frame_at(t, &f);

  uint16_t pal[SPLASH_PALETTE_SIZE];
  for (int i = 0; i < SPLASH_PALETTE_SIZE; i++) {
    const uint16_t c = splash_palette[i];
    pal[i] = swap ? (uint16_t)((c >> 8) | (c << 8)) : c;
  }
  float cw[SPLASH_COLS], cg[SPLASH_COLS];
  columns(&f, cw, cg);
  const bool snow = f.N > 0.002f;
  const float inv_n = snow ? 1.0f / f.N : 0.0f;

  for (int y = 0; y < ROWS; y++) {
    float rw, rg;
    row(&f, y, &rw, &rg);
    const float k = (y & 1) ? 0.65f : 1.0f;  // the scanlines: odd rows dim
    const float bk = f.B * k;
    const int band = y - SPLASH_LOGO_ROW0;
    const bool logo = band >= 0 && band < SPLASH_LOGO_ROWS;
    uint16_t *p0 = fb + (size_t)(y * 2) * (size_t)stride;
    uint16_t *p1 = p0 + stride;
    for (int x = 0; x < SPLASH_COLS; x++, p0 += 4, p1 += 4) {
      const float win = rw * cw[x] * bk, glow = rg * cg[x] * k;
      int glyph = 0;
      float fl = 0.0f, bl = 0.0f;
      if (logo) {
        const int i = band * SPLASH_COLS + x;
        glyph = splash_logo_glyph[i];
        fl = (float)splash_logo_fg[i] * (1.0f / 255.0f);
        bl = (float)splash_logo_bg[i] * (1.0f / 255.0f);
      }
      if (f.gs > 0.0f) {  // the glint lifts the lit parts
        float g = 1.0f - cs_abs((float)(x * 4 + y) - f.gl) * 0.07f;
        g = f.gs * (g > 0.0f ? g : 0.0f);
        if (fl > 0.3f) fl += g;
        if (bl > 0.3f) bl += g;
      }
      fl += (1.0f - fl) * f.F;
      bl += (1.0f - bl) * f.F;
      float ft = fl * win + glow, bt = bl * win + glow;
      if (snow) {  // one hash: below N it is snow, and its value picks the pattern
        const float h = noise_hash(x, y, f.seed);
        if (h < f.N) {
          glyph = (int)cs_floor(h * inv_n * 254.0f) + 1;
          ft = 0.8f * win + glow;
          bt = bl * 0.4f * win + glow;
        }
      }
      const uint16_t fc = pal[ramp_step(ft)], bc = pal[ramp_step(bt)];
      for (int c = 0; c < 4; c++) {
        p0[c] = ((glyph >> (7 - c)) & 1) ? fc : bc;
        p1[c] = ((glyph >> (3 - c)) & 1) ? fc : bc;
      }
    }
  }
  if (t >= SPLASH_T_TEXT) {
    draw_text(fb, stride, status, SPLASH_STATUS_Y, pal[TEXT_ENTRY]);
    draw_text(fb, stride, subtext, SPLASH_SUBTEXT_Y, pal[TEXT_ENTRY]);
  }
}
