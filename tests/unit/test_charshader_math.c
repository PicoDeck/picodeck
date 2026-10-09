// Host unit tests for src/os/charshader_math.h: the shader's polynomial
// maths against libm in double, plus the special values.
#include "charshader_math.h"
#include "check.h"

#include <math.h>

static double worst_abs(float (*f)(float), double (*ref)(double), double lo, double hi, int n) {
  double w = 0.0;
  for (int i = 0; i <= n; i++) {
    const float x = (float)(lo + (hi - lo) * i / n);
    const double e = fabs((double)f(x) - ref((double)x));
    if (e > w) w = e;
  }
  return w;
}

static void test_floor_family(void) {
  CHECK(cs_floor(2.5f) == 2.0f);
  CHECK(cs_floor(-2.5f) == -3.0f);
  CHECK(cs_floor(-2.0f) == -2.0f);
  CHECK(cs_floor(-0.25f) == -1.0f);
  CHECK(cs_floor(8388607.5f) == 8388607.0f);
  CHECK(cs_floor(1e30f) == 1e30f);
  CHECK(cs_floor(-1e30f) == -1e30f);
  CHECK(isnan(cs_floor(NAN)));
  CHECK(cs_ceil(2.25f) == 3.0f);
  CHECK(cs_ceil(-2.25f) == -2.0f);
  CHECK(cs_fract(-0.25f) == 0.75f);
  CHECK(cs_fract(3.5f) == 0.5f);
  CHECK(cs_abs(-3.0f) == 3.0f);
  CHECK(cs_sqrt(2.0f) == sqrtf(2.0f));
  CHECK(cs_sqrt(0.0f) == 0.0f);
  CHECK(isnan(cs_sqrt(-1.0f)));
  int bad = 0;
  for (int i = -200000; i <= 200000; i++) {
    const float x = (float)i * 0.0137f;
    if (cs_floor(x) != floorf(x)) bad++;
  }
  CHECK_EQ_INT(bad, 0);
}

static void test_sin_cos(void) {
  CHECK(worst_abs(cs_sin, sin, -10.0, 10.0, 400000) < 4e-6);
  CHECK(worst_abs(cs_cos, cos, -10.0, 10.0, 400000) < 4e-6);
  CHECK(worst_abs(cs_sin, sin, -100.0, 100.0, 400000) < 8e-6);
  CHECK(cs_sin(0.0f) == 0.0f);
  // NaN and the infinities read as 0; huge arguments stay bounded.
  CHECK(cs_sin(NAN) == 0.0f);
  CHECK(cs_sin(INFINITY) == 0.0f);
  CHECK(cs_cos(-INFINITY) == 0.0f);
  CHECK(fabsf(cs_sin(1e30f)) <= 1.0f);
}

static void test_exp(void) {
  double w = 0.0, w10 = 0.0;
  for (int i = 0; i <= 400000; i++) {
    const float x = (float)(-86.9 + 175.6 * i / 400000);
    const double want = exp((double)x), e = fabs((double)cs_exp(x) - want) / want;
    if (e > w) w = e;
    if (fabsf(x) <= 10.0f && e > w10) w10 = e;
  }
  CHECK(w10 < 1e-6);
  CHECK(w < 5e-6);
  CHECK(cs_exp(0.0f) == 1.0f);
  CHECK(isinf(cs_exp(89.0f)));
  CHECK(cs_exp(-87.0f) == 0.0f);
  CHECK(isnan(cs_exp(NAN)));
}

static void test_log(void) {
  CHECK(worst_abs(cs_log, log, 0.5, 2.0, 200000) < 1e-7);
  double w = 0.0;
  for (int i = 0; i <= 200000; i++) {  // 1e-38 .. 1e38, evenly in the exponent
    const float x = (float)pow(10.0, -38.0 + 76.0 * i / 200000);
    const double e = fabs((double)cs_log(x) - log((double)x));
    if (e > w) w = e;
  }
  CHECK(w < 1e-5);
  CHECK(cs_log(1.0f) == 0.0f);
  CHECK(isinf(cs_log(0.0f)) && cs_log(0.0f) < 0.0f);
  CHECK(isnan(cs_log(-1.0f)));
  CHECK(isnan(cs_log(NAN)));
  CHECK(isinf(cs_log(INFINITY)));
  CHECK(fabs((double)cs_log(1e-40f) - log((double)1e-40f)) < 1e-5);  // subnormal
}

static void test_atan2(void) {
  double w = 0.0;
  for (int i = -500; i <= 500; i++)
    for (int j = -500; j <= 500; j++) {
      const float y = (float)i * 0.01f, x = (float)j * 0.01f;
      if (x == 0.0f && y == 0.0f) continue;
      const double e = fabs((double)cs_atan2(y, x) - atan2((double)y, (double)x));
      if (e > w) w = e;
    }
  CHECK(w < 1.3e-5);
  CHECK(cs_atan2(0.0f, 0.0f) == 0.0f);
  CHECK(fabs((double)cs_atan2(1.0f, 0.0f) - M_PI / 2) < 1e-6);
  CHECK(fabs((double)cs_atan2(0.0f, -1.0f) - M_PI) < 1e-6);
  CHECK(fabs((double)cs_atan2(INFINITY, INFINITY) - M_PI / 4) < 2e-5);
  CHECK(isnan(cs_atan2(NAN, 1.0f)));
}

static void test_pow(void) {
  double w = 0.0;
  for (int i = 0; i <= 400; i++)
    for (int j = 0; j <= 80; j++) {
      const float a = (float)pow(10.0, -2.0 + 4.0 * i / 400), b = (float)(-4.0 + 0.1 * j);
      const double want = pow((double)a, (double)b), e = fabs((double)cs_pow(a, b) - want) / want;
      if (e > w) w = e;
    }
  CHECK(w < 1e-5);
  CHECK(fabsf(cs_pow(-2.0f, 3.0f) + 8.0f) < 1e-4f);
  CHECK(fabsf(cs_pow(-2.0f, 2.0f) - 4.0f) < 1e-4f);
  CHECK(isnan(cs_pow(-2.0f, 0.5f)));
  CHECK(cs_pow(0.0f, 2.0f) == 0.0f);
  CHECK(isinf(cs_pow(0.0f, -1.0f)));
  CHECK(cs_pow(NAN, 0.0f) == 1.0f);
  CHECK(isnan(cs_pow(NAN, 1.0f)));
}

int main(void) {
  test_floor_family();
  test_sin_cos();
  test_exp();
  test_log();
  test_atan2();
  test_pow();
  return check_report("test_charshader_math");
}
