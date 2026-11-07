// A frame of a valley through gfx3d's whole textured pipeline, for its cost: chunks of
// ground in fog, each drawn twice as run-tex draws its sheet twice, moving camera, tile
// textures with mips in PSRAM, and the lent SRAM as the firmware's. About the device
// spike's load: 6800 triangles tested, 2880 drawn, 140 K pixels. What it leaves out is a
// real world's shapes, so compare versions with it rather than reading its milliseconds.
//
// With TMESH_SKY, only a sky image of Meander's size filling the rows above its horizon, so
// the raster phase is the copy. With TMESH_REFLECT, a reflecting frame: that sky over a mirror
// pass of tall land and trees in the water's rows, then the valley and the trees with a
// blended sheet of water.
#include "gfx3d.h"
#include "gfx3d_tex_scene.h"
#include "rig.h"
#include "rig_alloc.h"
#include <stddef.h>
#include <string.h>

extern char __heap_base[];
extern uint16_t __fb_base[];
static char *s_heap;

void *rig_malloc(size_t n) {
  char *p = s_heap;
  s_heap += (n + 8 + 199) / 200 * 200;
  return p + 8;
}
void rig_free(void *p) { (void)p; }

// As gfx3d_main.c: s_jpeg_sram and s_mp3_sram's sizes, out of everything's way.
#ifndef LENT_SCRATCH
#define LENT_SCRATCH 17884
#endif
#ifndef LENT_TIER
#define LENT_TIER 23820
#endif
void *gfx3d_sram_scratch(size_t *bytes) {
  *bytes = LENT_SCRATCH;
  return LENT_SCRATCH ? (void *)0x20060000u : NULL;
}
void *gfx3d_sram_tier(size_t *bytes) {
  *bytes = LENT_TIER;
  return LENT_TIER ? (void *)0x20070000u : NULL;
}

#define TILES 4
#define CHUNKS 9   // per side
#define CELLS 5    // per side of a chunk
#define CELL_SIZE 6.0f
#define CV ((CELLS + 1) * (CELLS + 1))
#define CT (2 * CELLS * CELLS)
#define LAYERS 2

static float s_xyz[3 * CV], s_uv[2 * CV];
static uint16_t s_col[CV], s_idx[3 * CT];
static uint8_t s_tex[CT];
static gfx3d_tmesh_t *s_chunk[CHUNKS * CHUNKS];

static float hills(uint32_t x, uint32_t z) {
  uint32_t r = x * 2654435761u + z * 40503u + 0x9E37u;
  r ^= r >> 15;
  r *= 2246822519u;
  r ^= r >> 13;
  return (float)(r & 255u) * (1.0f / 256.0f) * 1.2f;
}

static void make_world(const int *slots) {
  uint32_t rng = 0xACE1u;
  for (int cz = 0; cz < CHUNKS; cz++)
    for (int cx = 0; cx < CHUNKS; cx++) {
      for (int r = 0; r <= CELLS; r++)
        for (int c = 0; c <= CELLS; c++) {
          const int v = r * (CELLS + 1) + c;
          const uint32_t wx = (uint32_t)(cx * CELLS + c), wz = (uint32_t)(cz * CELLS + r);
          s_xyz[3 * v] = (float)wx * CELL_SIZE;
          s_xyz[3 * v + 1] = hills(wx, wz);
          s_xyz[3 * v + 2] = -(float)wz * CELL_SIZE;
          s_uv[2 * v] = s_xyz[3 * v] * 0.25f;
          s_uv[2 * v + 1] = s_xyz[3 * v + 2] * 0.25f;
          const uint32_t k = tex_scene_rand(&rng);
          s_col[v] = (uint16_t)((((k & 7u) + 18u) << 11) | (((k >> 3 & 15u) + 36u) << 5) | ((k >> 7 & 7u) + 14u));
        }
      for (int r = 0; r < CELLS; r++)
        for (int c = 0; c < CELLS; c++) {
          const uint16_t a = (uint16_t)(r * (CELLS + 1) + c), b = (uint16_t)(a + 1);
          const uint16_t d = (uint16_t)(a + CELLS + 1), e = (uint16_t)(d + 1);
          const uint16_t tri[6] = {a, b, e, a, e, d};
          const int t = 2 * (r * CELLS + c);
          memcpy(&s_idx[3 * t], tri, sizeof tri);
          s_tex[t] = s_tex[t + 1] = (uint8_t)slots[(r + c + cx + cz) % TILES];
        }
      const gfx3d_tmesh_desc_t d = {CV, CT, s_xyz, s_uv, s_col, s_idx, s_tex, NULL};
      s_chunk[cz * CHUNKS + cx] = gfx3d_tmesh_new(&d);
    }
}

