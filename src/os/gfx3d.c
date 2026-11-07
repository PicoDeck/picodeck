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

// The firmware's triangle rasteriser runs from SRAM: its loops then fetch nothing
// through the XIP cache both cores share. -Os for its bytes, which keeps
// -ffp-contract=off (a VMLA rounds its product, so only VFMA would change a bit).
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define GFX3D_RAM_FUNC __attribute__((section(".time_critical.gfx3d_raster"), noinline, optimize("Os")))
#else
#define GFX3D_RAM_FUNC
#endif

// Textured code has a section of its own, which src/hot_gfx.ld and the perf rig's link
// script put after everything else: a scene with no textured triangle runs from the
// cache sets it always did, however much this code grows.
#ifdef __ELF__
#define GFX3D_TEX_FUNC __attribute__((section(".gfx3d_tex_text")))
#else
#define GFX3D_TEX_FUNC
#endif

// A record's next index, with REC_OTHER set on a record that is no flat triangle, so the
// rasteriser's one test per record is all flat triangles ever pay for the other kinds.
#define REC_END 0x7FFFu
#define REC_OTHER 0x8000u
// Such a record's `color`: 0 for a sprite; for a textured triangle, these.
#define REC_TEX 0x8000u
#define REC_TEX_BLEND 0x4000u
#define REC_TEX_SLOT 0x3Fu
_Static_assert(GFX3D_MAX_TEXTURES <= REC_TEX_SLOT + 1, "a texture's slot fits a record");
_Static_assert(GFX3D_REC_LIMIT <= REC_END && GFX3D_CAPACITY <= GFX3D_REC_LIMIT,
               "record indices are 15 bits with REC_END as the terminator");
#define BG_BUCKET GFX3D_BUCKETS  // the highest, so the first rasterised
#define USED_WORDS ((GFX3D_BUCKETS + 1 + 31) / 32)

// 28 bytes: in PSRAM every byte of a record is a write and a read of that line.
typedef struct {
  union {
    struct {
      float x[3], y[3];
    } tri;
    struct {
      int16_t x, y, w, h, sx, sy, sw, sh;
      uint16_t slot;
      uint8_t fog;  // 0..31: how far towards the fog colour; 0 when sprite fog is off
    } spr;
  } u;
  uint16_t color;  // triangles
  uint16_t next;
} gfx3d_rec_t;
_Static_assert(sizeof(gfx3d_rec_t) == 28, "see above");

// A textured triangle's record holds its screen positions, in the mesh's vertex order, and
// its texture in `color`. This is the rest of it, at xrec[i] for record i: there is no index
// to keep or run out of, and the lent SRAM's first records work as they do for flat triangles.
// It is in PSRAM, where a byte stored costs a line filled, written back and read again. So
// the uv stays in the mesh, by reference, as those lines are only read; the colours are here,
// with the fog in them, so the rasteriser needs neither the mesh's colours nor the fog.
typedef struct {
  float iw[3];                // focal / depth: tex_tri wants only the ratios of 1/z
  const gfx3d_tmesh_t *mesh;  // its uv by triangle: idx, then uv. NULL: fan[tri]
  uint16_t tri;
  uint16_t color[3];  // host RGB565, fog mixed in
} gfx3d_xrec_t;
_Static_assert(sizeof(void *) != 4 || sizeof(gfx3d_xrec_t) == 24, "three of the XIP cache's lines");

typedef struct {
  float depth;
  uint16_t rec;
} gfx3d_order_t;

// A transformed vertex: view space, then screen space when in front of the
// near plane.
typedef struct {
  float x, y, z, sx, sy;
} gfx3d_vert_t;

// The same for a textured mesh, in the same scratch.
typedef struct {
  float x, y, z, sx, sy;
  float iw;        // focal / depth, as sx and sy when in front of the near plane
  uint16_t color;  // fog mixed in
} gfx3d_tvert_t;

// head and tail share a line, and mean something only if the bucket's bit in
// `used` is set, so a frame's beginScene clears 132 bytes, not 4 KB of PSRAM.
typedef struct {
  uint16_t head, tail;
} gfx3d_bucket_t;

// What a scene's records touch for every triangle, in lent SRAM when there is
// some (gfx3d_sram_tier): the bitmap, the buckets and as many records as fit,
// the first ones. A record's index means the same in either home, so taking the
// block back is a copy (gfx3d_sram_take_back).
typedef struct {
  uint32_t used[USED_WORDS];
  gfx3d_bucket_t bucket[GFX3D_BUCKETS + 1];
  gfx3d_rec_t rec[];
} gfx3d_tier_t;

typedef struct {
  uint16_t *px;  // all mip levels, packed
  uint8_t lg_w, lg_h, nmips;
} gfx3d_tex_t;

typedef struct {
  uint16_t *px;
  uint16_t w, h, key;
} gfx3d_image_t;

