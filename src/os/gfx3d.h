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
//  * Angles (yaw, pitch, roll, fov_y, sky band angles) are in radians.
//  * A model with the identity rotation faces -z, like the camera.
//  * Triangle (a, b, c) faces where (b - a) x (c - a) points: seen from its
//    front, its vertices run counter-clockwise.
//  * 3x3 matrices are row-major; their columns are the rotated x, y, z axes.
//  * Colours are host-order RGB565; the target converts on write.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GFX3D_MAX_VERTS 4096
#define GFX3D_MAX_TRIS 8192   // per mesh
#define GFX3D_MAX_SKY_BANDS 8
#define GFX3D_BUCKETS 1024    // depth buckets, sqrt-spaced (finer near)
#define GFX3D_CAPACITY 4096   // triangle + sprite records per scene (gfx3d_new)
// The largest capacity gfx3d_new_ex accepts: what a record's 15-bit `next` index
// can address. A single constant, so a later change to the encoding moves it.
#define GFX3D_REC_LIMIT 0x7FFF
#define GFX3D_MAX_TEXTURES 64
#define GFX3D_MAX_IMAGES 64

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

// Per-triangle flags of a textured mesh.
#define GFX3D_TM_BLEND 1u         // 50% over what is there (water)
#define GFX3D_TM_DOUBLE_SIDED 2u
#define GFX3D_TM_NO_FOG 4u

typedef struct {
  int nverts, ntris;      // 1..GFX3D_MAX_VERTS, 1..GFX3D_MAX_TRIS
  const float *xyz;       // 3 * nverts, model space
  const float *uv;        // 2 * nverts, in texture repeats
  const uint16_t *color;  // nverts, host RGB565: colour times baked light
  const uint16_t *idx;    // 3 * ntris, 0-based; counter-clockwise from the front
  const uint8_t *tex;     // ntris: slot from gfx3d_tex_add
  const uint8_t *flags;   // ntris, or NULL for none
} gfx3d_tmesh_desc_t;
typedef struct gfx3d_tmesh gfx3d_tmesh_t;

// A model -> view transform (v = A p + t), with the eye and the light direction
// in model space for the face-plane tests. Valid until the camera or the light
// changes.
typedef struct {
  float A[9], t[3], eye[3], light[3];
  float scale;
} gfx3d_xform_t;

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

// ── Textured meshes ──
// One block copying the whole description (22 bytes per vertex, 8 per triangle),
// with the bounding sphere. NULL on a bad description (ranges, NULL arrays other
// than `flags`, an index out of range, a non-finite xyz or uv) or out of memory.
// Texture slots are not checked here: they belong to the context that draws it.
gfx3d_tmesh_t *gfx3d_tmesh_new(const gfx3d_tmesh_desc_t *d);
void gfx3d_tmesh_free(gfx3d_tmesh_t *m);
#ifdef GFX3D_TEST
void gfx3d_tmesh_test_arrays(const gfx3d_tmesh_t *m, const float **xyz, const float **uv,
                             const uint16_t **color, const uint16_t **idx,
                             const uint8_t **tex, const uint8_t **flags);
#endif

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
// As gfx3d_new, with room for `capacity` records per scene
// (1..GFX3D_REC_LIMIT); NULL outside that. gfx3d_new is capacity GFX3D_CAPACITY.
gfx3d_t *gfx3d_new_ex(int screen_w, int screen_h, int capacity);
void gfx3d_free(gfx3d_t *g);
// 2^lg_w x 2^lg_h host RGB565 (lg 0..10), then nmips - 1 levels, each half the
// last (nmips 1..min(lg_w, lg_h) + 1), tightly packed, copied into the context.
// A texel multiplies the vertex colour: 31 (63 for green) means x1.5,
// saturating. The slot (counting up from 0), or -1 on bad arguments, a full
// table (GFX3D_MAX_TEXTURES) or out of memory. Slots are valid only in this
// context: drawing with an unknown slot draws nothing and counts as culled.
int gfx3d_tex_add(gfx3d_t *g, int lg_w, int lg_h, int nmips, const uint16_t *pixels);
// A w x h (1..1024) host RGB565 image with colour key `key`, copied into the
// context. The slot, or -1 (bad arguments, GFX3D_MAX_IMAGES, out of memory).
int gfx3d_image_add(gfx3d_t *g, int w, int h, uint16_t key, const uint16_t *pixels);
// A gfx3d_sprite_fn for gfx3d_image_add's images: pass the context as `g`.
bool gfx3d_image_resolve(void *g, int slot, const uint16_t **data, int *w, int *h,
                         uint16_t *key);
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
// gfx3d_draw is these two: the same transform for many meshes needs working out
// only once (gfx3d.drawList).
void gfx3d_xform(const gfx3d_t *g, const float rot[9], const float pos[3],
                 float scale, gfx3d_xform_t *xf);
void gfx3d_draw_xf(gfx3d_t *g, const gfx3d_mesh_t *m, const gfx3d_xform_t *xf,
                   float bias, bool sort_as_one);
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
// Fast memory for one mesh's vertices during a draw call, or NULL. Weak in
// gfx3d.c (none); the firmware lends the video decoder's SRAM while no player
// exists. Nothing in it outlives the call, so nothing else may run meanwhile.
void *gfx3d_sram_scratch(size_t *bytes);
// Fast memory for a whole scene, from gfx3d_begin to the next one, or NULL: a
// scene's bitmap, buckets and first records, 4-byte aligned. Weak in gfx3d.c
// (none); the firmware lends the MP3 decoder's SRAM until an MP3 session starts.
// Lua runs between the calls of a scene, so a lender that wants its block back
// calls gfx3d_sram_take_back, mid-scene or not, and stops returning it. Both on
// the core that runs gfx3d, never during a gfx3d call.
void *gfx3d_sram_tier(size_t *bytes);
void gfx3d_sram_take_back(void);
