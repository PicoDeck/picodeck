// Textured triangles: the rasteriser behind gfx3d's textured records. Included only
// by gfx3d.c, which defines GFX3D_RAM_FUNC first.
//
// A pixel is a texel of the triangle's mip times the interpolated vertex colour. A
// triangle covers exactly the pixels disp_fill_tri_sorted covers for the same
// vertices (same edge formulas, same row split), so textured and flat meshes meet
// without cracks. Where the time goes on the device, and what follows from it:
//  * Spans run from SRAM and touch the stack only to enter and leave: the stack is
//    PSRAM there. A span's state is in registers, core ones for the pixel loop and
//    S0-S15 (which a call need not save) for the re-aim every TEX_SUB pixels. What fits
//    in neither is in s_tex, in SRAM.
//  * One span per kind (plain, blend) and per path (perspective, affine), taking the
//    texture's shape in registers: gfx3d_tex_add allows non-square textures.
//  * Triangle setup and the row loop run from flash. Most of a frame's triangles are
//    small, so rows are short and their cost matters: attributes step once per row
//    and once to the row's first pixel, with no plane equations.
//  * A triangle whose 1/z varies by under 20% is drawn affine: no division in a span.
//
// Output is the same bits on every target (tools/perf_rig's check-tex runs the M33's
// code): single-precision floats with no contraction, and every float to int conversion
// is clamped first, since out of range it is undefined and differs between x86 and ARM.
#pragma once
#include "display_clip.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// One vertex on screen: position, 1/z, uv in texture repeats, host RGB565.
typedef struct {
  float x, y, iw, u, v;
  uint16_t color;
} tex_vert_t;

#define TEX_SUB 8  // pixels between re-aims of a span: of the uv, of the colour

// 1/z varying by less than this ratio across a triangle: affine.
#define TEX_AFFINE_RATIO 1.2f
// A triangle with less area than this (px^2) takes every attribute from its top vertex:
// its gradients would be noise, and it has a pixel or two at most.
#define TEX_MIN_AREA 0.0625f

// uv in a span: u in the high half, v in the low, each 16 bits of fraction of a repeat,
// so adding a step wraps by itself. A v that wraps carries into u, 1/65536 of a repeat
// per wrap. The texel index is the top bits of each half.
#define TEX_UV_ONE 65536.0f

// A colour is a multiplier: pixel channel = (texel channel * multiplier) >> TEX_MOD_SHIFT,
// saturated. It steps with TEX_COLOR_FRAC more bits of fraction.
#define TEX_MOD_SHIFT 8
#define TEX_COLOR_FRAC 14
#define TEX_COLOR_ONE 16384.0f  // 1 << TEX_COLOR_FRAC
// A vertex channel at its maximum and a texel at its maximum mean x1.5. The offsets
// round: without them a neutral texel (2/3 of the maximum) loses half a level on average.
// They are under one level at the top texel, so black stays black.
#define TEX_MUL_RB (1.5f / 31.0f * 256.0f)
#define TEX_MUL_G (1.5f / 63.0f * 256.0f)
#define TEX_ROUND_RB 6.0f
#define TEX_ROUND_G 3.0f

// Everything converted from a float stays inside this, so that a difference of two
// values still fits an int32. As uv it is 8192 repeats either way.
#define TEX_F2I_LIMIT 536870912.0f  // 2^29

__attribute__((always_inline)) static inline float tex_clampf(float x) {
  // A NaN becomes the lower limit: fmaxf gives the number when one is a NaN.
  return fminf(fmaxf(x, -TEX_F2I_LIMIT), TEX_F2I_LIMIT);
}

__attribute__((always_inline)) static inline uint32_t tex_f2u(float x) {
  return (uint32_t)(int32_t)tex_clampf(x);
}

