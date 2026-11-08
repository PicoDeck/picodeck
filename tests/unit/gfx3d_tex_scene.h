// A fixed set of textured triangles, the same on every target: what pins the
// rasteriser's output (test_gfx3d.c's test_tex_scene_crc) and, through
// tools/perf_rig, checks that the M33's code draws the same bytes as a host's.
// Pure integer generation (floats only from integer / power of two), so the input
// is identical everywhere before the first rasteriser instruction runs. Needs
// GFX3D_TEST (the test-only triangle entry) and a context with no textures yet.
#pragma once
#include "gfx3d.h"
#include <stdint.h>

#define TEX_SCENE_W 320
#define TEX_SCENE_H 320
#define TEX_SCENE_TRIS 3000

static uint32_t tex_scene_rand(uint32_t *s) {
  uint32_t x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x;
  return x;
}

// In [lo, hi), to 1/256.
static float tex_scene_float(uint32_t *s, int lo, int hi) {
  const uint32_t span = (uint32_t)(hi - lo) * 256u;
  return (float)lo + (float)(tex_scene_rand(s) % span) * (1.0f / 256.0f);
}

static uint16_t tex_scene_texel(uint32_t seed, int x, int y) {
  uint32_t h = seed + (uint32_t)x * 2654435761u + (uint32_t)y * 40503u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return (uint16_t)h;
}

// Adds four textures, each a full chain of mips; `px` holds at least 5461 texels.
static void tex_scene_add_textures(gfx3d_t *g, uint16_t *px, int *slots) {
  static const uint8_t lg[4][3] = {{6, 6, 4}, {5, 2, 3}, {2, 5, 3}, {1, 1, 2}};
  for (int t = 0; t < 4; t++) {
    int n = 0;
    for (int k = 0; k < lg[t][2]; k++)
      for (int y = 0; y < 1 << (lg[t][1] - k); y++)
        for (int x = 0; x < 1 << (lg[t][0] - k); x++)
          px[n++] = tex_scene_texel((uint32_t)(t * 7 + k), x, y);
    slots[t] = gfx3d_tex_add(g, lg[t][0], lg[t][1], lg[t][2], px);
  }
}

// One triangle of the set, from the generator's state.
static void tex_scene_next(uint32_t *s, gfx3d_test_vert_t v[3], int *tex, int *level, bool *blend) {
  const uint32_t kind = tex_scene_rand(s) % 8u;
  const int span = kind == 0 ? 6 : kind == 1 ? 24 : kind == 2 ? 2000 : 120;
  const int cx = (int)(tex_scene_rand(s) % TEX_SCENE_W), cy = (int)(tex_scene_rand(s) % TEX_SCENE_H);
  // A depth at the first vertex, the rest the same (affine), up to 1/8 away (either,
  // by the triangle's size in texels) or several times (perspective).
  const float iw0 = tex_scene_float(s, 1, 200) * (1.0f / 256.0f);
  const uint32_t depth = tex_scene_rand(s) % 3u;
  for (int i = 0; i < 3; i++) {
    v[i].x = tex_scene_float(s, cx - span, cx + span);
    v[i].y = tex_scene_float(s, cy - span, cy + span);
    v[i].iw = depth == 0   ? iw0
              : depth == 1 ? iw0 * (1.0f + tex_scene_float(s, 0, 1) * 0.125f)
                           : iw0 * tex_scene_float(s, 1, 8);
    v[i].u = tex_scene_float(s, -6, 6);
    v[i].v = tex_scene_float(s, -6, 6);
    v[i].color = (uint16_t)tex_scene_rand(s);
  }
  *tex = (int)(tex_scene_rand(s) % 4u);
  *level = (int)(tex_scene_rand(s) % 5u);  // past the last level: clamped
  *blend = (tex_scene_rand(s) % 4u) == 0;
}

