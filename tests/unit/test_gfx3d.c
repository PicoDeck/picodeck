// Host tests for src/os/gfx3d.c: conventions, meshes, projection (Task 8),
// the scene pipeline (Task 9), sky, background and sprites (Task 10).
#include "check.h"
#include "display_clip.h"
#include "gfx3d.h"
#include "gfx3d_tex_scene.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 320
#define HALF_PI 1.5707964f

// The lent SRAM (gfx3d_sram_scratch, weak in gfx3d.c): off unless a test sets it.
static char s_lent[2048] __attribute__((aligned(8)));
static size_t s_lent_bytes;
static int s_lent_calls;
void *gfx3d_sram_scratch(size_t *bytes) {
  s_lent_calls++;
  if (!s_lent_bytes) return NULL;
  memset(s_lent, 0xA5, sizeof s_lent);  // nothing may rely on what it held
  *bytes = s_lent_bytes;
  return s_lent;
}

// The scene's lent SRAM (gfx3d_sram_tier, weak in gfx3d.c): off unless a test
// sets it. Exactly the size asked for, so ASan sees a write past its end.
static char *s_tier;
static size_t s_tier_bytes;
void *gfx3d_sram_tier(size_t *bytes) {
  if (!s_tier_bytes) return NULL;
  *bytes = s_tier_bytes;
  return s_tier;
}
static void tier_set(size_t bytes) {
  free(s_tier);
  s_tier = bytes ? (char *)malloc(bytes) : NULL;
  if (s_tier) memset(s_tier, 0xA5, bytes);  // nothing may rely on what it held
  s_tier_bytes = bytes;
}

static bool near_f(float a, float b, float eps) { return fabsf(a - b) <= eps; }
#define CHECK_NEAR(a, b, eps) CHECK(near_f((a), (b), (eps)))

static void apply(const float m[9], const float v[3], float o[3]) {
  for (int r = 0; r < 3; r++)
    o[r] = m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
}

static void test_euler_conventions(void) {
  const float fwd[3] = {0, 0, -1}, right[3] = {1, 0, 0};
  float m[9], o[3];
  gfx3d_rot_euler(m, HALF_PI, 0, 0);  // yaw +90: turn left
  apply(m, fwd, o);
  CHECK_NEAR(o[0], -1, 1e-5f);
  CHECK_NEAR(o[1], 0, 1e-5f);
  CHECK_NEAR(o[2], 0, 1e-5f);
  gfx3d_rot_euler(m, 0, HALF_PI, 0);  // pitch +90: look up
  apply(m, fwd, o);
  CHECK_NEAR(o[1], 1, 1e-5f);
  gfx3d_rot_euler(m, 0, 0, HALF_PI);  // roll +90: bank left, right side up
  apply(m, right, o);
  CHECK_NEAR(o[1], 1, 1e-5f);
}

static void test_basis(void) {
  float m[9], o[3];
  const float f[3] = {0, 0, -1}, up[3] = {0, 1, 0};
  const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  CHECK(gfx3d_rot_basis(m, f, up));
  for (int i = 0; i < 9; i++) CHECK_NEAR(m[i], id[i], 1e-6f);
  const float f2[3] = {1, 0, 0}, nz[3] = {0, 0, -1};
  CHECK(gfx3d_rot_basis(m, f2, up));
  apply(m, nz, o);  // the model's nose (-z) now points along +x
  CHECK_NEAR(o[0], 1, 1e-6f);
  const float zero[3] = {0, 0, 0};
  CHECK(!gfx3d_rot_basis(m, zero, up));
  CHECK(!gfx3d_rot_basis(m, up, up));
}

static gfx3d_mesh_t *tri_mesh(const float v[9], uint16_t color, uint8_t flags) {
  gfx3d_mesh_t *m = gfx3d_mesh_alloc(3, 1);
  memcpy(m->xyz, v, 9 * sizeof(float));
  m->idx[0] = 0;
  m->idx[1] = 1;
  m->idx[2] = 2;
  m->color[0] = color;
  m->flags[0] = flags;
  CHECK(gfx3d_mesh_finish(m) == NULL);
  return m;
}

static void test_mesh_planes_bounds_and_errors(void) {
  const float v[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};  // CCW from +z: faces +z
  gfx3d_mesh_t *m = tri_mesh(v, 0xFFFF, 0);
  CHECK_NEAR(m->plane[0], 0, 1e-6f);
  CHECK_NEAR(m->plane[1], 0, 1e-6f);
  CHECK_NEAR(m->plane[2], 1, 1e-6f);
  CHECK_NEAR(m->plane[3], 0, 1e-6f);
  CHECK_NEAR(m->center[0], 0.5f, 1e-6f);
  CHECK_NEAR(m->center[1], 0.5f, 1e-6f);
  CHECK_NEAR(m->radius, sqrtf(0.5f), 1e-5f);
  gfx3d_mesh_free(m);

  m = gfx3d_mesh_alloc(3, 1);
  m->idx[0] = 0;
  m->idx[1] = 1;
  m->idx[2] = 3;
  const char *e = gfx3d_mesh_finish(m);
  CHECK(e && strcmp(e, "triangle vertex index out of range") == 0);
  gfx3d_mesh_free(m);

  m = gfx3d_mesh_alloc(3, 1);
  m->xyz[4] = NAN;
  e = gfx3d_mesh_finish(m);
  CHECK(e && strcmp(e, "vertex coordinates must be finite") == 0);
  gfx3d_mesh_free(m);

  CHECK(gfx3d_mesh_alloc(0, 1) == NULL);
  CHECK(gfx3d_mesh_alloc(3, 0) == NULL);
  CHECK(gfx3d_mesh_alloc(GFX3D_MAX_VERTS + 1, 1) == NULL);
  CHECK(gfx3d_mesh_alloc(3, GFX3D_MAX_TRIS + 1) == NULL);
}

static void test_projection_and_look_at(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  CHECK(g != NULL);
  float sx, sy, d;
  const float p0[3] = {0, 0, -10};
  CHECK(gfx3d_project(g, p0, &sx, &sy, &d));
  CHECK_NEAR(sx, 160, 1e-3f);
  CHECK_NEAR(sy, 160, 1e-3f);
  CHECK_NEAR(d, 10, 1e-5f);
  const float focal = 160.0f / tanf(0.5f * 1.0471976f);
  const float p1[3] = {1, 2, -10};
  CHECK(gfx3d_project(g, p1, &sx, &sy, &d));
  CHECK_NEAR(sx, 160.0f + focal / 10.0f, 1e-2f);
  CHECK_NEAR(sy, 160.0f - 2.0f * focal / 10.0f, 1e-2f);
  const float behind[3] = {0, 0, 5};
  CHECK(!gfx3d_project(g, behind, &sx, &sy, &d));

  gfx3d_set_viewport(g, 0, 0, 320, 240);  // centre (160, 120)
  CHECK(gfx3d_project(g, p0, &sx, &sy, &d));
  CHECK_NEAR(sy, 120, 1e-3f);

  const float eye[3] = {0, 0, 0}, tgt[3] = {10, 0, 0}, up[3] = {0, 1, 0};
  CHECK(gfx3d_look_at(g, eye, tgt, up));  // looking along +x
  const float px[3] = {10, 0, 0};
  CHECK(gfx3d_project(g, px, &sx, &sy, &d));
  CHECK_NEAR(sx, 160, 1e-3f);
  CHECK_NEAR(d, 10, 1e-5f);
  CHECK(!gfx3d_look_at(g, eye, eye, up));

  const float cam[3] = {0, 0, 0};
  gfx3d_set_camera(g, cam, HALF_PI, 0, 0);  // yaw left: now looking along -x
  const float left[3] = {-10, 0, 0};
  CHECK(gfx3d_project(g, left, &sx, &sy, &d));
  CHECK_NEAR(d, 10, 1e-4f);

  CHECK(!gfx3d_set_projection(g, 0.0f, 1, 10));
  CHECK(!gfx3d_set_projection(g, 1, 5, 5));
  CHECK(!gfx3d_set_projection(g, 1, 0, 5));
  CHECK(gfx3d_set_projection(g, 1, 0.1f, 50));
  CHECK(!gfx3d_set_light(g, 0, 0, 0, 0.5f));
  gfx3d_free(g);
}

static uint16_t s_fb[W * H];
#define RED 0xF800
#define GREEN 0x07E0
#define BLUE 0x001F
#define BG 0x1111

static gfx3d_target_t target(void) {
  gfx3d_target_t t = {s_fb, W, 0, 0, W - 1, H - 1, false, NULL, NULL};
  return t;
}
static uint16_t px(int x, int y) { return s_fb[y * W + x]; }
static void fb_fill(uint16_t v) {
  for (int i = 0; i < W * H; i++) s_fb[i] = v;
}
static const float ID[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static const float ORIGIN[3] = {0, 0, 0};

// A camera-facing (+z) square of half-size s at depth -z, two triangles.
static gfx3d_mesh_t *square(float s, float z, uint16_t color, uint8_t flags) {
  gfx3d_mesh_t *m = gfx3d_mesh_alloc(4, 2);
  const float v[12] = {-s, -s, z, s, -s, z, s, s, z, -s, s, z};
  memcpy(m->xyz, v, sizeof v);
  const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
  memcpy(m->idx, idx, sizeof idx);
  m->color[0] = m->color[1] = color;
  m->flags[0] = m->flags[1] = flags;
  CHECK(gfx3d_mesh_finish(m) == NULL);
  return m;
}

static void render(gfx3d_t *g) {
  gfx3d_target_t t = target();
  gfx3d_end(g, &t);
}

static void test_backface_and_double_sided(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_mesh_t *front = square(1, -5, RED, GFX3D_UNLIT);
  const float rev[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};  // yaw 180: back to camera
  fb_fill(BG);
  gfx3d_begin(g, false, 0);
  gfx3d_draw(g, front, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == RED);
  CHECK(gfx3d_get_stats(g)->drawn == 2);

  fb_fill(BG);
  const float at[3] = {0, 0, -10};  // square now at z = -5 + ... rotated: faces -z
  gfx3d_begin(g, false, 0);
  gfx3d_draw(g, front, rev, at, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == BG);
  CHECK(gfx3d_get_stats(g)->culled == 2);

  gfx3d_mesh_t *two = square(1, -5, GREEN, GFX3D_UNLIT | GFX3D_DOUBLE_SIDED);
  gfx3d_begin(g, false, 0);
  gfx3d_draw(g, two, rev, at, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == GREEN);
  gfx3d_mesh_free(front);
  gfx3d_mesh_free(two);
  gfx3d_free(g);
}

static void test_lighting_and_fog(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_mesh_t *sq = square(1, -5, RED, 0);
  CHECK(gfx3d_set_light(g, 0, 0, 1, 0.25f));  // light faces the square
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, sq, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == RED);
  CHECK(gfx3d_set_light(g, 0, 0, -1, 0.25f));  // light behind: ambient only
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, sq, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == (8 << 11));  // 31 * 0.25 = 7.75 -> 8
  gfx3d_mesh_free(sq);

  gfx3d_mesh_t *far = square(20, -50, RED, GFX3D_UNLIT);
  gfx3d_set_fog(g, true, 10, 50, BLUE);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, far, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == BLUE);  // fully fogged at depth 50
  gfx3d_mesh_t *nofog = square(20, -50, RED, GFX3D_UNLIT | GFX3D_NO_FOG);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, nofog, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == RED);
  gfx3d_mesh_free(far);
  gfx3d_mesh_free(nofog);
  gfx3d_free(g);
}

static void test_painter_order_bias_and_stability(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_mesh_t *near_sq = square(1, -5, GREEN, GFX3D_UNLIT);
  gfx3d_mesh_t *far_sq = square(4, -10, RED, GFX3D_UNLIT);
  for (int order = 0; order < 2; order++) {
    gfx3d_begin(g, true, BG);
    if (order == 0) {
      gfx3d_draw(g, near_sq, ID, ORIGIN, 1, 0, false);
      gfx3d_draw(g, far_sq, ID, ORIGIN, 1, 0, false);
    } else {
      gfx3d_draw(g, far_sq, ID, ORIGIN, 1, 0, false);
      gfx3d_draw(g, near_sq, ID, ORIGIN, 1, 0, false);
    }
    render(g);
    CHECK(px(160, 160) == GREEN);  // nearer wins whatever the submission order
  }
  // Equal depth: later submission wins; a positive bias sends it behind.
  gfx3d_mesh_t *a = square(1, -5, RED, GFX3D_UNLIT), *b = square(1, -5, BLUE, GFX3D_UNLIT);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, a, ID, ORIGIN, 1, 0, false);
  gfx3d_draw(g, b, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == BLUE);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, a, ID, ORIGIN, 1, 0, false);
  gfx3d_draw(g, b, ID, ORIGIN, 1, 2.0f, false);
  render(g);
  CHECK(px(160, 160) == RED);
  gfx3d_mesh_free(near_sq);
  gfx3d_mesh_free(far_sq);
  gfx3d_mesh_free(a);
  gfx3d_mesh_free(b);
  gfx3d_free(g);
}

static void test_sort_as_one(void) {
  // A: plain square at depth 8. B: a square whose own geometry sits at
  // depth 12 but whose instance origin is at depth 4. Sorted per triangle,
  // B is behind A; sorted as one (origin depth), B is in front.
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_mesh_t *a = square(2, -8, RED, GFX3D_UNLIT);
  gfx3d_mesh_t *b = square(3, -8, GREEN, GFX3D_UNLIT);  // local z -8
  const float pos_b[3] = {0, 0, -4};                      // world z -12
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, b, ID, pos_b, 1, 0, false);
  gfx3d_draw(g, a, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == RED);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, b, ID, pos_b, 1, 0, true);
  gfx3d_draw(g, a, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 160) == GREEN);
  gfx3d_mesh_free(a);
  gfx3d_mesh_free(b);
  gfx3d_free(g);
}

