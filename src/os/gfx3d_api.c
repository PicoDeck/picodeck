// The table behind api->gfx3d (API version 11): gfx3d.c as a native app calls it, built into
// the firmware and the simulator alike. Nothing here but the handle types going through: each
// entry is its own function, not a cast, since WebAssembly traps an indirect call whose type
// differs at all. Only `end` does anything, finding the back buffer and the clip rect.
//
// On the device an app gets this through its per-launch table, whose create and free entries
// track the handles (native_loader.c); the simulator's trampolines track them on their way
// here.
#include "gfx3d.h"
#include "os.h"
#include "../drivers/display.h"
#include "../drivers/display_raster.h"

#include <stddef.h>
#include <string.h>

// The app's types are the renderer's, member by member, so arrays and descriptions pass
// straight through with nothing copied.
#define SAME_MEMBER(a, b, m)                                                                 \
  _Static_assert(offsetof(a, m) == offsetof(b, m) && sizeof(((a *)0)->m) == sizeof(((b *)0)->m), \
                 #a "." #m)

_Static_assert(sizeof(pc3d_sprite_t) == sizeof(gfx3d_sprite_t), "pc3d_sprite_t");
SAME_MEMBER(pc3d_sprite_t, gfx3d_sprite_t, x);
SAME_MEMBER(pc3d_sprite_t, gfx3d_sprite_t, y);
SAME_MEMBER(pc3d_sprite_t, gfx3d_sprite_t, z);
SAME_MEMBER(pc3d_sprite_t, gfx3d_sprite_t, size);
SAME_MEMBER(pc3d_sprite_t, gfx3d_sprite_t, slot);

_Static_assert(sizeof(pc3d_mesh_desc_t) == sizeof(gfx3d_tmesh_desc_t), "pc3d_mesh_desc_t");
SAME_MEMBER(pc3d_mesh_desc_t, gfx3d_tmesh_desc_t, nverts);
SAME_MEMBER(pc3d_mesh_desc_t, gfx3d_tmesh_desc_t, ntris);
SAME_MEMBER(pc3d_mesh_desc_t, gfx3d_tmesh_desc_t, xyz);
SAME_MEMBER(pc3d_mesh_desc_t, gfx3d_tmesh_desc_t, uv);
SAME_MEMBER(pc3d_mesh_desc_t, gfx3d_tmesh_desc_t, color);
SAME_MEMBER(pc3d_mesh_desc_t, gfx3d_tmesh_desc_t, idx);
SAME_MEMBER(pc3d_mesh_desc_t, gfx3d_tmesh_desc_t, tex);
SAME_MEMBER(pc3d_mesh_desc_t, gfx3d_tmesh_desc_t, flags);

_Static_assert(sizeof(pc3d_stats_t) == sizeof(gfx3d_stats_t), "pc3d_stats_t");
SAME_MEMBER(pc3d_stats_t, gfx3d_stats_t, tris_in);
SAME_MEMBER(pc3d_stats_t, gfx3d_stats_t, culled);
SAME_MEMBER(pc3d_stats_t, gfx3d_stats_t, clipped);
SAME_MEMBER(pc3d_stats_t, gfx3d_stats_t, drawn);
SAME_MEMBER(pc3d_stats_t, gfx3d_stats_t, sprites);
SAME_MEMBER(pc3d_stats_t, gfx3d_stats_t, overflow);
SAME_MEMBER(pc3d_stats_t, gfx3d_stats_t, pixels);

_Static_assert(PC3D_TM_BLEND == GFX3D_TM_BLEND, "PC3D_TM_BLEND");
_Static_assert(PC3D_TM_DOUBLE_SIDED == GFX3D_TM_DOUBLE_SIDED, "PC3D_TM_DOUBLE_SIDED");
_Static_assert(PC3D_TM_NO_FOG == GFX3D_TM_NO_FOG, "PC3D_TM_NO_FOG");

static pc3d_t api_create(int capacity) {
  return gfx3d_new_ex(FB_WIDTH, FB_HEIGHT, capacity == 0 ? GFX3D_CAPACITY : capacity);
}
static void api_destroy(pc3d_t g) { gfx3d_free((gfx3d_t *)g); }
static bool api_set_projection(pc3d_t g, float fov_y, float znear, float zfar) {
  return gfx3d_set_projection((gfx3d_t *)g, fov_y, znear, zfar);
}
static void api_set_camera(pc3d_t g, const float pos[3], float yaw, float pitch, float roll) {
  gfx3d_set_camera((gfx3d_t *)g, pos, yaw, pitch, roll);
}
static bool api_look_at(pc3d_t g, const float eye[3], const float target[3], const float up[3]) {
  return gfx3d_look_at((gfx3d_t *)g, eye, target, up);
}
static bool api_project(pc3d_t g, const float p[3], float *sx, float *sy, float *depth) {
  return gfx3d_project((const gfx3d_t *)g, p, sx, sy, depth);
}
static void api_set_fog(pc3d_t g, bool on, float fnear, float ffar, uint16_t color) {
  gfx3d_set_fog((gfx3d_t *)g, on, fnear, ffar, color);
}
static int api_tex_add(pc3d_t g, int lg_w, int lg_h, int nmips, const uint16_t *pixels) {
  return gfx3d_tex_add((gfx3d_t *)g, lg_w, lg_h, nmips, pixels);
}
static void api_tex_scroll(pc3d_t g, int slot, float du, float dv) {
  gfx3d_tex_scroll((gfx3d_t *)g, slot, du, dv);
}
static int api_image_add(pc3d_t g, int w, int h, uint16_t key, const uint16_t *pixels) {
  return gfx3d_image_add((gfx3d_t *)g, w, h, key, pixels);
}
static bool api_set_sky_image(pc3d_t g, int tex_slot, int horizon_row) {
  return gfx3d_set_sky_image((gfx3d_t *)g, tex_slot, horizon_row);
}
static void api_set_sky_rows(pc3d_t g, int y0, int y1) {
  gfx3d_set_sky_rows((gfx3d_t *)g, y0, y1);
}
static void api_set_sprite_fog(pc3d_t g, bool on) { gfx3d_set_sprite_fog((gfx3d_t *)g, on); }
static void api_set_mirror(pc3d_t g, bool on, float plane_y) {
  gfx3d_set_mirror((gfx3d_t *)g, on, plane_y);
}
static pc3dmesh_t api_mesh_new(const pc3d_mesh_desc_t *d) {
  return gfx3d_tmesh_new((const gfx3d_tmesh_desc_t *)d);
}
static void api_mesh_free(pc3dmesh_t m) { gfx3d_tmesh_free((gfx3d_tmesh_t *)m); }
static void api_begin(pc3d_t g, bool clear, uint16_t color) {
  gfx3d_begin((gfx3d_t *)g, clear, color);
}
static void api_draw_mesh(pc3d_t g, pc3dmesh_t m, const float rot[9], const float pos[3],
                          float scale, float bias) {
  gfx3d_draw_tmesh((gfx3d_t *)g, (const gfx3d_tmesh_t *)m, rot, pos, scale, bias);
}
static int api_draw_sprites(pc3d_t g, const pc3d_sprite_t *s, int n, float bias) {
  return gfx3d_draw_sprites((gfx3d_t *)g, (const gfx3d_sprite_t *)s, n, bias);
}
static void api_end(pc3d_t g) {
  display_raster_target_t d;
  display_get_raster_target(&d);
  const gfx3d_target_t t = {d.fb,       d.stride,   d.clip_x0,           d.clip_y0, d.clip_x1,
                            d.clip_y1, d.swap,     gfx3d_image_resolve, g};
  gfx3d_end((gfx3d_t *)g, &t);
}
static void api_get_stats(pc3d_t g, pc3d_stats_t *out) {
  memcpy(out, gfx3d_get_stats((const gfx3d_t *)g), sizeof *out);
}

const picocalc_gfx3d_t g_gfx3d_api = {
    .create = api_create,
    .destroy = api_destroy,
    .setProjection = api_set_projection,
    .setCamera = api_set_camera,
    .lookAt = api_look_at,
    .project = api_project,
    .setFog = api_set_fog,
    .texAdd = api_tex_add,
    .texScroll = api_tex_scroll,
    .imageAdd = api_image_add,
    .setSkyImage = api_set_sky_image,
    .setSkyRows = api_set_sky_rows,
    .setSpriteFog = api_set_sprite_fog,
    .setMirror = api_set_mirror,
    .meshNew = api_mesh_new,
    .meshFree = api_mesh_free,
    .begin = api_begin,
    .drawMesh = api_draw_mesh,
    .drawSprites = api_draw_sprites,
    .end = api_end,
    .getStats = api_get_stats,
};
