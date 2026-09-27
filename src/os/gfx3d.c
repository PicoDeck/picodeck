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

// ── Scene building ──────────────────────────────────────────────────────────

static int bucket_of(const gfx3d_t *g, float depth) {
  const float t = (depth - g->znear) / (g->zfar - g->znear);
  if (!(t > 0.0f)) return 0;
  if (t >= 1.0f) return GFX3D_BUCKETS - 1;
  return (int)(sqrtf(t) * (float)(GFX3D_BUCKETS - 1));
}

// Appends record i to bucket b (submission order is kept within a bucket).
static void link_rec(gfx3d_t *g, int b, int i) {
  g->rec[i].next = REC_END;
  if (g->head[b] == REC_END)
    g->head[b] = (uint16_t)i;
  else
    g->rec[g->tail[b]].next = (uint16_t)i;
  g->tail[b] = (uint16_t)i;
}

static int new_rec(gfx3d_t *g) {
  if (g->nrec >= GFX3D_CAPACITY) {
    g->stats.overflow++;
    return -1;
  }
  return g->nrec++;
}

static uint16_t rgb565f(float r, float gch, float b) {
  int ri = (int)(r + 0.5f), gi = (int)(gch + 0.5f), bi = (int)(b + 0.5f);
  ri = ri < 0 ? 0 : ri > 31 ? 31 : ri;
  gi = gi < 0 ? 0 : gi > 63 ? 63 : gi;
  bi = bi < 0 ? 0 : bi > 31 ? 31 : bi;
  return (uint16_t)((ri << 11) | (gi << 5) | bi);
}

static uint16_t shade(uint16_t c, float k) {
  return rgb565f((float)((c >> 11) & 31) * k, (float)((c >> 5) & 63) * k,
                 (float)(c & 31) * k);
}

static uint16_t fog_mix(const gfx3d_t *g, uint16_t c, float depth) {
  float t = (depth - g->fog_near) / (g->fog_far - g->fog_near);
  if (!(t > 0.0f)) return c;
  if (t > 1.0f) t = 1.0f;
  const uint16_t f = g->fog_color;
  const float r = (float)((c >> 11) & 31), gg = (float)((c >> 5) & 63);
  const float b = (float)(c & 31);
  return rgb565f(r + ((float)((f >> 11) & 31) - r) * t,
                 gg + ((float)((f >> 5) & 63) - gg) * t,
                 b + ((float)(f & 31) - b) * t);
}

// True when the sphere (view space) is entirely outside the frustum.
static bool sphere_outside(const gfx3d_t *g, const float c[3], float r,
                           bool background) {
  const float depth = -c[2];
  if (depth + r < g->znear) return true;
  if (!background && depth - r > g->zfar) return true;
  if ((c[0] - depth * g->tan_x) * g->inv_x > r) return true;
  if ((-c[0] - depth * g->tan_x) * g->inv_x > r) return true;
  if ((c[1] - depth * g->tan_y) * g->inv_y > r) return true;
  if ((-c[1] - depth * g->tan_y) * g->inv_y > r) return true;
  return false;
}

static bool ensure_scratch(gfx3d_t *g, int nverts, int norder) {
  if (nverts > g->vcap) {
    float *vv = (float *)GFX3D_MALLOC(sizeof(float) * 3u * (size_t)nverts);
    float *sv = (float *)GFX3D_MALLOC(sizeof(float) * 2u * (size_t)nverts);
    if (!vv || !sv) {
      if (vv) GFX3D_FREE(vv);
      if (sv) GFX3D_FREE(sv);
      return false;
    }
    if (g->vv) GFX3D_FREE(g->vv);
    if (g->sv) GFX3D_FREE(g->sv);
    g->vv = vv;
    g->sv = sv;
    g->vcap = nverts;
  }
  if (norder > g->ocap) {
    gfx3d_order_t *o =
        (gfx3d_order_t *)GFX3D_MALLOC(sizeof(gfx3d_order_t) * (size_t)norder);
    if (!o) return false;
    if (g->order) GFX3D_FREE(g->order);
    g->order = o;
    g->ocap = norder;
  }
  return true;
}