// The whole pipeline over one scene: ground through the near plane under fog, in every
// texture, with some triangles unfogged or double-sided, a blended sheet of water, a turned
// block, and flat triangles and a sprite among them. Returns the stats. `px` as above.
__attribute__((unused)) static gfx3d_stats_t tex_scene_pipeline(uint16_t *fb, bool swap, uint16_t *px) {
  enum { N = 16, NV = (N + 1) * (N + 1), NT = 2 * N * N };
  static float xyz[3 * NV], uv[2 * NV];
  static uint16_t col[NV], idx[3 * NT];
  static uint8_t tex[NT], fl[NT];
  gfx3d_t *g = gfx3d_new(TEX_SCENE_W, TEX_SCENE_H);
  int slots[4];
  tex_scene_add_textures(g, px, slots);
  // Not the default field of view: newlib's and glibc's tanf differ by one ulp there, and
  // agree here, so that every target gets the same picture.
  gfx3d_set_projection(g, 1.05f, 0.5f, 1000.0f);
  gfx3d_set_fog(g, true, 6.0f, 40.0f, 0x8C71);
  const float eye[3] = {1.5f, 2.0f, 3.0f}, at[3] = {0.0f, 0.0f, -12.0f}, up[3] = {0.0f, 1.0f, 0.0f};
  gfx3d_look_at(g, eye, at, up);
  uint32_t s = 0x2F6E2B1u;
  for (int r = 0; r <= N; r++)
    for (int c = 0; c <= N; c++) {
      const int v = r * (N + 1) + c;
      xyz[3 * v] = -30.0f + 3.75f * (float)c;
      xyz[3 * v + 1] = tex_scene_float(&s, -1, 1);
      xyz[3 * v + 2] = 6.0f - 4.0f * (float)r;
      uv[2 * v] = xyz[3 * v] * 0.25f;
      uv[2 * v + 1] = xyz[3 * v + 2] * 0.25f;
      col[v] = (uint16_t)(tex_scene_rand(&s) | 0x4208u);
    }
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) {
      const uint16_t a = (uint16_t)(r * (N + 1) + c), b = (uint16_t)(a + 1);
      const uint16_t d = (uint16_t)(a + N + 1), e = (uint16_t)(d + 1);
      const uint16_t tri[6] = {a, b, e, a, e, d};
      const int t = 2 * (r * N + c);
      for (int k = 0; k < 6; k++) idx[3 * t + k] = tri[k];
      for (int k = 0; k < 2; k++) {
        tex[t + k] = (uint8_t)slots[(r + c) % 4];
        fl[t + k] = (uint8_t)(((t + k) % 7 == 0 ? GFX3D_TM_NO_FOG : 0u) |
                              ((t + k) % 11 == 0 ? GFX3D_TM_DOUBLE_SIDED : 0u));
      }
    }
  const gfx3d_tmesh_desc_t gd = {NV, NT, xyz, uv, col, idx, tex, fl};
  gfx3d_tmesh_t *ground = gfx3d_tmesh_new(&gd);
  // The same grid as water: only the blend differs, up a little and in one texture.
  for (int t = 0; t < NT; t++) {
    tex[t] = (uint8_t)slots[0];
    fl[t] = GFX3D_TM_BLEND;
  }
  gfx3d_tmesh_t *water = gfx3d_tmesh_new(&gd);
  // A turned block.
  static const float bx[24] = {-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
                               -1, -1, 1,  1, -1, 1,  1, 1, 1,  -1, 1, 1};
  static const float buv[16] = {0, 0, 1, 0, 1, 1, 0, 1, 2, 0, 3, 0, 3, 1, 2, 1};
  static const uint16_t bi[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 4, 7, 0, 7, 3,
                                  1, 2, 6, 1, 6, 5, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2};
  static const uint16_t bc[8] = {0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0x7BEF};
  uint8_t bt[12];
  for (int t = 0; t < 12; t++) bt[t] = (uint8_t)slots[t & 3];
  const gfx3d_tmesh_desc_t bd = {8, 12, bx, buv, bc, bi, bt, NULL};
  gfx3d_tmesh_t *block = gfx3d_tmesh_new(&bd);
  gfx3d_mesh_t *flat = gfx3d_mesh_alloc(4, 2);
  static const float fx[12] = {-2, 0, -8, 2, 0, -8, 2, 3, -8, -2, 3, -8};
  static const uint16_t fi[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 12; i++) flat->xyz[i] = fx[i];
  for (int i = 0; i < 6; i++) flat->idx[i] = fi[i];
  flat->color[0] = flat->color[1] = 0x3333;
  gfx3d_mesh_finish(flat);
  uint16_t img[16];
  for (int i = 0; i < 16; i++) img[i] = (i & 5) == 5 ? 0xF81F : (uint16_t)(0x07E0 + 77 * i);
  const int spr = gfx3d_image_add(g, 4, 4, 0xF81F, img);

  static const float rot[9] = {0.8f, 0.0f, 0.6f, 0.36f, 0.8f, -0.48f, -0.48f, 0.6f, 0.64f};
  const float at0[3] = {0.0f, 0.0f, 0.0f}, wet[3] = {0.0f, 0.25f, 0.0f}, bp[3] = {-3.0f, 2.0f, -6.0f};
  const float sp[3] = {3.0f, 1.5f, -5.0f};
  gfx3d_begin(g, true, 0x2104);
  gfx3d_draw_tmesh(g, ground, NULL, at0, 1.0f, 0.0f);
  gfx3d_draw(g, flat, rot, bp, 0.5f, 0.0f, false);
  gfx3d_draw_tmesh(g, block, rot, bp, 1.5f, 0.0f);
  gfx3d_draw_sprite(g, spr, sp, 2.0f, 0, 0, 4, 4, 0.0f);
  gfx3d_draw_tmesh(g, water, NULL, wet, 1.0f, -0.5f);
  for (int i = 0; i < TEX_SCENE_W * TEX_SCENE_H; i++) fb[i] = 0;
  const gfx3d_target_t t = {fb, TEX_SCENE_W, 0, 0, TEX_SCENE_W - 1, TEX_SCENE_H - 1, swap,
                            gfx3d_image_resolve, g};
  gfx3d_end(g, &t);
  const gfx3d_stats_t st = *gfx3d_get_stats(g);
  gfx3d_tmesh_free(ground);
  gfx3d_tmesh_free(water);
  gfx3d_tmesh_free(block);
  gfx3d_mesh_free(flat);
  gfx3d_free(g);
  return st;
}

