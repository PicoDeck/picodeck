// See gfx3d.h. Built at -O2 with -ffp-contract=off on every target (shared
// triangle edges must evaluate bit-identically).
#include "gfx3d.h"
#include "display_clip.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef GFX3D_MALLOC
#include "umm_malloc.h"
#define GFX3D_MALLOC umm_malloc
#define GFX3D_FREE umm_free
#endif

#define REC_END 0xFFFFu
#define BG_BUCKET GFX3D_BUCKETS
enum { REC_TRI = 0, REC_SPRITE = 1 };

typedef struct {
  union {
    struct {
      float x[3], y[3];
      uint16_t color;
    } tri;
    struct {
      int16_t x, y, w, h, sx, sy, sw, sh;
      uint16_t slot;
    } spr;
  } u;
  uint16_t next;
  uint8_t kind;
} gfx3d_rec_t;

typedef struct {
  float depth;
  uint16_t rec;
} gfx3d_order_t;

// An instance's model -> view transform (v = A p + t), plus the eye and the
// light direction in model space for the face-plane tests.
typedef struct {
  float A[9], t[3], eye[3], light[3];
} gfx3d_xform_t;

struct gfx3d {
  int vx, vy, vw, vh;
  float fov_y, znear, zfar;
  float focal, cxs, cys;             // px per unit at depth 1; projection centre
  float tan_x, tan_y, inv_x, inv_y;  // frustum side planes
  float V[9], cam[3];                // view = V (p - cam)
  float light[3], ambient;
  bool fog_on;
  float fog_near, fog_far;
  uint16_t fog_color;
  int nsky;
  float sky_angle[GFX3D_MAX_SKY_BANDS];
  uint16_t sky_color[GFX3D_MAX_SKY_BANDS];
  bool clear;
  uint16_t clear_color;
  gfx3d_rec_t *rec;
  int nrec;
  uint16_t head[GFX3D_BUCKETS + 1], tail[GFX3D_BUCKETS + 1];
  float *vv;  // 3 * vcap view-space vertices (scratch)
  float *sv;  // 2 * vcap projected vertices (scratch)
  int vcap;
  gfx3d_order_t *order;  // sort_as_one scratch
  int ocap;
  gfx3d_stats_t stats;
};

// ── Small vector helpers ────────────────────────────────────────────────────

static void mat_mul(float o[9], const float a[9], const float b[9]) {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] +
                     a[r * 3 + 2] * b[6 + c];
}

static void mat_t(float o[9], const float a[9]) {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) o[c * 3 + r] = a[r * 3 + c];
}

static void mat_vec(float o[3], const float m[9], const float v[3]) {
  for (int r = 0; r < 3; r++)
    o[r] = m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
}

