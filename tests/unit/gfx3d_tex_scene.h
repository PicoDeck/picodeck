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
