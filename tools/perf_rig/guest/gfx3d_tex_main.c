// The textured rasteriser's workloads, through gfx3d.c's test-only triangle entry.
//  * TEX_WORK_SCENE: tests/unit/gfx3d_tex_scene.h's triangles, once in host byte order
//    and once swapped. On the M33 swapped pixels come from the written-out loop and the
//    others from the C: `make check-tex` holds both frames' hash to the host's.
//  * Otherwise, what a frame of a valley asks for, for its cost: about 2880 triangles
//    of 50 pixels (the device spike's load), a sheet of perspective ground drawn twice,
//    with a tile texture per quad in PSRAM and mips by the spike's rule.
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

// Working memory of this file, out of the way: at SRAM addresses nothing else uses, not
// in .bss, whose few bytes (the rasteriser's own state) sit below the code in SRAM.
#ifdef RIG_NATIVE
static uint16_t s_texel_mem[5461];
static uint8_t s_grid_mem[64 << 10];
#define s_texels s_texel_mem
#define GRID_MEM ((void *)s_grid_mem)
#else
#define s_texels ((uint16_t *)0x20070000u)
#define GRID_MEM ((void *)0x20060000u)
#endif

#ifdef TEX_WORK_SCENE
static void run_scene(bool swap) {
  gfx3d_t *g = gfx3d_new(TEX_SCENE_W, TEX_SCENE_H);
  int slots[4];
  tex_scene_add_textures(g, s_texels, slots);
  memset(__fb_base, 0, (size_t)TEX_SCENE_W * TEX_SCENE_H * 2u);
  const gfx3d_target_t t = {__fb_base, TEX_SCENE_W, 0, 0, TEX_SCENE_W - 1, TEX_SCENE_H - 1, swap, NULL, NULL};
  uint32_t s = 0x13579BDFu;
  int affine = 0, persp = 0;
  RIG_FRAME_START();
  RIG_PHASE(PHASE_RASTER);
  for (int i = 0; i < TEX_SCENE_TRIS; i++) {
    gfx3d_test_vert_t v[3];
    int tex, level;
    bool blend;
    tex_scene_next(&s, v, &tex, &level, &blend);
    gfx3d_test_tex_info_t info;
    gfx3d_test_tex_tri(g, &t, slots[tex], level, v, blend, &info);
    if (info.rows) {
      if (info.affine) affine++;
      else persp++;
    }
  }
  RIG_PHASE(PHASE_IDLE);
  RIG_STAT(0, TEX_SCENE_TRIS);
  RIG_STAT(1, gfx3d_get_stats(g)->pixels);
  RIG_STAT(2, (uint32_t)affine);
  RIG_STAT(3, (uint32_t)persp);
  RIG_STAT(4, 0);
  RIG_FRAME_END(__fb_base);
}
#else
#define COLS 24
#define ROWS 30
#define LAYERS 2
#define TILES 4
static float absf(float x) { return x < 0.0f ? -x : x; }

static int level_of(const gfx3d_test_vert_t *a, const gfx3d_test_vert_t *b, const gfx3d_test_vert_t *c) {
  const float dx = absf(b->x - a->x) > absf(c->x - a->x) ? absf(b->x - a->x) : absf(c->x - a->x);
  const float dy = absf(b->y - a->y) > absf(c->y - a->y) ? absf(b->y - a->y) : absf(c->y - a->y);
  const float du = absf(b->u - a->u) > absf(c->u - a->u) ? absf(b->u - a->u) : absf(c->u - a->u);
  const float dv = absf(b->v - a->v) > absf(c->v - a->v) ? absf(b->v - a->v) : absf(c->v - a->v);
  const float scr = dx > dy ? dx : dy, tex = (du > dv ? du : dv) * 64.0f;
  const float r = tex / (scr < 1.0f ? 1.0f : scr);
  return r < 2.83f ? 0 : r < 5.66f ? 1 : r < 11.3f ? 2 : r < 22.6f ? 3 : 4;
}