// Clips the view-space triangle (a, b, c) against the near plane and
// projects the result (3 or 4 vertices) into ox/oy. Each crossing is
// interpolated from its inside end, so two triangles sharing the edge get
// the same point.
static int clip_near(const gfx3d_t *g, const float *a, const float *b,
                     const float *c, float *ox, float *oy) {
  const float *in[3] = {a, b, c};
  float p[4][3];
  int n = 0;
  for (int i = 0; i < 3; i++) {
    const float *u = in[i], *w = in[(i + 1) % 3];
    const float du = -u[2] - g->znear, dw = -w[2] - g->znear;  // >= 0: inside
    if (du >= 0.0f) {
      memcpy(p[n], u, sizeof p[0]);
      n++;
    }
    if ((du >= 0.0f) != (dw >= 0.0f)) {
      const float *s = du >= 0.0f ? u : w, *o = du >= 0.0f ? w : u;
      const float ds = du >= 0.0f ? du : dw, dd = du >= 0.0f ? dw : du;
      const float t = ds / (ds - dd);
      for (int k = 0; k < 3; k++) p[n][k] = s[k] + (o[k] - s[k]) * t;
      n++;
    }
  }
  for (int i = 0; i < n; i++) {
    const float k = g->focal / fmaxf(-p[i][2], g->znear);
    ox[i] = g->cxs + p[i][0] * k;
    oy[i] = g->cys - p[i][1] * k;
  }
  return n;
}

static bool offscreen(const gfx3d_t *g, const float *x, const float *y, int n) {
  const float x0 = (float)g->vx, x1 = (float)(g->vx + g->vw);
  const float y0 = (float)g->vy, y1 = (float)(g->vy + g->vh);
  bool left = true, right = true, top = true, bottom = true;
  for (int i = 0; i < n; i++) {
    if (x[i] >= x0) left = false;
    if (x[i] <= x1) right = false;
    if (y[i] >= y0) top = false;
    if (y[i] <= y1) bottom = false;
  }
  return left || right || top || bottom;
}

static int order_cmp(const void *pa, const void *pb) {
  const gfx3d_order_t *a = (const gfx3d_order_t *)pa, *b = (const gfx3d_order_t *)pb;
  if (a->depth > b->depth) return -1;  // far first
  if (a->depth < b->depth) return 1;
  return (int)a->rec - (int)b->rec;    // then submission order
}

