// Runs compiled chargrid shaders (charshader.h). Each row of the rect goes
// in strips of CS_STRIP cells, one tight loop per instruction over the strip.
// Registers live in the shader's block (PSRAM, like the app stack: P0
// measured only 7-9 % from SRAM). Scalars (constants, uniforms, cols, rows)
// are read once per instruction; only the rarer operand mixes broadcast a
// scalar into a scratch row. Built at -O2.
#include "charshader_internal.h"

#include <string.h>

#define REG(i) (s->regs + (size_t)in->a[i] * CS_STRIP)
#define SC(i) (s->sc[in->a[i]])

// Operand i as a strip: its register, or its scalar broadcast to scratch row i.
static const float *strip_of(charshader_t *s, const cs_ins_t *in, int i, int n) {
  if (!((in->scal >> i) & 1u)) return REG(i);
  float *t = s->scratch + (size_t)i * CS_STRIP;
  const float v = SC(i);
  for (int j = 0; j < n; j++) t[j] = v;
  return t;
}

#define UNARY(F)                                  \
  if (in->scal & 1u) {                            \
    const float v = F(SC(0));                     \
    for (int j = 0; j < n; j++) d[j] = v;         \
  } else {                                        \
    const float *a = REG(0);                      \
    for (int j = 0; j < n; j++) d[j] = F(a[j]);   \
  }                                               \
  break;

#define BINARY(F)                                         \
  switch (in->scal & 3u) {                                \
    case 0: {                                             \
      const float *a = REG(0), *b = REG(1);               \
      for (int j = 0; j < n; j++) d[j] = F(a[j], b[j]);   \
    } break;                                              \
    case 1: {                                             \
      const float ka = SC(0), *b = REG(1);                \
      for (int j = 0; j < n; j++) d[j] = F(ka, b[j]);     \
    } break;                                              \
    case 2: {                                             \
      const float *a = REG(0), kb = SC(1);                \
      for (int j = 0; j < n; j++) d[j] = F(a[j], kb);     \
    } break;                                              \
    default: {                                            \
      const float v = F(SC(0), SC(1));                    \
      for (int j = 0; j < n; j++) d[j] = v;               \
    } break;                                              \
  }                                                       \
  break;

#define TERNARY(F)                                                                \
  {                                                                               \
    const float *a = strip_of(s, in, 0, n), *b = strip_of(s, in, 1, n);           \
    const float *c = strip_of(s, in, 2, n);                                       \
    for (int j = 0; j < n; j++) d[j] = F(a[j], b[j], c[j]);                       \
  }                                                                               \
  break;

static void mad(charshader_t *s, const cs_ins_t *in, float *d, int n) {
  switch (in->scal & 7u) {
    case 0: {
      const float *a = REG(0), *b = REG(1), *c = REG(2);
      for (int j = 0; j < n; j++) d[j] = cs_mad(a[j], b[j], c[j]);
    } return;
    case 2: {
      const float *a = REG(0), kb = SC(1), *c = REG(2);
      for (int j = 0; j < n; j++) d[j] = cs_mad(a[j], kb, c[j]);
    } return;
    case 4: {
      const float *a = REG(0), *b = REG(1), kc = SC(2);
      for (int j = 0; j < n; j++) d[j] = cs_mad(a[j], b[j], kc);
    } return;
    case 6: {
      const float *a = REG(0), kb = SC(1), kc = SC(2);
      for (int j = 0; j < n; j++) d[j] = cs_mad(a[j], kb, kc);
    } return;
    default: {
      const float *a = strip_of(s, in, 0, n), *b = strip_of(s, in, 1, n);
      const float *c = strip_of(s, in, 2, n);
      for (int j = 0; j < n; j++) d[j] = cs_mad(a[j], b[j], c[j]);
    } return;
  }
}

// A layer field over the strip: 0 where the layer is missing or ends.
static void load_layer(const chargrid_t *l, int plane, int x0, int y, int n, float *d) {
  int m = 0;
  if (l && y < l->rows && x0 < l->cols) {
    m = l->cols - x0 < n ? l->cols - x0 : n;
    const uint8_t *p = (plane == CG_PLANE_GLYPH ? l->glyph : plane == CG_PLANE_FG ? l->fg : l->bg) +
                       (size_t)y * (size_t)l->cols + (size_t)x0;
    for (int j = 0; j < m; j++) d[j] = (float)p[j];
  }
  for (int j = m; j < n; j++) d[j] = 0.0f;
}

