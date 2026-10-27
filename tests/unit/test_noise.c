// Host unit tests for src/os/noise.h: golden values, ascii.rest parity,
// ranges for any input, and seed independence.
#include "check.h"
#include "noise.h"

#include <math.h>
#include <stdbool.h>

// ascii.rest's hash in double, as its JS computes it (seed 0).
static double js_hash(int32_t x, int32_t y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (double)h / 4294967296.0;
}

static void test_lattice(void) {
  CHECK_EQ_INT(noise_lattice(0.0f), 0);
  CHECK_EQ_INT(noise_lattice(2.0f), 2);
  CHECK_EQ_INT(noise_lattice(2.75f), 2);
  CHECK_EQ_INT(noise_lattice(-0.5f), -1);
  CHECK_EQ_INT(noise_lattice(-2.0f), -2);
  CHECK_EQ_INT(noise_lattice(2147483520.0f), 2147483520);
  CHECK_EQ_INT(noise_lattice(3e9f), INT32_MAX);
  CHECK_EQ_INT(noise_lattice(-2147483648.0f), INT32_MIN);
  CHECK_EQ_INT(noise_lattice(-3e9f), INT32_MIN);
  CHECK_EQ_INT(noise_lattice(NAN), 0);
  CHECK_EQ_INT(noise_lattice(INFINITY), INT32_MAX);
  CHECK_EQ_INT(noise_lattice(-INFINITY), INT32_MIN);
}

static void test_hash_golden(void) {
  CHECK(noise_hash(0, 0, 0) == 0.0f);
  CHECK(noise_hash(1, 0, 0) == 0x1.0428e2p-1f);
  CHECK(noise_hash(0, 1, 0) == 0x1.895b48p-1f);
  CHECK(noise_hash(-1, -1, 0) == 0x1.5ca5d8p-3f);
  CHECK(noise_hash(123, 456, 0) == 0x1.d0e9f4p-1f);
  CHECK(noise_hash(7, 7, 3) == 0x1.0b5184p-2f);
  CHECK(noise_hash(INT32_MIN, INT32_MAX, -5) == 0x1.109b1p-3f);
}

static void test_hash_matches_ascii_rest(void) {
  int bad = 0;
  for (int32_t y = -40; y < 40; y++)
    for (int32_t x = -40; x < 40; x++) {
      const double js = js_hash(x, y), ours = (double)noise_hash(x, y, 0);
      if (!(ours <= js && js - ours < 1.0 / 16777216.0)) bad++;
    }
  CHECK_EQ_INT(bad, 0);
}

static void test_value_and_fbm_golden(void) {
  CHECK(fabsf(noise_value(0.5f, 0.5f, 0) - 0.48506250977516174f) < 1e-6f);
  CHECK(fabsf(noise_value(1.25f, -3.75f, 0) - 0.08276420831680298f) < 1e-6f);
  CHECK(fabsf(noise_value(10.1f, 20.2f, 0) - 0.19643111526966095f) < 1e-6f);
  CHECK(fabsf(noise_fbm(0.3f, 0.7f, 3, 0) - 0.6193975806236267f) < 1e-6f);
  CHECK(fabsf(noise_fbm(5.5f, -2.25f, 8, 0) - 0.27740350365638733f) < 1e-6f);
  // At lattice points value noise is the hash; one octave of fbm is value noise.
  CHECK(noise_value(3.0f, -4.0f, 9) == noise_hash(3, -4, 9));
  CHECK(noise_fbm(1.7f, 2.3f, 1, 5) == noise_value(1.7f, 2.3f, 5));
  // A tiny negative coordinate stays in cell 0's blend (x - floor(x) rounds to 1.0f).
  CHECK(fabsf(noise_value(-1e-9f, 0.0f, 0) - noise_value(0.0f, 0.0f, 0)) < 1e-6f);
  CHECK(fabsf(noise_value(0.0f, -1e-9f, 0) - noise_value(0.0f, 0.0f, 0)) < 1e-6f);
  // Octaves clamp to 1-8.
  CHECK(noise_fbm(1.7f, 2.3f, 0, 5) == noise_fbm(1.7f, 2.3f, 1, 5));
  CHECK(noise_fbm(1.7f, 2.3f, 99, 5) == noise_fbm(1.7f, 2.3f, 8, 5));
}

static bool in01(float v) { return v >= 0.0f && v < 1.0f; }

static void test_noise_extremes(void) {
  static const float k[] = {0.0f, -0.0f, 1e-30f, 0.5f, -0.5f, 16777217.0f, 2147483520.0f,
                            -2147483648.0f, 3e9f, -3e9f, 3.4e38f, -3.4e38f, INFINITY, -INFINITY, NAN};
  const int n = (int)(sizeof k / sizeof k[0]);
  int bad = 0;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++) {
      if (!in01(noise_value(k[i], k[j], 0))) bad++;
      for (int o = 1; o <= NOISE_MAX_OCTAVES; o++)
        if (!in01(noise_fbm(k[i], k[j], o, -7))) bad++;
      if (!in01(noise_hash(noise_lattice(k[i]), noise_lattice(k[j]), 3))) bad++;
    }
  CHECK_EQ_INT(bad, 0);
  uint32_t s = 1;  // and a random sweep
  for (int i = 0; i < 20000; i++) {
    s = s * 1103515245u + 12345u;
    const float x = (float)(int32_t)s * 1e-3f;
    s = s * 1103515245u + 12345u;
    const float y = (float)(int32_t)s * 1e-3f;
    if (!in01(noise_value(x, y, (int32_t)s)) || !in01(noise_fbm(x, y, 1 + (int)(s % 8), 0))) bad++;
  }
  CHECK_EQ_INT(bad, 0);
}

static void test_seed_independence(void) {
  double diff = 0.0;
  for (int i = 0; i < 4000; i++) {
    const float x = (float)i * 0.37f, y = (float)i * 0.11f;
    diff += fabs((double)noise_value(x, y, 1) - (double)noise_value(x, y, 2));
  }
  diff /= 4000.0;
  CHECK(diff > 0.15 && diff < 0.5);  // independent uniforms: E|a - b| = 1/3
}

int main(void) {
  test_lattice();
  test_hash_golden();
  test_hash_matches_ascii_rest();
  test_value_and_fbm_golden();
  test_noise_extremes();
  test_seed_independence();
  return check_report("test_noise");
}