static void process_mesh(gfx3d_t *g, const gfx3d_mesh_t *m,
                         const gfx3d_xform_t *xf, bool bg, float bias, bool one,
                         float inst_depth) {
  if (!ensure_scratch(g, m->nverts, one ? 2 * m->ntris : 0)) {
    g->stats.overflow += (uint32_t)m->ntris;
    return;
  }
  const float zn = g->znear, zf = g->zfar;
  for (int i = 0; i < m->nverts; i++) {
    float *v = &g->vv[3 * i];
    mat_vec(v, xf->A, &m->xyz[3 * i]);
    v[0] += xf->t[0];
    v[1] += xf->t[1];
    v[2] += xf->t[2];
    const float depth = -v[2];
    if (depth >= zn) {
      const float k = g->focal / depth;
      g->sv[2 * i] = g->cxs + v[0] * k;
      g->sv[2 * i + 1] = g->cys - v[1] * k;
    }
  }
  int n_one = 0;
  for (int t = 0; t < m->ntris; t++) {
    const float *pl = &m->plane[4 * t];
    const unsigned fl = m->flags[t];
    const bool front = dot3(pl, xf->eye) - pl[3] > 0.0f;
    if (!front && !(fl & GFX3D_DOUBLE_SIDED)) {
      g->stats.culled++;
      continue;
    }
    const int ia = m->idx[3 * t], ib = m->idx[3 * t + 1], ic = m->idx[3 * t + 2];
    const float da = -g->vv[3 * ia + 2], db = -g->vv[3 * ib + 2];
    const float dc = -g->vv[3 * ic + 2];
    if (da < zn && db < zn && dc < zn) {
      g->stats.culled++;
      continue;
    }
    if (!bg && da > zf && db > zf && dc > zf) {
      g->stats.culled++;
      continue;
    }
    uint16_t col = m->color[t];
    if (!(fl & GFX3D_UNLIT)) {
      float nl = dot3(pl, xf->light);
      if (!front) nl = -nl;
      col = shade(col, g->ambient + (1.0f - g->ambient) * (nl > 0.0f ? nl : 0.0f));
    }
    const float depth = (fmaxf(da, zn) + fmaxf(db, zn) + fmaxf(dc, zn)) * (1.0f / 3.0f);
    if (!bg && g->fog_on && !(fl & GFX3D_NO_FOG)) col = fog_mix(g, col, depth);
    float tx[4], ty[4];
    int nv;
    if (da >= zn && db >= zn && dc >= zn) {
      tx[0] = g->sv[2 * ia];
      ty[0] = g->sv[2 * ia + 1];
      tx[1] = g->sv[2 * ib];
      ty[1] = g->sv[2 * ib + 1];
      tx[2] = g->sv[2 * ic];
      ty[2] = g->sv[2 * ic + 1];
      nv = 3;
    } else {
      nv = clip_near(g, &g->vv[3 * ia], &g->vv[3 * ib], &g->vv[3 * ic], tx, ty);
      g->stats.clipped++;
    }
    if (offscreen(g, tx, ty, nv)) {
      g->stats.culled++;
      continue;
    }
    const int bucket = bg ? BG_BUCKET : bucket_of(g, depth + bias);
    for (int k = 1; k + 1 < nv; k++) {
      const int ri = new_rec(g);
      if (ri < 0) break;
      gfx3d_rec_t *r = &g->rec[ri];
      r->kind = REC_TRI;
      r->u.tri.x[0] = tx[0];
      r->u.tri.y[0] = ty[0];
      r->u.tri.x[1] = tx[k];
      r->u.tri.y[1] = ty[k];
      r->u.tri.x[2] = tx[k + 1];
      r->u.tri.y[2] = ty[k + 1];
      r->u.tri.color = col;
      if (one) {
        g->order[n_one].depth = depth;
        g->order[n_one].rec = (uint16_t)ri;
        n_one++;
      } else {
        link_rec(g, bucket, ri);
      }
    }
  }
  if (one && n_one > 0) {
    qsort(g->order, (size_t)n_one, sizeof *g->order, order_cmp);
    const int b = bucket_of(g, inst_depth + bias);
    for (int i = 0; i < n_one; i++) link_rec(g, b, g->order[i].rec);
  }
}

void gfx3d_draw(gfx3d_t *g, const gfx3d_mesh_t *m, const float rot[9],
                const float pos[3], float scale, float bias, bool sort_as_one) {
  g->stats.tris_in += (uint32_t)m->ntris;
  gfx3d_xform_t xf;
  float vr[9], rt[9];
  const float d[3] = {pos[0] - g->cam[0], pos[1] - g->cam[1], pos[2] - g->cam[2]};
  mat_mul(vr, g->V, rot);
  for (int i = 0; i < 9; i++) xf.A[i] = vr[i] * scale;
  mat_vec(xf.t, g->V, d);
  mat_t(rt, rot);
  const float back[3] = {-d[0] / scale, -d[1] / scale, -d[2] / scale};
  mat_vec(xf.eye, rt, back);
  mat_vec(xf.light, rt, g->light);
  float c[3];
  mat_vec(c, xf.A, m->center);
  c[0] += xf.t[0];
  c[1] += xf.t[1];
  c[2] += xf.t[2];
  if (sphere_outside(g, c, m->radius * scale, false)) {
    g->stats.culled += (uint32_t)m->ntris;
    return;
  }
  process_mesh(g, m, &xf, false, bias, sort_as_one, -xf.t[2]);
}

