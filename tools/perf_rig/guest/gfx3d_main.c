// The gfx3d workload: replays the gfx3d calls scene/dump_scene.lua recorded.
// Same structure as the bridge (src/os/lua_bridge_gfx3d.c): the phases bracket
// the same calls as its us_geom / us_raster timers, mesh and context memory
// come from a bump allocator in QMI PSRAM, the target is an SRAM framebuffer.
#include "gfx3d.h"
#include "rig.h"
#include "rig_alloc.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern char __heap_base[];
static char *s_heap;

// umm's granularity: with its header, every allocation is whole 200-byte blocks.
void *rig_malloc(size_t n) {
  char *p = s_heap;
  s_heap += (n + 8 + 199) / 200 * 200;
  return p + 8;
}
void rig_free(void *p) { (void)p; }

extern const uint32_t scene_blob[];
extern uint16_t __fb_base[];

static const uint32_t *s_p;
static uint32_t rd_u(void) { return *s_p++; }
static float rd_f(void) {
  float f;
  memcpy(&f, s_p++, 4);
  return f;
}

#define MAX_MESHES 256
#define MAX_LISTS 8
static gfx3d_mesh_t *s_mesh[MAX_MESHES];
static const uint32_t *s_list[MAX_LISTS];

static const float k_ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static const float k_origin[3] = {0, 0, 0};

static void draw_list(gfx3d_t *g, const uint32_t *list, float bias, int start) {
  const int n = (int)list[0];
  for (int k = 0; k < n; k++) {
    const int i = (start - 1 + k) % n + 1;
    gfx3d_mesh_t *m = s_mesh[list[i]];
    RIG_PHASE(PHASE_GEOM);
    gfx3d_draw(g, m, k_ident, k_origin, 1.0f, bias, false);
    RIG_PHASE(PHASE_IDLE);
  }
}

int main(void) {
  s_heap = __heap_base;
  s_p = scene_blob;
  rd_u();  // magic
  const int nmesh = (int)rd_u(), nlist = (int)rd_u(), nframe = (int)rd_u();

  gfx3d_t *g = gfx3d_new(320, 320);
  int vp[4];
  for (int i = 0; i < 4; i++) vp[i] = (int)rd_u();
  gfx3d_set_viewport(g, vp[0], vp[1], vp[2], vp[3]);
  float l[4];
  for (int i = 0; i < 4; i++) l[i] = rd_f();
  gfx3d_set_light(g, l[0], l[1], l[2], l[3]);
  const int nsky = (int)rd_u();
  float ang[GFX3D_MAX_SKY_BANDS];
  uint16_t col[GFX3D_MAX_SKY_BANDS];
  for (int i = 0; i < nsky; i++) {
    ang[i] = rd_f();
    col[i] = (uint16_t)rd_u();
  }
  gfx3d_set_sky(g, ang, col, nsky);
  const float fn = rd_f(), ff = rd_f();
  gfx3d_set_fog(g, true, fn, ff, (uint16_t)rd_u());

  for (int i = 0; i < nmesh; i++) {
    const int nv = (int)rd_u(), nt = (int)rd_u();
    gfx3d_mesh_t *m = gfx3d_mesh_alloc(nv, nt);
    for (int k = 0; k < 3 * nv; k++) m->xyz[k] = rd_f();
    for (int k = 0; k < 3 * nt; k++) m->idx[k] = (uint16_t)rd_u();
    for (int t = 0; t < nt; t++) {
      m->color[t] = (uint16_t)rd_u();
      m->flags[t] = (uint8_t)rd_u();
    }
    gfx3d_mesh_finish(m);
    s_mesh[i] = m;
  }
  for (int i = 0; i < nlist; i++) {
    s_list[i] = s_p;
    s_p += 1 + *s_p;
  }

  const gfx3d_target_t t = {__fb_base, 320, 0, 0, 319, 319, true, NULL, NULL};
  for (int f = 0; f < nframe; f++) {
    const float fov = rd_f(), zn = rd_f(), zf = rd_f();
    float e[3], tg[3], up[3];
    for (int i = 0; i < 3; i++) e[i] = rd_f();
    for (int i = 0; i < 3; i++) tg[i] = rd_f();
    for (int i = 0; i < 3; i++) up[i] = rd_f();
    RIG_FRAME_START();
    // Outside the bridge's timers on the device, so reported apart.
    RIG_PHASE(PHASE_BEGIN);
    gfx3d_set_projection(g, fov, zn, zf);
    gfx3d_look_at(g, e, tg, up);
    gfx3d_begin(g, false, 0);
    RIG_PHASE(PHASE_IDLE);
    const int nops = (int)rd_u();
    for (int i = 0; i < nops; i++) {
      const uint32_t op = rd_u(), a = rd_u();
      if (op == 0) {
        RIG_PHASE(PHASE_GEOM);
        gfx3d_draw_background(g, s_mesh[a]);
        RIG_PHASE(PHASE_IDLE);
      } else if (op == 1) {
        float pos[3], fw[3], u[3], rot[9];
        for (int k = 0; k < 3; k++) pos[k] = rd_f();
        for (int k = 0; k < 3; k++) fw[k] = rd_f();
        for (int k = 0; k < 3; k++) u[k] = rd_f();
        const float scale = rd_f(), bias = rd_f();
        const bool one = rd_u() != 0;
        gfx3d_rot_basis(rot, fw, u);
        RIG_PHASE(PHASE_GEOM);
        gfx3d_draw(g, s_mesh[a], rot, pos, scale, bias, one);
        RIG_PHASE(PHASE_IDLE);
      } else {
        const float bias = rd_f();
        const int start = (int)rd_u();
        draw_list(g, s_list[a], bias, start);
      }
    }
    RIG_PHASE(PHASE_RASTER);
    gfx3d_end(g, &t);
    RIG_PHASE(PHASE_IDLE);
    const gfx3d_stats_t *st = gfx3d_get_stats(g);
    RIG_STAT(0, st->tris_in);
    RIG_STAT(1, st->culled);
    RIG_STAT(2, st->clipped);
    RIG_STAT(3, st->drawn);
    RIG_STAT(4, st->overflow);
    RIG_FRAME_END(__fb_base);
  }
  return 0;
}
