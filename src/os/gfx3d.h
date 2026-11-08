// gfx3d: the renderer behind picocalc.gfx3d. Flat-shaded and textured low-poly
// triangle meshes and billboard sprites are transformed, culled, lit, fogged and
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
// Triangles a scene's near-plane cuts of textured meshes can leave (see gfx3d_draw_tmesh).
#define GFX3D_TM_FANS 256

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
// in model space for the face-plane tests. Valid until the camera, the light or
// the mirror changes.
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
  uint32_t pixels;    // textured triangles' pixels, and the clipped area each sprite covers (flat triangles: none)
} gfx3d_stats_t;

// Resolves sprite `slot` (as given to gfx3d_draw_sprite) at raster time.
// Returns false to skip the sprite. It must return, never longjmp out of gfx3d_end, which may
// be running on a stack of its own (gfx3d_stack_call).
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
// A textured mesh must outlive the scene it was drawn in (until gfx3d_end has returned):
// the scene's records refer to its uv, unlike a flat mesh's, which is copied into them.
void gfx3d_tmesh_free(gfx3d_tmesh_t *m);

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
// An image of at most 256 colours is also kept as palette indices, made inside gfx3d_end for its
// first sprite drawn there with the lent block: its block grows from w * h * 2 to about
// w * h * 3 + 2 * colours + 8 B, the old block freed afterwards (a hole of its size), with a peak
// of old + new + 512 B meanwhile, and that frame pays the scan. If that memory is not there, the
// image stays RGB565 for good, as fast as before. Count it in an app's min_psram_kb.
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
// A texture as the sky, instead of bands until it is cleared (tex_slot < 0): mip 0, one texel
// per screen pixel, copied with no colour multiplied. horizon_row (0..height) is its first row
// below the horizon. False, changing nothing, for another slot or row. In a scene's first step:
//   column at screen x = (x - viewport x - round(yaw * focal)) mod width
//   row at screen y    = horizon_row + (y - horizon_y), the edge rows repeating past the ends
// where focal = viewport h / 2 / tan(fov_y / 2) is pixels per radian, so at the centre of the
// view the sky moves as fast as distant terrain, a whole texel at a time, and a turn of
// 2 pi spans 2 pi * focal texels: the image repeats as often as its width goes into that. And
//   horizon_y = round(viewport centre y + focal * tan(pitch))
// is where the true horizon lands. yaw is what gfx3d_set_camera was given, so it may run past
// pi and the sky goes on scrolling; after gfx3d_look_at it is that direction's, in -pi..pi,
// and the sky jumps where it wraps. Roll is not supported: the image stays level and ignores
// it. Mirrored, rows run the other way about the line above row horizon_y:
//   row at screen y = horizon_row + (horizon_y - 1 - y)
// so that a frame's reflected sky is its real sky's rows upside down, whatever the plane.
bool gfx3d_set_sky_image(gfx3d_t *g, int tex_slot, int horizon_row);
// Rows the sky or clear fill may touch, inclusive; records still use the target clip. y1 < y0
// is none. The default is the whole viewport, as is any range that holds it.
void gfx3d_set_sky_rows(gfx3d_t *g, int y0, int y1);
// World y becomes 2 * plane_y - y for what is drawn while on, flat and textured meshes,
// sprites and background alike, as the camera is what gets reflected: a triangle faces the eye
// as its reflection does (GFX3D_TM_DOUBLE_SIDED or not), a sprite stays at its reflected
// centre and flips vertically, and meshes keep the real world's light. Taken when each thing
// is drawn; the sky, bands too, mirrors if it is on at gfx3d_end. gfx3d_project gives where the
// reflection lands, and a gfx3d_xform_t is valid only for the state it was made in. It stays
// on until turned off, across scenes. Off unless on and plane_y is finite.
//
// A reflecting frame is two scenes. (1) target clip and sky rows set to the water's rows,
// mirror on, draw tall land and trees, end; (2) full clip, sky rows above the water, mirror
// off, draw everything with water flagged GFX3D_TM_BLEND, end.
void gfx3d_set_mirror(gfx3d_t *g, bool on, float plane_y);
// Added to the uv of every triangle using this texture when it is rasterised, so the last
// offset set before gfx3d_end applies to the whole scene, near-clipped pieces too. An offset,
// not a step: it replaces the last one, keeps across scenes, and to move water pass a total
// that grows with the frame number. Only its fraction of a repeat is kept, so a long run's
// total costs no accuracy. An unknown slot or a value that is not finite is ignored, and so
// is a call that finds no memory, as the first may make what the first textured draw does. A
// sky image takes none.
void gfx3d_tex_scroll(gfx3d_t *g, int slot, float du, float dv);

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
// A textured mesh: world = rot * (scale * p) + pos, rot NULL being the identity. Everything
// but rasterisation happens now, but the scene's records refer to the mesh for their uv:
// unlike gfx3d_draw's, it must live until gfx3d_end has returned. bias as for gfx3d_draw.
// Its triangles join flat ones and sprites in the same depth buckets, a record each. Fog
// mixes into the vertex colours. A triangle the near plane cuts takes a record per piece and
// a place per piece in GFX3D_TM_FANS; past either, stats.overflow counts it. The first
// textured draw allocates 24 bytes per record of capacity, and 6.7 KB; failing that, or the
// vertices' scratch memory, the whole mesh counts as overflow.
void gfx3d_draw_tmesh(gfx3d_t *g, const gfx3d_tmesh_t *m, const float rot[9],
                      const float pos[3], float scale, float bias);
