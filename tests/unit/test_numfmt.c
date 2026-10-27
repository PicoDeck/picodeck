// Host unit test for src/os/lua_numfmt.c (Lua's float formatting, which does
// not use the C library): picodeck_numfmt_float and picodeck_lua_sprintf must print
// exactly what glibc prints for every float32.
//
// Deterministic sample (fixed-seed xorshift + hand-picked edge values), every
// conversion %e %E %f %F %g %G %a %A at precisions 0-20 plus none, and flag /
// width combinations.  The exhaustive 70M-value %.7g/%.9g sweep is the
// opt-in `numfmt_sweep` target (tests/unit/numfmt_sweep.c).
//
// Deliberate difference from glibc: NaN prints unsigned ("nan"), whatever
// its sign bit (x86 makes 0/0 negative and glibc shows "-nan"; ARM does not).
#include "check.h"
#include "lua_numfmt.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t s_rng = 88172645463325252ull;
static uint32_t rnd(void) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 7;
  s_rng ^= s_rng << 17;
  return (uint32_t)s_rng;
}

static long s_mismatch;
static long s_glibc_bug;

// glibc bug: with '#', %g/%G drops the trailing zeros when rounding carries
// into a new power of ten ("%#g" of 999999.5 prints "1.e+06"; C requires
// "1.00000e+06", which is what lua_numfmt prints - see test_hash_g).  Match
// that shape exactly: same text once the zeros between '.' and the exponent
// are removed from ours, and glibc's has none.
static bool glibc_hash_g_bug(const char *spec, const char *ours,
                             const char *want) {
  size_t n = strlen(spec);
  char conv = spec[n - 1];
  if (!strchr(spec, '#') || (conv != 'g' && conv != 'G'))
    return false;
  const char *we = strpbrk(want, "eE");
  const char *oe = strpbrk(ours, "eE");
  if (!we || !oe || we == want || we[-1] != '.' || strcmp(we, oe) != 0)
    return false;
  const char *od = strchr(ours, '.');
  if (!od)
    return false;
  for (const char *q = od + 1; q < oe; q++)
    if (*q != '0')
      return false;
  // Leading parts (sign/padding + digits up to '.') agree, ignoring the
  // width padding the dropped zeros freed up.
  const char *wd = we - 1;
  size_t wl = (size_t)(wd - want), ol = (size_t)(od - ours);
  const char *ws = want, *os = ours;
  while (*ws == ' ') { ws++; wl--; }
  while (*os == ' ') { os++; ol--; }
  return wl == ol && strncmp(ws, os, wl) == 0;
}

// Our two entry points against glibc for one spec and value.
static void check_one(const char *spec, float v) {
  char ours[512], lua[512], want[512];
  int ro = picodeck_numfmt_float(ours, sizeof ours, spec, v);
  int rl = picodeck_lua_sprintf(lua, sizeof lua, spec, (double)v);
  // NaN: compare against glibc's rendering of a positive NaN.
  double ref = isnan(v) ? fabs((double)v) : (double)v;
  int rw = snprintf(want, sizeof want, spec, ref);
  bool ok = ro == rw && rl == rw && strcmp(ours, want) == 0 &&
            strcmp(lua, want) == 0;
  s_check_count++;
  if (!ok && glibc_hash_g_bug(spec, ours, want) && strcmp(ours, lua) == 0) {
    s_glibc_bug++;
    return;
  }
  if (!ok) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    if (s_mismatch++ < 20)
      printf("FAIL %-10s %.9g (0x%08x): numfmt [%s] lua [%s] glibc [%s]\n",
             spec, v, bits, ours, lua, want);
    s_check_fails++;
  }
}

static float random_float(int i) {
  uint32_t b = rnd();
  // Half the sample in the common exponent band (roughly 1e-5 .. 1e7).
  if (i & 1)
    b = (b & 0x807FFFFFu) | ((110u + rnd() % 40u) << 23);
  float v;
  memcpy(&v, &b, 4);
  return v;
}

static const float k_special[] = {
    0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 0.5f, 1.5f, 2.5f, -0.5f, 25, 35, 51, 100,
    1e-5f, 1e-4f, 1e6f, 1e7f, 123456.7f, 1.0f / 3, 3.4028235e38f, 1.4e-45f,
    1e-38f, 1.17549435e-38f, 9.5f, 0.95f, 0.05f, 999999.5f, 9999999.0f,
    16777216.0f, 1e10f, 0.000123456f, 12345.678f, 1.9999999f, 0.999999f,
    99.5f, 0.0625f, 1e9f, 4294967296.0f, 0.3f, 2.675f,
    INFINITY, -INFINITY};

