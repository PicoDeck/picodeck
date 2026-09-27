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

int main(void) {
  test_euler_conventions();
  test_basis();
  test_mesh_planes_bounds_and_errors();
  test_projection_and_look_at();
  return check_report("test_gfx3d");
}