static void exec(charshader_t *s, const chargrid_t *g, chargrid_t *const *lay, int x0, int y,
                 int n, float inv_cols, float inv_rows) {
  const size_t row = (size_t)y * (size_t)g->cols + (size_t)x0;
  for (int k = 0; k < s->ninstr; k++) {
    const cs_ins_t *in = &s->ins[k];
    float *d = s->regs + (size_t)in->d * CS_STRIP;
    switch (in->op) {
      case CS_OP_X:
        for (int j = 0; j < n; j++) d[j] = (float)(x0 + j);
        break;
      case CS_OP_Y: {
        const float v = (float)y;
        for (int j = 0; j < n; j++) d[j] = v;
      } break;
      case CS_OP_U:
        for (int j = 0; j < n; j++) d[j] = (float)(x0 + j) * inv_cols;
        break;
      case CS_OP_V: {
        const float v = (float)y * inv_rows;
        for (int j = 0; j < n; j++) d[j] = v;
      } break;
      case CS_OP_PLANE: {
        const uint8_t *p =
            (in->aux == CG_PLANE_GLYPH ? g->glyph : in->aux == CG_PLANE_FG ? g->fg : g->bg) + row;
        for (int j = 0; j < n; j++) d[j] = (float)p[j];
      } break;
      case CS_OP_LAYER:
        load_layer(lay[in->aux >> 2], in->aux & 3, x0, y, n, d);
        break;
      case CS_OP_ARRAY: {
        const float *idx = strip_of(s, in, 0, n);
        const int32_t len = s->array_len[in->aux];
        const float *arr = s->arrays + (size_t)in->aux * CS_MAX_ARRAY_LEN;
        if (!len) {
          for (int j = 0; j < n; j++) d[j] = 0.0f;
          break;
        }
        for (int j = 0; j < n; j++) {
          const int32_t i = noise_lattice(idx[j]);
          d[j] = arr[i < 0 ? 0 : i >= len ? len - 1 : i];
        }
      } break;
      case CS_OP_SIN: UNARY(cs_sin)
      case CS_OP_COS: UNARY(cs_cos)
      case CS_OP_ABS: UNARY(cs_abs)
      case CS_OP_FLOOR: UNARY(cs_floor)
      case CS_OP_CEIL: UNARY(cs_ceil)
      case CS_OP_FRACT: UNARY(cs_fract)
      case CS_OP_SQRT: UNARY(cs_sqrt)
      case CS_OP_EXP: UNARY(cs_exp)
      case CS_OP_LOG: UNARY(cs_log)
      case CS_OP_ADD: BINARY(cs_add)
      case CS_OP_SUB: BINARY(cs_sub)
      case CS_OP_MUL: BINARY(cs_mul)
      case CS_OP_DIV: BINARY(cs_div)
      case CS_OP_MOD: BINARY(cs_mod)
      case CS_OP_POW: BINARY(cs_pow)
      case CS_OP_MIN: BINARY(cs_min)
      case CS_OP_MAX: BINARY(cs_max)
      case CS_OP_LT: BINARY(cs_lt)
      case CS_OP_LE: BINARY(cs_le)
      case CS_OP_EQ: BINARY(cs_eq)
      case CS_OP_NE: BINARY(cs_ne)
      case CS_OP_AND: BINARY(cs_and)
      case CS_OP_OR: BINARY(cs_or)
      case CS_OP_STEP: BINARY(cs_step)
      case CS_OP_ATAN2: BINARY(cs_atan2)
      case CS_OP_MAD:
        mad(s, in, d, n);
        break;
      case CS_OP_CLAMP:
        if ((in->scal & 7u) == 6u) {  // clamp(x, lo, hi) with scalar bounds
          const float *a = REG(0), lo = SC(1), hi = SC(2);
          for (int j = 0; j < n; j++) d[j] = cs_clamp(a[j], lo, hi);
          break;
        }
        TERNARY(cs_clamp)
      case CS_OP_MIX:
        if ((in->scal & 7u) == 4u) {  // mix(a, b, t) with a scalar t
          const float *a = REG(0), *b = REG(1), t = SC(2);
          for (int j = 0; j < n; j++) d[j] = cs_mix(a[j], b[j], t);
          break;
        }
        TERNARY(cs_mix)
      case CS_OP_SMOOTH:
        if ((in->scal & 7u) == 3u) {  // scalar edges: one reciprocal per instruction
          const float e0 = SC(0), inv = 1.0f / (SC(1) - e0), *a = REG(2);
          for (int j = 0; j < n; j++) d[j] = cs_smooth_k((a[j] - e0) * inv);
          break;
        }
        TERNARY(cs_smooth)
      case CS_OP_SELECT: TERNARY(cs_select)
      case CS_OP_HASH: TERNARY(cs_hash3)
      case CS_OP_NOISE: TERNARY(cs_noise3)
      case CS_OP_FBM: {
        const float *a = strip_of(s, in, 0, n), *b = strip_of(s, in, 1, n);
        const float *c = strip_of(s, in, 2, n);
        const int octaves = in->aux;
        for (int j = 0; j < n; j++) d[j] = cs_fbm3(a[j], b[j], c[j], octaves);
      } break;
      case CS_OP_DIST: {
        const float *a = strip_of(s, in, 0, n), *b = strip_of(s, in, 1, n);
        const float *c = strip_of(s, in, 2, n), *e = strip_of(s, in, 3, n);
        for (int j = 0; j < n; j++) d[j] = cs_dist(a[j], b[j], c[j], e[j]);
      } break;
      default:
        break;
    }
  }
}