static void run_terrain(gfx3d_t *g, const int *slots, int frame) {
  gfx3d_test_vert_t *const grid = (gfx3d_test_vert_t *)GRID_MEM;
  const gfx3d_target_t t = {__fb_base, TEX_SCENE_W, 0, 0, TEX_SCENE_W - 1, TEX_SCENE_H - 1, true, NULL, NULL};
  const float horizon = 90.0f, f = 277.0f, eye = 6.0f;
  uint32_t drawn = 0, affine = 0, persp = 0;
  RIG_FRAME_START();
  RIG_PHASE(PHASE_RASTER);
  for (int layer = 0; layer < LAYERS; layer++) {
    uint32_t rng = 0xACE1u + (uint32_t)layer;
    for (int j = 0; j <= ROWS; j++) {
      const float y = horizon + 10.0f + (float)j * (220.0f / ROWS);
      const float z = f * eye / (y - horizon);
      for (int i = 0; i <= COLS; i++) {
        const float x = (float)i * (320.0f / COLS) + (float)(frame % 8);
        gfx3d_test_vert_t *v = &grid[j * (COLS + 1) + i];
        // Depth changes along a row too, as on land that is not flat.
        const float lean = 1.0f + 0.25f * (float)(i - COLS / 2) / COLS;
        v->x = x;
        v->y = y;
        v->iw = 1.0f / (z * lean);
        v->u = (x - 160.0f) * z / f * 0.25f;
        v->v = z * 0.25f + (float)frame * 0.01f;
        const uint32_t c = tex_scene_rand(&rng);
        v->color = (uint16_t)((((c & 7u) + 18u) << 11) | (((c >> 3 & 15u) + 36u) << 5) | ((c >> 7 & 7u) + 14u));
      }
    }
    for (int j = 0; j < ROWS; j++)
      for (int i = 0; i < COLS; i++) {
        const gfx3d_test_vert_t *q = &grid[j * (COLS + 1) + i];
        const gfx3d_test_vert_t tris[2][3] = {{q[0], q[COLS + 1], q[1]}, {q[1], q[COLS + 1], q[COLS + 2]}};
        for (int k = 0; k < 2; k++) {
          gfx3d_test_tex_info_t info;
          gfx3d_test_tex_tri(g, &t, slots[(i + j) % TILES], level_of(&tris[k][0], &tris[k][1], &tris[k][2]),
                             tris[k], false, &info);
          drawn++;
          if (info.affine) affine++;
          else persp++;
        }
      }
  }
  RIG_PHASE(PHASE_IDLE);
  RIG_STAT(0, drawn);
  RIG_STAT(1, gfx3d_get_stats(g)->pixels);
  RIG_STAT(2, affine);
  RIG_STAT(3, persp);
  RIG_STAT(4, 0);
  RIG_FRAME_END(__fb_base);
}
#endif

int main(void) {
  s_heap = __heap_base;
#ifdef TEX_WORK_SCENE
  run_scene(false);
  run_scene(true);
#else
  gfx3d_t *g = gfx3d_new(TEX_SCENE_W, TEX_SCENE_H);
  int slots[TILES];
  for (int s = 0; s < TILES; s++) {
    int n = 0;
    for (int k = 0; k < 5; k++)
      for (int y = 0; y < 64 >> k; y++)
        for (int x = 0; x < 64 >> k; x++) s_texels[n++] = tex_scene_texel((uint32_t)(s * 11 + k), x, y) | 0x4210u;
    slots[s] = gfx3d_tex_add(g, 6, 6, 5, s_texels);
  }
  for (int frame = 0; frame < 8; frame++) {
    gfx3d_begin(g, false, 0);
    memset(__fb_base, 0, (size_t)TEX_SCENE_W * TEX_SCENE_H * 2u);
    run_terrain(g, slots, frame);
  }
#endif
  return 0;
}