static void test_near_clip_and_huge_ground(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  // Ground from behind the camera to far ahead, 0.5 below the eye, facing up.
  gfx3d_mesh_t *m = gfx3d_mesh_alloc(4, 2);
  const float v[12] = {-1e5f, -0.5f, 1e5f, 1e5f, -0.5f, 1e5f,
                       1e5f, -0.5f, -1e5f, -1e5f, -0.5f, -1e5f};
  memcpy(m->xyz, v, sizeof v);
  const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
  memcpy(m->idx, idx, sizeof idx);
  m->color[0] = m->color[1] = GREEN;
  m->flags[0] = m->flags[1] = GFX3D_UNLIT;
  CHECK(gfx3d_mesh_finish(m) == NULL);
  CHECK(gfx3d_set_projection(g, 1.0471976f, 0.5f, 1e6f));
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, m, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(gfx3d_get_stats(g)->clipped >= 1);
  CHECK(px(160, 319) == GREEN);  // below the horizon
  CHECK(px(160, 200) == GREEN);
  CHECK(px(160, 100) == BG);     // above it
  gfx3d_mesh_free(m);
  gfx3d_free(g);
}

// A degenerate (collinear) triangle gets a zero plane normal from
// gfx3d_mesh_finish; process_mesh must cull it before the backface test, so
// DOUBLE_SIDED cannot make it draw.
static void test_zero_area_culled_even_double_sided(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  const float v[9] = {-1, 0, -5, 0, 0, -5, 1, 0, -5};  // collinear: zero area
  gfx3d_mesh_t *m = tri_mesh(v, RED, GFX3D_DOUBLE_SIDED);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, m, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK_EQ_INT((int)gfx3d_get_stats(g)->drawn, 0);
  CHECK_EQ_INT((int)gfx3d_get_stats(g)->culled, 1);
  gfx3d_mesh_free(m);
  gfx3d_free(g);
}

// A vertex sitting exactly on the near plane (depth == znear) counts as
// inside (clip_near's test is du >= 0.0f), so a triangle with one such
// vertex and two vertices further in front needs no clipping and must still
// draw without crashing.
static void test_vertex_on_near_plane_draws(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  const float v[9] = {0, 0.05f, -0.5f, -0.3f, -0.3f, -2, 0.3f, -0.3f, -2};
  gfx3d_mesh_t *m = tri_mesh(v, GREEN, GFX3D_DOUBLE_SIDED | GFX3D_UNLIT);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, m, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(gfx3d_get_stats(g)->drawn >= 1);
  CHECK_EQ_INT((int)gfx3d_get_stats(g)->clipped, 0);
  gfx3d_mesh_free(m);
  gfx3d_free(g);
}

static void test_frustum_cull_and_overflow(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_mesh_t *sq = square(1, -5, RED, GFX3D_UNLIT);
  const float behind[3] = {0, 0, 20}, beyond[3] = {0, 0, -5000}, aside[3] = {500, 0, 0};
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, sq, ID, behind, 1, 0, false);
  gfx3d_draw(g, sq, ID, beyond, 1, 0, false);
  gfx3d_draw(g, sq, ID, aside, 1, 0, false);
  CHECK(gfx3d_get_stats(g)->culled == 6);
  CHECK(gfx3d_get_stats(g)->tris_in == 6);
  gfx3d_mesh_free(sq);

  // 4200 copies of one visible triangle: the scene holds GFX3D_CAPACITY.
  gfx3d_mesh_t *many = gfx3d_mesh_alloc(3, 4200);
  const float v[9] = {-1, -1, -5, 1, -1, -5, 0, 1, -5};
  memcpy(many->xyz, v, sizeof v);
  for (int t = 0; t < 4200; t++) {
    many->idx[3 * t] = 0;
    many->idx[3 * t + 1] = 1;
    many->idx[3 * t + 2] = 2;
    many->color[t] = RED;
  }
  CHECK(gfx3d_mesh_finish(many) == NULL);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, many, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(gfx3d_get_stats(g)->drawn == GFX3D_CAPACITY);
  CHECK(gfx3d_get_stats(g)->overflow == 4200 - GFX3D_CAPACITY);
  gfx3d_mesh_free(many);
  gfx3d_free(g);
}

static void test_viewport_and_target_clip(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_mesh_t *big = square(100, -5, RED, GFX3D_UNLIT);
  gfx3d_set_viewport(g, 0, 0, 320, 240);
  fb_fill(BG);
  gfx3d_begin(g, true, GREEN);
  gfx3d_draw(g, big, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(px(160, 239) == RED);
  CHECK(px(160, 240) == BG);  // below the viewport: untouched, not even cleared
  gfx3d_target_t t = target();
  t.clip_x0 = 100;  // a narrower target clip is honoured too
  fb_fill(BG);
  gfx3d_begin(g, true, GREEN);
  gfx3d_draw(g, big, ID, ORIGIN, 1, 0, false);
  gfx3d_end(g, &t);
  CHECK(px(99, 100) == BG);
  CHECK(px(100, 100) == RED);
  gfx3d_mesh_free(big);
  gfx3d_free(g);
}

// A 16x16 grid facing the camera (shared vertices): no background shows
// through anywhere inside it.
static void test_grid_has_no_cracks(void) {
  enum { N = 16 };
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_mesh_t *m = gfx3d_mesh_alloc((N + 1) * (N + 1), 2 * N * N);
  for (int j = 0; j <= N; j++)
    for (int i = 0; i <= N; i++) {
      float *p = &m->xyz[3 * (j * (N + 1) + i)];
      p[0] = -4.0f + 0.5f * (float)i + 0.013f * (float)((i * 7 + j * 3) % 5);
      p[1] = -4.0f + 0.5f * (float)j + 0.011f * (float)((i * 5 + j * 11) % 7);
      p[2] = -6.0f - 0.05f * (float)((i + 2 * j) % 3);
    }
  int t = 0;
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) {
      uint16_t a = (uint16_t)(j * (N + 1) + i), b = (uint16_t)(a + 1);
      uint16_t c = (uint16_t)(a + N + 2), d = (uint16_t)(a + N + 1);
      const uint16_t tri[6] = {a, b, c, a, c, d};
      memcpy(&m->idx[3 * t], tri, sizeof tri);
      m->color[t] = RED;
      m->color[t + 1] = GREEN;
      m->flags[t] = m->flags[t + 1] = GFX3D_UNLIT;
      t += 2;
    }
  CHECK(gfx3d_mesh_finish(m) == NULL);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, m, ID, ORIGIN, 1, 0, false);
  render(g);
  int holes = 0;
  for (int y = 60; y < 260; y++)  // well inside the grid's projection
    for (int x = 60; x < 260; x++) holes += px(x, y) == BG;
  CHECK_EQ_INT(holes, 0);
  gfx3d_mesh_free(m);
  gfx3d_free(g);
}

#define WHITE 0xFFFF
#define YELLOW 0xFFE0
#define MAGENTA 0xF81F

static void set_sky3(gfx3d_t *g) {
  const float ang[3] = {-1.0f, 0.0f, 0.3f};
  const uint16_t col[3] = {GREEN, WHITE, BLUE};
  CHECK(gfx3d_set_sky(g, ang, col, 3));
}

static void test_sky_level_rolled_and_vertical(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  set_sky3(g);
  gfx3d_begin(g, false, 0);
  render(g);
  // Level: horizon at row 160; the 0.3 rad band edge is focal*tan(0.3) above.
  CHECK(px(160, 10) == BLUE);
  CHECK(px(160, 120) == WHITE);
  CHECK(px(160, 200) == GREEN);
  CHECK(px(5, 120) == WHITE && px(314, 120) == WHITE);  // level: rows are uniform

  gfx3d_set_camera(g, ORIGIN, 0, 0, HALF_PI);  // banked 90 left: sky on the right
  gfx3d_begin(g, false, 0);
  render(g);
  CHECK(px(315, 160) == BLUE);
  CHECK(px(5, 160) == GREEN);

  const float eye[3] = {0, 0, 0}, up_t[3] = {0, 10, 0}, zup[3] = {0, 0, -1};
  CHECK(gfx3d_look_at(g, eye, up_t, zup));  // straight up: top band everywhere
  gfx3d_begin(g, false, 0);
  render(g);
  CHECK(px(160, 160) == BLUE && px(0, 0) == BLUE && px(319, 319) == BLUE);

  const float bad[2] = {0.5f, 0.2f};
  const uint16_t bc[2] = {RED, RED};
  CHECK(!gfx3d_set_sky(g, bad, bc, 2));  // must increase
  gfx3d_free(g);
}

static void test_background_is_drawn_first_and_never_far_culled(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  CHECK(gfx3d_set_projection(g, 1.0471976f, 0.5f, 100.0f));
  gfx3d_mesh_t *ring = square(2000, -900, RED, GFX3D_UNLIT);  // beyond zfar, fills the view
  gfx3d_mesh_t *near_sq = square(1, -5, GREEN, GFX3D_UNLIT);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, near_sq, ID, ORIGIN, 1, 0, false);  // submitted first...
  gfx3d_draw_background(g, ring);                    // ...yet drawn under
  render(g);
  CHECK(px(160, 160) == GREEN);
  CHECK(px(20, 20) == RED);
  // Moving the camera does not move background scenery.
  const float moved[3] = {50, 0, 0};
  gfx3d_set_camera(g, moved, 0, 0, 0);
  gfx3d_begin(g, true, BG);
  gfx3d_draw_background(g, ring);
  render(g);
  CHECK(px(160, 160) == RED);
  gfx3d_mesh_free(ring);
  gfx3d_mesh_free(near_sq);
  gfx3d_free(g);
}

static uint16_t s_img[16];
static bool resolve(void *ud, int slot, const uint16_t **data, int *w, int *h,
                    uint16_t *key) {
  (void)ud;
  if (slot != 7) return false;
  *data = s_img;
  *w = 4;
  *h = 4;
  *key = MAGENTA;
  return true;
}

static void test_sprites(void) {
  for (int i = 0; i < 16; i++) s_img[i] = YELLOW;
  s_img[0] = MAGENTA;  // keyed corner
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_target_t t = target();
  t.sprite = resolve;
  const float at[3] = {0, 0, -10}, behind[3] = {0, 0, 10};
  gfx3d_begin(g, true, BG);
  CHECK(gfx3d_draw_sprite(g, 7, at, 2.0f, 0, 0, 4, 4, 0));
  CHECK(!gfx3d_draw_sprite(g, 7, behind, 2.0f, 0, 0, 4, 4, 0));
  gfx3d_end(g, &t);
  CHECK(px(160, 160) == YELLOW);
  CHECK(gfx3d_get_stats(g)->sprites == 1);
  CHECK(gfx3d_get_stats(g)->culled == 1);
  // The keyed top-left source pixel shows the background.
  const float focal = 160.0f / tanf(0.5f * 1.0471976f);
  const int half = (int)(focal * 2.0f / 10.0f / 2.0f);
  CHECK(px(160 - half + 1, 160 - half + 1) == BG);
  // Depth-sorted with triangles: a nearer square hides it, a farther one not.
  gfx3d_mesh_t *near_sq = square(1, -5, GREEN, GFX3D_UNLIT);
  gfx3d_mesh_t *far_sq = square(8, -20, RED, GFX3D_UNLIT);
  gfx3d_begin(g, true, BG);
  gfx3d_draw_sprite(g, 7, at, 2.0f, 0, 0, 4, 4, 0);
  gfx3d_draw(g, near_sq, ID, ORIGIN, 1, 0, false);
  gfx3d_end(g, &t);
  CHECK(px(160, 160) == GREEN);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, far_sq, ID, ORIGIN, 1, 0, false);
  gfx3d_draw_sprite(g, 7, at, 2.0f, 0, 0, 4, 4, 0);
  gfx3d_end(g, &t);
  CHECK(px(160, 160) == YELLOW);
  // A slot the resolver refuses is skipped.
  gfx3d_begin(g, true, BG);
  gfx3d_draw_sprite(g, 3, at, 2.0f, 0, 0, 4, 4, 0);
  gfx3d_end(g, &t);
  CHECK(px(160, 160) == BG);
  gfx3d_mesh_free(near_sq);
  gfx3d_mesh_free(far_sq);
  gfx3d_free(g);
}

// A scene with every path of process_mesh: fog, light, a near clip, sort_as_one.
static void lent_scene(gfx3d_t *g, gfx3d_mesh_t *m, uint16_t *fb, gfx3d_stats_t *st) {
  const float e[3] = {0.3f, 1.2f, 6}, tg[3] = {0, 0, 0}, up[3] = {0, 1, 0};
  const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, p0[3] = {0, 0, 0}, p1[3] = {1.5f, 0, 1};
  const float p2[3] = {0.2f, 0.4f, 5.9f};  // crosses the near plane
  gfx3d_set_fog(g, true, 2, 12, 0x4208);
  CHECK(gfx3d_look_at(g, e, tg, up));
  gfx3d_begin(g, true, 0x0011);
  gfx3d_draw(g, m, id, p0, 1, 0, false);
  gfx3d_draw(g, m, id, p1, 0.5f, 0, true);
  gfx3d_draw(g, m, id, p2, 1, 0, false);
  memset(fb, 0, sizeof(uint16_t) * W * H);
  const gfx3d_target_t t = {fb, W, 0, 0, W - 1, H - 1, false, NULL, NULL};
  gfx3d_end(g, &t);
  *st = *gfx3d_get_stats(g);
}

