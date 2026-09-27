// gfx3d: the renderer behind picocalc.gfx3d. Flat-shaded low-poly triangle
// meshes and billboard sprites are transformed, culled, lit, fogged and
// near-clipped when drawn, painter's-ordered into depth buckets, then
// rasterised into a 16-bit framebuffer by gfx3d_end(). Pure C (no Lua, no
// Pico SDK): the firmware, the simulator and tests/unit/test_gfx3d.c build
// this same file.
//
// Conventions (right-handed, y up):
//  * The camera looks along -z with x to its right and y up; depth = -z_view.
//  * Rotations are R = Ry(yaw) * Rx(pitch) * Rz(roll): positive yaw turns
//    left, positive pitch looks up, positive roll banks left.
//  * A model with the identity rotation faces -z, like the camera.
//  * Triangle (a, b, c) faces where (b - a) x (c - a) points: seen from its
//    front, its vertices run counter-clockwise.
//  * 3x3 matrices are row-major; their columns are the rotated x, y, z axes.
//  * Colours are host-order RGB565; the target converts on write.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GFX3D_MAX_VERTS 4096
#define GFX3D_MAX_TRIS 8192   // per mesh
#define GFX3D_MAX_SKY_BANDS 8
#define GFX3D_BUCKETS 1024    // depth buckets, sqrt-spaced (finer near)
#define GFX3D_CAPACITY 4096   // triangle + sprite records per scene

// Per-triangle flags.
#define GFX3D_DOUBLE_SIDED 1u  // draw the back face too (lit as its own side)
#define GFX3D_UNLIT 2u         // ignore the light: full colour
#define GFX3D_NO_FOG 4u        // ignore fog
#define GFX3D_FLAG_MASK 7u

typedef struct {
  int nverts, ntris;
  float *xyz;       // 3 * nverts
  float *plane;     // 4 * ntris: unit normal (nx, ny, nz), d = n . a
  uint16_t *idx;    // 3 * ntris, 0-based
  uint16_t *color;  // ntris, host RGB565
  uint8_t *flags;   // ntris
  float center[3], radius;  // bounding sphere, model space
} gfx3d_mesh_t;

typedef struct gfx3d gfx3d_t;

typedef struct {
  uint32_t tris_in;   // triangles submitted (sum of the meshes' ntris)
  uint32_t culled;    // dropped by frustum, back face, near/far, off-viewport (sprites too)
  uint32_t clipped;   // triangles cut by the near plane
  uint32_t drawn;     // triangles rasterised
  uint32_t sprites;   // sprites rasterised
  uint32_t overflow;  // records dropped because the scene was full
} gfx3d_stats_t;

// Resolves sprite `slot` (as given to gfx3d_draw_sprite) at raster time.
// Returns false to skip the sprite.
typedef bool (*gfx3d_sprite_fn)(void *ud, int slot, const uint16_t **data,
                                int *img_w, int *img_h, uint16_t *key);

typedef struct {
  uint16_t *fb;
  int stride;                              // pixels per row
  int clip_x0, clip_y0, clip_x1, clip_y1;  // inclusive; empty if x1<x0 or y1<y0
  bool swap;                               // store pixels byte-swapped
  gfx3d_sprite_fn sprite;                  // NULL: sprites are skipped
  void *ud;
} gfx3d_target_t;

// ── Meshes ──
// One block for nverts (1..GFX3D_MAX_VERTS) vertices and ntris
// (1..GFX3D_MAX_TRIS) triangles, zeroed. Fill xyz/idx/color/flags, then call
// gfx3d_mesh_finish. NULL when out of range or out of memory.
gfx3d_mesh_t *gfx3d_mesh_alloc(int nverts, int ntris);
// Validates and computes planes and the bounding sphere. NULL on success,
// else a static error message.
const char *gfx3d_mesh_finish(gfx3d_mesh_t *m);
void gfx3d_mesh_free(gfx3d_mesh_t *m);

// ── Math ──
void gfx3d_rot_euler(float out[9], float yaw, float pitch, float roll);
// Rotation whose -z axis points along fwd, y axis towards up. False when fwd
// is zero or parallel to up.
bool gfx3d_rot_basis(float out[9], const float fwd[3], const float up[3]);

// ── Context ──
// Viewport (0, 0, screen_w, screen_h), fovY 60 degrees, near 0.5, far 1000,
// camera at the origin looking along -z, light towards (0.3, 1, 0.5) with
// ambient 0.25, no fog, no sky, empty scene. NULL on out of memory.
gfx3d_t *gfx3d_new(int screen_w, int screen_h);
void gfx3d_free(gfx3d_t *g);
void gfx3d_set_viewport(gfx3d_t *g, int x, int y, int w, int h);  // w, h >= 1
bool gfx3d_set_projection(gfx3d_t *g, float fov_y, float znear, float zfar);
void gfx3d_set_camera(gfx3d_t *g, const float pos[3], float yaw, float pitch,
                      float roll);
bool gfx3d_look_at(gfx3d_t *g, const float eye[3], const float target[3],
                   const float up[3]);
// (dx, dy, dz) points towards the light; ambient is clamped to 0..1.
bool gfx3d_set_light(gfx3d_t *g, float dx, float dy, float dz, float ambient);
// Off unless on && ffar > fnear.
void gfx3d_set_fog(gfx3d_t *g, bool on, float fnear, float ffar, uint16_t color);
// n bands (0 clears the sky). Band i covers elevations from angles[i] up to
// angles[i+1]; band 0 also everything below, the last everything above.
// Angles strictly increase within (-89, 89) degrees (radians). False if not.
bool gfx3d_set_sky(gfx3d_t *g, const float *angles, const uint16_t *colors,
                   int n);

// ── Scene ──
void gfx3d_begin(gfx3d_t *g, bool clear, uint16_t clear_color);
// world = rot * (scale * p) + pos. Everything but rasterisation happens now;
// only screen-space records are kept (the mesh may be freed right after).
// bias is added to the painter's depth (world units; positive = behind).
// sort_as_one files the whole instance at its origin's depth + bias, its own
// faces sorted far to near.
void gfx3d_draw(gfx3d_t *g, const gfx3d_mesh_t *m, const float rot[9],
                const float pos[3], float scale, float bias, bool sort_as_one);
// Scenery centred on the camera: rotation only, no fog, never far-culled,
// drawn first, in submission order.
void gfx3d_draw_background(gfx3d_t *g, const gfx3d_mesh_t *m);
// A camera-facing sprite of world height `size` centred at pos, showing the
// (sx, sy, sw, sh) part of the image the target resolves for `slot`.
bool gfx3d_draw_sprite(gfx3d_t *g, int slot, const float pos[3], float size,
                       int sx, int sy, int sw, int sh, float bias);
// Fills the viewport ∩ target clip (sky, else the clear colour if any), then
// rasterises the scene far to near.
void gfx3d_end(gfx3d_t *g, const gfx3d_target_t *t);
bool gfx3d_project(const gfx3d_t *g, const float p[3], float *sx, float *sy,
                   float *depth);
const gfx3d_stats_t *gfx3d_get_stats(const gfx3d_t *g);
