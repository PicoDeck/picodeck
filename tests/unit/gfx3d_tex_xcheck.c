// The textured rasteriser on the Cortex-M33 against the host, bit for bit (ctest
// test_gfx3d_tex_m33; tests/unit/CMakeLists.txt). On the M33, swapped pixels come from
// the written-out loop in gfx3d_tex.h and host-order pixels from the C, compiled for the
// M33; on the host both are the C. Built for both, the M33 builds run under qemu-arm and
// must print what the host printed, and a run fails by itself wherever swapped pixels
// are not the byte swap of host-order ones. Four parts:
//  * the pixel loop alone, across every texture shape, wild steps and multipliers over
//    their whole range, saturating and negative (what no scene reaches);
//  * whole triangles of tests/unit/gfx3d_tex_scene.h, spans and floats included;
//  * coverage against the flat rasteriser on coordinates whose differences overflow,
//    which rests on what VMINNM and VMAXNM do with a NaN;
//  * a whole scene through gfx3d_draw_tmesh and gfx3d_end, the floats of the transform,
//    fog, near clip and mip selection included.
#include "display_clip.h"
#include "gfx3d.h"
#include "gfx3d_tex_scene.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GFX3D_RAM_FUNC __attribute__((noinline))
#include "gfx3d_tex.h"

#define PIXEL_RUNS 600000
#define SCENE_TRIS 300
#define COVERAGE_TRIS 2000

static uint32_t s_rng = 0xC001D00Du;
static uint32_t rnd(void) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}

static uint32_t s_fnv = 2166136261u;
static void fnv_u16(uint16_t v) {
  s_fnv = (s_fnv ^ (v & 0xFFu)) * 16777619u;
  s_fnv = (s_fnv ^ (uint32_t)(v >> 8)) * 16777619u;
}

// ── The pixel loop ──────────────────────────────────────────────────────────

static uint16_t s_texels[1 << 20];  // 1024 x 1024, the largest gfx3d_tex_add takes

#define LOOP(name, blend, swap)                                                               \
  __attribute__((noinline)) static uint32_t name(uint16_t *d, uint16_t *e, uint32_t uv,       \
                                                 uint32_t duv, uint32_t su, uint32_t sv,      \
                                                 uint32_t vm, int32_t crg, int32_t cb) {      \
    return tex_pixels(d, e, s_texels, uv, duv, su, sv, vm, crg, cb, blend, swap);             \
  }
LOOP(plain_host, false, false)
LOOP(blend_host, true, false)
LOOP(plain_swap, false, true)
LOOP(blend_swap, true, true)

static int check_pixel_loop(void) {
  static const uint16_t corners[] = {0, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x0821, 0xF7DE};
  for (int i = 0; i < 1 << 20; i++) s_texels[i] = (uint16_t)rnd();
  for (int i = 0; i < 1 << 14; i++) s_texels[rnd() % (1u << 20)] = corners[rnd() % 7];
  static uint16_t host[64], swapped[64];
  long pixels = 0, negative = 0, saturating = 0, differ = 0;
  for (long run = 0; run < PIXEL_RUNS; run++) {
    int lw = 1 + (int)(rnd() % 10), lh = (int)(rnd() % 11u);  // past 16 bits, sv wraps round
    if (rnd() % 64 == 0) lw = lh = 0;
    uint32_t su, sv, vm, uv = rnd(), duv = rnd();
    if (rnd() % 2) duv = (duv & 0x01FF01FFu) - 0x00FF00FFu;  // small steps, and wild ones
    if (lw == 0) {
      su = 31;  // as tex_tri has it for 1 x 1
      sv = 16;
      vm = 0;
      uv = duv = 0;
    } else {
      su = 32u - (uint32_t)lw;
      sv = (uint32_t)(16 - lw - lh) & 31u;
      vm = ((1u << lh) - 1u) << lw;
    }
    int32_t crg, cb;
    switch (rnd() % 5) {
      case 0:  // the whole range tex_pack_rg can give
        crg = (int32_t)rnd();
        cb = (int32_t)(rnd() & 0x3FFFFu) - 0x20000;
        break;
      case 1:  // ordinary
        crg = (int32_t)(((rnd() % 400u) << 16) | (rnd() % 400u));
        cb = (int32_t)(rnd() % 400u);
        break;
      case 2:
        crg = (int32_t)0x7FFF7FFFu;
        cb = 131071;
        break;
      case 3:
        crg = (int32_t)0x80008000u;
        cb = -131072;
        break;
      default:
        crg = (int32_t)rnd();
        cb = (rnd() & 1u) ? -1 : 1;
        break;
    }
    if ((int16_t)crg < 0 || (crg >> 16) < 0 || cb < 0) negative++;
    if ((int16_t)crg > 256 || (crg >> 16) > 256 || cb > 256) saturating++;
    const int k = 1 + (int)(rnd() % 16);
    for (int i = 0; i < k + 2; i++) {  // two past the end: a loop must stop where told
      uint16_t c = (uint16_t)rnd();
      if (rnd() % 8 == 0) c = corners[rnd() % 7];
      host[i] = c;
      swapped[i] = disp_px(c, true);
    }
    uint32_t a, b;
    if (rnd() & 1u) {
      a = blend_host(host, host + k, uv, duv, su, sv, vm, crg, cb);
      b = blend_swap(swapped, swapped + k, uv, duv, su, sv, vm, crg, cb);
    } else {
      a = plain_host(host, host + k, uv, duv, su, sv, vm, crg, cb);
      b = plain_swap(swapped, swapped + k, uv, duv, su, sv, vm, crg, cb);
    }
    if (a != b) differ++;
    for (int i = 0; i < k + 2; i++) {
      if (host[i] != disp_px(swapped[i], true)) differ++;
      fnv_u16(host[i]);
    }
    pixels += k;
  }
  printf("pixel loop: %ld runs, %ld pixels, %ld negative, %ld saturating, hash %08lx, %ld differ\n",
         (long)PIXEL_RUNS, pixels, negative, saturating, (unsigned long)s_fnv, differ);
  return differ == 0 && negative > 0 && saturating > 0;
}