static float dot3(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(float o[3], const float a[3], const float b[3]) {
  o[0] = a[1] * b[2] - a[2] * b[1];
  o[1] = a[2] * b[0] - a[0] * b[2];
  o[2] = a[0] * b[1] - a[1] * b[0];
}

static bool norm3(float v[3]) {
  float l = sqrtf(dot3(v, v));
  if (!(l > 1e-12f)) return false;
  v[0] /= l;
  v[1] /= l;
  v[2] /= l;
  return true;
}

void gfx3d_rot_euler(float out[9], float yaw, float pitch, float roll) {
  const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
  const float cr = cosf(roll), sr = sinf(roll);
  const float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
  const float rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
  const float rz[9] = {cr, -sr, 0, sr, cr, 0, 0, 0, 1};
  float t[9];
  mat_mul(t, rx, rz);
  mat_mul(out, ry, t);
}

bool gfx3d_rot_basis(float out[9], const float fwd[3], const float up[3]) {
  float z[3] = {-fwd[0], -fwd[1], -fwd[2]}, x[3], y[3];
  if (!norm3(z)) return false;
  cross3(x, up, z);
  if (!norm3(x)) return false;
  cross3(y, z, x);
  out[0] = x[0]; out[1] = y[0]; out[2] = z[0];
  out[3] = x[1]; out[4] = y[1]; out[5] = z[1];
  out[6] = x[2]; out[7] = y[2]; out[8] = z[2];
  return true;
}

// ── Meshes ──────────────────────────────────────────────────────────────────

gfx3d_mesh_t *gfx3d_mesh_alloc(int nverts, int ntris) {
  if (nverts < 1 || nverts > GFX3D_MAX_VERTS || ntris < 1 ||
      ntris > GFX3D_MAX_TRIS)
    return NULL;
  const size_t head = (sizeof(gfx3d_mesh_t) + 7u) & ~(size_t)7u;
  const size_t floats = sizeof(float) * (3u * (size_t)nverts + 4u * (size_t)ntris);
  const size_t shorts = sizeof(uint16_t) * 4u * (size_t)ntris;  // idx + color
  const size_t total = head + floats + shorts + (size_t)ntris;
  uint8_t *b = (uint8_t *)GFX3D_MALLOC(total);
  if (!b) return NULL;
  memset(b, 0, total);
  gfx3d_mesh_t *m = (gfx3d_mesh_t *)(void *)b;
  m->nverts = nverts;
  m->ntris = ntris;
  m->xyz = (float *)(void *)(b + head);
  m->plane = m->xyz + 3 * nverts;
  m->idx = (uint16_t *)(void *)(m->plane + 4 * ntris);
  m->color = m->idx + 3 * ntris;
  m->flags = (uint8_t *)(void *)(m->color + ntris);
  return m;
}

void gfx3d_mesh_free(gfx3d_mesh_t *m) {
  if (m) GFX3D_FREE(m);
}

const char *gfx3d_mesh_finish(gfx3d_mesh_t *m) {
  for (int i = 0; i < 3 * m->nverts; i++)
    if (!isfinite(m->xyz[i])) return "vertex coordinates must be finite";
  for (int i = 0; i < 3 * m->ntris; i++)
    if (m->idx[i] >= m->nverts) return "triangle vertex index out of range";
  for (int t = 0; t < m->ntris; t++) {
    const float *a = &m->xyz[3 * m->idx[3 * t]];
    const float *b = &m->xyz[3 * m->idx[3 * t + 1]];
    const float *c = &m->xyz[3 * m->idx[3 * t + 2]];
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]}, n[3];
    cross3(n, ab, ac);
    float *pl = &m->plane[4 * t];
    if (norm3(n)) {
      pl[0] = n[0];
      pl[1] = n[1];
      pl[2] = n[2];
      pl[3] = dot3(n, a);
    } else {  // zero area: never faces the eye; draws nothing even double-sided
      pl[0] = pl[1] = pl[2] = 0.0f;
      pl[3] = 1.0f;
    }
    m->flags[t] &= GFX3D_FLAG_MASK;
  }
  float lo[3] = {m->xyz[0], m->xyz[1], m->xyz[2]}, hi[3] = {lo[0], lo[1], lo[2]};
  for (int i = 1; i < m->nverts; i++)
    for (int k = 0; k < 3; k++) {
      float v = m->xyz[3 * i + k];
      if (v < lo[k]) lo[k] = v;
      if (v > hi[k]) hi[k] = v;
    }
  for (int k = 0; k < 3; k++) m->center[k] = 0.5f * (lo[k] + hi[k]);
  float r2 = 0.0f;
  for (int i = 0; i < m->nverts; i++) {
    float dx = m->xyz[3 * i] - m->center[0], dy = m->xyz[3 * i + 1] - m->center[1];
    float dz = m->xyz[3 * i + 2] - m->center[2];
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > r2) r2 = d2;
  }
  m->radius = sqrtf(r2);
  return NULL;
}

// ── Context ─────────────────────────────────────────────────────────────────

static void update_projection(gfx3d_t *g) {
  const float t = tanf(0.5f * g->fov_y);
  g->focal = 0.5f * (float)g->vh / t;
  g->cxs = (float)g->vx + 0.5f * (float)g->vw;
  g->cys = (float)g->vy + 0.5f * (float)g->vh;
  g->tan_y = t;
  g->tan_x = 0.5f * (float)g->vw / g->focal;
  g->inv_x = 1.0f / sqrtf(1.0f + g->tan_x * g->tan_x);
  g->inv_y = 1.0f / sqrtf(1.0f + g->tan_y * g->tan_y);
}