static const char k_convs[] = "eEfFgGaA";
static const char *const k_flags[] = {"", "-", "+", " ", "#", "0", "-+",
                                      "+0", "#0", " #", "-#", "+#0", "- "};

static void check_value(float v, bool all_precisions) {
  char spec[32];
  for (const char *c = k_convs; *c; c++) {
    snprintf(spec, sizeof spec, "%%%c", *c);
    check_one(spec, v);
    if (all_precisions) {
      for (int p = 0; p <= 20; p++) {
        snprintf(spec, sizeof spec, "%%.%d%c", p, *c);
        check_one(spec, v);
      }
    } else {
      snprintf(spec, sizeof spec, "%%.%u%c", rnd() % 21u, *c);
      check_one(spec, v);
    }
    const char *fl = k_flags[rnd() % (sizeof k_flags / sizeof k_flags[0])];
    snprintf(spec, sizeof spec, "%%%s%u.%u%c", fl, rnd() % 30u, rnd() % 21u, *c);
    check_one(spec, v);
    fl = k_flags[rnd() % (sizeof k_flags / sizeof k_flags[0])];
    snprintf(spec, sizeof spec, "%%%s%u%c", fl, rnd() % 30u, *c);
    check_one(spec, v);
  }
  check_one("%.14g", v);  // LUAI_NUMFFORMAT-style
  check_one("%.7g", v);   // lua_Number (float) default
}

static void test_specials(void) {
  for (size_t i = 0; i < sizeof k_special / sizeof k_special[0]; i++)
    check_value(k_special[i], true);
}

static void test_sample(void) {
  for (int i = 0; i < 1500; i++) {
    float v = random_float(i);
    if (isnan(v))
      continue;
    check_value(v, (i % 25) == 0);
  }
}

static void test_nan(void) {
  float nans[] = {NAN, -NAN};
  uint32_t payload = 0x7FC12345u;  // quiet NaN with a payload
  float p;
  memcpy(&p, &payload, 4);
  for (int k = 0; k < 3; k++) {
    float v = k < 2 ? nans[k] : p;
    for (const char *c = k_convs; *c; c++) {
      char spec[32];
      for (size_t f = 0; f < sizeof k_flags / sizeof k_flags[0]; f++) {
        snprintf(spec, sizeof spec, "%%%s8.3%c", k_flags[f], *c);
        check_one(spec, v);
      }
      snprintf(spec, sizeof spec, "%%%c", *c);
      check_one(spec, v);
    }
  }
  char b[32];
  picodeck_numfmt_float(b, sizeof b, "%g", -NAN);
  CHECK_STR(b, "nan");
  picodeck_numfmt_float(b, sizeof b, "%F", NAN);
  CHECK_STR(b, "NAN");
  picodeck_numfmt_float(b, sizeof b, "%+E", -NAN);
  CHECK_STR(b, "+NAN");
  picodeck_numfmt_float(b, sizeof b, "%F", -INFINITY);
  CHECK_STR(b, "-INF");
  picodeck_numfmt_float(b, sizeof b, "%05.1f", INFINITY);
  CHECK_STR(b, "  inf");  // '0' does not pad inf/nan
}

static void test_F(void) {
  char b[64];
  picodeck_numfmt_float(b, sizeof b, "%F", 1.5f);
  CHECK_STR(b, "1.500000");
  picodeck_numfmt_float(b, sizeof b, "%.0F", 2.5f);
  CHECK_STR(b, "2");  // half-to-even
  picodeck_numfmt_float(b, sizeof b, "%#.0F", 3.0f);
  CHECK_STR(b, "3.");
  picodeck_numfmt_float(b, sizeof b, "%F", 1e10f);
  CHECK_STR(b, "10000000000.000000");
}

static void test_hash_g(void) {
  char b[64];
  picodeck_numfmt_float(b, sizeof b, "%#g", 999999.5f);
  CHECK_STR(b, "1.00000e+06");  // glibc: "1.e+06" (its bug, see above)
  picodeck_numfmt_float(b, sizeof b, "% #18G", 999999.5f);
  CHECK_STR(b, "       1.00000E+06");
  picodeck_numfmt_float(b, sizeof b, "%#g", 1.0f);
  CHECK_STR(b, "1.00000");
  picodeck_numfmt_float(b, sizeof b, "%#.3g", 9.9996f);
  CHECK_STR(b, "10.0");
}

