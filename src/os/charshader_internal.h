#pragma once
// Shared by the chargrid shader's compiler (charshader_compile.c) and VM
// (charshader_vm.c): limits, opcodes, the bytecode, the compiled block and
// the per-element operations. The constant folder evaluates through
// cs_eval, which calls the same element functions as the VM's loops, so
// folding never changes a result.
#include "charshader.h"
#include "charshader_math.h"
#include "noise.h"

#ifndef CG_MALLOC
#include "umm_malloc.h"
#define CG_MALLOC umm_malloc
#define CG_FREE umm_free
#else
#include <stdlib.h>  // host builds define CG_MALLOC=malloc
#endif

#define CS_MAX_INSTR 255
#define CS_MAX_REGS 32
#define CS_MAX_UNIFORMS 16
#define CS_MAX_ARRAYS 8
#define CS_MAX_NAME 31
#define CS_MAX_SCALARS 255
#ifndef CS_STRIP
#define CS_STRIP 32  // cells per strip (Task 9b: 32 beats 16 on the device with compact loops)
#endif

// Scalar slots: cols, rows, the uniforms, then the constants.
#define CS_SC_COLS 0
#define CS_SC_ROWS 1
#define CS_SC_UNIFORM0 2
#define CS_SC_CONST0 (CS_SC_UNIFORM0 + CS_MAX_UNIFORMS)

// Grouped by operand count, in this order: charshader_vm.c's nargs_of and
// its folding test (op >= CS_OP_SIN) read the count from the group
// boundaries, which _Static_asserts there pin. A new opcode goes inside
// its group.
enum {
  // Loads: no operands.
  CS_OP_X, CS_OP_Y, CS_OP_U, CS_OP_V,
  CS_OP_PLANE,  // aux: CG_PLANE_*
  CS_OP_LAYER,  // aux: layer * 4 + plane
  // One operand.
  CS_OP_ARRAY,  // aux: the array slot; a[0]: the index
  CS_OP_SIN, CS_OP_COS, CS_OP_ABS, CS_OP_FLOOR, CS_OP_CEIL, CS_OP_FRACT, CS_OP_SQRT,
  CS_OP_EXP, CS_OP_LOG,
  // Two.
  CS_OP_ADD, CS_OP_SUB, CS_OP_MUL, CS_OP_DIV, CS_OP_MOD, CS_OP_POW, CS_OP_MIN, CS_OP_MAX,
  CS_OP_LT, CS_OP_LE, CS_OP_EQ, CS_OP_NE, CS_OP_AND, CS_OP_OR,
  CS_OP_STEP,  // step(edge, x)
  CS_OP_ATAN2,
  // Three.
  CS_OP_MAD,  // a * b + c
  CS_OP_CLAMP, CS_OP_MIX, CS_OP_SMOOTH, CS_OP_SELECT,
  CS_OP_HASH, CS_OP_NOISE,  // (x, y, seed)
  CS_OP_FBM,                // (x, y, seed); aux: octaves
  // Four.
  CS_OP_DIST,
  CS_OP_COUNT
};

// d = op(a[0], a[1], ...): bit i of scal set means a[i] is a scalar slot,
// clear means a register.
typedef struct {
  uint8_t op, d, aux, scal;
  uint8_t a[4];
} cs_ins_t;

struct charshader {
  int ninstr, nregs, nscalars, nuniforms, narrays, nlayers;
  uint16_t out_used;              // bit per CS_OUT_*: the program assigns it
  uint16_t out_scal;              // bit per CS_OUT_*: its value is a scalar slot
  uint8_t out_ref[CS_OUT_COUNT];  // the output's register or scalar slot
  uint16_t array_len[CS_MAX_ARRAYS];
  char uniform_names[CS_MAX_UNIFORMS][CS_MAX_NAME + 1];
  char array_names[CS_MAX_ARRAYS][CS_MAX_NAME + 1];
  float sc[CS_MAX_SCALARS];  // cols, rows, the uniforms, the constants
  float *regs;               // nregs * CS_STRIP
  float *scratch;            // CS_OUT_COUNT * CS_STRIP: scalars broadcast to a strip
  float *arrays;             // narrays * CS_MAX_ARRAY_LEN
  cs_ins_t *ins;             // ninstr
};