static void test_lent_sram_changes_nothing(void) {
  gfx3d_mesh_t *m = gfx3d_mesh_alloc(8, 12);
  for (int i = 0; i < 8; i++) {
    m->xyz[3 * i] = (i & 1) ? 1.0f : -1.0f;
    m->xyz[3 * i + 1] = (i & 2) ? 1.0f : -1.0f;
    m->xyz[3 * i + 2] = (i & 4) ? 1.0f : -1.0f;
  }
  static const uint16_t q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                   {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (int f = 0; f < 6; f++) {
    const uint16_t tri[6] = {q[f][0], q[f][1], q[f][2], q[f][0], q[f][2], q[f][3]};
    for (int k = 0; k < 6; k++) m->idx[6 * f + k] = tri[k];
    m->color[2 * f] = m->color[2 * f + 1] = (uint16_t)(0xF800 >> f);
    m->flags[2 * f] = m->flags[2 * f + 1] = (uint8_t)(f == 3 ? GFX3D_DOUBLE_SIDED : 0);
  }
  CHECK(gfx3d_mesh_finish(m) == NULL);
  static uint16_t a[W * H], b[W * H], c[W * H];
  gfx3d_t *g = gfx3d_new(W, H);
  gfx3d_stats_t sa, sb, sc;
  s_lent_bytes = 0;
  lent_scene(g, m, a, &sa);
  s_lent_bytes = sizeof s_lent;
  s_lent_calls = 0;
  lent_scene(g, m, b, &sb);
  CHECK(s_lent_calls == 3);
  s_lent_bytes = 8 * 20 - 1;  // one byte short of the vertices: PSRAM's path
  lent_scene(g, m, c, &sc);
  s_lent_bytes = 0;
  CHECK(sa.clipped > 0);
  CHECK(memcmp(a, b, sizeof a) == 0);
  CHECK(memcmp(a, c, sizeof a) == 0);
  CHECK(memcmp(&sa, &sb, sizeof sa) == 0);
  CHECK(memcmp(&sa, &sc, sizeof sa) == 0);
  gfx3d_free(g);
  gfx3d_mesh_free(m);
}

// A slanted grid, so that its 800 triangles land in many buckets.
static gfx3d_mesh_t *tier_grid(void) {
  enum { N = 20 };
  gfx3d_mesh_t *m = gfx3d_mesh_alloc((N + 1) * (N + 1), 2 * N * N);
  for (int r = 0; r <= N; r++)
    for (int c = 0; c <= N; c++) {
      float *v = &m->xyz[3 * (r * (N + 1) + c)];
      v[0] = -4.0f + 8.0f * (float)c / N;
      v[1] = -3.0f + 6.0f * (float)r / N;
      v[2] = -5.0f - 6.0f * (float)r / N;
    }
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) {
      const uint16_t a = (uint16_t)(r * (N + 1) + c), b = (uint16_t)(a + 1);
      const uint16_t d = (uint16_t)(a + N + 1), e = (uint16_t)(d + 1);
      const uint16_t tri[6] = {a, b, e, a, e, d};
      const int t = 2 * (r * N + c);
      memcpy(&m->idx[3 * t], tri, sizeof tri);
      m->color[t] = (uint16_t)(0x1234 + 77 * t);
      m->color[t + 1] = (uint16_t)(0x4321 + 55 * t);
      m->flags[t] = m->flags[t + 1] = GFX3D_DOUBLE_SIDED | GFX3D_UNLIT | GFX3D_NO_FOG;
    }
  CHECK(gfx3d_mesh_finish(m) == NULL);
  return m;
}

static bool tier_sprite(void *ud, int slot, const uint16_t **data, int *w, int *h,
                        uint16_t *key) {
  static const uint16_t px[4] = {0xF81F, 0x07FF, 0xFFE0, 0x001F};
  (void)ud;
  (void)slot;
  *data = px;
  *w = *h = 2;
  *key = 0;
  return true;
}

// One scene: half a grid's records, then `mid` (between two draw calls, where Lua
// runs), then a sprite and the rest, with sprites at both ends of the record list.
typedef void (*tier_mid_fn)(gfx3d_t *);
static void tier_scene(gfx3d_t *g, const gfx3d_mesh_t *m, uint16_t *fb, gfx3d_stats_t *st,
                       tier_mid_fn mid) {
  const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, p0[3] = {0, 0, 0};
  const float back[3] = {0, 0.5f, -1.0f}, s1[3] = {-1, 1, -4}, s2[3] = {1, -1, -4};
  gfx3d_begin(g, true, 0x0841);
  CHECK(gfx3d_draw_sprite(g, 0, s1, 1.0f, 0, 0, 2, 2, 0));
  gfx3d_draw(g, m, id, p0, 1, 0, false);
  if (mid) mid(g);
  gfx3d_draw(g, m, id, back, 1, 0, false);
  CHECK(gfx3d_draw_sprite(g, 1, s2, 1.0f, 0, 0, 2, 2, 0));
  const gfx3d_target_t t = {fb, W, 0, 0, W - 1, H - 1, false, tier_sprite, NULL};
  gfx3d_end(g, &t);
  *st = *gfx3d_get_stats(g);
}

static void tier_mid_take_back(gfx3d_t *g) {
  (void)g;
  gfx3d_sram_take_back();
  memset(s_tier, 0x5A, s_tier_bytes);  // the lender uses it at once
  tier_set(0);                         // and stops lending
}

static gfx3d_t *s_tier_other;
static gfx3d_mesh_t *s_tier_mesh;
static void tier_mid_other_context(gfx3d_t *g) {
  (void)g;
  const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, p0[3] = {0, 0, 0};
  gfx3d_begin(s_tier_other, false, 0);  // takes the block over, records and all
  gfx3d_draw(s_tier_other, s_tier_mesh, id, p0, 1, 0, false);
}

// sizeof the bitmap and buckets; a record is 28 B.
#define TIER_HEAD ((((GFX3D_BUCKETS + 1 + 31) / 32) * 4) + (GFX3D_BUCKETS + 1) * 4)

static void test_lent_tier_changes_nothing(void) {
  gfx3d_mesh_t *m = tier_grid();
  static uint16_t want[W * H], got[W * H];
  gfx3d_stats_t swant, sgot;
  gfx3d_t *g = gfx3d_new(W, H);
  tier_set(0);
  tier_scene(g, m, want, &swant, NULL);
  CHECK(swant.drawn > 700);  // more than the firmware's block holds
  CHECK(swant.sprites == 2);

  // No room for a record, one record, part of the scene, all of it.
  const size_t sizes[] = {TIER_HEAD, TIER_HEAD + 27, TIER_HEAD + 28, TIER_HEAD + 28 * 100,
                          8192, 23820, TIER_HEAD + 28 * GFX3D_CAPACITY + 1000};
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    gfx3d_sram_take_back();
    tier_set(sizes[i]);
    for (int round = 0; round < 2; round++) {  // the same block, scene after scene
      tier_scene(g, m, got, &sgot, NULL);
      CHECK(memcmp(want, got, sizeof want) == 0);
      CHECK(memcmp(&swant, &sgot, sizeof swant) == 0);
    }
  }

  // Taken back between two draw calls, at every size the records may have filled.
  const size_t back_sizes[] = {TIER_HEAD + 28, TIER_HEAD + 28 * 100, 8192, 23820,
                               TIER_HEAD + 28 * GFX3D_CAPACITY};
  for (size_t i = 0; i < sizeof back_sizes / sizeof back_sizes[0]; i++) {
    gfx3d_sram_take_back();
    tier_set(back_sizes[i]);
    tier_scene(g, m, got, &sgot, NULL);  // holds it
    tier_scene(g, m, got, &sgot, tier_mid_take_back);
    CHECK(memcmp(want, got, sizeof want) == 0);
    CHECK(memcmp(&swant, &sgot, sizeof swant) == 0);
    // With the block gone, the next scene is PSRAM's.
    tier_scene(g, m, got, &sgot, NULL);
    CHECK(memcmp(want, got, sizeof want) == 0);
    // Taking back what is not lent does nothing.
    gfx3d_sram_take_back();
    tier_scene(g, m, got, &sgot, NULL);
    CHECK(memcmp(want, got, sizeof want) == 0);
    // Offered again, the next scene takes it.
    tier_set(back_sizes[i]);
    tier_scene(g, m, got, &sgot, NULL);
    CHECK(memcmp(want, got, sizeof want) == 0);
  }

  // A second context starting a scene takes it, and the first scene survives.
  gfx3d_sram_take_back();
  tier_set(23820);
  s_tier_other = gfx3d_new(W, H);
  s_tier_mesh = m;
  tier_scene(g, m, got, &sgot, tier_mid_other_context);
  CHECK(memcmp(want, got, sizeof want) == 0);
  // The holder freed: nothing is left to take back (ASan: no use of the freed one).
  gfx3d_free(s_tier_other);
  gfx3d_sram_take_back();
  tier_scene(g, m, got, &sgot, NULL);
  CHECK(memcmp(want, got, sizeof want) == 0);

  gfx3d_free(g);
  gfx3d_sram_take_back();
  tier_set(0);
  gfx3d_mesh_free(m);
}

// A non-finite projected vertex (vertices just in front of the near plane, huge
// coordinates) counts as drawn and paints nothing, as it did before records kept
// their vertices sorted.
static void test_nonfinite_vertex_draws_nothing(void) {
  const float v[9] = {1e38f, 1e38f, -1, -1e38f, 1e38f, -1, 0, -1e38f, -1};
  gfx3d_mesh_t *m = tri_mesh(v, 0xFFFF, GFX3D_DOUBLE_SIDED | GFX3D_UNLIT | GFX3D_NO_FOG);
  static uint16_t fb[W * H];
  gfx3d_t *g = gfx3d_new(W, H);
  const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, p0[3] = {0, 0, 0}, far_eye[3] = {0, 0, 0.5f};
  (void)far_eye;
  gfx3d_begin(g, false, 0);
  gfx3d_draw(g, m, id, p0, 1, 0, false);
  const gfx3d_target_t t = {fb, W, 0, 0, W - 1, H - 1, false, NULL, NULL};
  gfx3d_end(g, &t);
  // +-1e38 * focal overflows to inf; whatever is left of it, nothing is painted
  // from a non-finite vertex, and the count is the record's.
  const gfx3d_stats_t *s = gfx3d_get_stats(g);
  if (s->drawn == 1)
    for (int i = 0; i < W * H; i++) CHECK(fb[i] == 0);
  gfx3d_free(g);
  gfx3d_mesh_free(m);
}

// ── Pinned output of the flat renderer ──
// A fixed scene through every flat feature: lit and unlit meshes, a ground
// quad cut by the near plane, fog, sky bands, background scenery and sprites.
// Its framebuffer's CRC32 was captured on develop before the textured path
// existed; the flat path must keep producing exactly these bytes.
#define FLAT_SCENE_CRC 0xb79cbc35u