// A triangle's state, for its spans and its row loop. In SRAM on the device (it is .bss):
// a call to a span clobbers half the registers, and what the row loop then keeps on the
// stack is in PSRAM. One per program: gfx3d runs on one core, and nothing here outlives
// a triangle.
typedef struct {
  const uint16_t *tex;
  uint32_t su, sv, vmask;  // see tex_pixels
  uint32_t r, g, b;        // colour at the next pixel, fixed point (TEX_COLOR_FRAC)
  uint32_t dr, dg, db;     // and per TEX_SUB pixels
  int left;                // pixels after the ones being drawn
  // Affine: where the row starts, and per pixel.
  uint32_t uv, duv;
  // Perspective: 1/z, u/z and v/z with u and v in 1/65536 of a repeat, where the row
  // starts and per TEX_SUB pixels.
  float iw, uz, vz;
  float diw, duz, dvz;
  float iw_floor;  // the triangle's smallest 1/z: a step past its end divides by no less
  // The row loop's. Each attribute at column xref of the current row, and its steps.
  uint32_t r_y, g_y, b_y, uv_y;
  float iw_y, uz_y, vz_y;
  uint32_t dr_dx, dg_dx, db_dx, dr_dy, dg_dy, db_dy, duv_dy;
  float diw_dx, duz_dx, dvz_dx, diw_dy, duz_dy, dvz_dy;
  // The geometry, read once.
  void (*span)(uint16_t *d, int n);
  uint16_t *row;  // the first row
  int stride, ya, yb, xref;
  float x0, y0, x1, y1, x2, y2;
  float flo, fhi;  // the clip's x range: first column, one past the last
} tex_state_t;

static tex_state_t s_tex;

// ── The pixel loop ──────────────────────────────────────────────────────────

// The bits a blend's sum keeps: each channel without its lowest.
#define TEX_BLEND_MASK 0xF7DEu  // ~0x0821

// On the M33 the swapped loop is written out. It wants 13 of the 14 registers, and from
// C, GCC holds other things across it (s_tex's address, the uv at the start) and spills
// the loop's own state to the stack, in PSRAM, for every pixel. The C below is the
// definition, what every other target runs; check-tex holds this loop to it.
#if defined(__thumb2__) && defined(__ARM_FEATURE_DSP) && defined(__ARM_FEATURE_SAT)
#define TEX_M33_LOOP 1
#else
#define TEX_M33_LOOP 0
#endif

#if TEX_M33_LOOP
// In: e, i (bytes from e to the pixel, negative, up to 0), tex, uv, duv, su, sv, vmask,
// crg, cb. t0, t1, t2 are scratch. Leaves the modulated colour in t0.
#define TEX_M33_MODULATE                    \
  ".p2align 2\n"                            \
  "1:\n"                                    \
  "lsr    %[t0], %[uv], %[su]\n"            \
  "ror    %[t1], %[uv], %[sv]\n"            \
  "and    %[t1], %[t1], %[vmask]\n"         \
  "orr    %[t0], %[t0], %[t1]\n"            \
  "ldrh   %[t0], [%[tex], %[t0], lsl #1]\n" \
  "add    %[uv], %[uv], %[duv]\n"           \
  "lsr    %[t1], %[t0], #11\n"              \
  "ubfx   %[t2], %[t0], #5, #6\n"           \
  "and    %[t0], %[t0], #31\n"              \
  "smulbb %[t1], %[t1], %[crg]\n"           \
  "smulbt %[t2], %[t2], %[crg]\n"           \
  "mul    %[t0], %[t0], %[cb]\n"            \
  "usat   %[t1], #5, %[t1], asr #8\n"       \
  "usat   %[t2], #6, %[t2], asr #8\n"       \
  "usat   %[t0], #5, %[t0], asr #8\n"       \
  "orr    %[t0], %[t0], %[t2], lsl #5\n"    \
  "orr    %[t0], %[t0], %[t1], lsl #11\n"
#define TEX_M33_OPERANDS                                                      \
  : [i] "+r"(i), [uv] "+r"(uv), [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2) \
  : [e] "r"(e), [tex] "r"(tex), [duv] "r"(duv), [su] "r"(su), [sv] "r"(sv),    \
    [vmask] "r"(vmask), [crg] "r"(crg), [cb] "r"(cb)                          \
  : "cc", "memory"
#endif