struct gfx3d {
  int vx, vy, vw, vh;
  float fov_y, znear, zfar;
  float focal, cxs, cys;             // px per unit at depth 1; projection centre
  float tan_x, tan_y, inv_x, inv_y;  // frustum side planes
  float V[9], cam[3];                // view = V (p - cam)
  float light[3], ambient;
  bool fog_on;
  bool sprite_fog;  // gfx3d_set_sprite_fog: sprites take the scene's fog
  float fog_near, fog_far;
  uint16_t fog_color;
  int nsky;
  float sky_angle[GFX3D_MAX_SKY_BANDS];
  uint16_t sky_color[GFX3D_MAX_SKY_BANDS];
  bool clear;
  uint16_t clear_color;
  int cap;           // records per scene
  gfx3d_tex_t tex[GFX3D_MAX_TEXTURES];
  gfx3d_image_t img[GFX3D_MAX_IMAGES];
  int ntex, nimg;
  gfx3d_rec_t *rec;  // cap records: all but the first ntrec are here
  gfx3d_rec_t *trec;  // the lent block's records, or NULL
  int ntrec;
  int nrec;
  uint32_t *used;  // bit b of word w: bucket 32 * w + b has records
  gfx3d_bucket_t *bucket;
  uint32_t own_used[USED_WORDS];  // their home when no block is lent
  gfx3d_bucket_t own_bucket[GFX3D_BUCKETS + 1];
  gfx3d_vert_t *verts;  // vcap vertices, when the lent SRAM is not available
  int vcap;
  gfx3d_order_t *order;  // sort_as_one scratch
  int ocap;
  gfx3d_stats_t stats;
  // Made at the first textured draw: a scene with none has nothing here.
  void *xblock;
  gfx3d_xrec_t *xrec;  // cap of them, starting on a cache line
  float (*fan)[6];     // GFX3D_TM_FANS triangles' uv, for those the near plane cut
  int nfan;
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

// ── Textured meshes ─────────────────────────────────────────────────────────

struct gfx3d_tmesh {
  int nverts, ntris;
  float center[3], radius;
  float *xyz, *uv;
  uint16_t *color, *idx;
  uint8_t *tex, *flags;
};

gfx3d_tmesh_t *gfx3d_tmesh_new(const gfx3d_tmesh_desc_t *d) {
  if (!d || d->nverts < 1 || d->nverts > GFX3D_MAX_VERTS || d->ntris < 1 ||
      d->ntris > GFX3D_MAX_TRIS || !d->xyz || !d->uv || !d->color || !d->idx ||
      !d->tex)
    return NULL;
  const int nv = d->nverts, nt = d->ntris;
  for (int i = 0; i < 3 * nv; i++)
    if (!isfinite(d->xyz[i])) return NULL;
  for (int i = 0; i < 2 * nv; i++)
    if (!isfinite(d->uv[i])) return NULL;
  for (int i = 0; i < 3 * nt; i++)
    if (d->idx[i] >= nv) return NULL;
  const size_t head = (sizeof(gfx3d_tmesh_t) + 7u) & ~(size_t)7u;
  const size_t total = head + sizeof(float) * 5u * (size_t)nv + 7u +
                       sizeof(uint16_t) * ((size_t)nv + 3u * (size_t)nt) +
                       2u * (size_t)nt;
  uint8_t *b = (uint8_t *)GFX3D_MALLOC(total);
  if (!b) return NULL;
  gfx3d_tmesh_t *m = (gfx3d_tmesh_t *)(void *)b;
  m->nverts = nv;
  m->ntris = nt;
  m->xyz = (float *)(void *)(b + head);
  // Each vertex's uv in one of the XIP cache's lines: the rasteriser reads three a triangle.
  m->uv = (float *)(((uintptr_t)(m->xyz + 3 * nv) + 7u) & ~(uintptr_t)7u);
  m->color = (uint16_t *)(void *)(m->uv + 2 * nv);
  m->idx = m->color + nv;
  m->tex = (uint8_t *)(void *)(m->idx + 3 * nt);
  m->flags = m->tex + nt;
  memcpy(m->xyz, d->xyz, sizeof(float) * 3u * (size_t)nv);
  memcpy(m->uv, d->uv, sizeof(float) * 2u * (size_t)nv);
  memcpy(m->color, d->color, sizeof(uint16_t) * (size_t)nv);
  memcpy(m->idx, d->idx, sizeof(uint16_t) * 3u * (size_t)nt);
  memcpy(m->tex, d->tex, (size_t)nt);
  if (d->flags) {
    for (int t = 0; t < nt; t++) m->flags[t] = d->flags[t] & 7u;
  } else {
    memset(m->flags, 0, (size_t)nt);
  }
  float lo[3] = {m->xyz[0], m->xyz[1], m->xyz[2]}, hi[3] = {lo[0], lo[1], lo[2]};
  for (int i = 1; i < nv; i++)
    for (int k = 0; k < 3; k++) {
      const float v = m->xyz[3 * i + k];
      if (v < lo[k]) lo[k] = v;
      if (v > hi[k]) hi[k] = v;
    }
  for (int k = 0; k < 3; k++) m->center[k] = 0.5f * (lo[k] + hi[k]);
  float r2 = 0.0f;
  for (int i = 0; i < nv; i++) {
    const float dx = m->xyz[3 * i] - m->center[0], dy = m->xyz[3 * i + 1] - m->center[1];
    const float dz = m->xyz[3 * i + 2] - m->center[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > r2) r2 = d2;
  }
  m->radius = sqrtf(r2);
  return m;
}

void gfx3d_tmesh_free(gfx3d_tmesh_t *m) {
  if (m) GFX3D_FREE(m);
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

// The context holding the lent block. One at a time: an app has one context.
static gfx3d_t *s_tier_user;

__attribute__((weak)) void *gfx3d_sram_tier(size_t *bytes) {
  (void)bytes;
  return NULL;
}

void gfx3d_sram_take_back(void) {
  gfx3d_t *const g = s_tier_user;
  if (!g) return;
  s_tier_user = NULL;
  const int n = g->nrec < g->ntrec ? g->nrec : g->ntrec;
  if (n > 0) memcpy(g->rec, g->trec, (size_t)n * sizeof *g->rec);
  memcpy(g->own_used, g->used, sizeof g->own_used);
  memcpy(g->own_bucket, g->bucket, sizeof g->own_bucket);
  g->used = g->own_used;
  g->bucket = g->own_bucket;
  g->trec = NULL;
  g->ntrec = 0;
}

// At every scene's start, so a block given back since the last one is used again.
static void tier_acquire(gfx3d_t *g) {
  if (s_tier_user == g) return;  // a lender takes it back before it needs it
  gfx3d_sram_take_back();        // another context's
  size_t have = 0;
  gfx3d_tier_t *const t = (gfx3d_tier_t *)gfx3d_sram_tier(&have);
  if (!t || have < sizeof *t + sizeof t->rec[0]) return;
  const size_t n = (have - sizeof *t) / sizeof t->rec[0];
  g->ntrec = n < (size_t)g->cap ? (int)n : g->cap;
  g->trec = t->rec;
  g->used = t->used;
  g->bucket = t->bucket;
  s_tier_user = g;
}

gfx3d_t *gfx3d_new(int screen_w, int screen_h) {
  return gfx3d_new_ex(screen_w, screen_h, GFX3D_CAPACITY);
}

gfx3d_t *gfx3d_new_ex(int screen_w, int screen_h, int capacity) {
  if (capacity < 1 || capacity > GFX3D_REC_LIMIT) return NULL;
  gfx3d_t *g = (gfx3d_t *)GFX3D_MALLOC(sizeof *g);
  if (!g) return NULL;
  memset(g, 0, sizeof *g);
  g->used = g->own_used;
  g->bucket = g->own_bucket;
  g->cap = capacity;
  g->rec = (gfx3d_rec_t *)GFX3D_MALLOC(sizeof(gfx3d_rec_t) * (size_t)capacity);
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
  if (s_tier_user == g) s_tier_user = NULL;
  GFX3D_FREE(g->rec);
  for (int i = 0; i < g->ntex; i++) GFX3D_FREE(g->tex[i].px);
  for (int i = 0; i < g->nimg; i++) GFX3D_FREE(g->img[i].px);
  if (g->verts) GFX3D_FREE(g->verts);
  if (g->order) GFX3D_FREE(g->order);
  if (g->xblock) GFX3D_FREE(g->xblock);
  GFX3D_FREE(g);
}

int gfx3d_tex_add(gfx3d_t *g, int lg_w, int lg_h, int nmips, const uint16_t *pixels) {
  if (!g || !pixels || g->ntex >= GFX3D_MAX_TEXTURES || lg_w < 0 || lg_w > 10 ||
      lg_h < 0 || lg_h > 10 || nmips < 1)
    return -1;
  if (nmips - 1 > (lg_w < lg_h ? lg_w : lg_h)) return -1;
  size_t n = 0;
  for (int k = 0; k < nmips; k++) n += (size_t)1u << (lg_w - k + lg_h - k);
  uint16_t *px = (uint16_t *)GFX3D_MALLOC(n * sizeof *px);
  if (!px) return -1;
  memcpy(px, pixels, n * sizeof *px);
  gfx3d_tex_t *t = &g->tex[g->ntex];
  t->px = px;
  t->lg_w = (uint8_t)lg_w;
  t->lg_h = (uint8_t)lg_h;
  t->nmips = (uint8_t)nmips;
  return g->ntex++;
}

int gfx3d_image_add(gfx3d_t *g, int w, int h, uint16_t key, const uint16_t *pixels) {
  if (!g || !pixels || g->nimg >= GFX3D_MAX_IMAGES || w < 1 || w > 1024 || h < 1 ||
      h > 1024)
    return -1;
  const size_t n = (size_t)w * (size_t)h;
  uint16_t *px = (uint16_t *)GFX3D_MALLOC(n * sizeof *px);
  if (!px) return -1;
  memcpy(px, pixels, n * sizeof *px);
  gfx3d_image_t *im = &g->img[g->nimg];
  im->px = px;
  im->w = (uint16_t)w;
  im->h = (uint16_t)h;
  im->key = key;
  return g->nimg++;
}

bool gfx3d_image_resolve(void *ctx, int slot, const uint16_t **data, int *w, int *h,
                         uint16_t *key) {
  const gfx3d_t *g = (const gfx3d_t *)ctx;
  if (!g || slot < 0 || slot >= g->nimg) return false;
  const gfx3d_image_t *im = &g->img[slot];
  *data = im->px;
  *w = im->w;
  *h = im->h;
  *key = im->key;
  return true;
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

void gfx3d_set_sprite_fog(gfx3d_t *g, bool on) { g->sprite_fog = on; }

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
  g->nfan = 0;
  tier_acquire(g);
  memset(g->used, 0, sizeof g->own_used);
  memset(&g->stats, 0, sizeof g->stats);
}

// ── Scene building ──────────────────────────────────────────────────────────

__attribute__((always_inline)) static inline int bucket_of(float znear, float range, float depth) {
  const float t = (depth - znear) / range;
  if (!(t > 0.0f)) return 0;
  if (t >= 1.0f) return GFX3D_BUCKETS - 1;
  return (int)(sqrtf(t) * (float)(GFX3D_BUCKETS - 1));
}

static inline gfx3d_rec_t *rec_at(const gfx3d_t *g, int i) {
  return i < g->ntrec ? &g->trec[i] : &g->rec[i];
}

// Appends record i to bucket b (submission order is kept within a bucket).
// `kind` is 0 for a flat triangle, else REC_OTHER.
static void link_rec(gfx3d_t *g, int b, int i, uint16_t kind) {
  rec_at(g, i)->next = kind | REC_END;
  gfx3d_bucket_t *const bk = &g->bucket[b];
  uint32_t *const w = &g->used[b >> 5];
  const uint32_t bit = 1u << (b & 31);
  if (!(*w & bit)) {
    *w |= bit;
    bk->head = (uint16_t)i;
  } else {
    gfx3d_rec_t *const last = rec_at(g, bk->tail);
    last->next = (uint16_t)((last->next & REC_OTHER) | i);
  }
  bk->tail = (uint16_t)i;
}

static int new_rec(gfx3d_t *g) {
  if (g->nrec >= g->cap) {
    g->stats.overflow++;
    return -1;
  }
  return g->nrec++;
}

__attribute__((always_inline)) static inline uint16_t rgb565f(float r, float gch, float b) {
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

// (fr, fg, fb): the fog colour's channels as floats; range = fog_far - fog_near.
__attribute__((always_inline)) static inline uint16_t fog_mix(float fog_near, float range, float fr,
                                                              float fg, float fb, uint16_t c,
                                                              float depth) {
  float t = (depth - fog_near) / range;
  if (!(t > 0.0f)) return c;
  if (t > 1.0f) t = 1.0f;
  const float r = (float)((c >> 11) & 31), gg = (float)((c >> 5) & 63);
  const float b = (float)(c & 31);
  return rgb565f(r + (fr - r) * t, gg + (fg - gg) * t, b + (fb - b) * t);
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

__attribute__((weak)) void *gfx3d_sram_scratch(size_t *bytes) {
  (void)bytes;
  return NULL;
}

// Room for one mesh's vertices and sort entries, until process_mesh returns: in
// the lent SRAM when they fit (no XIP cache line touched, and PSRAM's stays
// unallocated), else blocks kept for the next mesh.
__attribute__((always_inline)) static inline bool get_scratch(gfx3d_t *g, int nverts, int norder,
                                                              gfx3d_vert_t **verts,
                                                              gfx3d_order_t **order) {
  const size_t vbytes = sizeof(gfx3d_vert_t) * (size_t)nverts;
  size_t have = 0;
  uint8_t *const sram = (uint8_t *)gfx3d_sram_scratch(&have);
  if (sram && vbytes + sizeof(gfx3d_order_t) * (size_t)norder <= have) {
    *verts = (gfx3d_vert_t *)(void *)sram;
    *order = (gfx3d_order_t *)(void *)(sram + vbytes);
    return true;
  }
  if (nverts > g->vcap) {
    gfx3d_vert_t *v = (gfx3d_vert_t *)GFX3D_MALLOC(vbytes);
    if (!v) return false;
    if (g->verts) GFX3D_FREE(g->verts);
    g->verts = v;
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
  *verts = g->verts;
  *order = g->order;
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

__attribute__((always_inline)) static inline bool offscreen(float x0, float x1, float y0, float y1,
                                                            const float *x, const float *y, int n) {
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

// Files one triangle; false when the scene is full. Its vertices are sorted here,
// once, for the rasteriser; a non-finite one gets zero height, so draws nothing.
static inline bool add_tri(gfx3d_t *g, gfx3d_order_t *order, int *n_one, bool one,
                           int bucket, uint16_t col, float depth, float x0,
                           float y0, float x1, float y1, float x2, float y2) {
  const int ri = new_rec(g);
  if (ri < 0) return false;
  if (!disp_tri_sort(&x0, &y0, &x1, &y1, &x2, &y2)) y0 = y1 = y2 = 0.0f;
  gfx3d_rec_t *const r = rec_at(g, ri);
  r->u.tri.x[0] = x0;
  r->u.tri.y[0] = y0;
  r->u.tri.x[1] = x1;
  r->u.tri.y[1] = y1;
  r->u.tri.x[2] = x2;
  r->u.tri.y[2] = y2;
  r->color = col;
  if (one) {
    order[*n_one].depth = depth;
    order[*n_one].rec = (uint16_t)ri;
    (*n_one)++;
  } else {
    link_rec(g, bucket, ri, 0);
  }
  return true;
}

// Everything the loops read from g, m and xf is a local first: a store through
// a float * could change any float in them, so the compiler would read them
// again from PSRAM on every iteration. Lighting and fog wait for the off-screen
// test, whose triangles keep no colour.
static void process_mesh(gfx3d_t *g, const gfx3d_mesh_t *m,
                         const gfx3d_xform_t *xf, bool bg, float bias, bool one,
                         float inst_depth) {
  const int nverts = m->nverts, ntris = m->ntris;
  gfx3d_vert_t *vs;
  gfx3d_order_t *order;
  if (!get_scratch(g, nverts, one ? 2 * ntris : 0, &vs, &order)) {
    g->stats.overflow += (uint32_t)ntris;
    return;
  }
  const float zn = g->znear, zf = g->zfar, zrange = zf - zn;
  {
    const float focal = g->focal, cxs = g->cxs, cys = g->cys;
    const float a0 = xf->A[0], a1 = xf->A[1], a2 = xf->A[2];
    const float a3 = xf->A[3], a4 = xf->A[4], a5 = xf->A[5];
    const float a6 = xf->A[6], a7 = xf->A[7], a8 = xf->A[8];
    const float t0 = xf->t[0], t1 = xf->t[1], t2 = xf->t[2];
    const float *p = m->xyz;
    gfx3d_vert_t *v = vs;
    for (int i = 0; i < nverts; i++, p += 3, v++) {
      const float px = p[0], py = p[1], pz = p[2];
      const float x = a0 * px + a1 * py + a2 * pz + t0;
      const float y = a3 * px + a4 * py + a5 * pz + t1;
      const float z = a6 * px + a7 * py + a8 * pz + t2;
      v->x = x;
      v->y = y;
      v->z = z;
      const float depth = -z;
      if (depth >= zn) {
        const float k = focal / depth;
        v->sx = cxs + x * k;
        v->sy = cys - y * k;
      }
    }
  }
  const float ex = xf->eye[0], ey = xf->eye[1], ez = xf->eye[2];
  const float lx = xf->light[0], ly = xf->light[1], lz = xf->light[2];
  const float ambient = g->ambient, diffuse = 1.0f - ambient;
  const float vx0 = (float)g->vx, vx1 = (float)(g->vx + g->vw);
  const float vy0 = (float)g->vy, vy1 = (float)(g->vy + g->vh);
  const bool fog = !bg && g->fog_on;
  const float fog_near = g->fog_near, fog_range = g->fog_far - g->fog_near;
  const uint16_t fc = g->fog_color;
  const float fog_r = (float)((fc >> 11) & 31), fog_g = (float)((fc >> 5) & 63);
  const float fog_b = (float)(fc & 31);
  const float *restrict pl = m->plane;
  const uint16_t *restrict idx = m->idx;
  const uint16_t *restrict colors = m->color;
  const uint8_t *restrict flags = m->flags;
  uint32_t culled = 0, clipped = 0;
  int n_one = 0;
  for (int t = 0; t < ntris; t++, pl += 4, idx += 3) {
    const float n0 = pl[0], n1 = pl[1], n2 = pl[2];
    if (n0 == 0.0f && n1 == 0.0f && n2 == 0.0f) {
      // Zero-area triangle (gfx3d_mesh_finish): never faces the eye, so it
      // never draws, even DOUBLE_SIDED.
      culled++;
      continue;
    }
    const unsigned fl = flags[t];
    const bool front = n0 * ex + n1 * ey + n2 * ez - pl[3] > 0.0f;
    if (!front && !(fl & GFX3D_DOUBLE_SIDED)) {
      culled++;
      continue;
    }
    const gfx3d_vert_t *const va = &vs[idx[0]], *const vb = &vs[idx[1]];
    const gfx3d_vert_t *const vc = &vs[idx[2]];
    const float da = -va->z, db = -vb->z, dc = -vc->z;
    if (da < zn && db < zn && dc < zn) {
      culled++;
      continue;
    }
    if (!bg && da > zf && db > zf && dc > zf) {
      culled++;
      continue;
    }
    float tx[4], ty[4];
    int nv = 3;
    const bool whole = da >= zn && db >= zn && dc >= zn;
    if (whole) {
      const float xa = va->sx, xb = vb->sx, xc = vc->sx;
      const float ya = va->sy, yb = vb->sy, yc = vc->sy;
      if ((!(xa >= vx0) && !(xb >= vx0) && !(xc >= vx0)) ||
          (!(xa <= vx1) && !(xb <= vx1) && !(xc <= vx1)) ||
          (!(ya >= vy0) && !(yb >= vy0) && !(yc >= vy0)) ||
          (!(ya <= vy1) && !(yb <= vy1) && !(yc <= vy1))) {
        culled++;
        continue;
      }
    } else {
      nv = clip_near(g, (const float *)(const void *)va,
                     (const float *)(const void *)vb,
                     (const float *)(const void *)vc, tx, ty);
      clipped++;
      if (offscreen(vx0, vx1, vy0, vy1, tx, ty, nv)) {
        culled++;
        continue;
      }
    }
    uint16_t col = colors[t];
    if (!(fl & GFX3D_UNLIT)) {
      float nl = n0 * lx + n1 * ly + n2 * lz;
      if (!front) nl = -nl;
      col = shade(col, ambient + diffuse * (nl > 0.0f ? nl : 0.0f));
    }
    const float depth = (fmaxf(da, zn) + fmaxf(db, zn) + fmaxf(dc, zn)) * (1.0f / 3.0f);
    if (fog && !(fl & GFX3D_NO_FOG))
      col = fog_mix(fog_near, fog_range, fog_r, fog_g, fog_b, col, depth);
    const int bucket = bg ? BG_BUCKET : bucket_of(zn, zrange, depth + bias);
    if (whole) {
      add_tri(g, order, &n_one, one, bucket, col, depth, va->sx, va->sy, vb->sx,
              vb->sy, vc->sx, vc->sy);
    } else {
      for (int k = 1; k + 1 < nv; k++)
        if (!add_tri(g, order, &n_one, one, bucket, col, depth, tx[0], ty[0],
                     tx[k], ty[k], tx[k + 1], ty[k + 1]))
          break;
    }
  }
  g->stats.culled += culled;
  g->stats.clipped += clipped;
  if (one && n_one > 0) {
    qsort(order, (size_t)n_one, sizeof *order, order_cmp);
    const int b = bucket_of(zn, zrange, inst_depth + bias);
    for (int i = 0; i < n_one; i++) link_rec(g, b, order[i].rec, 0);
  }
}

void gfx3d_xform(const gfx3d_t *g, const float rot[9], const float pos[3],
                 float scale, gfx3d_xform_t *xf) {
  float vr[9], rt[9];
  const float d[3] = {pos[0] - g->cam[0], pos[1] - g->cam[1], pos[2] - g->cam[2]};
  mat_mul(vr, g->V, rot);
  for (int i = 0; i < 9; i++) xf->A[i] = vr[i] * scale;
  mat_vec(xf->t, g->V, d);
  mat_t(rt, rot);
  const float back[3] = {-d[0] / scale, -d[1] / scale, -d[2] / scale};
  mat_vec(xf->eye, rt, back);
  mat_vec(xf->light, rt, g->light);
  xf->scale = scale;
}

void gfx3d_draw_xf(gfx3d_t *g, const gfx3d_mesh_t *m, const gfx3d_xform_t *xf,
                   float bias, bool sort_as_one) {
  g->stats.tris_in += (uint32_t)m->ntris;
  float c[3];
  mat_vec(c, xf->A, m->center);
  c[0] += xf->t[0];
  c[1] += xf->t[1];
  c[2] += xf->t[2];
  if (sphere_outside(g, c, m->radius * xf->scale, false)) {
    g->stats.culled += (uint32_t)m->ntris;
    return;
  }
  process_mesh(g, m, xf, false, bias, sort_as_one, -xf->t[2]);
}

void gfx3d_draw(gfx3d_t *g, const gfx3d_mesh_t *m, const float rot[9],
                const float pos[3], float scale, float bias, bool sort_as_one) {
  gfx3d_xform_t xf;
  gfx3d_xform(g, rot, pos, scale, &xf);
  gfx3d_draw_xf(g, m, &xf, bias, sort_as_one);
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

// Files n sprites. With ssw == 0 each is its slot's whole image (gfx3d_image_add's, whose
// size is read now); else the one sprite shows (ssx, ssy, ssw, ssh) of whatever the target
// resolves for its slot. Returns how many were filed. Everything it reads from `g` goes
// into locals first: the stores to the records could alias any of it, and each reload
// would be a read from PSRAM.
static int file_sprites(gfx3d_t *g, const gfx3d_sprite_t *s, int n, float bias, int ssx, int ssy,
                        int ssw, int ssh) {
  float V[9], cam[3];
  memcpy(V, g->V, sizeof V);
  memcpy(cam, g->cam, sizeof cam);
  const float focal = g->focal, zn = g->znear, zf = g->zfar, zrange = zf - zn;
  const float cxs = g->cxs, cys = g->cys;
  const int vx0 = g->vx, vy0 = g->vy, vx1 = g->vx + g->vw, vy1 = g->vy + g->vh;
  const bool fog = g->sprite_fog && g->fog_on;
  const float fog_near = g->fog_near, fog_range = g->fog_far - g->fog_near;
  gfx3d_rec_t *const rec = g->rec, *const trec = g->trec;
  const int ntrec = g->ntrec, cap = g->cap, nimg = g->nimg;
  uint32_t *const used = g->used;
  gfx3d_bucket_t *const bucket = g->bucket;
  int nrec = g->nrec;
  uint32_t culled = 0, overflow = 0;
  int filed = 0;
  for (int i = 0; i < n; i++) {
    const float size = s[i].size;
    const uint32_t slot = s[i].slot;
    int sw = ssw, sh = ssh;
    if (!(size > 0.0f)) continue;
    if (ssw == 0) {
      if (slot >= (uint32_t)nimg) continue;
      sw = g->img[slot].w;
      sh = g->img[slot].h;
    }
    const float d[3] = {s[i].x - cam[0], s[i].y - cam[1], s[i].z - cam[2]};
    float v[3];
    mat_vec(v, V, d);
    const float depth = -v[2];
    const float k = focal / depth;
    const float h = size * k, w = h * (float)sw / (float)sh;
    if (!(depth >= zn) || depth > zf || h > 4096.0f || w > 4096.0f) {
      culled++;
      continue;
    }
    const int iw = (int)(w + 0.5f), ih = (int)(h + 0.5f);
    const int ix = (int)floorf(cxs + v[0] * k - 0.5f * w + 0.5f);
    const int iy = (int)floorf(cys - v[1] * k - 0.5f * h + 0.5f);
    if (iw < 1 || ih < 1 || ix >= vx1 || iy >= vy1 || ix + iw <= vx0 || iy + ih <= vy0) {
      culled++;
      continue;
    }
    if (nrec >= cap) {
      overflow++;
      continue;
    }
    unsigned level = 0;
    if (fog) {
      const float ft = (depth - fog_near) / fog_range;
      level = !(ft > 0.0f) ? 0u : ft >= 1.0f ? 31u : (unsigned)(ft * 31.0f + 0.5f);
    }
    const int ri = nrec++;
    gfx3d_rec_t *const r = ri < ntrec ? &trec[ri] : &rec[ri];
    r->u.spr.x = (int16_t)ix;
    r->u.spr.y = (int16_t)iy;
    r->u.spr.w = (int16_t)iw;
    r->u.spr.h = (int16_t)ih;
    r->u.spr.sx = (int16_t)ssx;
    r->u.spr.sy = (int16_t)ssy;
    r->u.spr.sw = (int16_t)sw;
    r->u.spr.sh = (int16_t)sh;
    r->u.spr.slot = (uint16_t)slot;
    r->u.spr.fog = (uint8_t)level;
    r->color = 0;
    r->next = REC_OTHER | REC_END;
    const int b = bucket_of(zn, zrange, depth + bias);
    gfx3d_bucket_t *const bk = &bucket[b];
    uint32_t *const uw = &used[b >> 5];
    const uint32_t bit = 1u << (b & 31);
    if (!(*uw & bit)) {
      *uw |= bit;
      bk->head = (uint16_t)ri;
    } else {
      const unsigned ti = bk->tail;
      gfx3d_rec_t *const last = ti < (unsigned)ntrec ? &trec[ti] : &rec[ti];
      last->next = (uint16_t)((last->next & REC_OTHER) | ri);
    }
    bk->tail = (uint16_t)ri;
    filed++;
  }
  g->nrec = nrec;
  g->stats.culled += culled;
  g->stats.overflow += overflow;
  return filed;
}

bool gfx3d_draw_sprite(gfx3d_t *g, int slot, const float pos[3], float size,
                       int sx, int sy, int sw, int sh, float bias) {
  if (slot < 0 || slot > 0xFFFE || sw <= 0 || sh <= 0 || sx < 0 || sy < 0 ||
      sx > 0x7FFF || sy > 0x7FFF || sw > 0x7FFF || sh > 0x7FFF || !(size > 0.0f))
    return false;
  const gfx3d_sprite_t one = {pos[0], pos[1], pos[2], size, (uint32_t)slot};
  return file_sprites(g, &one, 1, bias, sx, sy, sw, sh) == 1;
}

int gfx3d_draw_sprites(gfx3d_t *g, const gfx3d_sprite_t *s, int n, float bias) {
  if (!s || n <= 0) return 0;
  return file_sprites(g, s, n, bias, 0, 0, 0, 0);
}

// ── Textured meshes in a scene ──────────────────────────────────────────────

GFX3D_TEX_FUNC static bool tmesh_scene_alloc(gfx3d_t *g) {
  const size_t rec = sizeof(gfx3d_xrec_t) * (size_t)g->cap, fan = sizeof(float[6]) * GFX3D_TM_FANS;
  uint8_t *const b = (uint8_t *)GFX3D_MALLOC(rec + fan + 7u);
  if (!b) return false;
  g->xblock = b;
  g->xrec = (gfx3d_xrec_t *)(void *)(((uintptr_t)b + 7u) & ~(uintptr_t)7u);
  g->fan = (float (*)[6])(void *)(g->xrec + g->cap);
  return true;
}

// A record's `color` for this triangle.
__attribute__((always_inline)) static inline uint16_t tmesh_kind(unsigned flags, unsigned slot) {
  return (uint16_t)(REC_TEX | ((flags & GFX3D_TM_BLEND) ? REC_TEX_BLEND : 0u) | slot);
}

// A triangle the near plane cuts, as clip_near does it and to the same points, so that it
// has the flat path's outline: each crossing is interpolated from its inside end, so two
// triangles sharing the edge get the same one, in position, uv and colour alike. The fan's
// pieces hold their uv in the arena. Not inlined: few triangles a frame come here.
__attribute__((noinline)) GFX3D_TEX_FUNC static void tmesh_cut(gfx3d_t *g, const gfx3d_tmesh_t *m,
                                                               int tri, const gfx3d_tvert_t *va,
                                                               const gfx3d_tvert_t *vb,
                                                               const gfx3d_tvert_t *vc, float bias) {
  const unsigned fl = m->flags[tri], slot = m->tex[tri];
  // No screen position yet to tell which way it faces: < 0 faces the eye, at the origin.
  const float ux = vb->x - va->x, uy = vb->y - va->y, uz = vb->z - va->z;
  const float wx = vc->x - va->x, wy = vc->y - va->y, wz = vc->z - va->z;
  const float facing = (uy * wz - uz * wy) * va->x + (uz * wx - ux * wz) * va->y +
                       (ux * wy - uy * wx) * va->z;
  if ((fl & GFX3D_TM_DOUBLE_SIDED) ? facing == 0.0f : !(facing < 0.0f)) {
    g->stats.culled++;
    return;
  }
  const gfx3d_tvert_t *const in[3] = {va, vb, vc};
  const uint16_t *const idx = &m->idx[3 * tri];
  const float zn = g->znear;
  uint16_t col[3];
  for (int i = 0; i < 3; i++) col[i] = (fl & GFX3D_TM_NO_FOG) ? m->color[idx[i]] : in[i]->color;
  float ox[4], oy[4], oiw[4], ou[4], ov[4];
  uint16_t oc[4];
  int n = 0;
  for (int i = 0; i < 3; i++) {
    const int j = (i + 1) % 3;
    const float du = -in[i]->z - zn, dw = -in[j]->z - zn;  // >= 0: inside
    if (du >= 0.0f) {
      const float *const uv = &m->uv[2 * idx[i]];
      ox[n] = in[i]->sx;
      oy[n] = in[i]->sy;
      oiw[n] = in[i]->iw;
      ou[n] = uv[0];
      ov[n] = uv[1];
      oc[n] = col[i];
      n++;
    }
    if ((du >= 0.0f) != (dw >= 0.0f)) {
      const int si = du >= 0.0f ? i : j, oi = du >= 0.0f ? j : i;
      const gfx3d_tvert_t *const s = in[si], *const o = in[oi];
      const float ds = du >= 0.0f ? du : dw, dd = du >= 0.0f ? dw : du;
      // Limited only where it is inf / inf, beyond any world, as a colour may not be NaN.
      const float t = fminf(fmaxf(ds / (ds - dd), 0.0f), 1.0f);
      const float k = g->focal / fmaxf(-(s->z + (o->z - s->z) * t), zn);
      const float *const us = &m->uv[2 * idx[si]], *const uo = &m->uv[2 * idx[oi]];
      ox[n] = g->cxs + (s->x + (o->x - s->x) * t) * k;
      oy[n] = g->cys - (s->y + (o->y - s->y) * t) * k;
      oiw[n] = k;
      ou[n] = us[0] + (uo[0] - us[0]) * t;
      ov[n] = us[1] + (uo[1] - us[1]) * t;
      const unsigned cs = col[si], co = col[oi];
      const float r0 = (float)(cs >> 11), g0 = (float)((cs >> 5) & 63), b0 = (float)(cs & 31);
      oc[n] = rgb565f(r0 + ((float)(co >> 11) - r0) * t, g0 + ((float)((co >> 5) & 63) - g0) * t,
                      b0 + ((float)(co & 31) - b0) * t);
      n++;
    }
  }
  g->stats.clipped++;
  if (offscreen((float)g->vx, (float)(g->vx + g->vw), (float)g->vy, (float)(g->vy + g->vh), ox, oy,
                n) ||
      (int)slot >= g->ntex) {
    g->stats.culled++;
    return;
  }
  const float depth = (fmaxf(-va->z, zn) + fmaxf(-vb->z, zn) + fmaxf(-vc->z, zn)) * (1.0f / 3.0f);
  const int bucket = bucket_of(zn, g->zfar - zn, depth + bias);
  for (int k = 1; k + 1 < n; k++) {
    if (g->nfan >= GFX3D_TM_FANS) {
      g->stats.overflow++;
      break;
    }
    const int ri = new_rec(g);
    if (ri < 0) break;
    const int v[3] = {0, k, k + 1};
    gfx3d_rec_t *const r = rec_at(g, ri);
    gfx3d_xrec_t *const x = &g->xrec[ri];
    float *const uv = g->fan[g->nfan];
    for (int c = 0; c < 3; c++) {
      r->u.tri.x[c] = ox[v[c]];
      r->u.tri.y[c] = oy[v[c]];
      x->iw[c] = oiw[v[c]];
      x->color[c] = oc[v[c]];
      uv[2 * c] = ou[v[c]];
      uv[2 * c + 1] = ov[v[c]];
    }
    r->color = tmesh_kind(fl, slot);
    x->mesh = NULL;
    x->tri = (uint16_t)g->nfan++;
    link_rec(g, bucket, ri, REC_OTHER);
  }
}

// As process_mesh: everything the loops read from g and m is a local first. Which way a
// triangle faces comes from its screen area, as a mesh of this kind has no planes.
GFX3D_TEX_FUNC static void process_tmesh(gfx3d_t *g, const gfx3d_tmesh_t *m, const float a[9],
                                         const float t[3], float bias) {
  const int nverts = m->nverts, ntris = m->ntris;
  gfx3d_vert_t *scratch;
  gfx3d_order_t *no_order;
  const size_t vbytes = sizeof(gfx3d_tvert_t) * (size_t)nverts;
  if (!get_scratch(g, (int)((vbytes + sizeof(gfx3d_vert_t) - 1u) / sizeof(gfx3d_vert_t)), 0, &scratch,
                   &no_order)) {
    g->stats.overflow += (uint32_t)ntris;
    return;
  }
  gfx3d_tvert_t *const vs = (gfx3d_tvert_t *)(void *)scratch;
  const float zn = g->znear, zf = g->zfar, zrange = zf - zn;
  {
    const bool fog = g->fog_on;
    const float fog_near = g->fog_near, fog_range = g->fog_far - g->fog_near;
    const uint16_t fc = g->fog_color;
    const float fog_r = (float)((fc >> 11) & 31), fog_g = (float)((fc >> 5) & 63);
    const float fog_b = (float)(fc & 31);
    const float focal = g->focal, cxs = g->cxs, cys = g->cys;
    const float a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4], a5 = a[5];
    const float a6 = a[6], a7 = a[7], a8 = a[8], t0 = t[0], t1 = t[1], t2 = t[2];
    const float *p = m->xyz;
    const uint16_t *restrict colors = m->color;
    gfx3d_tvert_t *v = vs;
    for (int i = 0; i < nverts; i++, p += 3, v++) {
      const float px = p[0], py = p[1], pz = p[2];
      const float x = a0 * px + a1 * py + a2 * pz + t0;
      const float y = a3 * px + a4 * py + a5 * pz + t1;
      const float z = a6 * px + a7 * py + a8 * pz + t2;
      v->x = x;
      v->y = y;
      v->z = z;
      const float depth = -z;
      if (depth >= zn) {
        const float k = focal / depth;
        v->sx = cxs + x * k;
        v->sy = cys - y * k;
        v->iw = k;
      }
      const uint16_t c = colors[i];
      v->color = fog ? fog_mix(fog_near, fog_range, fog_r, fog_g, fog_b, c, depth) : c;
    }
  }
  const float vx0 = (float)g->vx, vx1 = (float)(g->vx + g->vw);
  const float vy0 = (float)g->vy, vy1 = (float)(g->vy + g->vh);
  const unsigned ntex = (unsigned)g->ntex;
  const uint16_t *restrict idx = m->idx;
  const uint16_t *restrict colors = m->color;
  const uint8_t *restrict tex = m->tex;
  const uint8_t *restrict flags = m->flags;
  gfx3d_xrec_t *const xrec = g->xrec;
  uint32_t culled = 0;
  for (int i = 0; i < ntris; i++, idx += 3) {
    const unsigned ia = idx[0], ib = idx[1], ic = idx[2];
    const gfx3d_tvert_t *const va = &vs[ia], *const vb = &vs[ib], *const vc = &vs[ic];
    const float da = -va->z, db = -vb->z, dc = -vc->z;
    if ((da < zn && db < zn && dc < zn) || (da > zf && db > zf && dc > zf)) {
      culled++;
      continue;
    }
    if (!(da >= zn && db >= zn && dc >= zn)) {
      tmesh_cut(g, m, i, va, vb, vc, bias);
      continue;
    }
    const unsigned fl = flags[i], slot = tex[i];
    const float xa = va->sx, xb = vb->sx, xc = vc->sx, ya = va->sy, yb = vb->sy, yc = vc->sy;
    // Below 0 it faces the eye: screen y runs down, and its front is counter-clockwise.
    const float area = (xb - xa) * (yc - ya) - (xc - xa) * (yb - ya);
    if (((fl & GFX3D_TM_DOUBLE_SIDED) ? area == 0.0f : !(area < 0.0f)) ||
        (!(xa >= vx0) && !(xb >= vx0) && !(xc >= vx0)) ||
        (!(xa <= vx1) && !(xb <= vx1) && !(xc <= vx1)) ||
        (!(ya >= vy0) && !(yb >= vy0) && !(yc >= vy0)) ||
        (!(ya <= vy1) && !(yb <= vy1) && !(yc <= vy1)) || slot >= ntex) {
      culled++;
      continue;
    }
    const int ri = new_rec(g);
    if (ri < 0) continue;
    gfx3d_rec_t *const r = rec_at(g, ri);
    r->u.tri.x[0] = xa;
    r->u.tri.y[0] = ya;
    r->u.tri.x[1] = xb;
    r->u.tri.y[1] = yb;
    r->u.tri.x[2] = xc;
    r->u.tri.y[2] = yc;
    r->color = tmesh_kind(fl, slot);
    gfx3d_xrec_t *const x = &xrec[ri];
    x->iw[0] = va->iw;
    x->iw[1] = vb->iw;
    x->iw[2] = vc->iw;
    x->mesh = m;
    x->tri = (uint16_t)i;
    if (fl & GFX3D_TM_NO_FOG) {
      x->color[0] = colors[ia];
      x->color[1] = colors[ib];
      x->color[2] = colors[ic];
    } else {
      x->color[0] = va->color;
      x->color[1] = vb->color;
      x->color[2] = vc->color;
    }
    link_rec(g, bucket_of(zn, zrange, (da + db + dc) * (1.0f / 3.0f) + bias), ri, REC_OTHER);
  }
  g->stats.culled += culled;
}

GFX3D_TEX_FUNC void gfx3d_draw_tmesh(gfx3d_t *g, const gfx3d_tmesh_t *m, const float rot[9],
                                     const float pos[3], float scale, float bias) {
  g->stats.tris_in += (uint32_t)m->ntris;
  float a[9], t[3], c[3];
  if (rot) {
    mat_mul(a, g->V, rot);
  } else {
    memcpy(a, g->V, sizeof a);
  }
  for (int i = 0; i < 9; i++) a[i] *= scale;
  const float d[3] = {pos[0] - g->cam[0], pos[1] - g->cam[1], pos[2] - g->cam[2]};
  mat_vec(t, g->V, d);
  mat_vec(c, a, m->center);
  c[0] += t[0];
  c[1] += t[1];
  c[2] += t[2];
  if (sphere_outside(g, c, m->radius * fabsf(scale), false)) {
    g->stats.culled += (uint32_t)m->ntris;
    return;
  }
  if (!g->xrec && !tmesh_scene_alloc(g)) {
    g->stats.overflow += (uint32_t)m->ntris;
    return;
  }
  process_tmesh(g, m, a, t, bias);
}

// ── Rasterisation ───────────────────────────────────────────────────────────

static void fill_rect(const gfx3d_target_t *t, const disp_clip_t *c, uint16_t v) {
  const int x0 = c->x0, x1 = c->x1, y1 = c->y1;
  uint16_t *const fb = t->fb;
  const int stride = t->stride;
  for (int y = c->y0; y <= y1; y++) disp_span16(fb + (size_t)y * stride, x0, x1, v);
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
  const int cx0 = c->x0, cx1 = c->x1, cy1 = c->y1;
  uint16_t *const fb = t->fb;
  const int stride = t->stride;
  for (int y = c->y0; y <= cy1; y++) {
    uint16_t *row = fb + (size_t)y * stride;
    const float h0 = B * ((float)y + 0.5f) + C;  // h = A*(px + 0.5) + h0
    int x = cx0, b = 0;
    const float hx = A * ((float)x + 0.5f) + h0;
    while (b + 1 < n && hx >= th[b + 1]) b++;
    while (x <= cx1) {
      int end = cx1 + 1, nb = b;
      if (A > 0.0f && b + 1 < n) {  // h rises: band b+1 from h >= th[b+1]
        end = first_px((th[b + 1] - h0) / A - 0.5f, false, x, cx1 + 1);
        nb = b + 1;
      } else if (A < 0.0f && b > 0) {  // h falls: band b-1 once h < th[b]
        end = first_px((th[b] - h0) / A - 0.5f, true, x, cx1 + 1);
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

#include "gfx3d_tex.h"

// Mip `level` of a texture (past the last: the last).
GFX3D_TEX_FUNC static const uint16_t *tex_level(const gfx3d_tex_t *t, int level, int *lw, int *lh) {
  if (level > t->nmips - 1) level = t->nmips - 1;
  const uint16_t *px = t->px;
  int w = t->lg_w, h = t->lg_h;
  for (int k = 0; k < level; k++, w--, h--) px += (size_t)1u << (w + h);
  *lw = w;
  *lh = h;
  return px;
}

// A triangle's mip, from its texels per pixel along its longest edge on screen: level 0
// below TEX_MIP_RATIO, then one more per doubling. Squared, to spare the two roots, so
// each level is four times the last.
__attribute__((always_inline)) static inline int tex_mip(const gfx3d_tex_t *t, const tex_vert_t *v) {
  const int last = t->nmips - 1;
  if (last <= 0) return 0;
  const float e0x = v[1].x - v[0].x, e0y = v[1].y - v[0].y;
  const float e1x = v[2].x - v[1].x, e1y = v[2].y - v[1].y;
  const float e2x = v[0].x - v[2].x, e2y = v[0].y - v[2].y;
  const float l0 = e0x * e0x + e0y * e0y, l1 = e1x * e1x + e1y * e1y, l2 = e2x * e2x + e2y * e2y;
  const tex_vert_t *p = &v[0], *q = &v[1];
  float len = l0;
  if (l1 > len) {
    len = l1;
    p = &v[1];
    q = &v[2];
  }
  if (l2 > len) {
    len = l2;
    p = &v[2];
    q = &v[0];
  }
  const float du = (q->u - p->u) * (float)(1 << t->lg_w), dv = (q->v - p->v) * (float)(1 << t->lg_h);
  const float texels = du * du + dv * dv;
  float limit = len * (TEX_MIP_RATIO * TEX_MIP_RATIO);
  int level = 0;
  while (level < last && texels >= limit) {
    level++;
    limit *= 4.0f;
  }
  return level;
}

// The vertices tex_tri reads: in SRAM on the device, where the stack is PSRAM.
static tex_vert_t s_tex_verts[3];

// A textured record: everything is read here, once, and the rest is tex_tri's.
__attribute__((noinline)) GFX3D_TEX_FUNC static void raster_tex(gfx3d_t *g, const gfx3d_target_t *t,
                                                                const disp_clip_t *c,
                                                                const gfx3d_rec_t *r) {
  const size_t in_tier = (uintptr_t)r - (uintptr_t)g->trec;
  const size_t i = in_tier < (size_t)g->ntrec * sizeof *r ? in_tier / sizeof *r : (size_t)(r - g->rec);
  const gfx3d_xrec_t *const x = &g->xrec[i];
  const gfx3d_tmesh_t *const m = x->mesh;
  tex_vert_t *const v = s_tex_verts;
  if (m) {
    const uint16_t *const idx = &m->idx[3u * x->tri];
    const float *const uv = m->uv;
    for (int k = 0; k < 3; k++) {
      const float *const p = &uv[2u * idx[k]];
      v[k].u = p[0];
      v[k].v = p[1];
    }
  } else {
    const float *const p = g->fan[x->tri];
    for (int k = 0; k < 3; k++) {
      v[k].u = p[2 * k];
      v[k].v = p[2 * k + 1];
    }
  }
  for (int k = 0; k < 3; k++) {
    v[k].x = r->u.tri.x[k];
    v[k].y = r->u.tri.y[k];
    v[k].iw = x->iw[k];
    v[k].color = x->color[k];
  }
  const unsigned kind = r->color;
  const gfx3d_tex_t *const tx = &g->tex[kind & REC_TEX_SLOT];
  int lw, lh;
  const uint16_t *const px = tex_level(tx, tex_mip(tx, v), &lw, &lh);
  g->stats.pixels += tex_tri(t->fb, t->stride, c->x0, c->y0, c->x1, c->y1, &v[0], &v[1], &v[2], px,
                             lw, lh, (kind & REC_TEX_BLEND) != 0, t->swap);
  g->stats.drawn++;
}

// A sprite record. The one-texel and 2 x 2 ones, which are most of a valley's, skip the
// blitter's setup: the pixels are the ones it would make, sampled the same way.
// pixels counts what the call covers inside the clip, the keyed ones too.
__attribute__((noinline)) static void raster_sprite(gfx3d_t *g, const gfx3d_target_t *t,
                                                    const disp_clip_t *c, const gfx3d_rec_t *r) {
  const uint16_t *data;
  int iw, ih;
  uint16_t key;
  if (!t->sprite || !t->sprite(t->ud, r->u.spr.slot, &data, &iw, &ih, &key)) return;
  const int sx = r->u.spr.sx, sy = r->u.spr.sy, sw = r->u.spr.sw, sh = r->u.spr.sh;
  if (sx + sw > iw || sy + sh > ih) return;  // the image is smaller than when it was queued
  const int x = r->u.spr.x, y = r->u.spr.y, w = r->u.spr.w, h = r->u.spr.h;
  const unsigned level = r->u.spr.fog;
  const uint16_t fogc = g->fog_color;
  g->stats.sprites++;
  if (w <= 2 && h <= 2) {
    // Destination (dx, dy) samples source (sx + dx * sw / w, sy + dy * sh / h); with w, h
    // at most 2, dx * sw / w is 0 or sw / 2.
    const int colx[2] = {0, sw / 2}, rowy[2] = {0, sh / 2};
    uint32_t covered = 0;
    for (int dy = 0; dy < h; dy++) {
      const int py = y + dy;
      if (py < c->y0 || py > c->y1) continue;
      const uint16_t *const sp = data + (size_t)(sy + rowy[dy]) * (size_t)iw + sx;
      for (int dx = 0; dx < w; dx++) {
        const int px = x + dx;
        if (px < c->x0 || px > c->x1) continue;
        covered++;
        uint16_t v = sp[colx[dx]];
        if (key && v == key) continue;
        if (level) v = disp_fog_px(v, fogc, level);
        t->fb[(size_t)py * (size_t)t->stride + (size_t)px] = disp_px(v, t->swap);
      }
    }
    g->stats.pixels += covered;
    return;
  }
  g->stats.pixels += level ? disp_blit_scaled_rect_fog(t->fb, t->stride, c, x, y, data, iw, sx, sy,
                                                       sw, sh, w, h, key, t->swap, fogc, level)
                           : disp_blit_scaled_rect(t->fb, t->stride, c, x, y, data, iw, sx, sy,
                                                   sw, sh, w, h, key, t->swap);
}

// A record that is no flat triangle: two tail calls, so no frame of its own.
__attribute__((noinline)) static void raster_other(gfx3d_t *g, const gfx3d_target_t *t,
                                                   const disp_clip_t *c,
                                                   const gfx3d_rec_t *r) {
  if (r->color & REC_TEX)
    raster_tex(g, t, c, r);
  else
    raster_sprite(g, t, c, r);
}

// Every record, the background bucket then far to near. Only the buckets in use
// are looked at (of 1025, a frame has a few hundred).
GFX3D_RAM_FUNC static void raster_scene(gfx3d_t *g, const gfx3d_target_t *t,
                                        const disp_clip_t *c) {
  const gfx3d_rec_t *const rec = g->rec, *const trec = g->trec;
  const uint32_t ntrec = (uint32_t)g->ntrec;
  const uint32_t *const used = g->used;
  const gfx3d_bucket_t *const bucket = g->bucket;
  uint16_t *const fb = t->fb;
  const int stride = t->stride;
  const int cx0 = c->x0, cy0 = c->y0, cx1 = c->x1, cy1 = c->y1;
  const bool swap = t->swap;
  uint32_t drawn = 0;
  for (int w = USED_WORDS - 1; w >= 0; w--) {
    uint32_t bits = used[w];
    while (bits) {
      const int bit = 31 - __builtin_clz(bits);
      bits &= ~(1u << bit);
      uint32_t i = bucket[w * 32 + bit].head;
      while (i != REC_END) {
        const gfx3d_rec_t *const r = i < ntrec ? &trec[i] : &rec[i];
        const uint32_t next = r->next;
        if (next & REC_OTHER) {
          raster_other(g, t, c, r);
        } else {
          disp_fill_tri_sorted(fb, stride, cx0, cy0, cx1, cy1, r->u.tri.x[0],
                               r->u.tri.y[0], r->u.tri.x[1], r->u.tri.y[1],
                               r->u.tri.x[2], r->u.tri.y[2],
                               disp_px(r->color, swap));
          drawn++;
        }
        i = next & ~REC_OTHER;
      }
    }
  }
  g->stats.drawn += drawn;
}

#ifdef GFX3D_TEST
void gfx3d_test_tex_tri(gfx3d_t *g, const gfx3d_target_t *t, int slot, int level,
                        const gfx3d_test_vert_t v[3], bool blend, gfx3d_test_tex_info_t *info) {
  tex_vert_t tv[3];
  for (int i = 0; i < 3; i++) {
    tv[i].x = v[i].x;
    tv[i].y = v[i].y;
    tv[i].iw = v[i].iw;
    tv[i].u = v[i].u;
    tv[i].v = v[i].v;
    tv[i].color = v[i].color;
  }
  int lw, lh;
  const uint16_t *px = tex_level(&g->tex[slot], level, &lw, &lh);
  g->stats.pixels += tex_tri(t->fb, t->stride, t->clip_x0, t->clip_y0, t->clip_x1, t->clip_y1,
                             &tv[0], &tv[1], &tv[2], px, lw, lh, blend, t->swap);
  if (info) {
    info->affine = s_tex_test_affine;
    info->rows = s_tex_test_rows;
  }
}
#endif

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
  raster_scene(g, t, &c);
}