#if defined(TMESH_SKY) || defined(TMESH_REFLECT)
#define SKY_LG_W 10
#define SKY_LG_H 7
#define HORIZON_Y 125  // of the 320 rows: 125 x 320 = 40,000 pixels of sky
static float s_yaw, s_pitch;

static int add_sky(gfx3d_t *g) {
  uint16_t *const px = (uint16_t *)rig_malloc(2u << (SKY_LG_W + SKY_LG_H));
  for (int y = 0; y < 1 << SKY_LG_H; y++)
    for (int x = 0; x < 1 << SKY_LG_W; x++) px[(y << SKY_LG_W) + x] = tex_scene_texel(99, x, y);
  const int slot = gfx3d_tex_add(g, SKY_LG_W, SKY_LG_H, 1, px);
  gfx3d_set_sky_image(g, slot, 90);
  return slot;
}

static void set_frame_camera(gfx3d_t *g, const float eye[3], int frame) {
  // The camera's real pitch, for the horizon where it wants it: 64 degrees of view.
  s_yaw = 0.08f * (float)frame - 0.07f;
  s_pitch = -0.1357f;  // atan((125 - 160) / 256)
  gfx3d_set_camera(g, eye, s_yaw, s_pitch, 0.0f);
}
#endif

int main(void) {
  s_heap = __heap_base;
  gfx3d_t *g = gfx3d_new_ex(TEX_SCENE_W, TEX_SCENE_H, 8000);
  uint16_t *const texels = (uint16_t *)rig_malloc(5461 * 2);
  int slots[TILES];
  for (int s = 0; s < TILES; s++) {
    int n = 0;
    for (int k = 0; k < 5; k++)
      for (int y = 0; y < 64 >> k; y++)
        for (int x = 0; x < 64 >> k; x++) texels[n++] = tex_scene_texel((uint32_t)(s * 11 + k), x, y) | 0x4210u;
    slots[s] = gfx3d_tex_add(g, 6, 6, 5, texels);
  }
  make_world(slots);
  gfx3d_set_fog(g, true, 60.0f, 220.0f, 0x8C71);
  const gfx3d_target_t t = {__fb_base, TEX_SCENE_W, 0, 0, TEX_SCENE_W - 1, TEX_SCENE_H - 1, true, NULL, NULL};
  const float world = (float)(CHUNKS * CELLS) * CELL_SIZE;
#if defined(TMESH_SKY) || defined(TMESH_REFLECT)
  gfx3d_set_projection(g, 1.1170107f, 0.5f, 1000.0f);
  add_sky(g);
#endif
#ifdef TMESH_SKY
  gfx3d_set_sky_rows(g, 0, HORIZON_Y - 1);
  for (int frame = 0; frame < 8; frame++) {
    const float eye[3] = {0.0f, 9.0f, 0.0f};
    set_frame_camera(g, eye, frame);
    memset(__fb_base, 0, (size_t)TEX_SCENE_W * TEX_SCENE_H * 2u);
    RIG_FRAME_START();
    RIG_PHASE(PHASE_BEGIN);
    gfx3d_begin(g, false, 0);
    RIG_PHASE(PHASE_RASTER);
    gfx3d_end(g, &t);
    RIG_PHASE(PHASE_IDLE);
    RIG_FRAME_END(__fb_base);
  }
  return 0;
#elif defined(TMESH_REFLECT)
  // A sheet of water over the ground, and the ground turned upright as tall land: a turn of 90
  // degrees about x makes it face the camera, 25 m high, at the far end of the water.
  {
    static const float wx[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f};
    static const float wuv[8] = {0.0f, 0.0f, 16.0f, 0.0f, 16.0f, 16.0f, 0.0f, 16.0f};
    static const uint16_t wc[4] = {0x9CF3, 0x9CF3, 0x9CF3, 0x9CF3}, wi[6] = {0, 1, 2, 0, 2, 3};
    const uint8_t wt[2] = {(uint8_t)slots[0], (uint8_t)slots[0]}, wf[2] = {GFX3D_TM_BLEND, GFX3D_TM_BLEND};
    const gfx3d_tmesh_desc_t wd = {4, 2, wx, wuv, wc, wi, wt, wf};
    gfx3d_tmesh_t *const water = gfx3d_tmesh_new(&wd);
    static const float upright[9] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    const float plane = 1.3f;
    const int row0 = 125, row1 = TEX_SCENE_H - 1;
    // Trees along the far shore, a third of the spike's sprites, in both scenes.
    uint16_t *const pine = (uint16_t *)rig_malloc(2u * 16u * 32u);
    for (int y = 0; y < 32; y++)
      for (int x = 0; x < 16; x++) pine[y * 16 + x] = 2 * (x > 8 ? x - 8 : 8 - x) > y / 2 ? 0xF81Fu : 0x2444u;
    const int tree = gfx3d_image_add(g, 16, 32, 0xF81F, pine);
    gfx3d_sprite_t *const trees = (gfx3d_sprite_t *)rig_malloc(330u * sizeof *trees);
    for (int i = 0; i < 330; i++) {
      const float size = 6.0f + (float)(i % 5);
      trees[i].x = (float)(i * 7 % 330) - 10.0f;
      trees[i].y = plane + size * 0.5f;
      trees[i].z = -60.0f - (float)(i % 90);
      trees[i].size = size;
      trees[i].slot = (uint32_t)tree;
    }
    gfx3d_target_t whole = t;
    whole.sprite = gfx3d_image_resolve;
    whole.ud = g;
    gfx3d_target_t water_rows = whole;
    water_rows.clip_y0 = row0;
    water_rows.clip_y1 = row1;
    for (int frame = 0; frame < 8; frame++) {
      const float eye[3] = {world * 0.5f + (float)frame * 0.6f, 9.0f, -world * 0.05f - (float)frame * 0.9f};
      set_frame_camera(g, eye, frame);
      memset(__fb_base, 0, (size_t)TEX_SCENE_W * TEX_SCENE_H * 2u);
      RIG_FRAME_START();
      RIG_PHASE(PHASE_BEGIN);
      gfx3d_tex_scroll(g, slots[0], 0.0f, (float)frame * 0.004f);
      gfx3d_set_sky_rows(g, row0, row1);
      gfx3d_set_mirror(g, true, plane);
      gfx3d_begin(g, false, 0);
      RIG_PHASE(PHASE_GEOM);
      for (int i = 0; i < CHUNKS * CHUNKS; i++) {
        const float at[3] = {0.0f, plane, -160.0f - (float)((i / CHUNKS) * 4)};
        gfx3d_draw_tmesh(g, s_chunk[i], upright, at, 1.0f, 0.0f);
      }
      gfx3d_draw_sprites(g, trees, 330, 0.0f);
      RIG_PHASE(PHASE_RASTER);
      gfx3d_end(g, &water_rows);
      const uint32_t mirrored = gfx3d_get_stats(g)->drawn;
      RIG_PHASE(PHASE_BEGIN);
      gfx3d_set_sky_rows(g, 0, row0 - 1);
      gfx3d_set_mirror(g, false, 0.0f);
      gfx3d_begin(g, false, 0);
      RIG_PHASE(PHASE_GEOM);
      for (int layer = 0; layer < LAYERS; layer++) {
        const float pos[3] = {0.0f, (float)layer * 0.05f, 0.0f};
        for (int i = 0; i < CHUNKS * CHUNKS; i++) gfx3d_draw_tmesh(g, s_chunk[i], NULL, pos, 1.0f, 0.0f);
      }
      gfx3d_draw_sprites(g, trees, 330, 0.0f);
      const float at[3] = {0.0f, plane, 0.0f};
      gfx3d_draw_tmesh(g, water, NULL, at, world, -30.0f);
      RIG_PHASE(PHASE_RASTER);
      gfx3d_end(g, &whole);
      RIG_PHASE(PHASE_IDLE);
      const gfx3d_stats_t *st = gfx3d_get_stats(g);
      RIG_STAT(0, st->tris_in);
      RIG_STAT(1, mirrored);
      RIG_STAT(2, st->pixels);
      RIG_STAT(3, st->drawn);
      RIG_STAT(4, st->overflow);
      RIG_FRAME_END(__fb_base);
    }
    return 0;
  }
#endif
  for (int frame = 0; frame < 8; frame++) {
    const float eye[3] = {world * 0.5f + (float)frame * 0.6f, 9.0f, -world * 0.05f - (float)frame * 0.9f};
    const float at[3] = {eye[0] + 3.0f, 2.0f, eye[2] - 40.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    gfx3d_look_at(g, eye, at, up);
    memset(__fb_base, 0, (size_t)TEX_SCENE_W * TEX_SCENE_H * 2u);
    RIG_FRAME_START();
    RIG_PHASE(PHASE_BEGIN);
    gfx3d_begin(g, false, 0);
    RIG_PHASE(PHASE_GEOM);
    for (int layer = 0; layer < LAYERS; layer++) {
      const float pos[3] = {0.0f, (float)layer * 0.05f, 0.0f};
      for (int i = 0; i < CHUNKS * CHUNKS; i++) gfx3d_draw_tmesh(g, s_chunk[i], NULL, pos, 1.0f, 0.0f);
    }
    RIG_PHASE(PHASE_RASTER);
    gfx3d_end(g, &t);
    RIG_PHASE(PHASE_IDLE);
    const gfx3d_stats_t *st = gfx3d_get_stats(g);
    RIG_STAT(0, st->tris_in);
    RIG_STAT(1, st->culled);
    RIG_STAT(2, st->pixels);
    RIG_STAT(3, st->drawn);
    RIG_STAT(4, st->overflow);
    RIG_FRAME_END(__fb_base);
  }
  return 0;
}