// From `d` up to `e`, reading the texture at `uv`; returns the uv after the last pixel.
// The index is the top bits of the uv's two halves: u's by shifting right by su (never
// 32: tex_tri), v's by rotating them into place above u's bits and masking with vmask.
// crg holds two 16-bit multipliers, green's in the high half.
__attribute__((always_inline)) static inline uint32_t tex_pixels(
    uint16_t *d, uint16_t *const e, const uint16_t *const tex, uint32_t uv, const uint32_t duv,
    const uint32_t su, const uint32_t sv, const uint32_t vmask, const int32_t crg,
    const int32_t cb, const bool blend, const bool swap) {
#if TEX_M33_LOOP
  if (swap) {
    uint32_t t0, t1, t2;
    int32_t i = (int32_t)((const char *)d - (const char *)e);
    if (blend) {
      __asm__ volatile(TEX_M33_MODULATE
                       "ldrh   %[t1], [%[e], %[i]]\n"
                       "rev16  %[t1], %[t1]\n"
                       "bic    %[t0], %[t0], #0x820\n"
                       "bic    %[t0], %[t0], #1\n"
                       "bic    %[t1], %[t1], #0x820\n"
                       "bic    %[t1], %[t1], #1\n"
                       "add    %[t0], %[t0], %[t1]\n"
                       "lsr    %[t0], %[t0], #1\n"
                       "rev16  %[t0], %[t0]\n"
                       "strh   %[t0], [%[e], %[i]]\n"
                       "adds   %[i], %[i], #2\n"
                       "bne    1b\n" TEX_M33_OPERANDS);
    } else {
      __asm__ volatile(TEX_M33_MODULATE
                       "rev16  %[t0], %[t0]\n"
                       "strh   %[t0], [%[e], %[i]]\n"
                       "adds   %[i], %[i], #2\n"
                       "bne    1b\n" TEX_M33_OPERANDS);
    }
    return uv;
  }
#endif
  do {
    const uint32_t t = tex[(uv >> su) | (((uv >> sv) | (uv << (-sv & 31u))) & vmask)];
    uv += duv;
    int32_t pr = ((int32_t)(int16_t)(t >> 11) * (int16_t)crg) >> TEX_MOD_SHIFT;
    int32_t pg = ((int32_t)(int16_t)((t >> 5) & 63u) * (crg >> 16)) >> TEX_MOD_SHIFT;
    int32_t pb = ((int32_t)(t & 31u) * cb) >> TEX_MOD_SHIFT;
    pr = pr < 0 ? 0 : pr > 31 ? 31 : pr;
    pg = pg < 0 ? 0 : pg > 63 ? 63 : pg;
    pb = pb < 0 ? 0 : pb > 31 ? 31 : pb;
    uint32_t c = ((uint32_t)pr << 11) | ((uint32_t)pg << 5) | (uint32_t)pb;
    if (blend) {
      const uint32_t under = disp_px(*d, swap);
      // (c >> 1 & 0x7BEF) + (under >> 1 & 0x7BEF)
      c = ((c & TEX_BLEND_MASK) + (under & TEX_BLEND_MASK)) >> 1;
    }
    *d++ = disp_px((uint16_t)c, swap);
  } while (d != e);
  return uv;
}

// Green's multiplier over red's. Inside a triangle both are at most ~390; one past 16
// bits, on a sliver that came through TEX_MIN_AREA, comes out wrong on that pixel.
__attribute__((always_inline)) static inline int32_t tex_pack_rg(uint32_t r, uint32_t g) {
  const uint32_t cr = (uint32_t)((int32_t)r >> TEX_COLOR_FRAC);
  const uint32_t cg = (uint32_t)((int32_t)g >> TEX_COLOR_FRAC);
  return (int32_t)((cg << 16) | (cr & 0xFFFFu));
}

// ── Spans ───────────────────────────────────────────────────────────────────
// What outlives a pixel loop with no register left for it stays in s_tex, where the
// loop's "memory" clobber makes the compiler read it back: how far along the span, the
// colour, and vmask and iw_floor, which spare a register there for what runs in between.

__attribute__((always_inline)) static inline void tex_span_affine(uint16_t *d, int n,
                                                                  const bool blend,
                                                                  const bool swap) {
  const uint16_t *const tex = s_tex.tex;
  const uint32_t su = s_tex.su, sv = s_tex.sv, duv = s_tex.duv;
  uint32_t uv = s_tex.uv;
  for (;;) {
    const int k = n < TEX_SUB ? n : TEX_SUB;
    uint16_t *const e = d + k;
    s_tex.left = n - k;
    uv = tex_pixels(d, e, tex, uv, duv, su, sv, s_tex.vmask, tex_pack_rg(s_tex.r, s_tex.g),
                    (int32_t)s_tex.b >> TEX_COLOR_FRAC, blend, swap);
    n = s_tex.left;
    if (!n) return;
    d = e;
    s_tex.r += s_tex.dr;
    s_tex.g += s_tex.dg;
    s_tex.b += s_tex.db;
  }
}

