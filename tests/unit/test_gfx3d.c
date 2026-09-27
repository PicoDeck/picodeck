// Host tests for src/os/gfx3d.c: conventions, meshes, projection (Task 8),
// the scene pipeline (Task 9), sky, background and sprites (Task 10).
#include "check.h"
#include "gfx3d.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 320
#define H 320
#define HALF_PI 1.5707964f

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
  test_frustum_cull_and_overflow();
  test_viewport_and_target_clip();
  test_grid_has_no_cracks();
  test_sky_level_rolled_and_vertical();
  test_background_is_drawn_first_and_never_far_culled();
  test_sprites();
  return check_report("test_gfx3d");
}