static uint32_t crc32_u16(const uint16_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    for (int b = 0; b < 2; b++) {  // low byte first: independent of host endianness
      c ^= (uint8_t)(p[i] >> (8 * b));
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

// A unit cube centred on the origin, 12 triangles, a colour per face.
static gfx3d_mesh_t *cube_mesh(uint8_t flags) {
  gfx3d_mesh_t *m = gfx3d_mesh_alloc(8, 12);
  CHECK(m != NULL);
  static const float v[24] = {-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
                              -1, -1, 1,  1, -1, 1,  1, 1, 1,  -1, 1, 1};
  static const uint16_t idx[36] = {
      0, 2, 1, 0, 3, 2,  // -z
      4, 5, 6, 4, 6, 7,  // +z
      0, 4, 7, 0, 7, 3,  // -x
      1, 2, 6, 1, 6, 5,  // +x
      0, 1, 5, 0, 5, 4,  // -y
      3, 7, 6, 3, 6, 2   // +y
  };
  static const uint16_t face[6] = {RED, GREEN, BLUE, 0xFFE0, 0xF81F, 0x07FF};
  memcpy(m->xyz, v, sizeof v);
  memcpy(m->idx, idx, sizeof idx);
  for (int t = 0; t < 12; t++) {
    m->color[t] = face[t / 2];
    m->flags[t] = flags;
  }
  CHECK(gfx3d_mesh_finish(m) == NULL);
  return m;
}

// A quad in the y = 0 plane from (x0, z0) to (x1, z1), facing up.
static gfx3d_mesh_t *ground_mesh(float x0, float z0, float x1, float z1, uint16_t a,
                                 uint16_t b) {
  gfx3d_mesh_t *m = gfx3d_mesh_alloc(4, 2);
  CHECK(m != NULL);
  const float v[12] = {x0, 0, z0, x1, 0, z0, x1, 0, z1, x0, 0, z1};
  memcpy(m->xyz, v, sizeof v);
  const uint16_t idx[6] = {0, 3, 2, 0, 2, 1};
  memcpy(m->idx, idx, sizeof idx);
  m->color[0] = a;
  m->color[1] = b;
  m->flags[0] = m->flags[1] = 0;
  CHECK(gfx3d_mesh_finish(m) == NULL);
  return m;
}

// Scenery round the camera: a ring of eight tall triangles, unlit.
static gfx3d_mesh_t *skyline_mesh(void) {
  gfx3d_mesh_t *m = gfx3d_mesh_alloc(24, 8);
  CHECK(m != NULL);
  for (int i = 0; i < 8; i++) {
    const float a0 = (float)i * 0.7853982f, a1 = (float)(i + 1) * 0.7853982f;
    const float am = (a0 + a1) * 0.5f, h = (i & 1) ? 60.0f : 90.0f;
    float *v = m->xyz + 9 * i;
    v[0] = 400.0f * sinf(a0); v[1] = 0;  v[2] = -400.0f * cosf(a0);
    v[3] = 400.0f * sinf(a1); v[4] = 0;  v[5] = -400.0f * cosf(a1);
    v[6] = 400.0f * sinf(am); v[7] = h;  v[8] = -400.0f * cosf(am);
    m->idx[3 * i] = (uint16_t)(3 * i);
    m->idx[3 * i + 1] = (uint16_t)(3 * i + 2);
    m->idx[3 * i + 2] = (uint16_t)(3 * i + 1);
    m->color[i] = (i & 1) ? 0x4A69 : 0x6B4D;
    m->flags[i] = GFX3D_UNLIT | GFX3D_DOUBLE_SIDED | GFX3D_NO_FOG;
  }
  CHECK(gfx3d_mesh_finish(m) == NULL);
  return m;
}

// Two 8x8 images, a diagonal gradient with a keyed corner and a ring.
static uint16_t s_flat_img[2][64];
static bool flat_resolve(void *ud, int slot, const uint16_t **data, int *w, int *h,
                         uint16_t *key) {
  (void)ud;
  if (slot < 0 || slot > 1) return false;
  *data = s_flat_img[slot];
  *w = 8;
  *h = 8;
  *key = MAGENTA;
  return true;
}

static void test_flat_scene_crc(void) {
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      s_flat_img[0][y * 8 + x] = (uint16_t)(((x * 4) << 11) | ((y * 8) << 5) | (31 - x - y));
      const int r = (x - 4) * (x - 4) + (y - 4) * (y - 4);
      s_flat_img[1][y * 8 + x] = (r >= 4 && r <= 10) ? YELLOW : MAGENTA;
    }
  s_flat_img[0][0] = MAGENTA;

  gfx3d_t *g = gfx3d_new(W, H);
  CHECK(g != NULL);
  const float cam[3] = {0.5f, 2.0f, 6.0f};
  gfx3d_set_camera(g, cam, 0.2f, -0.12f, 0.05f);
  CHECK(gfx3d_set_projection(g, 1.05f, 0.5f, 300.0f));
  CHECK(gfx3d_set_light(g, 0.4f, 1.0f, 0.6f, 0.3f));
  gfx3d_set_fog(g, true, 20.0f, 160.0f, 0x8C71);
  const float ang[4] = {-0.6f, -0.1f, 0.15f, 0.5f};
  const uint16_t col[4] = {0x2965, 0x5ABC, 0x9E7F, 0x3D9F};
  CHECK(gfx3d_set_sky(g, ang, col, 4));

  gfx3d_mesh_t *cube = cube_mesh(0);
  gfx3d_mesh_t *cube_u = cube_mesh(GFX3D_UNLIT | GFX3D_NO_FOG | GFX3D_DOUBLE_SIDED);
  // From behind the camera to far away: the near plane cuts it.
  gfx3d_mesh_t *ground = ground_mesh(-60, 40, 60, -280, 0x3366, 0x2B24);
  gfx3d_mesh_t *skyline = skyline_mesh();

  gfx3d_begin(g, true, 0x0000);
  gfx3d_draw_background(g, skyline);
  gfx3d_draw(g, ground, ID, (const float[3]){0, -1, 0}, 1, 0, false);
  for (int i = 0; i < 6; i++) {
    float rot[9];
    gfx3d_rot_euler(rot, 0.5f * (float)i, 0.3f * (float)i, 0.1f * (float)i);
    const float pos[3] = {-9.0f + 3.5f * (float)i, 0.5f * (float)(i & 1), -6.0f - 14.0f * (float)i};
    gfx3d_draw(g, (i & 1) ? cube_u : cube, rot, pos, 1.0f + 0.25f * (float)i, 0, (i % 3) == 2);
  }
  // One cube straddling the near plane, one cut by the left edge.
  const float close[3] = {0.5f, 1.5f, 5.0f}, edge[3] = {-6.0f, 2.0f, -3.0f};
  gfx3d_draw(g, cube, ID, close, 1.2f, 0, false);
  gfx3d_draw(g, cube, ID, edge, 1.5f, 0, false);
  // Biased: pushed behind a nearer cube.
  const float bpos[3] = {2.0f, 1.0f, -4.0f};
  gfx3d_draw(g, cube_u, ID, bpos, 1.0f, 8.0f, false);
  const float s1[3] = {-2.0f, 2.0f, -10.0f}, s2[3] = {3.0f, 1.5f, -25.0f};
  const float s3[3] = {0.0f, 3.0f, -70.0f}, s4[3] = {1.0f, 2.0f, 40.0f};
  CHECK(gfx3d_draw_sprite(g, 0, s1, 2.0f, 0, 0, 8, 8, 0));
  CHECK(gfx3d_draw_sprite(g, 1, s2, 4.0f, 0, 0, 8, 8, 0));
  CHECK(gfx3d_draw_sprite(g, 0, s3, 12.0f, 1, 1, 6, 6, 0));
  CHECK(!gfx3d_draw_sprite(g, 0, s4, 2.0f, 0, 0, 8, 8, 0));  // behind the camera

  fb_fill(0);
  gfx3d_target_t t = target();
  t.sprite = flat_resolve;
  gfx3d_end(g, &t);

  const gfx3d_stats_t *st = gfx3d_get_stats(g);
  CHECK(st->clipped > 0);
  CHECK(st->drawn > 20);
  CHECK(st->sprites == 3);
  CHECK(st->overflow == 0);
  CHECK_EQ_U32(crc32_u16(s_fb, (size_t)W * H), FLAT_SCENE_CRC);

  gfx3d_mesh_free(cube);
  gfx3d_mesh_free(cube_u);
  gfx3d_mesh_free(ground);
  gfx3d_mesh_free(skyline);
  gfx3d_free(g);
}

// ── Textures and textured meshes (data model only; drawing comes later) ──

static void test_tex_add_validates(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  static uint16_t px[64 * 64 * 2];
  for (size_t i = 0; i < sizeof px / sizeof px[0]; i++) px[i] = (uint16_t)i;
  CHECK(gfx3d_tex_add(g, -1, 2, 1, px) == -1);
  CHECK(gfx3d_tex_add(g, 2, -1, 1, px) == -1);
  CHECK(gfx3d_tex_add(g, 11, 2, 1, px) == -1);
  CHECK(gfx3d_tex_add(g, 2, 11, 1, px) == -1);
  CHECK(gfx3d_tex_add(g, 2, 2, 0, px) == -1);
  CHECK(gfx3d_tex_add(g, 2, 2, 4, px) == -1);  // 4x4 has levels 4, 2, 1 only
  CHECK(gfx3d_tex_add(g, 3, 1, 3, px) == -1);  // 8x2: levels 8x2, 4x1, then 2x0.5
  CHECK(gfx3d_tex_add(g, 2, 2, 1, NULL) == -1);
  CHECK(gfx3d_tex_add(NULL, 2, 2, 1, px) == -1);
  CHECK(gfx3d_tex_add(g, 2, 2, 3, px) == 0);   // slots count up from 0
  CHECK(gfx3d_tex_add(g, 0, 0, 1, px) == 1);
  CHECK(gfx3d_tex_add(g, 3, 1, 2, px) == 2);
  for (int i = 3; i < GFX3D_MAX_TEXTURES; i++) CHECK(gfx3d_tex_add(g, 1, 1, 2, px) == i);
  CHECK(gfx3d_tex_add(g, 1, 1, 1, px) == -1);  // the 65th
  gfx3d_free(g);
}

static void test_image_add_resolves(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  const uint16_t a[6] = {1, 2, 3, 4, 5, 6}, b[1] = {9};
  CHECK(gfx3d_image_add(g, 0, 2, 0, a) == -1);
  CHECK(gfx3d_image_add(g, 3, 0, 0, a) == -1);
  CHECK(gfx3d_image_add(g, 3, 2, 0, NULL) == -1);
  CHECK(gfx3d_image_add(g, 3, 2, 0xF81F, a) == 0);
  CHECK(gfx3d_image_add(g, 1, 1, 7, b) == 1);
  const uint16_t *d = NULL;
  int w = 0, h = 0;
  uint16_t key = 0;
  CHECK(gfx3d_image_resolve(g, 0, &d, &w, &h, &key));
  CHECK(w == 3 && h == 2 && key == 0xF81F && d != NULL && d != a);  // a copy
  for (int i = 0; i < 6; i++) CHECK(d[i] == a[i]);
  CHECK(gfx3d_image_resolve(g, 1, &d, &w, &h, &key));
  CHECK(w == 1 && h == 1 && key == 7 && d[0] == 9);
  CHECK(!gfx3d_image_resolve(g, 2, &d, &w, &h, &key));
  CHECK(!gfx3d_image_resolve(g, -1, &d, &w, &h, &key));
  CHECK(!gfx3d_image_resolve(g, GFX3D_MAX_IMAGES, &d, &w, &h, &key));
  for (int i = 2; i < GFX3D_MAX_IMAGES; i++) CHECK(gfx3d_image_add(g, 1, 1, 0, b) == i);
  CHECK(gfx3d_image_add(g, 1, 1, 0, b) == -1);
  gfx3d_free(g);
}

static void test_tmesh_new_validates(void) {
  float xyz[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  float uv[6] = {0, 0, 1, 0, 0, 1};
  uint16_t col[3] = {0xFFFF, 0xFFFF, 0xFFFF}, idx[3] = {0, 1, 2};
  uint8_t tex[1] = {0};
  gfx3d_tmesh_desc_t d = {3, 1, xyz, uv, col, idx, tex, NULL};
  gfx3d_tmesh_t *m = gfx3d_tmesh_new(&d);
  CHECK(m != NULL);
  gfx3d_tmesh_free(m);
  gfx3d_tmesh_free(NULL);
  CHECK(gfx3d_tmesh_new(NULL) == NULL);

  idx[2] = 3;
  CHECK(gfx3d_tmesh_new(&d) == NULL);  // index out of range
  idx[2] = 2;
  xyz[4] = NAN;
  CHECK(gfx3d_tmesh_new(&d) == NULL);
  xyz[4] = INFINITY;
  CHECK(gfx3d_tmesh_new(&d) == NULL);
  xyz[4] = 0;
  uv[3] = NAN;
  CHECK(gfx3d_tmesh_new(&d) == NULL);
  uv[3] = 0;

  gfx3d_tmesh_desc_t e = d;
  e.nverts = GFX3D_MAX_VERTS + 1;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  e = d;
  e.nverts = 0;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  e = d;
  e.ntris = 0;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  e = d;
  e.ntris = GFX3D_MAX_TRIS + 1;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  e = d;
  e.uv = NULL;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  e = d;
  e.xyz = NULL;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  e = d;
  e.color = NULL;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  e = d;
  e.idx = NULL;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  e = d;
  e.tex = NULL;
  CHECK(gfx3d_tmesh_new(&e) == NULL);
  uint8_t fl[1] = {GFX3D_TM_BLEND | GFX3D_TM_NO_FOG};
  e = d;
  e.flags = fl;  // flags are optional
  m = gfx3d_tmesh_new(&e);
  CHECK(m != NULL);
  gfx3d_tmesh_free(m);
}

static void test_tmesh_new_copies(void) {
  float xyz[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  float uv[6] = {0, 0, 1, 0, 0, 1};
  uint16_t col[3] = {0x1111, 0x2222, 0x3333}, idx[3] = {0, 1, 2};
  uint8_t tex[1] = {5}, fl[1] = {GFX3D_TM_DOUBLE_SIDED};
  const float xyz0[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const float uv0[6] = {0, 0, 1, 0, 0, 1};
  const gfx3d_tmesh_desc_t d = {3, 1, xyz, uv, col, idx, tex, fl};
  gfx3d_tmesh_t *m = gfx3d_tmesh_new(&d);
  CHECK(m != NULL);
  memset(xyz, 0, sizeof xyz);
  memset(uv, 0, sizeof uv);
  memset(col, 0, sizeof col);
  memset(idx, 0, sizeof idx);
  tex[0] = 0;
  fl[0] = 0;
  const float *sx, *su;
  const uint16_t *sc, *si;
  const uint8_t *st, *sf;
  gfx3d_tmesh_test_arrays(m, &sx, &su, &sc, &si, &st, &sf);
  CHECK(memcmp(sx, xyz0, sizeof xyz0) == 0);
  CHECK(memcmp(su, uv0, sizeof uv0) == 0);
  CHECK(sc[0] == 0x1111 && sc[1] == 0x2222 && sc[2] == 0x3333);
  CHECK(si[0] == 0 && si[1] == 1 && si[2] == 2);
  CHECK(st[0] == 5 && sf[0] == GFX3D_TM_DOUBLE_SIDED);
  gfx3d_tmesh_free(m);

  // Without flags the stored flags are zero.
  const gfx3d_tmesh_desc_t d2 = {3, 1, xyz0, uv0, col, idx, tex, NULL};
  idx[1] = 1;
  idx[2] = 2;
  m = gfx3d_tmesh_new(&d2);
  CHECK(m != NULL);
  gfx3d_tmesh_test_arrays(m, &sx, &su, &sc, &si, &st, &sf);
  CHECK(sf[0] == 0);
  gfx3d_tmesh_free(m);
}

static void test_new_ex_capacity(void) {
  CHECK(gfx3d_new_ex(W, H, 0) == NULL);
  CHECK(gfx3d_new_ex(W, H, -1) == NULL);
  CHECK(gfx3d_new_ex(W, H, GFX3D_REC_LIMIT + 1) == NULL);
  gfx3d_t *g = gfx3d_new_ex(W, H, 8000);
  CHECK(g != NULL);
  gfx3d_mesh_t *many = gfx3d_mesh_alloc(3, 8000);
  const float v[9] = {-1, -1, -5, 1, -1, -5, 0, 1, -5};
  memcpy(many->xyz, v, sizeof v);
  for (int t = 0; t < 8000; t++) {
    many->idx[3 * t] = 0;
    many->idx[3 * t + 1] = 1;
    many->idx[3 * t + 2] = 2;
    many->color[t] = RED;
  }
  CHECK(gfx3d_mesh_finish(many) == NULL);
  gfx3d_begin(g, true, BG);
  gfx3d_draw(g, many, ID, ORIGIN, 1, 0, false);
  render(g);
  CHECK(gfx3d_get_stats(g)->drawn == 8000);
  CHECK(gfx3d_get_stats(g)->overflow == 0);
  gfx3d_begin(g, true, BG);  // one more than it holds overflows
  gfx3d_draw(g, many, ID, ORIGIN, 1, 0, false);
  gfx3d_draw(g, many, ID, ORIGIN, 1, 0, false);
  CHECK(gfx3d_get_stats(g)->overflow == 8000);
  gfx3d_mesh_free(many);
  gfx3d_free(g);
}

// ── Textured triangles, through the test-only raster entry ──

// Small buffers: thousands of triangles are compared whole.
#define TW 96
#define TH 64
static uint16_t s_ta[TW * TH], s_tb[TW * TH];

static uint32_t s_trng = 0x1234ABCDu;
static uint32_t trand(void) {
  s_trng ^= s_trng << 13;
  s_trng ^= s_trng >> 17;
  s_trng ^= s_trng << 5;
  return s_trng;
}
static float trandf(float lo, float hi) {
  return lo + (hi - lo) * (float)(trand() & 0xFFFFFFu) / 16777216.0f;
}

static gfx3d_target_t tex_target(uint16_t *fb, bool swap) {
  gfx3d_target_t t = {fb, TW, 0, 0, TW - 1, TH - 1, swap, NULL, NULL};
  return t;
}

static gfx3d_test_vert_t tv(float x, float y, float iw, float u, float v, uint16_t c) {
  gfx3d_test_vert_t r = {x, y, iw, u, v, c};
  return r;
}

// One solid colour, 2^lg x 2^lg, one level.
static int tex_solid(gfx3d_t *g, int lg, uint16_t c) {
  static uint16_t p[64 * 64];
  for (int i = 0; i < 1 << (2 * lg); i++) p[i] = c;
  return gfx3d_tex_add(g, lg, lg, 1, p);
}

static void tex_tri(gfx3d_t *g, const gfx3d_target_t *t, int slot, gfx3d_test_vert_t a,
                    gfx3d_test_vert_t b, gfx3d_test_vert_t c, bool blend,
                    gfx3d_test_tex_info_t *info) {
  const gfx3d_test_vert_t v[3] = {a, b, c};
  gfx3d_test_tex_tri(g, t, slot, 0, v, blend, info);
}

// A random triangle, in one of the shapes that break a rasteriser.
static void random_tri(float p[6]) {
  const uint32_t kind = trand() % 9u;
  const float cx = trandf(-20.0f, TW + 20.0f), cy = trandf(-20.0f, TH + 20.0f);
  for (int i = 0; i < 6; i += 2) {
    switch (kind) {
      case 0:  // anywhere near the buffer
        p[i] = trandf(-30.0f, TW + 30.0f);
        p[i + 1] = trandf(-30.0f, TH + 30.0f);
        break;
      case 1:  // on whole and half pixels: every tie the top-left rule decides
        p[i] = (float)(int)trandf(0.0f, TW * 2.0f) * 0.5f;
        p[i + 1] = (float)(int)trandf(0.0f, TH * 2.0f) * 0.5f;
        break;
      case 2:  // tiny
        p[i] = cx + trandf(-2.0f, 2.0f);
        p[i + 1] = cy + trandf(-2.0f, 2.0f);
        break;
      case 3:  // a sliver: nearly a line
        p[i] = cx + (float)(i - 2) * 30.0f + trandf(-0.05f, 0.05f);
        p[i + 1] = cy + (float)(i - 2) * 12.0f + trandf(-0.05f, 0.05f);
        break;
      case 4:  // flat top or bottom
        p[i] = trandf(0.0f, (float)TW);
        p[i + 1] = i < 4 ? cy : trandf(0.0f, (float)TH);
        break;
      case 5:  // big
        p[i] = trandf(-5000.0f, 5000.0f);
        p[i + 1] = trandf(-5000.0f, 5000.0f);
        break;
      case 6:  // huge
        p[i] = trandf(-3e7f, 3e7f);
        p[i + 1] = trandf(-3e7f, 3e7f);
        break;
      case 7:  // differences that overflow, on row centres: 0 * inf is a NaN on a vertex's row
        p[i] = (trand() & 3u) ? (trand() & 1u ? 1.0f : -1.0f) * trandf(1e37f, 3e38f) : trandf(0.0f, (float)TW);
        p[i + 1] = (float)(trand() % TH) + 0.5f;
        break;
      default:  // two vertices the same
        p[i] = i < 4 ? cx : trandf(0.0f, (float)TW);
        p[i + 1] = i < 4 ? cy : trandf(0.0f, (float)TH);
        break;
    }
  }
}

static void test_tex_tri_coverage_matches_flat(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  const int white = tex_solid(g, 2, 0xFFFF);
  CHECK(white >= 0);
  int bad = 0, drew = 0, persp = 0, affine = 0;
  for (int n = 0; n < 12000; n++) {
    float p[6];
    random_tri(p);
    disp_clip_t c = {0, 0, TW - 1, TH - 1};
    if (n % 3 == 1) {  // a part of the buffer
      c.x0 = (int)(trand() % (TW / 2));
      c.y0 = (int)(trand() % (TH / 2));
      c.x1 = c.x0 + (int)(trand() % (TW / 2));
      c.y1 = c.y0 + (int)(trand() % (TH / 2));
    }
    memset(s_ta, 0, sizeof s_ta);
    memset(s_tb, 0, sizeof s_tb);
    disp_fill_tri_f(s_ta, TW, &c, p[0], p[1], p[2], p[3], p[4], p[5], 0xFFFF);
    // Depth and uv have no say in coverage. Half of them take each path.
    const bool flat_depth = (n & 1) != 0;
    const gfx3d_test_vert_t v[3] = {
        tv(p[0], p[1], 0.5f, trandf(-4, 4), trandf(-4, 4), 0xFFFF),
        tv(p[2], p[3], flat_depth ? 0.5f : 0.05f, trandf(-4, 4), trandf(-4, 4), 0xFFFF),
        tv(p[4], p[5], flat_depth ? 0.5f : 0.2f, trandf(-4, 4), trandf(-4, 4), 0xFFFF)};
    gfx3d_begin(g, false, 0);
    gfx3d_target_t t = tex_target(s_tb, false);
    t.clip_x0 = c.x0;
    t.clip_y0 = c.y0;
    t.clip_x1 = c.x1;
    t.clip_y1 = c.y1;
    gfx3d_test_tex_info_t info;
    gfx3d_test_tex_tri(g, &t, white, 0, v, false, &info);
    int covered = 0, wrong = 0;
    for (int i = 0; i < TW * TH; i++) {
      if (s_ta[i]) covered++;
      if ((s_ta[i] != 0) != (s_tb[i] != 0)) wrong++;
    }
    if (wrong || gfx3d_get_stats(g)->pixels != (uint32_t)covered) {
      if (bad++ < 5)
        printf("coverage differs (%d px, %d covered, %u counted): (%g,%g) (%g,%g) (%g,%g) clip %d,%d,%d,%d\n",
               wrong, covered, (unsigned)gfx3d_get_stats(g)->pixels, (double)p[0], (double)p[1],
               (double)p[2], (double)p[3], (double)p[4], (double)p[5], c.x0, c.y0, c.x1, c.y1);
    }
    if (covered) {
      drew++;
      if (info.affine) affine++;
      else persp++;
    }
  }
  CHECK_EQ_INT(bad, 0);
  CHECK(drew > 3000);  // the test tests something
  CHECK(persp > 1000);
  CHECK(affine > 1000);

  // Coordinates whose differences overflow: an edge's x is NaN on a row that starts
  // on the vertex (0 * inf), and that row is left out, as the flat rasteriser does,
  // whichever edge of the row it is.
  static const float overflow[][6] = {{-3e38f, 10.5f, 3e38f, 50.0f, 0.0f, 30.0f},
                                      {3e38f, 10.5f, -3e38f, 50.0f, 0.0f, 30.0f},
                                      {40.0f, 10.5f, 3e38f, 20.5f, -3e38f, 50.5f},
                                      {40.0f, 10.5f, -3e38f, 20.5f, 3e38f, 50.5f}};
  int overflow_px = 0;
  for (size_t k = 0; k < sizeof overflow / sizeof overflow[0]; k++) {
    const float *p = overflow[k];
    memset(s_ta, 0, sizeof s_ta);
    memset(s_tb, 0, sizeof s_tb);
    const disp_clip_t c = {0, 0, TW - 1, TH - 1};
    disp_fill_tri_f(s_ta, TW, &c, p[0], p[1], p[2], p[3], p[4], p[5], 0xFFFF);
    gfx3d_target_t t = tex_target(s_tb, false);
    tex_tri(g, &t, white, tv(p[0], p[1], 1, 0, 0, 0xFFFF), tv(p[2], p[3], 1, 1, 0, 0xFFFF),
            tv(p[4], p[5], 1, 0, 1, 0xFFFF), false, NULL);
    int differ = 0;
    for (int i = 0; i < TW * TH; i++) {
      if ((s_ta[i] != 0) != (s_tb[i] != 0)) differ++;
      if (s_ta[i]) overflow_px++;
    }
    CHECK_EQ_INT(differ, 0);
  }
  CHECK(overflow_px > 0);

  // Non-finite vertices draw nothing, as the flat rasteriser.
  const float bad_v[4] = {NAN, INFINITY, -INFINITY, 1e30f};
  for (int k = 0; k < 4; k++) {
    for (int slot = 0; slot < 6; slot++) {
      float p[6] = {10, 10, 60, 12, 30, 50};
      p[slot] = bad_v[k];
      memset(s_ta, 0, sizeof s_ta);
      memset(s_tb, 0, sizeof s_tb);
      const disp_clip_t c = {0, 0, TW - 1, TH - 1};
      disp_fill_tri_f(s_ta, TW, &c, p[0], p[1], p[2], p[3], p[4], p[5], 0xFFFF);
      gfx3d_target_t t = tex_target(s_tb, false);
      tex_tri(g, &t, white, tv(p[0], p[1], 1, 0, 0, 0xFFFF), tv(p[2], p[3], 1, 1, 0, 0xFFFF),
              tv(p[4], p[5], 1, 0, 1, 0xFFFF), false, NULL);
      for (int i = 0; i < TW * TH; i++) CHECK((s_ta[i] != 0) == (s_tb[i] != 0));
    }
  }
  gfx3d_free(g);
}

// A triangle across the whole buffer in one colour on one colour of texture.
static uint16_t modulated(gfx3d_t *g, uint16_t texel, uint16_t vertex) {
  const int t = tex_solid(g, 0, texel);
  CHECK(t >= 0);
  memset(s_tb, 0, sizeof s_tb);
  gfx3d_target_t tg = tex_target(s_tb, false);
  tex_tri(g, &tg, t, tv(-10, -10, 1, 0, 0, vertex), tv(300, -10, 1, 1, 0, vertex),
          tv(-10, 300, 1, 0, 1, vertex), false, NULL);
  return s_tb[20 * TW + 20];
}

static void test_tex_modulate(void) {
  gfx3d_t *g = gfx3d_new(W, H);
#define MOD(tr, tg, tb, vr, vg, vb) \
  modulated(g, (uint16_t)(((tr) << 11) | ((tg) << 5) | (tb)), (uint16_t)(((vr) << 11) | ((vg) << 5) | (vb)))
  uint16_t c = MOD(31, 0, 0, 16, 0, 0);
  CHECK_EQ_INT(c >> 11, 24);
  c = MOD(31, 0, 0, 31, 0, 0);
  CHECK_EQ_INT(c >> 11, 31);  // saturates
  CHECK_EQ_INT((c >> 5) & 63, 0);
  CHECK_EQ_INT(c & 31, 0);
  c = MOD(0, 63, 0, 0, 32, 0);
  CHECK_EQ_INT((c >> 5) & 63, 48);
  c = MOD(0, 63, 0, 0, 63, 0);
  CHECK_EQ_INT((c >> 5) & 63, 63);
  c = MOD(0, 0, 31, 0, 0, 16);
  CHECK_EQ_INT(c & 31, 24);
  c = MOD(0, 0, 31, 0, 0, 31);
  CHECK_EQ_INT(c & 31, 31);
  CHECK_EQ_U32(MOD(31, 63, 31, 0, 0, 0), 0);  // no colour: black
  CHECK_EQ_U32(MOD(0, 0, 0, 31, 63, 31), 0);  // no texel: black
  CHECK_EQ_U32(MOD(31, 63, 31, 31, 63, 31), 0xFFFF);
  // Channels stay apart.
  c = MOD(31, 0, 31, 0, 63, 0);
  CHECK_EQ_U32(c, 0);
  gfx3d_free(g);

  // Every texel with every vertex value, a channel at a time: t * v * 1.5 / max,
  // saturating, within rounding. (A new context per sweep: textures are finite.)
  for (int ch = 0; ch < 3; ch++) {
    const int bits = ch == 1 ? 6 : 5, shift = ch == 0 ? 11 : ch == 1 ? 5 : 0;
    const int max = (1 << bits) - 1;
    gfx3d_t *h = gfx3d_new(W, H);
    int used = 0;
    for (int ti = 0; ti <= max; ti += 3) {
      if (used++ == GFX3D_MAX_TEXTURES - 1) {
        gfx3d_free(h);
        h = gfx3d_new(W, H);
        used = 1;
      }
      const int slot = tex_solid(h, 0, (uint16_t)(ti << shift));
      for (int vi = 0; vi <= max; vi += 2) {
        memset(s_tb, 0, sizeof s_tb);
        gfx3d_target_t tg = tex_target(s_tb, false);
        const uint16_t vc = (uint16_t)(vi << shift);
        tex_tri(h, &tg, slot, tv(0, 0, 1, 0, 0, vc), tv(40, 0, 1, 1, 0, vc), tv(0, 40, 1, 0, 1, vc), false, NULL);
        const int got = (s_tb[5 * TW + 5] >> shift) & max;
        double want = (double)ti * vi * 1.5 / max;
        if (want > max) want = max;
        CHECK(fabs((double)got - want) <= 1.0);
      }
    }
    gfx3d_free(h);
  }

  // The colour runs across the triangle, in steps of at most 8 pixels along a row and
  // exactly down the rows, in each channel. From 0 to 20 of it over 96 pixels.
  gfx3d_t *k = gfx3d_new(W, H);
  const int t = tex_solid(k, 0, 0xFFFF);
  for (int ch = 0; ch < 3; ch++) {
    const int shift = ch == 0 ? 11 : ch == 1 ? 5 : 0, max = ch == 1 ? 63 : 31;
    const uint16_t c20 = (uint16_t)(20 << shift);
    const double per_px = 20.0 / 96.0 * 1.5;  // a texel at its maximum is x1.5
    gfx3d_target_t tg = tex_target(s_tb, false);
    memset(s_tb, 0, sizeof s_tb);
    tex_tri(k, &tg, t, tv(0, 0, 1, 0, 0, 0), tv(96, 0, 1, 1, 0, c20), tv(0, 960, 1, 0, 1, 0), false, NULL);
    for (int x = 0; x < 90; x++) {
      const int got = (s_tb[2 * TW + x] >> shift) & max;
      CHECK(fabs((double)got - per_px * x) <= per_px * 8.0 + 1.0);
    }
    memset(s_tb, 0, sizeof s_tb);
    tex_tri(k, &tg, t, tv(0, 0, 1, 0, 0, 0), tv(960, 0, 1, 1, 0, 0), tv(0, 96, 1, 0, 1, c20), false, NULL);
    for (int y = 0; y < TH; y++) {
      const int got = (s_tb[y * TW + 2] >> shift) & max;
      CHECK(fabs((double)got - per_px * y) <= per_px + 1.0);
    }
  }
  gfx3d_free(k);
}

// ── An exact reference: the same triangle evaluated at every pixel, in double ──

typedef struct {
  double a, b, c;
} plane_t;

static plane_t plane_of(const gfx3d_test_vert_t *v, const double k[3]) {
  const double x0 = (double)v[0].x, y0 = (double)v[0].y, x1 = (double)v[1].x;
  const double y1 = (double)v[1].y, x2 = (double)v[2].x, y2 = (double)v[2].y;
  const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
  plane_t p;
  p.a = ((k[1] - k[0]) * (y2 - y0) - (k[2] - k[0]) * (y1 - y0)) / area;
  p.b = ((k[2] - k[0]) * (x1 - x0) - (k[1] - k[0]) * (x2 - x0)) / area;
  p.c = k[0] - p.a * x0 - p.b * y0;
  return p;
}
static double plane_at(plane_t p, int x, int y) {
  return p.a * ((double)x + 0.5) + p.b * ((double)y + 0.5) + p.c;
}

// How many drawn pixels of `fb` hold no colour of `tex` (w x h) within `tol` texels
// of where the exact (perspective or affine) uv lands. An undrawn pixel (`bg`) is
// not looked at.
static int reference_misses(const uint16_t *fb, uint16_t bg, const gfx3d_test_vert_t *v,
                            const uint16_t *want, int w, int h, int tol, bool perspective,
                            int *checked) {
  double ku[3], kv[3], ki[3];
  for (int i = 0; i < 3; i++) {
    const double wgt = perspective ? (double)v[i].iw : 1.0;
    ki[i] = wgt;
    ku[i] = (double)v[i].u * wgt;
    kv[i] = (double)v[i].v * wgt;
  }
  const plane_t pi = plane_of(v, ki), pu = plane_of(v, ku), pv = plane_of(v, kv);
  int misses = 0;
  for (int y = 0; y < TH; y++)
    for (int x = 0; x < TW; x++) {
      const uint16_t got = fb[y * TW + x];
      if (got == bg) continue;
      const double iw = plane_at(pi, x, y);
      const double tu = plane_at(pu, x, y) / iw * w, tv_ = plane_at(pv, x, y) / iw * h;
      bool ok = false;
      for (int dy = -tol; dy <= tol && !ok; dy++)
        for (int dx = -tol; dx <= tol && !ok; dx++) {
          const int ix = ((int)floor(tu) + dx) & (w - 1), iy = ((int)floor(tv_) + dy) & (h - 1);
          ok = want[iy * w + ix] == got;
        }
      if (!ok) misses++;
      if (checked) (*checked)++;
    }
  return misses;
}

// Black and white squares of `cell` texels. A white vertex keeps them black and
// white: 31 * 1.5 saturates.
static void make_checker(uint16_t *p, int w, int h, int cell) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) p[y * w + x] = ((x / cell + y / cell) & 1) ? 0xFFFF : 0x0000;
}

// A wall going away to the right: depth changes along every row, so only a span
// that follows 1/z gets it right. `far` is the far edge's depth (the near's is 1).
static void wall(gfx3d_test_vert_t q[4], float far, float us, float vs) {
  const float iw = 1.0f / far, cy = TH * 0.5f, hn = TH * 0.7f, hf = hn * iw;
  q[0] = tv(-4, cy - hn, 1, 0, 0, 0xFFFF);
  q[1] = tv(TW + 4, cy - hf, iw, us, 0, 0xFFFF);
  q[2] = tv(TW + 4, cy + hf, iw, us, vs, 0xFFFF);
  q[3] = tv(-4, cy + hn, 1, 0, vs, 0xFFFF);
}

static void test_tex_perspective(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  static uint16_t chk[32 * 32];
  make_checker(chk, 32, 32, 4);
  const int slot = gfx3d_tex_add(g, 5, 5, 1, chk);
  gfx3d_test_vert_t q[4];
  wall(q, 3.0f, 1.0f, 1.0f);
  // The two triangles of the quad, each against its own reference. A sentinel
  // that is neither texel colour marks what was not drawn.
  const uint16_t bg = 0x1234;
  const gfx3d_test_vert_t tris[2][3] = {{q[0], q[1], q[2]}, {q[0], q[2], q[3]}};
  int checked = 0, affine_misses = 0;
  for (int i = 0; i < 2; i++) {
    for (int p = 0; p < TW * TH; p++) s_tb[p] = bg;
    gfx3d_target_t t = tex_target(s_tb, false);
    gfx3d_test_tex_info_t info;
    gfx3d_test_tex_tri(g, &t, slot, 0, tris[i], false, &info);
    CHECK(!info.affine);
    CHECK_EQ_INT(reference_misses(s_tb, bg, tris[i], chk, 32, 32, 1, true, &checked), 0);
    affine_misses += reference_misses(s_tb, bg, tris[i], chk, 32, 32, 1, false, NULL);
  }
  CHECK(checked > TW * TH / 2);
  CHECK(affine_misses > 100);  // a span that ignored 1/z would fail this test
  gfx3d_free(g);
}

// Every texel its own colour, so a wrong row or column shows.
static void make_unique(uint16_t *p, int w, int h) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) p[y * w + x] = (uint16_t)(0x8000u | (uint32_t)(y * w + x));
}