// The uv is exact at the start of each TEX_SUB pixels and linear between. One division
// per stretch, in S registers, so it costs no memory. The end of the last stretch can
// be one pixel past the triangle, where 1/z may be anything: iw_floor keeps that sane.
__attribute__((always_inline)) static inline void tex_span_persp(uint16_t *d, int n,
                                                                 const bool blend,
                                                                 const bool swap) {
  const uint16_t *const tex = s_tex.tex;
  const uint32_t su = s_tex.su, sv = s_tex.sv;
  const float diw = s_tex.diw, duz = s_tex.duz, dvz = s_tex.dvz;
  float iw = s_tex.iw, uz = s_tex.uz, vz = s_tex.vz;
  float z = 1.0f / fmaxf(iw, s_tex.iw_floor);
  float u0 = tex_clampf(uz * z), v0 = tex_clampf(vz * z);
  for (;;) {
    int k = TEX_SUB;
    float per_pixel = 1.0f / (float)TEX_SUB;
    if (n >= TEX_SUB) {
      iw += diw;
      uz += duz;
      vz += dvz;
    } else {
      k = n;
      const float fk = (float)k, part = fk * (1.0f / (float)TEX_SUB);
      iw += diw * part;
      uz += duz * part;
      vz += dvz * part;
      per_pixel = 1.0f / fk;
    }
    z = 1.0f / fmaxf(iw, s_tex.iw_floor);
    const float u1 = tex_clampf(uz * z), v1 = tex_clampf(vz * z);
    const uint32_t du = (uint32_t)(int32_t)((u1 - u0) * per_pixel);
    const uint32_t dv = (uint32_t)(int32_t)((v1 - v0) * per_pixel);
    const uint32_t uv = ((uint32_t)(int32_t)u0 << 16) | ((uint32_t)(int32_t)v0 & 0xFFFFu);
    uint16_t *const e = d + k;
    s_tex.left = n - k;
    tex_pixels(d, e, tex, uv, (du << 16) + dv, su, sv, s_tex.vmask,
               tex_pack_rg(s_tex.r, s_tex.g), (int32_t)s_tex.b >> TEX_COLOR_FRAC, blend, swap);
    n = s_tex.left;
    if (!n) return;
    d = e;
    u0 = u1;
    v0 = v1;
    s_tex.r += s_tex.dr;
    s_tex.g += s_tex.dg;
    s_tex.b += s_tex.db;
  }
}

// The firmware's framebuffer is byte-swapped: those four run from SRAM. `used`, as until
// a scene draws textured records nothing else keeps them, and the map's figure should be
// the real one. Host-order targets (the simulator, an off-screen image) get the same
// code from ordinary memory.
#define TEX_SPANS(name, body, blend)                                                 \
  GFX3D_RAM_FUNC __attribute__((used)) static void name##_swap(uint16_t *d, int n) { \
    body(d, n, blend, true);                                                         \
  }                                                                                  \
  __attribute__((noinline)) static void name##_host(uint16_t *d, int n) {            \
    body(d, n, blend, false);                                                        \
  }

TEX_SPANS(tex_affine_plain, tex_span_affine, false)
TEX_SPANS(tex_affine_blend, tex_span_affine, true)
TEX_SPANS(tex_persp_plain, tex_span_persp, false)
TEX_SPANS(tex_persp_blend, tex_span_persp, true)

// ── Rows ────────────────────────────────────────────────────────────────────

// disp_ceil_px(x, cx0, cxe) of a number, with flo = cx0 and fhi = cxe, without branches.
__attribute__((always_inline)) static inline int tex_ceil_px(float x, float flo, float fhi) {
  return (int)ceilf(fminf(fmaxf(x, flo), fhi));
}