// A reflecting frame, gfx3d.h's two scenes, through the near plane in both: a sky image, land
// rising from behind the eye, trees, moving water. Returns the two scenes' stats, added up. `px`
// as above; a look-at camera, as the turn's sinf and cosf differ between libraries.
__attribute__((unused)) static gfx3d_stats_t tex_scene_reflection(uint16_t *fb, bool swap, uint16_t *px) {
  enum { N = 10, NV = (N + 1) * (N + 1), NT = 2 * N * N };
  static float xyz[3 * NV], uv[2 * NV];
  static uint16_t col[NV], idx[3 * NT];
  static uint8_t tex[NT];
  gfx3d_t *g = gfx3d_new(TEX_SCENE_W, TEX_SCENE_H);
  int slots[4];
  tex_scene_add_textures(g, px, slots);
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 64; x++) px[(y << 6) + x] = tex_scene_texel(200, x, y);
  const int sky = gfx3d_tex_add(g, 6, 5, 1, px);
  gfx3d_set_sky_image(g, sky, 20);
  gfx3d_set_projection(g, 1.05f, 0.5f, 1000.0f);
  gfx3d_set_fog(g, true, 8.0f, 60.0f, 0x8C71);
  gfx3d_set_sprite_fog(g, true);
  const float eye[3] = {1.0f, 3.5f, 0.5f}, at[3] = {-1.0f, 2.0f, -16.0f}, up[3] = {0.0f, 1.0f, 0.0f};
  gfx3d_look_at(g, eye, at, up);
  // Banks of land to either side of the water, from behind the eye to far away, and rising.
  uint32_t s = 0x51D7A3Fu;
  for (int r = 0; r <= N; r++)
    for (int c = 0; c <= N; c++) {
      const int v = r * (N + 1) + c;
      const float side = (float)c - 5.0f;
      xyz[3 * v] = side * 4.0f;
      xyz[3 * v + 1] = (side * side) * 0.3f + (float)(r * r) * 0.05f + tex_scene_float(&s, 0, 1) * 0.5f - 1.0f;
      xyz[3 * v + 2] = 3.0f - 4.0f * (float)r;
      uv[2 * v] = xyz[3 * v] * 0.25f;
      uv[2 * v + 1] = xyz[3 * v + 2] * 0.25f;
      col[v] = (uint16_t)(tex_scene_rand(&s) | 0x4208u);
    }
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) {
      const uint16_t a = (uint16_t)(r * (N + 1) + c), b = (uint16_t)(a + 1);
      const uint16_t d = (uint16_t)(a + N + 1), e = (uint16_t)(d + 1);
      const uint16_t tri[6] = {a, b, e, a, e, d};
      const int t = 2 * (r * N + c);
      for (int k = 0; k < 6; k++) idx[3 * t + k] = tri[k];
      tex[t] = tex[t + 1] = (uint8_t)slots[(r + c) % 4];
    }
  const gfx3d_tmesh_desc_t gd = {NV, NT, xyz, uv, col, idx, tex, NULL};
  gfx3d_tmesh_t *land = gfx3d_tmesh_new(&gd);
  static const float wx[12] = {-30, 0, 2, 30, 0, 2, 30, 0, -36, -30, 0, -36};
  static const float wuv[8] = {0, 0, 7.5f, 0, 7.5f, 9.5f, 0, 9.5f};
  static const uint16_t wc[4] = {0xFFFF, 0xCE79, 0xAD55, 0xFFFF}, wi[6] = {0, 1, 2, 0, 2, 3};
  const uint8_t wt[2] = {(uint8_t)slots[0], (uint8_t)slots[0]};
  const uint8_t wf[2] = {GFX3D_TM_BLEND, GFX3D_TM_BLEND};
  const gfx3d_tmesh_desc_t wd = {4, 2, wx, wuv, wc, wi, wt, wf};
  gfx3d_tmesh_t *water = gfx3d_tmesh_new(&wd);
  gfx3d_tex_scroll(g, slots[0], 0.0f, 3.75f);
  uint16_t img[16];
  for (int i = 0; i < 16; i++) img[i] = (i & 5) == 5 ? 0xF81F : (uint16_t)(0x07E0 + 77 * i);
  const int spr = gfx3d_image_add(g, 4, 4, 0xF81F, img);
  const gfx3d_sprite_t trees[4] = {{-6.0f, 2.5f, -14.0f, 5.0f, (uint32_t)spr},
                                   {5.0f, 2.0f, -20.0f, 4.0f, (uint32_t)spr},
                                   {-9.0f, 3.0f, -28.0f, 0.2f, (uint32_t)spr},
                                   {2.0f, 1.5f, -9.0f, 3.0f, (uint32_t)spr}};
  const float sp[3] = {-2.0f, 1.0f, -12.0f};
  // The water's rows, by where its far edge lands.
  const float far_edge[3] = {0.0f, 0.0f, -36.0f};
  float sx, sy, depth;
  gfx3d_project(g, far_edge, &sx, &sy, &depth);
  const int row0 = (int)sy - 3;
  for (int i = 0; i < TEX_SCENE_W * TEX_SCENE_H; i++) fb[i] = 0;
  gfx3d_target_t t = {fb, TEX_SCENE_W, 0, row0, TEX_SCENE_W - 1, TEX_SCENE_H - 1, swap,
                      gfx3d_image_resolve, g};
  const float at0[3] = {0.0f, 0.0f, 0.0f};
  gfx3d_stats_t sum;
  for (int pass = 0; pass < 2; pass++) {
    gfx3d_set_sky_rows(g, pass ? 0 : row0, pass ? row0 - 1 : TEX_SCENE_H - 1);
    gfx3d_set_mirror(g, !pass, 0.0f);
    gfx3d_begin(g, false, 0);
    gfx3d_draw_tmesh(g, land, NULL, at0, 1.0f, 0.0f);
    gfx3d_draw_sprites(g, trees, 4, 0.0f);
    gfx3d_draw_sprite(g, spr, sp, 2.0f, 0, 0, 4, 4, 0.0f);
    if (pass) gfx3d_draw_tmesh(g, water, NULL, at0, 1.0f, 0.0f);
    t.clip_y0 = pass ? 0 : row0;
    gfx3d_end(g, &t);
    const gfx3d_stats_t st = *gfx3d_get_stats(g);
    if (!pass) {
      sum = st;
    } else {
      sum.tris_in += st.tris_in;
      sum.culled += st.culled;
      sum.clipped += st.clipped;
      sum.drawn += st.drawn;
      sum.sprites += st.sprites;
      sum.overflow += st.overflow;
      sum.pixels += st.pixels;
    }
  }
  gfx3d_tmesh_free(land);
  gfx3d_tmesh_free(water);
  gfx3d_free(g);
  return sum;
}