// About x1 under the texel multiply, so that different texels stay different.
#define WRAP_VERTEX 0xAD55

// What the rasteriser makes of each of n texels under WRAP_VERTEX, from textures of one
// texel: channels do not mix (test_tex_modulate), so one draw per channel value does.
static void modulated_table(const uint16_t *tex, int n, uint16_t *out) {
  gfx3d_t *g = gfx3d_new(W, H);
  static uint16_t by_value[64];
  for (int v = 0; v < 64; v++)
    by_value[v] = modulated(g, (uint16_t)(((v & 31) << 11) | (v << 5) | (v & 31)), WRAP_VERTEX);
  gfx3d_free(g);
  for (int i = 0; i < n; i++) {
    const uint16_t r = by_value[tex[i] >> 11] & 0xF800u, gr = by_value[(tex[i] >> 5) & 63] & 0x07E0u;
    const uint16_t b = by_value[tex[i] & 31] & 0x001Fu;
    out[i] = (uint16_t)(r | gr | b);
  }
}

// Whether a triangle with these runs of uv (in repeats) goes affine, with 1/z varying by
// `ratio` along the edge that carries them.
static bool goes_affine(gfx3d_t *g, int slot, int level, float ratio, float run_u, float run_v) {
  gfx3d_target_t t = tex_target(s_tb, false);
  const gfx3d_test_vert_t v[3] = {tv(3, 3, ratio, 0, 0, 0xFFFF), tv(70, 7, 1, run_u, 0, 0xFFFF),
                                  tv(9, 60, ratio, 0, run_v, 0xFFFF)};
  gfx3d_test_tex_info_t info;
  gfx3d_test_tex_tri(g, &t, slot, level, v, false, &info);
  return info.affine;
}