void gfx3d_draw_background(gfx3d_t *g, const gfx3d_mesh_t *m) {
  g->stats.tris_in += (uint32_t)m->ntris;
  gfx3d_xform_t xf;
  memcpy(xf.A, g->V, sizeof xf.A);
  xf.t[0] = xf.t[1] = xf.t[2] = 0.0f;
  xf.eye[0] = xf.eye[1] = xf.eye[2] = 0.0f;  // the camera sits at the origin
  memcpy(xf.light, g->light, sizeof xf.light);
  float c[3];
  mat_vec(c, xf.A, m->center);
  if (sphere_outside(g, c, m->radius, true)) {
    g->stats.culled += (uint32_t)m->ntris;
    return;
  }
  process_mesh(g, m, &xf, true, 0.0f, false, 0.0f);
}

bool gfx3d_draw_sprite(gfx3d_t *g, int slot, const float pos[3], float size,
                       int sx, int sy, int sw, int sh, float bias) {
  if (slot < 0 || slot > 0xFFFE || sw <= 0 || sh <= 0 || sx < 0 || sy < 0 ||
      sx > 0x7FFF || sy > 0x7FFF || sw > 0x7FFF || sh > 0x7FFF || !(size > 0.0f))
    return false;
  const float d[3] = {pos[0] - g->cam[0], pos[1] - g->cam[1], pos[2] - g->cam[2]};
  float v[3];
  mat_vec(v, g->V, d);
  const float depth = -v[2];
  const float k = g->focal / depth;
  const float h = size * k, w = h * (float)sw / (float)sh;
  if (!(depth >= g->znear) || depth > g->zfar || h > 4096.0f || w > 4096.0f) {
    g->stats.culled++;
    return false;
  }
  const int iw = (int)(w + 0.5f), ih = (int)(h + 0.5f);
  const int ix = (int)floorf(g->cxs + v[0] * k - 0.5f * w + 0.5f);
  const int iy = (int)floorf(g->cys - v[1] * k - 0.5f * h + 0.5f);
  if (iw < 1 || ih < 1 || ix >= g->vx + g->vw || iy >= g->vy + g->vh ||
      ix + iw <= g->vx || iy + ih <= g->vy) {
    g->stats.culled++;
    return false;
  }
  const int ri = new_rec(g);
  if (ri < 0) return false;
  gfx3d_rec_t *r = &g->rec[ri];
  r->kind = REC_SPRITE;
  r->u.spr.x = (int16_t)ix;
  r->u.spr.y = (int16_t)iy;
  r->u.spr.w = (int16_t)iw;
  r->u.spr.h = (int16_t)ih;
  r->u.spr.sx = (int16_t)sx;
  r->u.spr.sy = (int16_t)sy;
  r->u.spr.sw = (int16_t)sw;
  r->u.spr.sh = (int16_t)sh;
  r->u.spr.slot = (uint16_t)slot;
  link_rec(g, bucket_of(g, depth + bias), ri);
  return true;
}

// ── Rasterisation ───────────────────────────────────────────────────────────

static void fill_rect(const gfx3d_target_t *t, const disp_clip_t *c, uint16_t v) {
  for (int y = c->y0; y <= c->y1; y++)
    disp_span16(t->fb + (size_t)y * t->stride, c->x0, c->x1, v);
}

// Smallest integer >= v (strict: > v), clamped to [lo, hi].
static int first_px(float v, bool strict, int lo, int hi) {
  if (!(v > (float)lo - 1.0f)) return lo;
  if (v >= (float)hi) return hi;
  int p = (int)ceilf(v);
  if (strict && (float)p == v) p++;
  return p < lo ? lo : p > hi ? hi : p;
}