// Row by row as disp_fill_tri_sorted goes, every edge from its upper end, handing each
// row's visible span to the span function. Returns the pixels drawn.
__attribute__((always_inline)) static inline uint32_t tex_walk(const bool persp) {
  tex_state_t *const s = &s_tex;
  const float x0 = s->x0, y0 = s->y0, x1 = s->x1, y1 = s->y1, x2 = s->x2, y2 = s->y2;
  const float flo = s->flo, fhi = s->fhi;
  const float s02 = (x2 - x0) / (y2 - y0);
  uint16_t *row = s->row;
  const int stride = s->stride, ya = s->ya, yb = s->yb, xref = s->xref;
  // The first row at or below the middle vertex: !(yc < y1). y1 - 0.5f may round, so the
  // rows' own test has the last word.
  int ym = disp_ceil_px(y1 - 0.5f, ya, yb);
  while (ym > ya && !((float)(ym - 1) + 0.5f < y1)) ym--;
  while (ym < yb && (float)ym + 0.5f < y1) ym++;

  uint32_t pixels = 0;
  float yc = (float)ya + 0.5f;
  int py = ya;
  for (int lower = 0; lower < 2; lower++) {
    const int end = lower ? yb : ym;
    if (py >= end) continue;
    // The short side: from the top vertex, then from the middle one.
    const float xs = lower ? x1 : x0, ys = lower ? y1 : y0;
    const float ss = lower ? (x2 - x1) / (y2 - y1) : (x1 - x0) / (y1 - y0);
    do {
      float xl = x0 + (yc - y0) * s02, xr = xs + (yc - ys) * ss;
      // Where a side has no height one is a NaN, and disp_tri_row draws nothing. So it
      // does here: a NaN ends up in xr, tex_ceil_px makes that the first column, and n
      // is not above 0.
      if (!(xl <= xr)) {
        const float t = xl;
        xl = xr;
        xr = t;
      }
      const int xa = tex_ceil_px(xl - 0.5f, flo, fhi);
      const int n = tex_ceil_px(xr - 0.5f, flo, fhi) - xa;
      if (n > 0) {
        pixels += (uint32_t)n;
        const uint32_t dx = (uint32_t)(xa - xref);
        s->r = s->r_y + dx * s->dr_dx;
        s->g = s->g_y + dx * s->dg_dx;
        s->b = s->b_y + dx * s->db_dx;
        if (persp) {
          const float fdx = (float)(xa - xref);
          s->iw = s->iw_y + fdx * s->diw_dx;
          s->uz = s->uz_y + fdx * s->duz_dx;
          s->vz = s->vz_y + fdx * s->dvz_dx;
        } else {
          s->uv = s->uv_y + dx * s->duv;
        }
        s->span(row + xa, n);
      }
      row += stride;
      yc += 1.0f;
      s->r_y += s->dr_dy;
      s->g_y += s->dg_dy;
      s->b_y += s->db_dy;
      if (persp) {
        s->iw_y += s->diw_dy;
        s->uz_y += s->duz_dy;
        s->vz_y += s->dvz_dy;
      } else {
        s->uv_y += s->duv_dy;
      }
    } while (++py < end);
  }
  return pixels;
}

__attribute__((noinline)) static uint32_t tex_walk_affine(void) { return tex_walk(false); }
__attribute__((noinline)) static uint32_t tex_walk_persp(void) { return tex_walk(true); }

// ── A triangle ──────────────────────────────────────────────────────────────

#ifdef GFX3D_TEST
static bool s_tex_test_affine;
static int s_tex_test_rows;
#endif

