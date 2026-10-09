#pragma once
// Single-precision maths for the chargrid shader (charshader_*.c) without
// libm: the same operations in the same order on the simulator and the
// device, so shader output matches bit for bit (sqrtf, the one libm call,
// is IEEE-exact everywhere). Built with -ffp-contract=off. Accuracy against
// libm (tests/unit/test_charshader_math.c):
// - sin, cos: 4e-6 absolute for |x| <= 10, 8e-6 to 100; NaN and inf give 0;
// - exp: 1e-6 relative for |x| <= 10, 5e-6 over its range;
// - log: 1e-7 absolute on [0.5, 2], 1e-5 everywhere;
// - atan2: 1.3e-5; pow: exp(b * log(a)), 1e-5 relative in common ranges.
#include <math.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t cs_bits(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return u;
}
static inline float cs_from_bits(uint32_t u) {
  float f;
  memcpy(&f, &u, sizeof f);
  return f;
}
#define CS_INF cs_from_bits(0x7F800000u)
#define CS_NAN cs_from_bits(0x7FC00000u)

// floor for every float: magnitudes from 2^23 up are integers already.
static inline float cs_floor(float x) {
  if (!(x > -8388608.0f && x < 8388608.0f)) return x;  // NaN too
  const float t = (float)(int32_t)x;
  return t > x ? t - 1.0f : t;
}
static inline float cs_ceil(float x) { return -cs_floor(-x); }
static inline float cs_fract(float x) { return x - cs_floor(x); }
static inline float cs_abs(float x) { return cs_from_bits(cs_bits(x) & 0x7FFFFFFFu); }
static inline float cs_sqrt(float x) { return x >= 0.0f ? sqrtf(x) : CS_NAN; }

// sin(2 pi t): t reduced to [-0.25, 0.25] by symmetry, then the Taylor
// series to t^9. NaN, the infinities and |t| >= 2^23 give 0.
static inline float cs_sin_turns(float t) {
  t -= cs_floor(t + 0.5f);  // [-0.5, 0.5]
  if (!(t >= -0.5f && t <= 0.5f)) t = 0.0f;
  if (t > 0.25f)
    t = 0.5f - t;  // sin(pi - a) = sin(a)
  else if (t < -0.25f)
    t = -0.5f - t;
  const float t2 = t * t;
  return t * (6.28318531f +
              t2 * (-41.3417022f + t2 * (81.6052493f + t2 * (-76.7058598f + t2 * 42.0586939f))));
}
#define CS_INV_TAU 0.159154943f  // 1 / (2 pi)
static inline float cs_sin(float x) { return cs_sin_turns(x * CS_INV_TAU); }
static inline float cs_cos(float x) { return cs_sin_turns(x * CS_INV_TAU + 0.25f); }

// e^x = 2^n * 2^f with f in [-0.5, 0.5]; 2^f is the Taylor series of
// e^(f ln 2) to f^6.
static inline float cs_exp(float x) {
  if (x != x) return x;
  if (x > 88.72f) return CS_INF;
  if (x < -86.9f) return 0.0f;  // keeps 2^n normal (n >= -125); true values there are < 2e-38
  const float z = x * 1.44269504f;     // log2(e)
  const float n = cs_floor(z + 0.5f);  // -125..128
  const float f = z - n;
  const float p =
      1.0f + f * (0.693147181f +
                  f * (0.240226507f +
                       f * (0.0555041087f +
                            f * (0.00961812911f + f * (0.00133335581f + f * 0.000154035304f)))));
  // 2^(n - 1) * 2: n reaches 128 just below the overflow threshold.
  return p * cs_from_bits((uint32_t)((int32_t)n + 126) << 23) * 2.0f;
}

// log x = e ln 2 + 2 atanh((m - 1) / (m + 1)) with m in [0.707, 1.414]; the
// atanh series to s^9.
static inline float cs_log(float x) {
  if (!(x > 0.0f)) return x == 0.0f ? -CS_INF : CS_NAN;  // NaN and negatives give NaN
  if (x == CS_INF) return x;
  uint32_t u = cs_bits(x);
  int32_t e = -127;
  if (u < 0x00800000u) {  // subnormal: scale into the normal range
    u = cs_bits(x * 8388608.0f);
    e -= 23;
  }
  e += (int32_t)(u >> 23);
  float m = cs_from_bits((u & 0x007FFFFFu) | 0x3F800000u);  // [1, 2)
  if (m > 1.41421356f) {
    m *= 0.5f;
    e += 1;
  }
  const float s = (m - 1.0f) / (m + 1.0f), s2 = s * s;  // |s| <= 0.1716
  const float l = 2.0f * s *
                  (1.0f + s2 * (0.333333333f + s2 * (0.2f + s2 * (0.142857143f + s2 * 0.111111111f))));
  return (float)e * 0.693147181f + l;
}

// Abramowitz & Stegun 4.4.47 on [0, 1] (|error| <= 1e-5), then the octant.
static inline float cs_atan2(float y, float x) {
  if (x != x || y != y) return CS_NAN;
  const float ax = cs_abs(x), ay = cs_abs(y);
  const float hi = ax > ay ? ax : ay, lo = ax > ay ? ay : ax;
  if (hi == 0.0f) return 0.0f;
  float a = lo / hi;
  if (a != a) a = 1.0f;  // both infinite
  const float a2 = a * a;
  float r = a * (0.9998660f +
                 a2 * (-0.3302995f + a2 * (0.1801410f + a2 * (-0.0851330f + a2 * 0.0208351f))));
  if (ay > ax) r = 1.57079633f - r;
  if (x < 0.0f) r = 3.14159265f - r;
  return y < 0.0f ? -r : r;
}

static inline float cs_pow(float a, float b) {
  if (b == 0.0f) return 1.0f;
  if (a != a || b != b) return CS_NAN;
  if (a > 0.0f) return cs_exp(b * cs_log(a));
  if (a == 0.0f) return b > 0.0f ? 0.0f : CS_INF;
  if (cs_floor(b) != b) return CS_NAN;  // a negative base needs an integer power
  const float r = cs_exp(b * cs_log(-a));
  return cs_fract(b * 0.5f) == 0.0f ? r : -r;  // even or odd power
}