// Colour bands parallel to the horizon. h(X, Y) = A*X + B*Y + C is the signed
// pixel distance of the pixel centre (X, Y) from the horizon line (positive
// towards the sky); band i begins where h >= focal * tan(angle[i]). Exact at
// the horizon, an approximation for other angles away from the centre.
static void fill_sky(const gfx3d_t *g, const gfx3d_target_t *t,
                     const disp_clip_t *c) {
  const int n = g->nsky;
  uint16_t col[GFX3D_MAX_SKY_BANDS];
  float th[GFX3D_MAX_SKY_BANDS];
  for (int i = 0; i < n; i++) {
    col[i] = disp_px(g->sky_color[i], t->swap);
    th[i] = g->focal * tanf(g->sky_angle[i]);
  }
  const float ux = g->V[1], uy = g->V[4], uz = g->V[7];  // world up, view space
  const float nrm = sqrtf(ux * ux + uy * uy);
  if (nrm < 1e-6f) {  // looking straight up or down: one band everywhere
    fill_rect(t, c, col[uz < 0.0f ? n - 1 : 0]);
    return;
  }
  const float A = ux / nrm, B = -uy / nrm;
  const float C = (-ux * g->cxs + uy * g->cys - uz * g->focal) / nrm;
  for (int y = c->y0; y <= c->y1; y++) {
    uint16_t *row = t->fb + (size_t)y * t->stride;
    const float h0 = B * ((float)y + 0.5f) + C;  // h = A*(px + 0.5) + h0
    int x = c->x0, b = 0;
    const float hx = A * ((float)x + 0.5f) + h0;
    while (b + 1 < n && hx >= th[b + 1]) b++;
    while (x <= c->x1) {
      int end = c->x1 + 1, nb = b;
      if (A > 0.0f && b + 1 < n) {  // h rises: band b+1 from h >= th[b+1]
        end = first_px((th[b + 1] - h0) / A - 0.5f, false, x, c->x1 + 1);
        nb = b + 1;
      } else if (A < 0.0f && b > 0) {  // h falls: band b-1 once h < th[b]
        end = first_px((th[b] - h0) / A - 0.5f, true, x, c->x1 + 1);
        nb = b - 1;
      }
      if (end > x) {
        disp_span16(row, x, end - 1, col[b]);
        x = end;
      }
      if (nb == b) break;
      b = nb;
    }
  }
}

static void raster_bucket(gfx3d_t *g, const gfx3d_target_t *t,
                          const disp_clip_t *c, int b) {
  for (uint16_t i = g->head[b]; i != REC_END; i = g->rec[i].next) {
    const gfx3d_rec_t *r = &g->rec[i];
    if (r->kind == REC_TRI) {
      disp_fill_tri_f(t->fb, t->stride, c, r->u.tri.x[0], r->u.tri.y[0],
                      r->u.tri.x[1], r->u.tri.y[1], r->u.tri.x[2],
                      r->u.tri.y[2], disp_px(r->u.tri.color, t->swap));
      g->stats.drawn++;
      continue;
    }
    const uint16_t *data;
    int iw, ih;
    uint16_t key;
    if (!t->sprite || !t->sprite(t->ud, r->u.spr.slot, &data, &iw, &ih, &key))
      continue;
    if (r->u.spr.sx + r->u.spr.sw > iw || r->u.spr.sy + r->u.spr.sh > ih)
      continue;  // the image is smaller than when it was queued
    disp_blit_scaled_rect(t->fb, t->stride, c, r->u.spr.x, r->u.spr.y, data, iw,
                          r->u.spr.sx, r->u.spr.sy, r->u.spr.sw, r->u.spr.sh,
                          r->u.spr.w, r->u.spr.h, key, t->swap);
    g->stats.sprites++;
  }
}

void gfx3d_end(gfx3d_t *g, const gfx3d_target_t *t) {
  disp_clip_t c = {t->clip_x0 > g->vx ? t->clip_x0 : g->vx,
                   t->clip_y0 > g->vy ? t->clip_y0 : g->vy,
                   t->clip_x1 < g->vx + g->vw - 1 ? t->clip_x1 : g->vx + g->vw - 1,
                   t->clip_y1 < g->vy + g->vh - 1 ? t->clip_y1 : g->vy + g->vh - 1};
  if (c.x1 < c.x0 || c.y1 < c.y0) return;
  if (g->nsky > 0)
    fill_sky(g, t, &c);
  else if (g->clear)
    fill_rect(t, &c, disp_px(g->clear_color, t->swap));
  raster_bucket(g, t, &c, BG_BUCKET);
  for (int b = GFX3D_BUCKETS - 1; b >= 0; b--) raster_bucket(g, t, &c, b);
}
