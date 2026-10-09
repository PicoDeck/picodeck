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

#define NOISE_MAX_OCTAVES 8

#define NOISE_K_X 374761393u
#define NOISE_K_Y 668265263u
#define NOISE_K_SEED 1442695041u
#define NOISE_K_MIX 1274126177u
#define NOISE_ONE_BELOW 0.99999994f  // the largest float below 1

// floor(v) as an int32, saturating; NaN is 0.
static inline int32_t noise_lattice(float v) {
  if (v != v) return 0;
  if (v >= 2147483648.0f) return INT32_MAX;
  if (v < -2147483648.0f) return INT32_MIN;
  const int32_t i = (int32_t)v;  // truncates toward zero
  return (float)i > v ? i - 1 : i;
}

// ascii.rest's finaliser, keeping the top 24 bits.
static inline float noise_finish(uint32_t h) {
  h = (h ^ (h >> 13)) * NOISE_K_MIX;
  h ^= h >> 16;
  return (float)(h >> 8) * (1.0f / 16777216.0f);
}

static inline float noise_hash(int32_t x, int32_t y, int32_t seed) {
  return noise_finish((uint32_t)x * NOISE_K_X + (uint32_t)y * NOISE_K_Y +
                      (uint32_t)seed * NOISE_K_SEED);
}

// Value noise with the seed term premultiplied: the four corners share the
// lattice sum, and only the finaliser runs per corner.
static inline float noise_value_at(float x, float y, uint32_t seed_term) {
  const int32_t xi = noise_lattice(x), yi = noise_lattice(y);
  float fx = x - (float)xi, fy = y - (float)yi;
  // A tiny negative x rounds x - floor(x) up to exactly 1.0f: keep it just
  // below 1 so the blend stays at the next cell's corner. Huge, infinite or
  // NaN coordinates give 0.
  if (!(fx >= 0.0f && fx < 1.0f)) fx = (fx >= 1.0f && fx < 2.0f) ? NOISE_ONE_BELOW : 0.0f;
  if (!(fy >= 0.0f && fy < 1.0f)) fy = (fy >= 1.0f && fy < 2.0f) ? NOISE_ONE_BELOW : 0.0f;
  const float u = fx * fx * (3.0f - 2.0f * fx), v = fy * fy * (3.0f - 2.0f * fy);
  const uint32_t h = (uint32_t)xi * NOISE_K_X + (uint32_t)yi * NOISE_K_Y + seed_term;
  const float a = noise_finish(h), b = noise_finish(h + NOISE_K_X);
  const float c = noise_finish(h + NOISE_K_Y), d = noise_finish(h + NOISE_K_X + NOISE_K_Y);
  const float r = a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
  return r > 0.0f ? (r < 1.0f ? r : NOISE_ONE_BELOW) : 0.0f;
}

static inline float noise_value(float x, float y, int32_t seed) {
  return noise_value_at(x, y, (uint32_t)seed * NOISE_K_SEED);
}

// octaves clamps to 1-NOISE_MAX_OCTAVES.
static inline float noise_fbm(float x, float y, int octaves, int32_t seed) {
  if (octaves < 1) octaves = 1;
  if (octaves > NOISE_MAX_OCTAVES) octaves = NOISE_MAX_OCTAVES;
  const uint32_t st = (uint32_t)seed * NOISE_K_SEED;
  float s = 0.0f, n = 0.0f, amp = 0.5f, f = 1.0f;
  for (int i = 0; i < octaves; i++) {
    s += amp * noise_value_at(x * f, y * f, st);
    n += amp;
    amp *= 0.5f;
    f *= 2.0f;
  }
  const float r = s / n;
  return r < 1.0f ? r : NOISE_ONE_BELOW;
}