static void test_hex_ties(void) {
  char b[32];
  picodeck_numfmt_float(b, sizeof b, "%.0a", 1.5f);   // 0x1.8p+0: tie, 1 odd
  CHECK_STR(b, "0x2p+0");
  picodeck_numfmt_float(b, sizeof b, "%.0a", 1.25f);  // below the tie
  CHECK_STR(b, "0x1p+0");
  picodeck_numfmt_float(b, sizeof b, "%#.0A", 0.75f);
  CHECK_STR(b, "0X2.P-1");
  picodeck_numfmt_float(b, sizeof b, "%.1a", 1.03125f);  // 0x1.08: tie, 0 even
  CHECK_STR(b, "0x1.0p+0");
}

static void test_sprintf_passthrough(void) {
  char o[64];
  picodeck_lua_sprintf(o, sizeof o, "x%%%.2fy%%", 1.25);
  CHECK_STR(o, "x%1.25y%");
  picodeck_lua_sprintf(o, sizeof o, "%5.3d", 7);
  CHECK_STR(o, "  007");
  picodeck_lua_sprintf(o, sizeof o, "%s|%c", "ab", 'z');
  CHECK_STR(o, "ab|z");
  picodeck_lua_sprintf(o, sizeof o, "%.14g", 0.1);  // Lua's number format
  CHECK_STR(o, "0.10000000149012");  // the float32 nearest 0.1
}

// The "%d" fast path (every Lua integer-to-text conversion) against glibc:
// edge values, a sample, and snprintf's truncation and return.
static void test_int_d(void) {
  const int edge[] = {0, 1, -1, 9, 10, -10, 99, 100, 123456789, -123456789,
                      2147483647, -2147483647 - 1, 1000000000, -1000000000};
  char o[32], want[32];
  int bad = 0;
  for (int i = 0; i < (int)(sizeof edge / sizeof edge[0]) + 20000; i++) {
    int v = i < (int)(sizeof edge / sizeof edge[0]) ? edge[i] : (int)rnd() >> (rnd() & 31);
    int r = picodeck_lua_sprintf(o, sizeof o, "%d", v);
    int w = snprintf(want, sizeof want, "%d", v);
    if (r != w || strcmp(o, want) != 0) bad++;
  }
  CHECK_EQ_INT(bad, 0);
  for (size_t size = 0; size <= 12; size++) {
    memset(o, 'Z', sizeof o);
    memset(want, 'Z', sizeof want);
    int r = picodeck_lua_sprintf(o, size, "%d", -2147483647 - 1);
    int w = snprintf(want, size, "%d", -2147483647 - 1);
    CHECK_EQ_INT(r, w);
    CHECK(memcmp(o, want, sizeof o) == 0);
  }
}

static void test_truncation(void) {
  char o[8];
  memset(o, 'Z', sizeof o);
  int r = picodeck_numfmt_float(o, 5, "%f", 3.25f);
  CHECK_EQ_INT(r, 8);  // full length, snprintf semantics
  CHECK_STR(o, "3.25");
  CHECK_EQ_INT(o[5], 'Z');  // nothing written past size
  r = picodeck_numfmt_float(o, 0, "%f", 3.25f);
  CHECK_EQ_INT(r, 8);
  CHECK_EQ_INT(o[0], '3');  // size 0 writes nothing
  CHECK_EQ_INT(picodeck_numfmt_float(o, sizeof o, "%q", 1.0f), -1);  // malformed
  CHECK_EQ_INT(picodeck_numfmt_float(o, sizeof o, "f", 1.0f), -1);
  // Precision capped at 150.
  char big[400];
  r = picodeck_numfmt_float(big, sizeof big, "%.200f", 1.0f);
  CHECK_EQ_INT(r, 152);
}

int main(void) {
  test_specials();
  test_sample();
  test_nan();
  test_F();
  test_hash_g();
  test_hex_ties();
  test_sprintf_passthrough();
  test_truncation();
  test_int_d();
  printf("test_numfmt: %ld mismatches vs glibc (+%ld known glibc %%#g bugs)\n",
         s_mismatch, s_glibc_bug);
  return check_report("test_numfmt");
}