gfx3d_t *gfx3d_new(int screen_w, int screen_h) {
  gfx3d_t *g = (gfx3d_t *)GFX3D_MALLOC(sizeof *g);
  if (!g) return NULL;
  memset(g, 0, sizeof *g);
  g->rec = (gfx3d_rec_t *)GFX3D_MALLOC(sizeof(gfx3d_rec_t) * GFX3D_CAPACITY);
  if (!g->rec) {
    GFX3D_FREE(g);
    return NULL;
  }
  g->vw = screen_w;
  g->vh = screen_h;
  g->fov_y = 1.0471976f;
  g->znear = 0.5f;
  g->zfar = 1000.0f;
  update_projection(g);
  const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  memcpy(g->V, id, sizeof id);
  gfx3d_set_light(g, 0.3f, 1.0f, 0.5f, 0.25f);
  gfx3d_begin(g, false, 0);
  return g;
}

void gfx3d_free(gfx3d_t *g) {
  if (!g) return;
  GFX3D_FREE(g->rec);
  if (g->vv) GFX3D_FREE(g->vv);
  if (g->sv) GFX3D_FREE(g->sv);
  if (g->order) GFX3D_FREE(g->order);
  GFX3D_FREE(g);
}

void gfx3d_set_viewport(gfx3d_t *g, int x, int y, int w, int h) {
  g->vx = x;
  g->vy = y;
  g->vw = w;
  g->vh = h;
  update_projection(g);
}

bool gfx3d_set_projection(gfx3d_t *g, float fov_y, float znear, float zfar) {
  if (!(fov_y >= 0.01f && fov_y <= 3.0f) || !(znear > 0.0f) || !(zfar > znear))
    return false;
  g->fov_y = fov_y;
  g->znear = znear;
  g->zfar = zfar;
  update_projection(g);
  return true;
}

void gfx3d_set_camera(gfx3d_t *g, const float pos[3], float yaw, float pitch,
                      float roll) {
  float r[9];
  gfx3d_rot_euler(r, yaw, pitch, roll);
  mat_t(g->V, r);
  memcpy(g->cam, pos, sizeof g->cam);
}

bool gfx3d_look_at(gfx3d_t *g, const float eye[3], const float target[3],
                   const float up[3]) {
  const float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  float r[9];
  if (!gfx3d_rot_basis(r, f, up)) return false;
  mat_t(g->V, r);
  memcpy(g->cam, eye, sizeof g->cam);
  return true;
}

bool gfx3d_set_light(gfx3d_t *g, float dx, float dy, float dz, float ambient) {
  float l[3] = {dx, dy, dz};
  if (!norm3(l)) return false;
  memcpy(g->light, l, sizeof l);
  g->ambient = ambient < 0.0f ? 0.0f : ambient > 1.0f ? 1.0f : ambient;
  return true;
}

void gfx3d_set_fog(gfx3d_t *g, bool on, float fnear, float ffar, uint16_t color) {
  g->fog_on = on && ffar > fnear;
  g->fog_near = fnear;
  g->fog_far = ffar;
  g->fog_color = color;
}

bool gfx3d_set_sky(gfx3d_t *g, const float *angles, const uint16_t *colors,
                   int n) {
  if (n < 0 || n > GFX3D_MAX_SKY_BANDS) return false;
  for (int i = 0; i < n; i++) {
    if (!(angles[i] > -1.5533430f && angles[i] < 1.5533430f)) return false;
    if (i > 0 && !(angles[i] > angles[i - 1])) return false;
  }
  for (int i = 0; i < n; i++) {
    g->sky_angle[i] = angles[i];
    g->sky_color[i] = colors[i];
  }
  g->nsky = n;
  return true;
}

bool gfx3d_project(const gfx3d_t *g, const float p[3], float *sx, float *sy,
                   float *depth) {
  const float d[3] = {p[0] - g->cam[0], p[1] - g->cam[1], p[2] - g->cam[2]};
  float v[3];
  mat_vec(v, g->V, d);
  const float z = -v[2];
  if (!(z >= g->znear)) return false;
  const float k = g->focal / z;
  *sx = g->cxs + v[0] * k;
  *sy = g->cys - v[1] * k;
  *depth = z;
  return true;
}

const gfx3d_stats_t *gfx3d_get_stats(const gfx3d_t *g) { return &g->stats; }

void gfx3d_begin(gfx3d_t *g, bool clear, uint16_t clear_color) {
  g->clear = clear;
  g->clear_color = clear_color;
  g->nrec = 0;
  memset(g->head, 0xFF, sizeof g->head);
  memset(g->tail, 0xFF, sizeof g->tail);
  memset(&g->stats, 0, sizeof g->stats);
}
