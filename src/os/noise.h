#pragma once
// Lattice hash, value noise and fBm for picocalc.noise and the chargrid
// shader (specs/2026-10-09-chargrid-design.md, "Lua API: noise"). The hash
// is ascii.rest's (imul mixing) plus a seed term, so seed 0 reproduces its
// pieces; its top 24 bits make the float, so it is exact and below 1.
// Integer hashing and float maths without libm: the simulator and the
// device agree bit for bit. Every result is in [0, 1) for any input,
// infinities and NaN included (NaN reads as 0). Header-only so the shader
// VM's strip loops inline it.
#include <stdint.h>
#include <string.h>

#define NOISE_MAX_OCTAVES 8

#define NOISE_K_X 374761393u
#define NOISE_K_Y 668265263u
#define NOISE_K_SEED 1442695041u
#define NOISE_K_MIX 1274126177u
#define NOISE_ONE_BELOW 0.99999994f  // the largest float below 1

// ARMv8 FP (the device's Cortex-M33) does these steps in one instruction
// each: VCVTM floors to an int32 (saturating, NaN 0), a fixed-point VCVT
// scales the hash, VMINNM/VMAXNM clamp. Each gives exactly the portable C
// result (the simulator and the host tests run the C) for every input it
// receives here.
#if defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 8 && defined(__ARM_FP) && (__ARM_FP & 4)
#define NOISE_ARMV8_FP 1
#else
#define NOISE_ARMV8_FP 0
#endif

static inline uint32_t noise_bits(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return u;
}

#if !NOISE_ARMV8_FP
static inline int32_t noise_lattice_c(float v) {
  if (v != v) return 0;
  if (v >= 2147483648.0f) return INT32_MAX;
  if (v < -2147483648.0f) return INT32_MIN;
  const int32_t i = (int32_t)v;  // truncates toward zero
  return (float)i > v ? i - 1 : i;
}
#endif

// floor(v) as an int32, saturating; NaN is 0.
static inline int32_t noise_lattice(float v) {
#if NOISE_ARMV8_FP
  float t;
  __asm__("vcvtm.s32.f32 %0, %1" : "=t"(t) : "t"(v));
  return (int32_t)noise_bits(t);
#else
  return noise_lattice_c(v);
#endif
}

// noise_lattice(v), and *fl = (float) of it.
static inline int32_t noise_floor2(float v, float *fl) {
#if NOISE_ARMV8_FP
  float t;
  __asm__("vcvtm.s32.f32 %0, %2\n\tvcvt.f32.s32 %1, %0" : "=t"(t), "=t"(*fl) : "t"(v));
  return (int32_t)noise_bits(t);
#else
  const int32_t i = noise_lattice_c(v);
  *fl = (float)i;
  return i;
#endif
}

// v / 2^24 for v < 2^24: exact.
static inline float noise_u24(uint32_t v) {
#if NOISE_ARMV8_FP
  float f;
  memcpy(&f, &v, sizeof f);
  __asm__("vcvt.f32.u32 %0, %0, #24" : "+t"(f));
  return f;
#else
  return (float)v * (1.0f / 16777216.0f);
#endif
}

// r < 1 ? r : NOISE_ONE_BELOW, for r not NaN.
static inline float noise_below1(float r) {
#if NOISE_ARMV8_FP
  __asm__("vminnm.f32 %0, %0, %1" : "+t"(r) : "t"(NOISE_ONE_BELOW));
  return r;
#else
  return r < 1.0f ? r : NOISE_ONE_BELOW;
#endif
}

// r > 0 ? noise_below1(r) : 0, for r not NaN (-0 gives +0).
static inline float noise_unit(float r) {
#if NOISE_ARMV8_FP
  __asm__("vmaxnm.f32 %0, %0, %1" : "+t"(r) : "t"(0.0f));
  return noise_below1(r);
#else
  return r > 0.0f ? (r < 1.0f ? r : NOISE_ONE_BELOW) : 0.0f;
#endif
}

// ascii.rest's finaliser, keeping the top 24 bits.
static inline float noise_finish(uint32_t h) {
  h = (h ^ (h >> 13)) * NOISE_K_MIX;
  h ^= h >> 16;
  return noise_u24(h >> 8);
}

static inline float noise_hash(int32_t x, int32_t y, int32_t seed) {
  return noise_finish((uint32_t)x * NOISE_K_X + (uint32_t)y * NOISE_K_Y +
                      (uint32_t)seed * NOISE_K_SEED);
}