static void test_tex_affine_shortcut_close(void) {
  static uint16_t tex[64 * 64], moded[64 * 64];
  static uint16_t wrong[TW * TH];
  const uint16_t bg = 0x0001;  // no texel comes out as this: their red is high
  for (int lg = 5; lg <= 6; lg++) {
    const int size = 1 << lg;
    make_unique(tex, size, size);
    modulated_table(tex, size * size, moded);
    gfx3d_t *g = gfx3d_new(W, H);
    const int slot = gfx3d_tex_add(g, lg, lg, 1, tex);
    gfx3d_target_t t = tex_target(s_tb, false);
    // The uv runs one repeat each way, so the extent is the texture's size. Up to the
    // most 1/z may vary for that size, whichever vertex has which depth: the shortcut,
    // and at every pixel within a texel of the exact picture.
    const float most = 1.0f + 4.0f / (float)size;
    static const float part[3] = {0.0f, 0.5f, 1.0f};
    for (int p = 0; p < 3; p++) {
      const float r = 1.0f + (most - 1.0f) * part[p];
      for (int near_vert = 0; near_vert < 6; near_vert++) {
        float iw[3] = {1.0f, 1.0f, 1.0f};
        if (near_vert < 3) {
          iw[near_vert] = r;
        } else {  // two near and one far
          iw[0] = iw[1] = iw[2] = r;
          iw[near_vert - 3] = 1.0f;
        }
        const gfx3d_test_vert_t tri[3] = {tv(-2, -2, iw[0], 0, 0, WRAP_VERTEX),
                                          tv(TW + 2, 0, iw[1], 1, 0, WRAP_VERTEX),
                                          tv(0, TH + 2, iw[2], 0, 1, WRAP_VERTEX)};
        for (int i = 0; i < TW * TH; i++) s_tb[i] = bg;
        gfx3d_test_tex_info_t info;
        gfx3d_test_tex_tri(g, &t, slot, 0, tri, false, &info);
        CHECK(info.affine);
        int checked = 0;
        CHECK_EQ_INT(reference_misses(s_tb, bg, tri, moded, size, size, 1, true, &checked), 0);
        CHECK(checked > TW * TH / 3);

        // That check can fail. The picture three texels along the uv is another picture
        // (nearly a texel of error, and it could still be within the window at two); the
        // exact picture of a depth varying six times as much is too; and neither one
        // colour nor noise is near it.
        if (p == 2) {
          gfx3d_test_vert_t moved[3] = {tri[0], tri[1], tri[2]};
          for (int i = 0; i < 3; i++) moved[i].u += 3.0f / (float)size;
          for (int i = 0; i < TW * TH; i++) s_ta[i] = bg;
          gfx3d_target_t ta = tex_target(s_ta, false);
          gfx3d_test_tex_tri(g, &ta, slot, 0, moved, false, NULL);
          int seen = 0;
          CHECK(reference_misses(s_ta, bg, tri, moded, size, size, 1, true, &seen) > seen / 2);
          for (int i = 0; i < TW * TH; i++) wrong[i] = moded[0];
          seen = 0;
          CHECK(reference_misses(wrong, bg, tri, moded, size, size, 1, true, &seen) > seen * 9 / 10);
          for (int i = 0; i < TW * TH; i++) wrong[i] = (uint16_t)(0x8000u | (trand() & 0x7FFFu));
          seen = 0;
          CHECK(reference_misses(wrong, bg, tri, moded, size, size, 1, true, &seen) > seen * 9 / 10);
          gfx3d_test_vert_t deeper[3] = {tri[0], tri[1], tri[2]};
          for (int i = 0; i < 3; i++) deeper[i].iw = 1.0f + (iw[i] - 1.0f) * 6.0f;
          CHECK(reference_misses(s_tb, bg, deeper, moded, size, size, 1, true, NULL) > 50);
        }
      }
    }
    // Just past that, the same triangle divides.
    CHECK(goes_affine(g, slot, 0, most, 1, 1));
    CHECK(!goes_affine(g, slot, 0, most + 2.0f / 256.0f, 1, 1));
    gfx3d_free(g);
  }

  // Where each limit stops. Half a repeat of 32 texels leaves only the 20%, whichever
  // vertex has which depth.
  static uint16_t mips[64 * 64 + 32 * 32];
  for (int i = 0; i < 64 * 64 + 32 * 32; i++) mips[i] = 0xFFFF;
  gfx3d_t *g = gfx3d_new(W, H);
  const int t32 = gfx3d_tex_add(g, 5, 5, 1, mips), t64 = gfx3d_tex_add(g, 6, 6, 2, mips);
  const int wide = gfx3d_tex_add(g, 6, 4, 1, mips), tall = gfx3d_tex_add(g, 4, 6, 1, mips);
  const int one_wide = gfx3d_tex_add(g, 0, 3, 1, mips), one = gfx3d_tex_add(g, 0, 0, 1, mips);
  static const float ratio[4] = {1.0f, 1.1f, 1.19f, 1.21f};
  static const bool want[4] = {true, true, true, false};
  for (int i = 0; i < 4; i++)
    for (int order = 0; order < 3; order++) {
      float iw[3] = {0.25f, 0.25f, 0.25f};
      iw[order] = 0.25f * ratio[i];
      gfx3d_target_t tg = tex_target(s_tb, false);
      gfx3d_test_tex_info_t info;
      tex_tri(g, &tg, t32, tv(3, 3, iw[0], 0, 0, 0xFFFF), tv(70, 7, iw[1], 0.5f, 0, 0xFFFF),
              tv(9, 60, iw[2], 0, 0.5f, 0xFFFF), false, &info);
      CHECK(info.affine == want[i]);
    }
  // The extent is in texels of the mip drawn: level 1 of 64 is 32.
  CHECK(!goes_affine(g, t64, 0, 1.08f, 1, 1));
  CHECK(goes_affine(g, t64, 1, 1.08f, 1, 1));
  // Twice the repeats is twice the extent.
  CHECK(goes_affine(g, t32, 0, 1.08f, 1, 1));
  CHECK(!goes_affine(g, t32, 0, 1.08f, 2, 2));
  // The longer way in texels decides, on a texture that is not square.
  CHECK(!goes_affine(g, wide, 0, 1.08f, 1, 0.25f));
  CHECK(goes_affine(g, wide, 0, 1.08f, 0.25f, 1));
  CHECK(!goes_affine(g, tall, 0, 1.08f, 0.25f, 1));
  CHECK(goes_affine(g, tall, 0, 1.08f, 1, 0.25f));
  // The run of the uv, not its size: far from 0, or across the opposite corners.
  gfx3d_target_t tg = tex_target(s_tb, false);
  gfx3d_test_tex_info_t info;
  tex_tri(g, &tg, t32, tv(3, 3, 1.08f, 500.25f, -300.5f, 0xFFFF), tv(70, 7, 1, 500.75f, -300.5f, 0xFFFF),
          tv(9, 60, 1.08f, 500.25f, -300.0f, 0xFFFF), false, &info);
  CHECK(info.affine);
  tex_tri(g, &tg, t32, tv(3, 3, 1.08f, 0, 0, 0xFFFF), tv(70, 7, 1, 1.5f, 0, 0xFFFF),
          tv(9, 60, 1.08f, -1.5f, 0, 0xFFFF), false, &info);
  CHECK(!info.affine);
  // Depth that does not change: any size. A one-texel texture has no uv to get wrong.
  CHECK(goes_affine(g, t64, 0, 1.0f, 1000, 1000));
  CHECK(goes_affine(g, one, 0, 1.15f, 1000, 1000));
  CHECK(!goes_affine(g, one_wide, 0, 1.15f, 1000, 1000));
  gfx3d_free(g);
}