// ── Whole triangles ─────────────────────────────────────────────────────────

static uint32_t crc32_u16(const uint16_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    for (int b = 0; b < 2; b++) {
      c ^= (uint32_t)((p[i] >> (8 * b)) & 0xFF);
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

static uint16_t s_scene_texels[5461];
static uint16_t s_fb[2][TEX_SCENE_W * TEX_SCENE_H];

static int check_scene(void) {
  for (int swap = 0; swap < 2; swap++) {
    gfx3d_t *g = gfx3d_new(TEX_SCENE_W, TEX_SCENE_H);
    int slots[4];
    tex_scene_add_textures(g, s_scene_texels, slots);
    const gfx3d_target_t t = {s_fb[swap], TEX_SCENE_W, 0, 0, TEX_SCENE_W - 1, TEX_SCENE_H - 1, swap != 0, NULL, NULL};
    uint32_t s = 0x13579BDFu;
    for (int i = 0; i < SCENE_TRIS; i++) {
      gfx3d_test_vert_t v[3];
      int tex, level;
      bool blend;
      tex_scene_next(&s, v, &tex, &level, &blend);
      gfx3d_test_tex_tri(g, &t, slots[tex], level, v, blend, NULL);
    }
    printf("scene, swap %d: crc %08lx, %lu pixels\n", swap,
           (unsigned long)crc32_u16(s_fb[swap], (size_t)TEX_SCENE_W * TEX_SCENE_H),
           (unsigned long)gfx3d_get_stats(g)->pixels);
    gfx3d_free(g);
  }
  long differ = 0;
  for (size_t i = 0; i < (size_t)TEX_SCENE_W * TEX_SCENE_H; i++)
    if (s_fb[0][i] != disp_px(s_fb[1][i], true)) differ++;
  printf("scene: %ld pixels differ between byte orders\n", differ);
  return differ == 0;
}

// ── Coverage where an edge goes NaN ─────────────────────────────────────────

#define COV_W 64
#define COV_H 48

// A huge x, on either side.
static float huge_x(void) {
  const float m = 1e37f + (float)(rnd() & 0xFFFFu) * (2.9e38f / 65536.0f);
  return (rnd() & 1u) ? m : -m;
}

static int check_coverage(void) {
  static uint16_t flat[COV_W * COV_H], tex[COV_W * COV_H];
  static const uint16_t white[1] = {0xFFFF};
  gfx3d_t *g = gfx3d_new(COV_W, COV_H);
  const int slot = gfx3d_tex_add(g, 0, 0, 1, white);
  const gfx3d_target_t t = {tex, COV_W, 0, 0, COV_W - 1, COV_H - 1, false, NULL, NULL};
  const disp_clip_t c = {0, 0, COV_W - 1, COV_H - 1};
  long differ = 0, drawn = 0;
  for (int n = 0; n < COVERAGE_TRIS; n++) {
    gfx3d_test_vert_t v[3];
    memset(v, 0, sizeof v);
    for (int i = 0; i < 3; i++) {
      // On row centres, so that 0 * inf gives a NaN on the vertex's own row.
      v[i].x = (rnd() % 4u) ? huge_x() : (float)(rnd() % COV_W);
      v[i].y = (float)(rnd() % COV_H) + 0.5f;
      v[i].iw = 1.0f;
      v[i].color = 0xFFFF;
    }
    memset(flat, 0, sizeof flat);
    memset(tex, 0, sizeof tex);
    disp_fill_tri_f(flat, COV_W, &c, v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, 0xFFFF);
    gfx3d_test_tex_tri(g, &t, slot, 0, v, false, NULL);
    for (int i = 0; i < COV_W * COV_H; i++) {
      if ((flat[i] != 0) != (tex[i] != 0)) differ++;
      if (flat[i]) drawn++;
    }
  }
  gfx3d_free(g);
  printf("coverage: %d triangles, %ld pixels, %ld differ from flat\n", COVERAGE_TRIS, drawn, differ);
  return differ == 0 && drawn > 0;
}

static int check_pipeline(void) {
  for (int swap = 0; swap < 2; swap++) {
    const gfx3d_stats_t st = tex_scene_pipeline(s_fb[swap], swap != 0, s_scene_texels);
    printf("pipeline, swap %d: crc %08lx, %lu drawn, %lu culled, %lu clipped, %lu pixels\n", swap,
           (unsigned long)crc32_u16(s_fb[swap], (size_t)TEX_SCENE_W * TEX_SCENE_H),
           (unsigned long)st.drawn, (unsigned long)st.culled, (unsigned long)st.clipped,
           (unsigned long)st.pixels);
  }
  long differ = 0;
  for (size_t i = 0; i < (size_t)TEX_SCENE_W * TEX_SCENE_H; i++)
    if (s_fb[0][i] != disp_px(s_fb[1][i], true)) differ++;
  printf("pipeline: %ld pixels differ between byte orders\n", differ);
  return differ == 0;
}

int main(void) {
  const int a = check_pixel_loop(), b = check_scene(), c = check_coverage(), d = check_pipeline();
  return a && b && c && d ? 0 : 1;
}