static inline uint32_t noise_seed_term(int32_t seed) { return (uint32_t)seed * NOISE_K_SEED; }

// x - floor(x) as value noise uses it. It comes in [0, 1) for coordinates
// below 2^31 in size, except that a tiny negative x rounds it up to exactly
// 1.0f: keep it just below 1, so the blend stays at the next cell's corner.
// Huge, infinite or NaN coordinates give 0.
static inline float noise_frac(float f) {
  if (f >= 0.0f && f < 1.0f) return f;
  return f >= 1.0f && f < 2.0f ? NOISE_ONE_BELOW : 0.0f;
}

// Value noise in three pieces, which noise_value_at puts together and the
// shader VM's strip loops reuse while a lattice cell or a coordinate repeats
// (the same functions, so the same results). One coordinate: its lattice
// index, and *w its smoothstep weight.
static inline __attribute__((always_inline)) int32_t noise_axis(float x, float *w) {
  float fl;
  const int32_t i = noise_floor2(x, &fl);
  float f = x - fl;
  // One integer test for the common case, [+0, 1); -0.0f takes the slow
  // path, which keeps it.
  if (__builtin_expect(noise_bits(f) >= 0x3F800000u, 0)) f = noise_frac(f);
  *w = f * f * (3.0f - 2.0f * f);
  return i;
}

// A lattice cell's corner hashes a (xi, yi), b (xi + 1, yi), c (xi, yi + 1)
// and d (xi + 1, yi + 1), as the blend uses them. The seed term is
// premultiplied: the corners share the lattice sum, and only the finaliser
// runs per corner.
typedef struct {
  float a, ba, ca, abcd;  // a, b - a, c - a, a - b - c + d
} noise_corners_t;

static inline __attribute__((always_inline)) noise_corners_t noise_corners(int32_t xi, int32_t yi,
                                                                           uint32_t seed_term) {
  const uint32_t h = (uint32_t)xi * NOISE_K_X + (uint32_t)yi * NOISE_K_Y + seed_term;
  const float a = noise_finish(h), b = noise_finish(h + NOISE_K_X);
  const float c = noise_finish(h + NOISE_K_Y), d = noise_finish(h + NOISE_K_X + NOISE_K_Y);
  const noise_corners_t k = {a, b - a, c - a, a - b - c + d};
  return k;
}

// a + (b - a) u + (c - a) v + (a - b - c + d) u v, clamped to [0, 1).
static inline __attribute__((always_inline)) float noise_blend(noise_corners_t k, float u,
                                                               float v) {
  return noise_unit(k.a + k.ba * u + k.ca * v + k.abcd * u * v);
}

// Value noise with the seed term premultiplied. Always inlined, so the
// shader VM's loops keep the constants in registers.
static inline __attribute__((always_inline)) float noise_value_at(float x, float y,
                                                                  uint32_t seed_term) {
  float u, v;
  const int32_t xi = noise_axis(x, &u), yi = noise_axis(y, &v);
  return noise_blend(noise_corners(xi, yi, seed_term), u, v);
}

static inline float noise_value(float x, float y, int32_t seed) {
  return noise_value_at(x, y, noise_seed_term(seed));
}

// fBm in pieces, so the shader VM hoists the per-instruction ones: the
// octave count clamped to 1-NOISE_MAX_OCTAVES, the sum of the gains, and
// one point's octaves divided by that sum.
static inline int noise_fbm_octaves(int octaves) {
  return octaves < 1 ? 1 : octaves > NOISE_MAX_OCTAVES ? NOISE_MAX_OCTAVES : octaves;
}
static inline float noise_fbm_norm(int octaves) {
  float n = 0.0f, amp = 0.5f;
  for (int i = 0; i < octaves; i++) {
    n += amp;
    amp *= 0.5f;
  }
  return n;
}
static inline __attribute__((always_inline)) float noise_fbm_at(float x, float y, int octaves, uint32_t seed_term, float norm) {
  float s = 0.0f, amp = 0.5f, f = 1.0f;
  for (int i = 0; i < octaves; i++) {
    s += amp * noise_value_at(x * f, y * f, seed_term);
    amp *= 0.5f;
    f *= 2.0f;
  }
  return noise_below1(s / norm);
}

// octaves clamps to 1-NOISE_MAX_OCTAVES.
static inline float noise_fbm(float x, float y, int octaves, int32_t seed) {
  octaves = noise_fbm_octaves(octaves);
  return noise_fbm_at(x, y, octaves, noise_seed_term(seed), noise_fbm_norm(octaves));
}