// Scenery centred on the camera: rotation only, no fog, never far-culled,
// drawn first, in submission order.
void gfx3d_draw_background(gfx3d_t *g, const gfx3d_mesh_t *m);
// A camera-facing sprite of world height `size` centred at pos, showing the
// (sx, sy, sw, sh) part of the image the target resolves for `slot`.
bool gfx3d_draw_sprite(gfx3d_t *g, int slot, const float pos[3], float size,
                       int sx, int sy, int sw, int sh, float bias);
// Many sprites in one call, each a whole image from gfx3d_image_add (so the target should
// resolve slots with gfx3d_image_resolve) centred at (x, y, z), `size` the world height of
// the image; its width follows the image's shape. The same records, culling and pixels as
// gfx3d_draw_sprite with (0, 0, w, h) for each. A slot that is no image, or a size that is
// not positive, is skipped without a count. Returns how many were filed; those past the
// scene's capacity count in stats.overflow.
typedef struct {
  float x, y, z, size;
  uint32_t slot;
} gfx3d_sprite_t;
int gfx3d_draw_sprites(gfx3d_t *g, const gfx3d_sprite_t *s, int n, float bias);
// Off by default. On (and the scene's fog on), a sprite's pixels mix towards the fog colour
// by its depth in 1/31 steps, as a flat triangle's colour does, and the tiny ones too. The
// fog colour is the one set when gfx3d_end runs. Off, a sprite is drawn exactly as before.
void gfx3d_set_sprite_fog(gfx3d_t *g, bool on);
// Fills the viewport ∩ target clip ∩ sky rows (sky, else the clear colour if any), then
// rasterises the scene far to near in the viewport ∩ target clip.
void gfx3d_end(gfx3d_t *g, const gfx3d_target_t *t);
bool gfx3d_project(const gfx3d_t *g, const float p[3], float *sx, float *sy,
                   float *depth);
const gfx3d_stats_t *gfx3d_get_stats(const gfx3d_t *g);
#ifdef GFX3D_TEST
// One textured triangle straight to the rasteriser, past the scene: screen
// positions (any values), 1/z, uv in texture repeats, host RGB565. Mip `level` of
// texture `slot` (clamped to its last), clipped to the target's clip rect, added to
// stats.pixels. `info`, if not NULL, says what it did: `affine` no per-span division,
// `rows` the rows it walked, 0 if it stopped before any.
typedef struct {
  float x, y, iw, u, v;
  uint16_t color;
} gfx3d_test_vert_t;
typedef struct {
  bool affine;
  int rows;
} gfx3d_test_tex_info_t;
void gfx3d_test_tex_tri(gfx3d_t *g, const gfx3d_target_t *t, int slot, int level,
                        const gfx3d_test_vert_t v[3], bool blend, gfx3d_test_tex_info_t *info);
// Sprites gfx3d_end drew from their images' palette indices, and those it drew from RGB565 as
// their palettes found no room in the lent block: counted, never reset. gfx3d_test_idx_by_slot
// counts the first by image slot.
extern int gfx3d_test_idx_sprites, gfx3d_test_idx_no_room;
extern int gfx3d_test_idx_by_slot[GFX3D_MAX_IMAGES];
// The allocator a test may give gfx3d.c (GFX3D_MALLOC=gfx3d_test_malloc), defined by the test.
void *gfx3d_test_malloc(size_t bytes);
#endif
// Fast memory for one mesh's vertices during a draw call, or for gfx3d_end's sprite fill (the fog
// tables, a magnified sprite's runs, the images' palettes) and its stack, or NULL. Weak in gfx3d.c
// (none); the firmware lends the video decoder's SRAM while no player exists. Nothing in it
// outlives the call, so nothing else may run meanwhile.
void *gfx3d_sram_scratch(size_t *bytes);
// What gfx3d_end uses of that block when it holds this much: the sprite fill's 15,744 B, then a
// 2 KB stack. With less it runs on the app's stack, and with under 15,744 B its sprites go the
// slower ways too (mixed pixel by pixel, every row sampled, RGB565), so a lender keeps its block
// at least this large (video_player.cpp checks it at build time).
#define GFX3D_LENT_BYTES 17792u
// Runs fn(arg) with [lo, lo + bytes) as its stack, a part of what gfx3d_sram_scratch lent, and
// returns true; false, having run nothing, where it cannot. gfx3d_end runs its fills and its
// raster this way. Weak in gfx3d.c (false); the firmware moves the app's PSP there
// (app_stack_call_lent), so a target's sprite function must return (no longjmp out of it).
bool gfx3d_stack_call(void *lo, size_t bytes, void (*fn)(void *), void *arg);
// Fast memory for a whole scene, from gfx3d_begin to the next one, or NULL: a
// scene's bitmap, buckets and first records, 4-byte aligned. Weak in gfx3d.c
// (none); the firmware lends the MP3 decoder's SRAM until an MP3 session starts.
// Lua runs between the calls of a scene, so a lender that wants its block back
// calls gfx3d_sram_take_back, mid-scene or not, and stops returning it. Both on
// the core that runs gfx3d, never during a gfx3d call.
void *gfx3d_sram_tier(size_t *bytes);
void gfx3d_sram_take_back(void);