// Output k over the strip: its register, or its scalar broadcast to scratch
// row k (each output has its own row, so all of them can be live at once).
static const float *output(charshader_t *s, int k, int n) {
  if (!((s->out_scal >> k) & 1u)) return s->regs + (size_t)s->out_ref[k] * CS_STRIP;
  float *t = s->scratch + (size_t)k * CS_STRIP;
  const float v = s->sc[s->out_ref[k]];
  for (int j = 0; j < n; j++) t[j] = v;
  return t;
}

// The rect clipped to g, in strips; output `out` into values. Task 6 adds the
// path that writes the grid.
static int run(charshader_t *s, chargrid_t *g, int x, int y, int w, int h,
               chargrid_t *const *layers, int nlayers, int out, float *values) {
  if (w <= 0 || h <= 0) return 0;
  int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > g->cols) x1 = g->cols;
  if (y1 > g->rows) y1 = g->rows;
  if (x0 >= x1 || y0 >= y1) return 0;
  chargrid_t *lay[CS_MAX_LAYERS] = {NULL, NULL, NULL, NULL};
  for (int i = 0; i < s->nlayers && i < nlayers; i++) lay[i] = layers ? layers[i] : NULL;
  s->sc[CS_SC_COLS] = (float)g->cols;
  s->sc[CS_SC_ROWS] = (float)g->rows;
  const float inv_cols = 1.0f / (float)g->cols, inv_rows = 1.0f / (float)g->rows;
  for (int cy = (int)y0; cy < (int)y1; cy++)
    for (int cx = (int)x0; cx < (int)x1; cx += CS_STRIP) {
      const int n = (int)x1 - cx < CS_STRIP ? (int)x1 - cx : CS_STRIP;
      exec(s, g, lay, cx, cy, n, inv_cols, inv_rows);
      memcpy(values, output(s, out, n), (size_t)n * sizeof(float));
      values += n;
    }
  return (int)((x1 - x0) * (y1 - y0));
}

int charshader_run_values(charshader_t *s, chargrid_t *g, int x, int y, int w, int h,
                          chargrid_t *const *layers, int nlayers, int out, float *values) {
  if (out < 0 || out >= CS_OUT_COUNT || !((s->out_used >> out) & 1u)) return -1;
  return run(s, g, x, y, w, h, layers, nlayers, out, values);
}

bool charshader_set_uniform(charshader_t *s, const char *name, float v) {
  for (int i = 0; i < s->nuniforms; i++)
    if (!strcmp(s->uniform_names[i], name)) {
      s->sc[CS_SC_UNIFORM0 + i] = v;
      return true;
    }
  return false;
}

float *charshader_array(charshader_t *s, const char *name, int n) {
  if (n < 0 || n > CS_MAX_ARRAY_LEN) return NULL;
  for (int i = 0; i < s->narrays; i++)
    if (!strcmp(s->array_names[i], name)) {
      s->array_len[i] = (uint16_t)n;
      return s->arrays + (size_t)i * CS_MAX_ARRAY_LEN;
    }
  return NULL;
}

bool charshader_set_array(charshader_t *s, const char *name, const float *v, int n) {
  float *a = charshader_array(s, name, n);
  if (!a) return false;
  if (n > 0) memcpy(a, v, (size_t)n * sizeof(float));
  return true;
}