static void test_tex_wrap(void) {
  static const int lg[][2] = {{3, 3}, {4, 2}, {2, 4}, {0, 3}, {3, 0}, {6, 1}, {1, 6}, {0, 0}};
  static uint16_t tex[128], moded[128];
  for (size_t n = 0; n < sizeof lg / sizeof lg[0]; n++) {
    const int lw = lg[n][0], lh = lg[n][1], w = 1 << lw, h = 1 << lh;
    make_unique(tex, w, h);
    // What the rasteriser makes of each texel, from a texture of that one texel:
    // this test looks at where a texel lands, not at its arithmetic.
    gfx3d_t *g = NULL;
    for (int i = 0; i < w * h; i++) {
      if (i % 60 == 0) {
        gfx3d_free(g);
        g = gfx3d_new(W, H);
      }
      moded[i] = modulated(g, tex[i], WRAP_VERTEX);
    }
    gfx3d_free(g);

    g = gfx3d_new(W, H);
    CHECK(gfx3d_tex_add(g, lw, lh, 1, tex) == 0);
    const uint16_t bg = 0x0001;  // no texel comes out as this: their red is high
    gfx3d_target_t t = tex_target(s_tb, false);
    // u from -0.75 to 1.25, v from -1.25 to 0.75: a seam in the middle of each.
    // Constant depth (the shortcut), then perspective.
    for (int persp = 0; persp < 2; persp++) {
      const float i0 = 1.0f, i1 = persp ? 0.6f : 1.0f, i2 = persp ? 0.8f : 1.0f;
      const gfx3d_test_vert_t tri[2][3] = {
          {tv(0, 0, i0, -0.75f, -1.25f, WRAP_VERTEX), tv(TW, 0, i1, 1.25f, -1.25f, WRAP_VERTEX),
           tv(0, TH, i2, -0.75f, 0.75f, WRAP_VERTEX)},
          {tv(TW, 0, i1, 1.25f, -1.25f, WRAP_VERTEX), tv(TW, TH, i0, 1.25f, 0.75f, WRAP_VERTEX),
           tv(0, TH, i2, -0.75f, 0.75f, WRAP_VERTEX)}};
      for (int i = 0; i < 2; i++) {
        for (int p = 0; p < TW * TH; p++) s_tb[p] = bg;
        gfx3d_test_tex_info_t info;
        gfx3d_test_tex_tri(g, &t, 0, 0, tri[i], false, &info);
        if (w * h > 1) CHECK(info.affine == !persp);
        int checked = 0;
        CHECK_EQ_INT(reference_misses(s_tb, bg, tri[i], moded, w, h, 1, true, &checked), 0);
        CHECK(checked > 1000);
      }
    }
    // Whole repeats change nothing: the same picture with uv moved far either way.
    static const float shift[][2] = {{1, 0}, {0, 1}, {-3, 5}, {1024, -2048}, {-4096, 512}};
    for (int persp = 0; persp < 2; persp++) {
      const gfx3d_test_vert_t base[3] = {
          tv(5.5f, 3.25f, 0.5f, 0.125f, 0.25f, WRAP_VERTEX),
          tv(90.25f, 20.5f, persp ? 0.2f : 0.5f, 1.625f, 0.5f, WRAP_VERTEX),
          tv(30.75f, 60.5f, 0.5f, 0.5f, 1.75f, WRAP_VERTEX)};
      memset(s_ta, 0, sizeof s_ta);
      gfx3d_target_t ta = tex_target(s_ta, false);
      gfx3d_test_tex_tri(g, &ta, 0, 0, base, false, NULL);
      for (size_t s = 0; s < sizeof shift / sizeof shift[0]; s++) {
        gfx3d_test_vert_t moved[3];
        for (int i = 0; i < 3; i++) {
          moved[i] = base[i];
          moved[i].u += shift[s][0];
          moved[i].v += shift[s][1];
        }
        memset(s_tb, 0, sizeof s_tb);
        gfx3d_test_tex_tri(g, &t, 0, 0, moved, false, NULL);
        CHECK(memcmp(s_ta, s_tb, sizeof s_ta) == 0);
      }
    }
    gfx3d_free(g);
  }
}

// Large textures: past 16 bits of index, v's bits rotate round to reach their place.
static void test_tex_large_shapes(void) {
  static const int lg[][2] = {{8, 8}, {10, 6}, {6, 10}, {10, 7}, {7, 10}, {9, 9}, {10, 10}};
  static uint16_t tex[1024 * 1024], moded[1024 * 1024];
  const uint16_t bg = 0x0001;
  for (size_t n = 0; n < sizeof lg / sizeof lg[0]; n++) {
    const int lw = lg[n][0], lh = lg[n][1], w = 1 << lw, h = 1 << lh;
    // Random texels, with red high so that none comes out as bg; a neighbour rarely
    // has the same colour, so the window below is nearly as strict as unique ones.
    uint32_t r = 0x51ED270Bu;
    for (int i = 0; i < w * h; i++) {
      r ^= r << 13;
      r ^= r >> 17;
      r ^= r << 5;
      tex[i] = (uint16_t)(0x8000u | (r & 0x7FFFu));
    }
    modulated_table(tex, w * h, moded);
    gfx3d_t *g = gfx3d_new(W, H);
    CHECK(gfx3d_tex_add(g, lw, lh, 1, tex) == 0);
    gfx3d_target_t t = tex_target(s_tb, false);
    // About a texel to the pixel, somewhere in the middle of the texture; then the
    // same with depth, for the other path.
    const float u0 = 0.37f, v0 = 0.61f, du = (float)TW / (float)w, dv = (float)TH / (float)h;
    for (int persp = 0; persp < 2; persp++) {
      const float far = persp ? 0.8f : 1.0f;
      const gfx3d_test_vert_t tri[3] = {tv(0, 0, 1, u0, v0, WRAP_VERTEX), tv(TW, 0, far, u0 + du, v0, WRAP_VERTEX),
                                        tv(0, TH, 1, u0, v0 + dv, WRAP_VERTEX)};
      for (int p = 0; p < TW * TH; p++) s_tb[p] = bg;
      gfx3d_test_tex_info_t info;
      gfx3d_test_tex_tri(g, &t, 0, 0, tri, false, &info);
      CHECK(info.affine == !persp);
      int checked = 0;
      CHECK_EQ_INT(reference_misses(s_tb, bg, tri, moded, w, h, 1, true, &checked), 0);
      CHECK(checked > TW * TH / 3);
    }
    gfx3d_free(g);
  }
}

// Each mip level a solid colour: `level` picks it, and past the last means the last.
static void test_tex_levels(void) {
  static const int lg[][3] = {{4, 4, 5}, {3, 1, 2}, {1, 3, 2}, {0, 0, 1}};
  static const uint16_t colour[5] = {RED, GREEN, BLUE, 0xFFE0, 0x07FF};
  for (size_t n = 0; n < sizeof lg / sizeof lg[0]; n++) {
    gfx3d_t *g = gfx3d_new(W, H);
    static uint16_t px[1024];
    int at = 0;
    for (int k = 0; k < lg[n][2]; k++)
      for (int i = 0; i < 1 << (lg[n][0] - k + lg[n][1] - k); i++) px[at++] = colour[k];
    const int slot = gfx3d_tex_add(g, lg[n][0], lg[n][1], lg[n][2], px);
    for (int level = 0; level < lg[n][2] + 2; level++) {
      const int last = lg[n][2] - 1;
      const uint16_t want = modulated(g, colour[level > last ? last : level], 0xFFFF);
      memset(s_tb, 0, sizeof s_tb);
      gfx3d_target_t t = tex_target(s_tb, false);
      const gfx3d_test_vert_t v[3] = {tv(0, 0, 1, 0, 0, 0xFFFF), tv(TW, 0, 1, 3, 0, 0xFFFF),
                                      tv(0, TH, 1, 0, 3, 0xFFFF)};
      gfx3d_test_tex_tri(g, &t, slot, level, v, false, NULL);
      CHECK_EQ_U32(s_tb[10 * TW + 10], want);
    }
    gfx3d_free(g);
  }
}

// With swap, every pixel is the same pixel byte-swapped, in both spans and both
// paths: the variants of a span differ in byte order and nothing else.
static void test_tex_swap_is_only_byte_order(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  static uint16_t tex[64 * 16];
  for (int i = 0; i < 64 * 16; i++) tex[i] = (uint16_t)(i * 2654435761u >> 16);
  const int slot = gfx3d_tex_add(g, 6, 4, 1, tex);
  int drew = 0;
  for (int n = 0; n < 400; n++) {
    const bool flat_depth = (n & 1) != 0, blend = (n & 2) != 0;
    const gfx3d_test_vert_t v[3] = {
        tv(trandf(-40, TW + 40), trandf(-40, TH + 40), 0.5f, trandf(-3, 3), trandf(-3, 3), (uint16_t)trand()),
        tv(trandf(-40, TW + 40), trandf(-40, TH + 40), flat_depth ? 0.51f : 0.1f, trandf(-3, 3), trandf(-3, 3),
           (uint16_t)trand()),
        tv(trandf(-40, TW + 40), trandf(-40, TH + 40), flat_depth ? 0.49f : 0.3f, trandf(-3, 3), trandf(-3, 3),
           (uint16_t)trand())};
    for (int i = 0; i < TW * TH; i++) {
      const uint16_t d = (uint16_t)(i * 40503u);
      s_ta[i] = d;
      s_tb[i] = disp_px(d, true);
    }
    gfx3d_target_t ta = tex_target(s_ta, false), tb = tex_target(s_tb, true);
    gfx3d_test_tex_tri(g, &ta, slot, 0, v, blend, NULL);
    gfx3d_test_tex_tri(g, &tb, slot, 0, v, blend, NULL);
    bool same = true;
    for (int i = 0; i < TW * TH; i++) same = same && s_tb[i] == disp_px(s_ta[i], true);
    CHECK(same);
    if (gfx3d_get_stats(g)->pixels) drew++;
  }
  CHECK(drew > 100);
  gfx3d_free(g);
}