// ── per-element operations: the VM's loops and the folder share these ──────
// Truth is "not 0" (and, or, not, select); comparisons give 0 or 1.
// CS_LANE_FN marks the functions the VM's LANES_* macros apply
// (charshader_vm.c): always inlined, because on the device those macros pin
// a lane's operands and results to s0-s15, which any call clobbers.
#define CS_LANE_FN static inline __attribute__((always_inline))
CS_LANE_FN float cs_add(float a, float b) { return a + b; }
CS_LANE_FN float cs_sub(float a, float b) { return a - b; }
CS_LANE_FN float cs_mul(float a, float b) { return a * b; }
static inline float cs_div(float a, float b) { return a / b; }
static inline float cs_mod(float a, float b) { return a - cs_floor(a / b) * b; }
CS_LANE_FN float cs_min(float a, float b) { return a < b ? a : b; }
CS_LANE_FN float cs_max(float a, float b) { return a > b ? a : b; }
static inline float cs_lt(float a, float b) { return a < b ? 1.0f : 0.0f; }
static inline float cs_le(float a, float b) { return a <= b ? 1.0f : 0.0f; }
static inline float cs_eq(float a, float b) { return a == b ? 1.0f : 0.0f; }
static inline float cs_ne(float a, float b) { return a != b ? 1.0f : 0.0f; }
static inline float cs_and(float a, float b) { return a != 0.0f && b != 0.0f ? 1.0f : 0.0f; }
static inline float cs_or(float a, float b) { return a != 0.0f || b != 0.0f ? 1.0f : 0.0f; }
static inline float cs_step(float e, float x) { return x >= e ? 1.0f : 0.0f; }
CS_LANE_FN float cs_mad(float a, float b, float c) {
  const float t = a * b;  // two roundings, as a separate multiply and add
  return t + c;
}
CS_LANE_FN float cs_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
CS_LANE_FN float cs_mix(float a, float b, float t) { return a + (b - a) * t; }
// smooth(e0, e1, x) is cs_smooth_k((x - e0) * (1 / (e1 - e0))): the VM
// computes the reciprocal once per instruction when e0 and e1 are scalars.
CS_LANE_FN float cs_smooth_k(float k) {
  k = k > 0.0f ? (k < 1.0f ? k : 1.0f) : 0.0f;  // NaN is 0
  return k * k * (3.0f - 2.0f * k);
}
static inline float cs_smooth(float e0, float e1, float x) {
  return cs_smooth_k((x - e0) * (1.0f / (e1 - e0)));
}
static inline float cs_select(float c, float a, float b) { return c != 0.0f ? a : b; }
static inline float cs_dist(float x0, float y0, float x1, float y1) {
  const float dx = x0 - x1, dy = y0 - y1;
  return cs_sqrt(dx * dx + dy * dy);
}
static inline float cs_hash3(float x, float y, float seed) {
  return noise_hash(noise_lattice(x), noise_lattice(y), noise_lattice(seed));
}
static inline float cs_noise3(float x, float y, float seed) {
  return noise_value(x, y, noise_lattice(seed));
}
static inline float cs_fbm3(float x, float y, float seed, int octaves) {
  return noise_fbm(x, y, octaves, noise_lattice(seed));
}

// One operation on scalars: the constant folder. Loads and CS_OP_ARRAY are
// never folded.
static inline float cs_eval(int op, int aux, float a, float b, float c, float d) {
  switch (op) {
    case CS_OP_SIN: return cs_sin(a);
    case CS_OP_COS: return cs_cos(a);
    case CS_OP_ABS: return cs_abs(a);
    case CS_OP_FLOOR: return cs_floor(a);
    case CS_OP_CEIL: return cs_ceil(a);
    case CS_OP_FRACT: return cs_fract(a);
    case CS_OP_SQRT: return cs_sqrt(a);
    case CS_OP_EXP: return cs_exp(a);
    case CS_OP_LOG: return cs_log(a);
    case CS_OP_ADD: return cs_add(a, b);
    case CS_OP_SUB: return cs_sub(a, b);
    case CS_OP_MUL: return cs_mul(a, b);
    case CS_OP_DIV: return cs_div(a, b);
    case CS_OP_MOD: return cs_mod(a, b);
    case CS_OP_POW: return cs_pow(a, b);
    case CS_OP_MIN: return cs_min(a, b);
    case CS_OP_MAX: return cs_max(a, b);
    case CS_OP_LT: return cs_lt(a, b);
    case CS_OP_LE: return cs_le(a, b);
    case CS_OP_EQ: return cs_eq(a, b);
    case CS_OP_NE: return cs_ne(a, b);
    case CS_OP_AND: return cs_and(a, b);
    case CS_OP_OR: return cs_or(a, b);
    case CS_OP_STEP: return cs_step(a, b);
    case CS_OP_ATAN2: return cs_atan2(a, b);
    case CS_OP_MAD: return cs_mad(a, b, c);
    case CS_OP_CLAMP: return cs_clamp(a, b, c);
    case CS_OP_MIX: return cs_mix(a, b, c);
    case CS_OP_SMOOTH: return cs_smooth(a, b, c);
    case CS_OP_SELECT: return cs_select(a, b, c);
    case CS_OP_HASH: return cs_hash3(a, b, c);
    case CS_OP_NOISE: return cs_noise3(a, b, c);
    case CS_OP_FBM: return cs_fbm3(a, b, c, aux);
    case CS_OP_DIST: return cs_dist(a, b, c, d);
    default: return 0.0f;
  }
}