// Draws one triangle into fb, clipped to the inclusive rect (not empty), with the
// 2^lw x 2^lh texels at px. Returns the pixels it wrote.
__attribute__((unused)) static uint32_t tex_tri(uint16_t *fb, int stride, int cx0, int cy0, int cx1,
                                                int cy1, const tex_vert_t *a, const tex_vert_t *b,
                                                const tex_vert_t *c, const uint16_t *px, int lw,
                                                int lh, bool blend, bool swap) {
#ifdef GFX3D_TEST
  s_tex_test_affine = false;
  s_tex_test_rows = 0;
#endif
  // disp_tri_sort, keeping the vertices' other values with them.
  const float fin = (a->x - a->x) + (a->y - a->y) + (b->x - b->x) + (b->y - b->y) +
                    (c->x - c->x) + (c->y - c->y);
  if (fin != fin) return 0;
  const tex_vert_t *t;
  if (b->y < a->y || (b->y == a->y && b->x < a->x)) { t = a; a = b; b = t; }
  if (c->y < a->y || (c->y == a->y && c->x < a->x)) { t = a; a = c; c = t; }
  if (c->y < b->y || (c->y == b->y && c->x < b->x)) { t = b; b = c; c = t; }
  const float x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y, x2 = c->x, y2 = c->y;

  if (!(y2 > y0)) return 0;  // zero height
  const int ya = disp_ceil_px(y0 - 0.5f, cy0, cy1 + 1);
  const int yb = disp_ceil_px(y2 - 0.5f, cy0, cy1 + 1);  // exclusive
  if (ya >= yb) return 0;

  float u0 = a->u, u1 = b->u, u2 = c->u, v0 = a->v, v1 = b->v, v2 = c->v;
  if (lw == 0) {
    // One texel wide is laid out as one texel high: read it as a row, along v. So u has
    // a bit to give whenever there is more than one texel, and su is never 32.
    float s;
    s = u0; u0 = v0; v0 = s;
    s = u1; u1 = v1; v1 = s;
    s = u2; u2 = v2; v2 = s;
    lw = lh;
    lh = 0;
  }
  const bool single = lw == 0;  // 1 x 1: the uv is always 0, so (uv >> 31) too
  const float iw0 = a->iw, iw1 = b->iw, iw2 = c->iw;
  const float iw_min = fminf(fminf(iw0, iw1), iw2), iw_max = fmaxf(fmaxf(iw0, iw1), iw2);
  const bool affine = single || iw_max < TEX_AFFINE_RATIO * iw_min;

  s_tex.tex = px;
  s_tex.su = single ? 31u : (uint32_t)(32 - lw);
  s_tex.sv = (uint32_t)(16 - lw - lh) & 31u;
  s_tex.vmask = ((1u << lh) - 1u) << lw;

  // Attributes start at a column on screen, near the top vertex: any pixel is less than
  // a clip's width from it, and the first row is where they start down the triangle.
  const float flo = (float)cx0, fhi = (float)(cx1 + 1);
  const int xref = (int)fminf(fmaxf(x0, flo), (float)cx1);
  s_tex.row = fb + (size_t)ya * stride;
  s_tex.stride = stride;
  s_tex.ya = ya;
  s_tex.yb = yb;
  s_tex.xref = xref;
  s_tex.x0 = x0;
  s_tex.y0 = y0;
  s_tex.x1 = x1;
  s_tex.y1 = y1;
  s_tex.x2 = x2;
  s_tex.y2 = y2;
  s_tex.flo = flo;
  s_tex.fhi = fhi;
  const float fx = (float)xref + 0.5f - x0, fy = (float)ya + 0.5f - y0;

  // An attribute's gradient is (d1 * gxa - d2 * gxb, d2 * gya - d1 * gyb), d1 and d2 its
  // differences from the top vertex. Too little area to trust, or coordinates beyond any
  // screen (a NaN or infinite area), makes them all 0: flat, from the top vertex.
  const float dx1 = x1 - x0, dy1 = y1 - y0, dx2 = x2 - x0, dy2 = y2 - y0;
  const float area = dx1 * dy2 - dx2 * dy1;
  float gxa = 0.0f, gxb = 0.0f, gya = 0.0f, gyb = 0.0f;
  if (fabsf(area) >= TEX_MIN_AREA && !isinf(area)) {
    const float inv = 1.0f / area;
    gxa = dy2 * inv;
    gxb = dy1 * inv;
    gya = dx1 * inv;
    gyb = dx2 * inv;
  }

#define TEX_CHANNEL(name, shift, mask, mul, rnd)                                            \
  {                                                                                         \
    const int c0 = (a->color >> (shift)) & (mask);                                          \
    const float d1 = (float)(((b->color >> (shift)) & (mask)) - c0);                        \
    const float d2 = (float)(((c->color >> (shift)) & (mask)) - c0);                        \
    const float scale = (mul) * TEX_COLOR_ONE;                                              \
    const float gx = (d1 * gxa - d2 * gxb) * scale, gy = (d2 * gya - d1 * gyb) * scale;     \
    s_tex.name##_y = tex_f2u(((float)c0 * (mul) + (rnd)) * TEX_COLOR_ONE + gx * fx + gy * fy); \
    s_tex.d##name##_dx = tex_f2u(gx);                                                       \
    s_tex.d##name##_dy = tex_f2u(gy);                                                       \
    s_tex.d##name = (uint32_t)TEX_SUB * s_tex.d##name##_dx;                                 \
  }
  TEX_CHANNEL(r, 11, 31, TEX_MUL_RB, TEX_ROUND_RB)
  TEX_CHANNEL(g, 5, 63, TEX_MUL_G, TEX_ROUND_G)
  TEX_CHANNEL(b, 0, 31, TEX_MUL_RB, TEX_ROUND_RB)
#undef TEX_CHANNEL

  // Whole repeats off the top vertex: the fractions keep their bits, and the fixed point
  // has the range. Differences need no such help.
  const float fu0 = u0 - floorf(u0), fv0 = v0 - floorf(v0);
  const float du1 = u1 - u0, du2 = u2 - u0, dv1 = v1 - v0, dv2 = v2 - v0;
#ifdef GFX3D_TEST
  s_tex_test_affine = affine;
  s_tex_test_rows = yb - ya;
#endif
  if (affine) {
    const float gux = (du1 * gxa - du2 * gxb) * TEX_UV_ONE, guy = (du2 * gya - du1 * gyb) * TEX_UV_ONE;
    const float gvx = (dv1 * gxa - dv2 * gxb) * TEX_UV_ONE, gvy = (dv2 * gya - dv1 * gyb) * TEX_UV_ONE;
    const uint32_t pu = tex_f2u(fu0 * TEX_UV_ONE + gux * fx + guy * fy);
    const uint32_t pv = tex_f2u(fv0 * TEX_UV_ONE + gvx * fx + gvy * fy);
    // Rounded: a span adds its step up, and only this ends a long one near the right
    // texel. A negative dv in the sum borrows from u what its low half will carry back.
    s_tex.duv = (tex_f2u(floorf(gux + 0.5f)) << 16) + tex_f2u(floorf(gvx + 0.5f));
    s_tex.duv_dy = (tex_f2u(floorf(guy + 0.5f)) << 16) + tex_f2u(floorf(gvy + 0.5f));
    s_tex.uv_y = (pu << 16) | (pv & 0xFFFFu);
    if (single) s_tex.uv_y = s_tex.duv = s_tex.duv_dy = 0;
    s_tex.span = swap ? (blend ? tex_affine_blend_swap : tex_affine_plain_swap)
                      : (blend ? tex_affine_blend_host : tex_affine_plain_host);
    return tex_walk_affine();
  }
  {
    const float k0 = iw0 * TEX_UV_ONE, k1 = iw1 * TEX_UV_ONE, k2 = iw2 * TEX_UV_ONE;
    const float uz0 = fu0 * k0, vz0 = fv0 * k0;
    const float duz1 = (fu0 + du1) * k1 - uz0, duz2 = (fu0 + du2) * k2 - uz0;
    const float dvz1 = (fv0 + dv1) * k1 - vz0, dvz2 = (fv0 + dv2) * k2 - vz0;
    const float diw1 = iw1 - iw0, diw2 = iw2 - iw0;
    const float gix = diw1 * gxa - diw2 * gxb, giy = diw2 * gya - diw1 * gyb;
    const float gux = duz1 * gxa - duz2 * gxb, guy = duz2 * gya - duz1 * gyb;
    const float gvx = dvz1 * gxa - dvz2 * gxb, gvy = dvz2 * gya - dvz1 * gyb;
    s_tex.diw_dx = gix;
    s_tex.diw_dy = giy;
    s_tex.duz_dx = gux;
    s_tex.duz_dy = guy;
    s_tex.dvz_dx = gvx;
    s_tex.dvz_dy = gvy;
    s_tex.diw = gix * (float)TEX_SUB;
    s_tex.duz = gux * (float)TEX_SUB;
    s_tex.dvz = gvx * (float)TEX_SUB;
    // Above 0 whatever came in (NaN included): no division by a zero of either sign.
    s_tex.iw_floor = fmaxf(iw_min, 1e-30f);
    s_tex.iw_y = iw0 + gix * fx + giy * fy;
    s_tex.uz_y = uz0 + gux * fx + guy * fy;
    s_tex.vz_y = vz0 + gvx * fx + gvy * fy;
    s_tex.span = swap ? (blend ? tex_persp_blend_swap : tex_persp_plain_swap)
                      : (blend ? tex_persp_blend_host : tex_persp_plain_host);
    return tex_walk_persp();
  }
}