static void test_tex_blend(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  static uint16_t tex[16 * 16];
  for (int i = 0; i < 16 * 16; i++) tex[i] = (uint16_t)((uint32_t)i * 2654435761u >> 16);
  const int slot = gfx3d_tex_add(g, 4, 4, 1, tex);
  for (int swap = 0; swap < 2; swap++) {
    for (int persp = 0; persp < 2; persp++) {
      const float far = persp ? 0.25f : 1.0f;
      const gfx3d_test_vert_t v[3] = {tv(-3, 2, 1, 0, 0, 0xFFFF), tv(TW + 8, 9, far, 2, 0, 0x7BEF),
                                      tv(11, TH + 3, far, 0, 2, 0xC638)};
      // src: the same triangle opaque on black.
      memset(s_ta, 0, sizeof s_ta);
      gfx3d_target_t ta = tex_target(s_ta, swap != 0);
      gfx3d_test_tex_tri(g, &ta, slot, 0, v, false, NULL);
      // dst: a pattern, in the target's byte order.
      static uint16_t dst[TW * TH];
      for (int i = 0; i < TW * TH; i++) {
        dst[i] = (uint16_t)((uint32_t)i * 40503u);
        s_tb[i] = disp_px(dst[i], swap != 0);
      }
      gfx3d_target_t tb = tex_target(s_tb, swap != 0);
      gfx3d_test_tex_info_t info;
      gfx3d_test_tex_tri(g, &tb, slot, 0, v, true, &info);
      CHECK(info.affine == !persp);
      int blended = 0;
      for (int i = 0; i < TW * TH; i++) {
        const uint16_t s = disp_px(s_ta[i], swap != 0), d = dst[i];
        // The plain draw left 0 where it did not draw, and a black texel draws 0
        // too: such a pixel may be either, and only the covered ones count.
        const uint16_t mixed = (uint16_t)(((s >> 1) & 0x7BEF) + ((d >> 1) & 0x7BEF));
        const uint16_t got = disp_px(s_tb[i], swap != 0);
        if (s == 0) {
          CHECK(got == d || got == mixed);
        } else {
          CHECK_EQ_U32(got, mixed);
          blended++;
        }
      }
      CHECK(blended > 2000);
    }
  }
  // 50% of white over black, and of black over white: each half the full value.
  const int white = tex_solid(g, 0, 0xFFFF);
  memset(s_tb, 0, sizeof s_tb);
  gfx3d_target_t t = tex_target(s_tb, false);
  tex_tri(g, &t, white, tv(-9, -9, 1, 0, 0, 0xFFFF), tv(300, -9, 1, 1, 0, 0xFFFF),
          tv(-9, 300, 1, 0, 1, 0xFFFF), true, NULL);
  CHECK_EQ_U32(s_tb[5 * TW + 5], 0x7BEF);
  gfx3d_free(g);
}

static void test_tex_clip(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  static uint16_t chk[64 * 64];
  make_checker(chk, 64, 64, 8);
  const int slot = gfx3d_tex_add(g, 6, 6, 1, chk);
  const uint16_t bg = 0x1234;
  // A corner of the buffer, through a triangle far bigger than the screen, and the
  // whole buffer. Both paths.
  static const int clips[2][4] = {{10, 20, 49, 59}, {0, 0, TW - 1, TH - 1}};
  static const float size[3] = {2e3f, 1e6f, 3e7f};
  for (int c = 0; c < 2; c++)
    for (int sz = 0; sz < 3; sz++)
      for (int persp = 0; persp < 2; persp++) {
        for (int p = 0; p < TW * TH; p++) s_tb[p] = bg;
        gfx3d_begin(g, false, 0);
        gfx3d_target_t t = tex_target(s_tb, false);
        t.clip_x0 = clips[c][0];
        t.clip_y0 = clips[c][1];
        t.clip_x1 = clips[c][2];
        t.clip_y1 = clips[c][3];
        const float s = size[sz], far = persp ? 0.5f : 1.0f;
        const float rep = s / 100.0f;  // a repeat per 100 pixels, whatever the size
        const gfx3d_test_vert_t v[3] = {tv(-s, -s, 1, -rep, -rep, 0xFFFF), tv(2 * s, -s, far, 2 * rep, -rep, 0xFFFF),
                                        tv(-s, 2 * s, far, -rep, 2 * rep, 0xFFFF)};
        gfx3d_test_tex_info_t info;
        gfx3d_test_tex_tri(g, &t, slot, 0, v, false, &info);
        const int w = clips[c][2] - clips[c][0] + 1, h = clips[c][3] - clips[c][1] + 1;
        CHECK_EQ_U32(gfx3d_get_stats(g)->pixels, (uint32_t)(w * h));
        CHECK_EQ_INT(info.rows, h);  // the rows it walked: the visible ones
        int inside = 0, outside = 0;
        for (int y = 0; y < TH; y++)
          for (int x = 0; x < TW; x++) {
            const bool in = x >= clips[c][0] && x <= clips[c][2] && y >= clips[c][1] && y <= clips[c][3];
            if (s_tb[y * TW + x] != bg) {
              if (in) inside++;
              else outside++;
            }
          }
        CHECK_EQ_INT(inside, w * h);
        CHECK_EQ_INT(outside, 0);
        // And it is still the right picture, where the floats can say so.
        if (sz == 0) {
          int checked = 0;
          CHECK_EQ_INT(reference_misses(s_tb, bg, v, chk, 64, 64, 1, true, &checked), 0);
          CHECK_EQ_INT(checked, w * h);
        }
      }
  // Nothing on screen, and an empty clip, walk nothing.
  gfx3d_begin(g, false, 0);
  gfx3d_target_t t = tex_target(s_tb, false);
  gfx3d_test_tex_info_t info;
  tex_tri(g, &t, slot, tv(10, 500, 1, 0, 0, 0xFFFF), tv(40, 500, 1, 1, 0, 0xFFFF), tv(10, 900, 1, 0, 1, 0xFFFF),
          false, &info);
  CHECK_EQ_INT(info.rows, 0);
  CHECK_EQ_U32(gfx3d_get_stats(g)->pixels, 0);
  gfx3d_free(g);
}

// Whatever comes in, the coverage is the flat rasteriser's and nothing is read
// outside the texture or converted out of range (ASan and UBSan watch this).
static void test_tex_bad_values_stay_safe(void) {
  gfx3d_t *g = gfx3d_new(W, H);
  static uint16_t tex[8 * 2];
  for (int i = 0; i < 16; i++) tex[i] = 0xFFFF;
  const int slot = gfx3d_tex_add(g, 3, 1, 1, tex);
  const float bad[] = {0.0f, -0.0f, -1.0f, 1e-30f, 1e-45f, 1e30f, -1e30f, 3e9f, INFINITY, -INFINITY, NAN};
  const int nb = (int)(sizeof bad / sizeof bad[0]);
  const float p[6] = {-20, 3, TW + 40, 17, 20, TH + 11};
  memset(s_ta, 0, sizeof s_ta);
  const disp_clip_t c = {0, 0, TW - 1, TH - 1};
  disp_fill_tri_f(s_ta, TW, &c, p[0], p[1], p[2], p[3], p[4], p[5], 0xFFFF);
  for (int blend = 0; blend < 2; blend++)
    for (int which = 0; which < 3; which++)     // iw, u, v
      for (int vert = 0; vert < 3; vert++)
        for (int i = 0; i < nb; i++) {
          gfx3d_test_vert_t v[3] = {tv(p[0], p[1], 0.5f, 0, 0, 0xFFFF), tv(p[2], p[3], 0.25f, 1, 0, 0xFFFF),
                                    tv(p[4], p[5], 0.5f, 0, 1, 0xFFFF)};
          if (which == 0) v[vert].iw = bad[i];
          else if (which == 1) v[vert].u = bad[i];
          else v[vert].v = bad[i];
          gfx3d_begin(g, false, 0);
          memset(s_tb, 0, sizeof s_tb);
          gfx3d_target_t t = tex_target(s_tb, false);
          gfx3d_test_tex_tri(g, &t, slot, 0, v, blend != 0, NULL);
          // Every covered pixel counted, every pixel drawn inside the coverage.
          int covered = 0;
          bool outside = false;
          for (int k = 0; k < TW * TH; k++) {
            if (s_ta[k]) covered++;
            else if (s_tb[k]) outside = true;
          }
          CHECK(!outside);
          CHECK_EQ_U32(gfx3d_get_stats(g)->pixels, (uint32_t)covered);
        }

  // A one-texel texture reads its one texel, whatever the arithmetic before it comes
  // to: here coordinates whose differences overflow, in both paths.
  static const uint16_t one[1] = {0xFFFF};
  const int single = gfx3d_tex_add(g, 0, 0, 1, one);
  const float huge = 3e38f;
  static const float depth[2] = {0.5f, 0.05f};
  for (int d = 0; d < 2; d++) {
    const gfx3d_test_vert_t tris[3][3] = {
        {tv(-huge, -huge, 0.5f, 1e30f, -1e30f, 0xFFFF), tv(huge, -huge, depth[d], -1e30f, 1e30f, 0xFFFF),
         tv(0, huge, 0.5f, NAN, INFINITY, 0xFFFF)},
        {tv(-huge, 5, 0.5f, 0, 0, 0xFFFF), tv(huge, 5, depth[d], 1, 0, 0xFFFF), tv(0, 40, 0.5f, 0, 1, 0xFFFF)},
        {tv(10, 3, 0.5f, 0, 0, 0xFFFF), tv(70, 4, depth[d], 1e30f, 0, 0xFFFF), tv(20, 60, 0.5f, 0, 1e30f, 0xFFFF)}};
    for (int i = 0; i < 3; i++) {
      memset(s_tb, 0, sizeof s_tb);
      gfx3d_target_t t = tex_target(s_tb, false);
      gfx3d_test_tex_tri(g, &t, single, 0, tris[i], false, NULL);
      for (int k = 0; k < TW * TH; k++) CHECK(s_tb[k] == 0 || s_tb[k] == 0xFFFF);
    }
  }
  gfx3d_free(g);
}

// The rasteriser's bytes, pinned. test_gfx3d_tex_m33 and tools/perf_rig's check-tex draw
// these triangles on the M33's code: if they disagree, its pixel loop and the C here differ.
#define TEX_SCENE_CRC_HOST 0x4539b902u
#define TEX_SCENE_CRC_SWAP 0x6ef4cbbcu

static void test_tex_scene_crc(void) {
  static uint16_t fb[TEX_SCENE_W * TEX_SCENE_H];
  static uint16_t texels[5461];
  for (int swap = 0; swap < 2; swap++) {
    gfx3d_t *g = gfx3d_new(TEX_SCENE_W, TEX_SCENE_H);
    int slots[4];
    tex_scene_add_textures(g, texels, slots);
    memset(fb, 0, sizeof fb);
    const gfx3d_target_t t = {fb, TEX_SCENE_W, 0, 0, TEX_SCENE_W - 1, TEX_SCENE_H - 1, swap != 0, NULL, NULL};
    uint32_t s = 0x13579BDFu;
    int persp = 0, affine = 0;
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
    CHECK(persp > 400);
    CHECK(affine > 400);
    const uint32_t crc = crc32_u16(fb, (size_t)TEX_SCENE_W * TEX_SCENE_H);
    CHECK_EQ_U32(crc, swap ? TEX_SCENE_CRC_SWAP : TEX_SCENE_CRC_HOST);
    gfx3d_free(g);
  }
}

int main(void) {
  test_euler_conventions();
  test_basis();
  test_mesh_planes_bounds_and_errors();
  test_projection_and_look_at();
  test_backface_and_double_sided();
  test_lighting_and_fog();
  test_painter_order_bias_and_stability();
  test_sort_as_one();
  test_near_clip_and_huge_ground();
  test_zero_area_culled_even_double_sided();
  test_vertex_on_near_plane_draws();
  test_frustum_cull_and_overflow();
  test_viewport_and_target_clip();
  test_grid_has_no_cracks();
  test_sky_level_rolled_and_vertical();
  test_background_is_drawn_first_and_never_far_culled();
  test_sprites();
  test_lent_sram_changes_nothing();
  test_lent_tier_changes_nothing();
  test_nonfinite_vertex_draws_nothing();
  test_flat_scene_crc();
  test_tex_add_validates();
  test_image_add_resolves();
  test_tmesh_new_validates();
  test_tmesh_new_copies();
  test_new_ex_capacity();
  test_tex_tri_coverage_matches_flat();
  test_tex_modulate();
  test_tex_perspective();
  test_tex_affine_shortcut_close();
  test_tex_wrap();
  test_tex_large_shapes();
  test_tex_levels();
  test_tex_swap_is_only_byte_order();
  test_tex_blend();
  test_tex_clip();
  test_tex_bad_values_stay_safe();
  test_tex_scene_crc();
  return check_report("test_gfx3d");
}
