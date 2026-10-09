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

// d[j] = F(...) over the strip's lanes, each operand a register (R) or a
// scalar (K). On ARMv8 FP (the device) four lanes at a time: VLDM and VSTM
// move four floats in one instruction, so a simple operation fetches about
// half the code bytes per lane (the loops are bound by fetching code from
// the XIP cache); the per-lane arithmetic is the C loops'. Register
// operands go in s0-s3, s4-s7 and s8-s11, the results in s12-s15. The
// lanes from n up to the next multiple of 4 are computed too and never
// read: a register has CS_STRIP lanes.
// The rule: F and its operand expressions must compile to code with no
// calls. s0-s15 are call-clobbered and GCC keeps no local register variable
// across a call, so one call corrupts the lanes, on the device only (the
// host runs the plain loops). Every F is CS_LANE_FN (always inlined, at any
// optimisation level), and K and L are read once before the loop.
_Static_assert(CS_STRIP % 4 == 0, "four lanes at a time");
#if CS_ARMV8_FP
#define LD4(p, v, R0, R1, R2, R3, LIST)                                                    \
  register float v##0 __asm__(R0), v##1 __asm__(R1), v##2 __asm__(R2), v##3 __asm__(R3); \
  __asm__("vldmia %4!, {" LIST "}"                                                         \
          : "=t"(v##0), "=t"(v##1), "=t"(v##2), "=t"(v##3), "+r"(p)                      \
          : "m"(*(const float(*)[4])(p)))
#define LDA(p) LD4(p, a, "s0", "s1", "s2", "s3", "s0-s3")
#define LDB(p) LD4(p, b, "s4", "s5", "s6", "s7", "s4-s7")
#define LDC(p) LD4(p, c, "s8", "s9", "s10", "s11", "s8-s11")
#define ST4(p, e0, e1, e2, e3)                                           \
  do {                                                                   \
    register float r0_ __asm__("s12") = (e0), r1_ __asm__("s13") = (e1); \
    register float r2_ __asm__("s14") = (e2), r3_ __asm__("s15") = (e3); \
    __asm__("vstmia %0!, {s12-s15}"                                      \
            : "+r"(p), "=m"(*(float(*)[4])(p))                           \
            : "t"(r0_), "t"(r1_), "t"(r2_), "t"(r3_));                   \
  } while (0)
#define LANES(SETUP, LOADS, F, X0, X1, X2, X3) \
  {                                            \
    SETUP;                                     \
    float *pd_ = d;                            \
    for (int j_ = 0; j_ < n; j_ += 4) {        \
      LOADS;                                   \
      ST4(pd_, F X0, F X1, F X2, F X3);        \
    }                                          \
  }
#define LANES_RR(F, A, B)                                                         \
  LANES(const float *pa_ = (A); const float *pb_ = (B), LDA(pa_); LDB(pb_), F, \
        (a0, b0), (a1, b1), (a2, b2), (a3, b3))
#define LANES_KR(F, K, B)                                                    \
  LANES(const float k_ = (K); const float *pb_ = (B), LDB(pb_), F, (k_, b0), \
        (k_, b1), (k_, b2), (k_, b3))
#define LANES_RK(F, A, K)                                                    \
  LANES(const float *pa_ = (A); const float k_ = (K), LDA(pa_), F, (a0, k_), \
        (a1, k_), (a2, k_), (a3, k_))
#define LANES_RRR(F, A, B, C)                                                       \
  LANES(const float *pa_ = (A); const float *pb_ = (B); const float *pc_ = (C),     \
        LDA(pa_); LDB(pb_); LDC(pc_), F, (a0, b0, c0), (a1, b1, c1), (a2, b2, c2), \
        (a3, b3, c3))
#define LANES_RKR(F, A, K, C)                                                                \
  LANES(const float *pa_ = (A); const float k_ = (K); const float *pc_ = (C),                \
        LDA(pa_); LDC(pc_), F, (a0, k_, c0), (a1, k_, c1), (a2, k_, c2), (a3, k_, c3))
#define LANES_RRK(F, A, B, K)                                                                \
  LANES(const float *pa_ = (A); const float *pb_ = (B); const float k_ = (K),                \
        LDA(pa_); LDB(pb_), F, (a0, b0, k_), (a1, b1, k_), (a2, b2, k_), (a3, b3, k_))
#define LANES_RKK(F, A, K, L)                                                                \
  LANES(const float *pa_ = (A); const float k_ = (K); const float l_ = (L), LDA(pa_), F,     \
        (a0, k_, l_), (a1, k_, l_), (a2, k_, l_), (a3, k_, l_))
#else
#define LANES_RR(F, A, B) \
  { const float *pa_ = (A), *pb_ = (B); for (int j = 0; j < n; j++) d[j] = F(pa_[j], pb_[j]); }
#define LANES_KR(F, K, B) \
  { const float k_ = (K), *pb_ = (B); for (int j = 0; j < n; j++) d[j] = F(k_, pb_[j]); }
#define LANES_RK(F, A, K) \
  { const float *pa_ = (A), k_ = (K); for (int j = 0; j < n; j++) d[j] = F(pa_[j], k_); }
#define LANES_RRR(F, A, B, C)                                                  \
  {                                                                            \
    const float *pa_ = (A), *pb_ = (B), *pc_ = (C);                            \
    for (int j = 0; j < n; j++) d[j] = F(pa_[j], pb_[j], pc_[j]);              \
  }
#define LANES_RKR(F, A, K, C)                                                  \
  {                                                                            \
    const float *pa_ = (A), k_ = (K), *pc_ = (C);                              \
    for (int j = 0; j < n; j++) d[j] = F(pa_[j], k_, pc_[j]);                  \
  }
#define LANES_RRK(F, A, B, K)                                                  \
  {                                                                            \
    const float *pa_ = (A), *pb_ = (B), k_ = (K);                              \
    for (int j = 0; j < n; j++) d[j] = F(pa_[j], pb_[j], k_);                  \
  }
#define LANES_RKK(F, A, K, L)                                                  \
  {                                                                            \
    const float *pa_ = (A), k_ = (K), l_ = (L);                                \
    for (int j = 0; j < n; j++) d[j] = F(pa_[j], k_, l_);                      \
  }
#endif

// Operand i as a strip: its register, or its scalar broadcast to scratch row i.
static const float *strip_of(charshader_t *s, const cs_ins_t *in, int i, int n) {
  if (!((in->scal >> i) & 1u)) return REG(i);
  float *t = s->scratch + (size_t)i * CS_STRIP;
  const float v = SC(i);
  for (int j = 0; j < n; j++) t[j] = v;
  return t;
}

// The opcodes come grouped by operand count (charshader_internal.h): loads,
// CS_OP_ARRAY and the other one-operand operations, then two, three, four.
_Static_assert(CS_OP_X == 0 && CS_OP_LAYER + 1 == CS_OP_ARRAY, "loads, then one operand");
_Static_assert(CS_OP_ARRAY + 1 == CS_OP_SIN, "CS_OP_SIN starts the operations to fold");
_Static_assert(CS_OP_LOG + 1 == CS_OP_ADD, "one operand, then two");
_Static_assert(CS_OP_ATAN2 + 1 == CS_OP_MAD, "two operands, then three");
_Static_assert(CS_OP_FBM + 1 == CS_OP_DIST && CS_OP_DIST + 1 == CS_OP_COUNT, "three, then four");
static inline int nargs_of(int op) {
  return op >= CS_OP_DIST ? 4 : op >= CS_OP_MAD ? 3 : op >= CS_OP_ADD ? 2 : op >= CS_OP_ARRAY ? 1 : 0;
}

// An operation whose operands are all scalars (uniforms, constants, cols,
// rows): the folder's cs_eval, once per strip and broadcast. So the strip
// loops below only ever see a register among the operands, and the
// operations' code holds no scalar-only copies.
static float __attribute__((noinline)) eval_scalars(const charshader_t *s, const cs_ins_t *in) {
  return cs_eval(in->op, in->aux, SC(0), SC(1), SC(2), SC(3));
}

#define UNARY(F)                                  \
  {                                               \
    const float *a = REG(0);                      \
    for (int j = 0; j < n; j++) d[j] = F(a[j]);   \
  }                                               \
  break;

// Never both scalars (eval_scalars).
#define BINARY(F)                           \
  switch (in->scal & 3u) {                  \
    case 0:                                 \
      LANES_RR(F, REG(0), REG(1));          \
      break;                                \
    case 1:                                 \
      LANES_KR(F, SC(0), REG(1));           \
      break;                                \
    default:                                \
      LANES_RK(F, REG(0), SC(1));           \
      break;                                \
  }                                         \
  break;

#define TERNARY(F)                                                                \
  {                                                                               \
    const float *a = strip_of(s, in, 0, n), *b = strip_of(s, in, 1, n);           \
    const float *c = strip_of(s, in, 2, n);                                       \
    for (int j = 0; j < n; j++) d[j] = F(a[j], b[j], c[j]);                       \
  }                                                                               \
  break;

// smooth(e0, e1, x) as cs_smooth computes it, given 1 / (e1 - e0).
CS_LANE_FN float smooth_scaled(float x, float e0, float inv) { return cs_smooth_k((x - e0) * inv); }

// The rarer operations and operand forms, out of the main loop's way (its
// code stays compact in the XIP cache): one loop each, a scalar operand
// broadcast to a strip. Comparisons are here too: a program has a few.
static void __attribute__((noinline)) exec_rare(charshader_t *s, const cs_ins_t *in, float *d,
                                                int n) {
  const int na = nargs_of(in->op);
  const float *a = strip_of(s, in, 0, n);
  const float *b = na > 1 ? strip_of(s, in, 1, n) : a;
  const float *c = na > 2 ? strip_of(s, in, 2, n) : a;
  const float *e = na > 3 ? strip_of(s, in, 3, n) : a;
  switch (in->op) {
    case CS_OP_LOG:
      for (int j = 0; j < n; j++) d[j] = cs_log(a[j]);
      break;
    case CS_OP_DIV:
      for (int j = 0; j < n; j++) d[j] = cs_div(a[j], b[j]);
      break;
    case CS_OP_MOD:
      for (int j = 0; j < n; j++) d[j] = cs_mod(a[j], b[j]);
      break;
    case CS_OP_POW:
      for (int j = 0; j < n; j++) d[j] = cs_pow(a[j], b[j]);
      break;
    case CS_OP_LT:
      for (int j = 0; j < n; j++) d[j] = cs_lt(a[j], b[j]);
      break;
    case CS_OP_LE:
      for (int j = 0; j < n; j++) d[j] = cs_le(a[j], b[j]);
      break;
    case CS_OP_EQ:
      for (int j = 0; j < n; j++) d[j] = cs_eq(a[j], b[j]);
      break;
    case CS_OP_NE:
      for (int j = 0; j < n; j++) d[j] = cs_ne(a[j], b[j]);
      break;
    case CS_OP_AND:
      for (int j = 0; j < n; j++) d[j] = cs_and(a[j], b[j]);
      break;
    case CS_OP_OR:
      for (int j = 0; j < n; j++) d[j] = cs_or(a[j], b[j]);
      break;
    case CS_OP_STEP:
      for (int j = 0; j < n; j++) d[j] = cs_step(a[j], b[j]);
      break;
    case CS_OP_ATAN2:
      for (int j = 0; j < n; j++) d[j] = cs_atan2(a[j], b[j]);
      break;
    case CS_OP_CLAMP:  // with a bound in a register
      for (int j = 0; j < n; j++) d[j] = cs_clamp(a[j], b[j], c[j]);
      break;
    case CS_OP_SMOOTH:  // with an edge in a register
      for (int j = 0; j < n; j++) d[j] = cs_smooth(a[j], b[j], c[j]);
      break;
    case CS_OP_SELECT:
      for (int j = 0; j < n; j++) d[j] = cs_select(a[j], b[j], c[j]);
      break;
    case CS_OP_HASH:
      for (int j = 0; j < n; j++) d[j] = cs_hash3(a[j], b[j], c[j]);
      break;
    case CS_OP_DIST:  // to a point not two scalars
      for (int j = 0; j < n; j++) d[j] = cs_dist(a[j], b[j], c[j], e[j]);
      break;
    default:
      break;
  }
}

static void mad(charshader_t *s, const cs_ins_t *in, float *d, int n) {
  switch (in->scal & 7u) {
    case 0:
      LANES_RRR(cs_mad, REG(0), REG(1), REG(2));
      return;
    case 2:
      LANES_RKR(cs_mad, REG(0), SC(1), REG(2));
      return;
    case 4:
      LANES_RRK(cs_mad, REG(0), REG(1), SC(2));
      return;
    case 6:
      LANES_RKK(cs_mad, REG(0), SC(1), SC(2));
      return;
    default: {
      const float *a = strip_of(s, in, 0, n), *b = strip_of(s, in, 1, n);
      const float *c = strip_of(s, in, 2, n);
      for (int j = 0; j < n; j++) d[j] = cs_mad(a[j], b[j], c[j]);
    } return;
  }
}

// noise and fbm with a seed per cell: cs_noise3 and cs_fbm3, out of the hot
// loops (a seed is nearly always a constant).
static void __attribute__((noinline)) noise_seeded(const float *a, const float *b, const float *c,
                                                   int op, int octaves, float *d, int n) {
  if (op == CS_OP_NOISE)
    for (int j = 0; j < n; j++) d[j] = cs_noise3(a[j], b[j], c[j]);
  else
    for (int j = 0; j < n; j++) d[j] = cs_fbm3(a[j], b[j], c[j], octaves);
}

static const float k_zeros[CS_STRIP] = {0.0f};

// Value noise at (a[j] * f, b[j] * f) for the strip, scaled by amp: stored
// in d, or with acc added to it (the first adds to k_zeros, with no branch
// per cell: 0.0f + r is r, as r is never -0.0f or NaN).
// Neighbouring cells usually share a lattice cell, and often y: the
// corners and y's weight are reused while they repeat (noise_value_at's
// own pieces, so the same results). k0 is cell (0, 0)'s corners; py starts
// as NaN, which equals nothing.
static inline __attribute__((always_inline)) void noise_loop(const float *a, const float *b,
                                                             float f, uint32_t st,
                                                             noise_corners_t k0, float amp,
                                                             float *d, int n, bool acc) {
  const float *prev = acc ? d : k_zeros;
  noise_corners_t k = k0;
  int32_t cxi = 0, cyi = 0, yi = 0;
  float py = CS_NAN, v = 0.0f;
  for (int j = 0; j < n; j++) {
    float u;
    const int32_t xi = noise_axis(a[j] * f, &u);
    const float y = b[j] * f;
    if (__builtin_expect(y != py, 0)) {
      yi = noise_axis(y, &v);
      py = y;
    }
    if (__builtin_expect(xi != cxi || yi != cyi, 0)) {
      k = noise_corners(xi, yi, st);
      cxi = xi;
      cyi = yi;
    }
    d[j] = prev[j] + amp * noise_blend(k, u, v);
  }
}

// noise(a, b, seed) and fbm(a, b, octaves, seed) over the strip, with a
// scalar seed's term, the octave count and fbm's divisor worked out once:
// noise_value_at and noise_fbm_at's arithmetic, fbm an octave at a time.
// fbm sums in scratch row 3 (the destination may be a or b; strip_of uses
// rows 0-2): noise_fbm_at's 0.0f plus each octave's amp * value in turn,
// where 0.0f + the first is the first (a value is never -0.0f or NaN).
// noise is one octave of weight 1.0f (1.0f * value is the value). The one
// noise_loop instance is all the noise code in the strip loops.
static void noise_strip(charshader_t *s, const cs_ins_t *in, float *d, int n) {
  const float *a = strip_of(s, in, 0, n), *b = strip_of(s, in, 1, n);
  if (!(in->scal & 4u)) {
    noise_seeded(a, b, REG(2), in->op, in->aux, d, n);
    return;
  }
  const uint32_t st = noise_seed_term(noise_lattice(SC(2)));
  const bool fbm = in->op == CS_OP_FBM;
  const int octaves = fbm ? noise_fbm_octaves(in->aux) : 1;
  const noise_corners_t k0 = noise_corners(0, 0, st);
  float *sum = fbm ? s->scratch + 3 * CS_STRIP : d, amp = fbm ? 0.5f : 1.0f, f = 1.0f;
  for (int i = 0; i < octaves; i++) {
    noise_loop(a, b, f, st, k0, amp, sum, n, i > 0);
    amp *= 0.5f;
    f *= 2.0f;
  }
  if (fbm) {
    const float norm = noise_fbm_norm(octaves);
    for (int j = 0; j < n; j++) d[j] = noise_below1(sum[j] / norm);
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
    const unsigned all = (1u << nargs_of(in->op)) - 1u;
    if (in->op >= CS_OP_SIN && (in->scal & all) == all) {
      const float v = eval_scalars(s, in);
      for (int j = 0; j < n; j++) d[j] = v;
      continue;
    }
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
      case CS_OP_SIN:
      case CS_OP_COS: {  // one loop: cs_sin's turns, or cs_cos's plus a quarter
        const float *a = REG(0);
        const bool is_cos = in->op == CS_OP_COS;
        for (int j = 0; j < n; j++) {
          float t = a[j] * CS_INV_TAU;
          if (is_cos) t = t + 0.25f;
          d[j] = cs_sin_turns(t);
        }
      } break;
      case CS_OP_ABS: UNARY(cs_abs)
      case CS_OP_FLOOR: UNARY(cs_floor)
      case CS_OP_CEIL: UNARY(cs_ceil)
      case CS_OP_FRACT: UNARY(cs_fract)
      case CS_OP_SQRT: UNARY(cs_sqrt)
      case CS_OP_EXP: UNARY(cs_exp)
      case CS_OP_ADD: BINARY(cs_add)
      case CS_OP_SUB: BINARY(cs_sub)
      case CS_OP_MUL: BINARY(cs_mul)
      case CS_OP_MIN: BINARY(cs_min)
      case CS_OP_MAX: BINARY(cs_max)
      case CS_OP_MAD:
        mad(s, in, d, n);
        break;
      case CS_OP_CLAMP:
        if ((in->scal & 7u) == 6u) {  // clamp(x, lo, hi) with scalar bounds
          LANES_RKK(cs_clamp, REG(0), SC(1), SC(2));
        } else {
          exec_rare(s, in, d, n);
        }
        break;
      case CS_OP_MIX:
        if ((in->scal & 7u) == 4u) {  // mix(a, b, t) with a scalar t
          LANES_RRK(cs_mix, REG(0), REG(1), SC(2));
          break;
        }
        TERNARY(cs_mix)
      case CS_OP_SMOOTH:
        if ((in->scal & 7u) == 3u) {  // scalar edges: one reciprocal per instruction
          const float e0 = SC(0), inv = 1.0f / (SC(1) - e0);
          LANES_RKK(smooth_scaled, REG(2), e0, inv);
        } else {
          exec_rare(s, in, d, n);
        }
        break;
      case CS_OP_NOISE:
      case CS_OP_FBM:
        noise_strip(s, in, d, n);
        break;
      case CS_OP_DIST:
        if ((in->scal & 15u) == 12u) {  // dist(x, y, px, py) to a scalar point
          const float *a = REG(0), *b = REG(1), kc = SC(2), kd = SC(3);
          for (int j = 0; j < n; j++) d[j] = cs_dist(a[j], b[j], kc, kd);
        } else {
          exec_rare(s, in, d, n);
        }
        break;
      default:
        exec_rare(s, in, d, n);
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

// A glyph, fg or bg value as an index: rounded to nearest, clamped to
// 0-255; NaN is 0.
static inline uint8_t cs_index(float v) {
  if (!(v > 0.0f)) return 0;
  if (v >= 255.0f) return 255;
  return (uint8_t)(int)(v + 0.5f);
}

#define HAS(k) ((s->out_used >> (k)) & 1u)

// One plane over the strip, leaving alone the cells the keep output keeps:
// values as indices, or through a ramp of steps entries. A ramp works out
// its scale and its row of dither offsets once (zeros when it does not
// dither: chargrid_ramp_index adds the offset either way).
static const float k_no_dither[4] = {0.0f, 0.0f, 0.0f, 0.0f};

static inline void put_index_k(uint8_t *p, const float *v, const float *keep, bool has_keep,
                               int n) {
  for (int j = 0; j < n; j++)
    if (!has_keep || !(keep[j] >= 0.5f)) p[j] = cs_index(v[j]);
}
static inline __attribute__((always_inline)) void put_index(uint8_t *p, const float *v,
                                                            const float *keep, int n) {
  if (keep)
    put_index_k(p, v, keep, true, n);
  else
    put_index_k(p, v, NULL, false, n);
}

static inline void put_ramp_k(uint8_t *p, const float *v, const float *keep, bool has_keep, int n,
                              const uint8_t *map, int last, const float *off, int x0) {
  const float top = (float)last;
  for (int j = 0; j < n; j++)
    if (!has_keep || !(keep[j] >= 0.5f))
      p[j] = map[chargrid_ramp_index(v[j], top, last, off[(unsigned)(x0 + j) & 3u])];
}
static inline __attribute__((always_inline)) void put_ramp(uint8_t *p, const float *v,
                                                           const float *keep, int n,
                                                           const uint8_t *map, int steps,
                                                           bool dither, int x0, int y) {
  if (steps <= 1) {  // chargrid_ramp_step is 0
    for (int j = 0; j < n; j++)
      if (!keep || !(keep[j] >= 0.5f)) p[j] = map[0];
    return;
  }
  const float *off = dither ? chargrid_bayer_off + ((unsigned)y & 3u) * 4u : k_no_dither;
  if (keep)
    put_ramp_k(p, v, keep, true, n, map, steps - 1, off, x0);
  else
    put_ramp_k(p, v, NULL, false, n, map, steps - 1, off, x0);
}

// The output rules (spec, "Lua API: shader"): keep; glyph, else level
// through the ramp; fg, else r,g,b through the nearest palette entry, else
// tone, else level, through the colour ramp; bg, else bgtone through the
// colour ramp. A plane with none of these stays as it is. Plane by plane,
// with one call site for each way of writing one.
static void apply(charshader_t *s, chargrid_t *g, int x0, int y, int n) {
  const float *keep = HAS(CS_OUT_KEEP) ? output(s, CS_OUT_KEEP, n) : NULL;
  const float *level = HAS(CS_OUT_LEVEL) ? output(s, CS_OUT_LEVEL, n) : NULL;
  const size_t base = (size_t)y * (size_t)g->cols + (size_t)x0;
  const int rn = g->ramp_n, cn = g->cramp_n;
  for (int plane = CG_PLANE_GLYPH; plane <= CG_PLANE_BG; plane++) {
    uint8_t *p = chargrid_plane(g, plane) + base;
    int index = -1, steps = 0;  // an index output, or a ramp of steps entries
    const float *v = NULL;
    const uint8_t *map = g->cramp;
    bool dither = g->cramp_dither, rgb = false;
    if (plane == CG_PLANE_GLYPH) {
      if (HAS(CS_OUT_GLYPH)) {
        index = CS_OUT_GLYPH;
      } else if (level && rn) {
        v = level, steps = rn, map = g->ramp, dither = g->ramp_dither;
      }
    } else if (plane == CG_PLANE_FG) {
      if (HAS(CS_OUT_FG))
        index = CS_OUT_FG;
      else if (HAS(CS_OUT_R) || HAS(CS_OUT_G) || HAS(CS_OUT_B))
        rgb = true;
      else if (HAS(CS_OUT_TONE) && cn)
        v = output(s, CS_OUT_TONE, n), steps = cn;
      else if (level && cn)
        v = level, steps = cn;
    } else {
      if (HAS(CS_OUT_BG))
        index = CS_OUT_BG;
      else if (HAS(CS_OUT_BGTONE) && cn)
        v = output(s, CS_OUT_BGTONE, n), steps = cn;
    }
    if (index >= 0) {
      put_index(p, output(s, index, n), keep, n);
    } else if (v) {
      put_ramp(p, v, keep, n, map, steps, dither, x0, y);
    } else if (rgb) {
      const float *r = HAS(CS_OUT_R) ? output(s, CS_OUT_R, n) : NULL;
      const float *gr = HAS(CS_OUT_G) ? output(s, CS_OUT_G, n) : NULL;
      const float *b = HAS(CS_OUT_B) ? output(s, CS_OUT_B, n) : NULL;
      for (int j = 0; j < n; j++)
        if (!keep || !(keep[j] >= 0.5f))
          p[j] = chargrid_nearest(g, r ? r[j] : 0.0f, gr ? gr[j] : 0.0f, b ? b[j] : 0.0f);
    }
  }
}

// The rect clipped to g, in strips: the outputs to the grid, or output `out` into values.
static int run(charshader_t *s, chargrid_t *g, int x, int y, int w, int h,
               chargrid_t *const *layers, int nlayers, int out, float *values) {
  const bool to_grid = values == NULL;
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
      if (values) {
        memcpy(values, output(s, out, n), (size_t)n * sizeof(float));
        values += n;
      } else {
        apply(s, g, cx, cy, n);
      }
    }
  if (to_grid && s->out_used) g->dirty = true;
  return (int)((x1 - x0) * (y1 - y0));
}

int charshader_run_values(charshader_t *s, chargrid_t *g, int x, int y, int w, int h,
                          chargrid_t *const *layers, int nlayers, int out, float *values) {
  if (out < 0 || out >= CS_OUT_COUNT || !((s->out_used >> out) & 1u)) return -1;
  return run(s, g, x, y, w, h, layers, nlayers, out, values);
}

int charshader_run(charshader_t *s, chargrid_t *g, int x, int y, int w, int h,
                   chargrid_t *const *layers, int nlayers) {
  return run(s, g, x, y, w, h, layers, nlayers, -1, NULL);
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
