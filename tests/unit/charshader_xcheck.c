// The chargrid shader's device-only paths, checked against the host's C
// (ctest test_charshader_m33; tests/unit/CMakeLists.txt). On the device the
// maths and noise floor with ARMv8 instructions (charshader_math.h,
// noise.h) and the VM's strip loops move four lanes at a time with
// VLDM/VSTM (charshader_vm.c); the simulator and the host run portable C.
// This program is built for the host and for the Cortex-M33 with the
// firmware's flags (at -O2, the shader's flag, and at -Os, the firmware's
// default), and the M33 builds run under qemu-arm. Each prints a hash per
// maths function, per shader operation (every operand mix, three program
// shapes, adversarial inputs) and per noise lattice-cache program; the
// outputs must be equal, bit for bit.
#include "charshader_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_MATHS 200000  // inputs per maths function
#define REPS 4          // input sets per shader program
#define W 71            // not a multiple of the strip
#define H 6

static uint32_t s_rs = 0x12345678u;
static uint32_t rnd(void) {
  s_rs ^= s_rs << 13;
  s_rs ^= s_rs >> 17;
  s_rs ^= s_rs << 5;
  return s_rs;
}

// An adversarial float: signed zeros, subnormals, the int32 and 2^23
// edges, exp's overflow and underflow edges, any bit pattern (NaN, inf),
// values near lattice points and integers, and ordinary ranges.
static float rf(void) {
  static const float sp[] = {0.0f,          -0.0f,         1.0f,        -1.0f,   0.5f,   -0.5f,
                             1e-40f,        -1e-40f,       1e-30f,      -1e-30f, 2147483648.0f,
                             -2147483648.0f, 8388608.0f,   -8388608.5f, 3e9f,    -3e9f,  1e38f,
                             -1e38f,        86.9f,         -86.9f,      88.72f,  -87.5f, 0.99999994f,
                             -2.9e-8f};
  switch (rnd() % 8) {
    case 0: return sp[rnd() % (sizeof sp / sizeof sp[0])];
    case 1: return cs_from_bits(rnd());
    case 2: return (float)((int32_t)(rnd() % 2001) - 1000) * 0.1f;
    case 3: {  // +-inf or +-0
      const uint32_t r1 = rnd(), r2 = rnd();
      return cs_from_bits((0x7F800000u * (r1 & 1u)) | (r2 & 0x80000000u));
    }
    case 4: return ((float)(rnd() & 0xFFFFFFu) / 16777216.0f - 0.5f) * 200.0f;
    case 5: return ((float)(rnd() & 0xFFFFFFu) / 16777216.0f - 0.5f) * 4.0f;
    case 6: return (float)(int32_t)rnd();
    default: return ((float)(rnd() & 0xFFFFu) / 65536.0f) * 20.0f - 10.0f;
  }
}

static uint64_t s_h;
static void mix_u32(uint32_t v) {  // FNV-1a
  for (int i = 0; i < 4; i++) {
    s_h ^= (v >> (8 * i)) & 0xFFu;
    s_h *= 1099511628211ull;
  }
}
static void mix_f(float f) {
  mix_u32(f != f ? 0x7FC00000u : cs_bits(f));  // a NaN's sign and payload are never observed
}

// Array contents: runs of repeats, +-0 alternation, NaN, small steps (the
// lattice cache's reuse), and adversarial values.
static void fill(float *a, int n) {
  for (int i = 0; i < n;) {
    const int run = 1 + (int)(rnd() % 40), kind = (int)(rnd() % 6);
    const float base = rf(), step = (float)(int)(rnd() % 7) * 0.05f;
    for (int j = 0; j < run && i < n; j++, i++) {
      switch (kind) {
        case 0: a[i] = base; break;
        case 1: a[i] = (j & 1) ? -0.0f : 0.0f; break;
        case 2: a[i] = rnd() % 5 == 0 ? CS_NAN : base + (float)j * step; break;
        case 3: a[i] = base + (float)j * step; break;
        case 4: a[i] = (float)j * 0.1f - 0.05f; break;
        default: a[i] = rf(); break;
      }
    }
  }
}

// Coordinates for noise's lattice cache (noise_loop reuses a lattice cell's
// corners, and y's weight, while they repeat): repeats, +-0 alternation,
// NaN between repeats, points in cell (0, 0) (the loop's first corners),
// jumps between cells, small steps across cell edges, adversarial values.
static void fill_lattice(float *a, int n) {
  for (int i = 0; i < n;) {
    const int run = 1 + (int)(rnd() % 12), kind = (int)(rnd() % 7);
    const float base = rf();
    for (int j = 0; j < run && i < n; j++, i++) {
      switch (kind) {
        case 0: a[i] = base; break;
        case 1: a[i] = (j & 1) ? -0.0f : 0.0f; break;
        case 2: a[i] = (j & 1) ? CS_NAN : base; break;
        case 3: a[i] = (float)(rnd() % 1000) * 0.001f; break;
        case 4: a[i] = (float)((int32_t)(rnd() % 64) - 32) + 0.5f; break;
        case 5: a[i] = (float)(int32_t)(rnd() % 8) * 0.5f - 2.0f + (float)j * 0.03f; break;
        default: a[i] = rf(); break;
      }
    }
  }
}

static chargrid_t *s_g, *s_lay;
static int s_progs;

// Compiles src and runs it REPS times, with new uniforms and array contents
// each time: over the whole grid (output level, then all outputs to the
// grid) and over a rect that starts mid-strip; every value and cell goes
// into the hash.
static void run_prog(const char *src, void (*fill_fn)(float *, int)) {
  char err[160];
  charshader_t *s = charshader_compile(src, strlen(src), err, sizeof err);
  if (!s) {
    printf("COMPILE FAILED: %s\n%s\n", err, src);
    exit(1);
  }
  static float vals[W * H], arr[CS_MAX_ARRAY_LEN];
  static const char *const an[] = {"A", "B", "C", "D"}, *const un[] = {"u0", "u1", "u2", "u3"};
  for (int rep = 0; rep < REPS; rep++) {
    for (int k = 0; k < 4; k++) {
      fill_fn(arr, CS_MAX_ARRAY_LEN);
      charshader_set_array(s, an[k], arr, CS_MAX_ARRAY_LEN);
    }
    for (int k = 0; k < 4; k++) charshader_set_uniform(s, un[k], rf());
    chargrid_t *layers[CS_MAX_LAYERS] = {s_lay, NULL, s_g, s_lay};
    const int nl = charshader_layer_count(s);
    if (charshader_run_values(s, s_g, 0, 0, W, H, layers, nl, CS_OUT_LEVEL, vals) == W * H)
      for (int i = 0; i < W * H; i++) mix_f(vals[i]);
    if (charshader_run_values(s, s_g, 5, 2, 37, 3, layers, nl, CS_OUT_LEVEL, vals) == 37 * 3)
      for (int i = 0; i < 37 * 3; i++) mix_f(vals[i]);
    charshader_run(s, s_g, 0, 0, W, H, layers, nl);
    for (int i = 0; i < W * H; i++) {
      mix_u32(s_g->glyph[i]);
      mix_u32(s_g->fg[i]);
      mix_u32(s_g->bg[i]);
    }
  }
  charshader_free(s);
  s_progs++;
}

static void maths(void) {
  static const char *const names[] = {"floor", "ceil",  "fract",   "sin",  "cos",   "exp",   "log",
                                      "pow",   "atan2", "sqrt",    "mod",  "smooth", "ifloor",
                                      "lattice", "hash", "value",  "fbm",  "hash3", "noise3",
                                      "fbm3"};
  enum { NF = sizeof names / sizeof names[0] };
  uint64_t hs[NF];
  for (int f = 0; f < NF; f++) hs[f] = 1469598103934665603ull;
#define M(F, E)    \
  do {             \
    s_h = hs[F];   \
    mix_f(E);      \
    hs[F] = s_h;   \
  } while (0)
#define MU(F, E)   \
  do {             \
    s_h = hs[F];   \
    mix_u32(E);    \
    hs[F] = s_h;   \
  } while (0)
  for (int i = 0; i < N_MATHS; i++) {
    const float a = rf(), b = rf(), c = rf();
    const int32_t s1 = (int32_t)rnd(), s2 = (int32_t)rnd(), s3 = (int32_t)rnd();
    const int o1 = 1 + (int)(rnd() % 8), o2 = 1 + (int)(rnd() % 8);
    M(0, cs_floor(a));
    M(1, cs_ceil(a));
    M(2, cs_fract(a));
    M(3, cs_sin(a));
    M(4, cs_cos(a));
    M(5, cs_exp(a));
    M(6, cs_log(a));
    M(7, cs_pow(a, b));
    M(8, cs_atan2(a, b));
    M(9, cs_sqrt(a));
    M(10, cs_mod(a, b));
    M(11, cs_smooth(a, b, c));
    if (cs_abs(a) < 2147483520.0f) MU(12, (uint32_t)cs_ifloor(a));
    MU(13, (uint32_t)noise_lattice(a));
    M(14, noise_hash(noise_lattice(a), noise_lattice(b), s1));
    M(15, noise_value(a, b, s2));
    M(16, noise_fbm(a, b, o1, s3));
    M(17, cs_hash3(a, b, c));
    M(18, cs_noise3(a, b, c));
    M(19, cs_fbm3(a, b, c, o2));
  }
#undef M
#undef MU
  for (int f = 0; f < NF; f++) printf("maths %-8s %016llx\n", names[f], (unsigned long long)hs[f]);
}

static const char *const k_hdr =
    "uniform u0, u1, u2, u3\narray A, B, C, D\nlayer L0, L1, L2, L3\ni = x + y * cols\n";

// Every operation in every register/uniform operand mix, plain and inside
// register reuse chains, fused forms and every output plane.
static void operations(void) {
  static const char *const reg[4] = {"A[i]", "B[i]", "C[i]", "D[i]"};
  static const char *const uni[4] = {"u0", "u1", "u2", "u3"};
  static const struct {
    const char *fmt;
    int n;
  } ops[] = {
      {"sin(%s)", 1}, {"cos(%s)", 1}, {"abs(%s)", 1}, {"floor(%s)", 1}, {"ceil(%s)", 1},
      {"fract(%s)", 1}, {"sqrt(%s)", 1}, {"exp(%s)", 1}, {"log(%s)", 1}, {"-(%s)", 1},
      {"not (%s)", 1}, {"(%s) + (%s)", 2}, {"(%s) - (%s)", 2}, {"(%s) * (%s)", 2},
      {"(%s) / (%s)", 2}, {"(%s) %% (%s)", 2}, {"(%s) ^ (%s)", 2}, {"min(%s, %s)", 2},
      {"max(%s, %s)", 2}, {"(%s) < (%s)", 2}, {"(%s) <= (%s)", 2}, {"(%s) > (%s)", 2},
      {"(%s) >= (%s)", 2}, {"(%s) == (%s)", 2}, {"(%s) ~= (%s)", 2}, {"(%s) and (%s)", 2},
      {"(%s) or (%s)", 2}, {"step(%s, %s)", 2}, {"atan2(%s, %s)", 2}, {"pow(%s, %s)", 2},
      {"hash(%s, %s)", 2}, {"noise(%s, %s)", 2}, {"fbm(%s, %s, 3)", 2}, {"fbm(%s, %s, 8)", 2},
      {"fbm(%s, %s, 1)", 2}, {"clamp(%s, %s, %s)", 3}, {"mix(%s, %s, %s)", 3},
      {"smooth(%s, %s, %s)", 3}, {"select(%s, %s, %s)", 3}, {"hash(%s, %s, %s)", 3},
      {"noise(%s, %s, %s)", 3}, {"fbm(%s, %s, 3, %s)", 3}, {"(%s) * (%s) + (%s)", 3},
      {"(%s) + (%s) * (%s)", 3}, {"dist(%s, %s, %s, %s)", 4},
  };
  char src[1024], body[512];
  for (size_t o = 0; o < sizeof ops / sizeof ops[0]; o++) {
    s_h = 1469598103934665603ull;
    for (int m = 0; m < (1 << ops[o].n); m++) {
      const char *a[4];
      for (int k = 0; k < 4; k++) a[k] = ((m >> k) & 1) ? uni[k] : reg[k];
      snprintf(body, sizeof body, ops[o].fmt, a[0], a[1], a[2], a[3]);
      snprintf(src, sizeof src, "%slevel = %s\n", k_hdr, body);
      run_prog(src, fill);
      snprintf(src, sizeof src, "%st = %s\nlevel = t * t - 0.25\n", k_hdr, body);
      run_prog(src, fill);
      snprintf(src, sizeof src,
               "%st = %s\nq = t * 3 + A[i]\nlevel = q\nglyph = t * 40\nfg = q * 30\nbgtone = t\n",
               k_hdr, body);
      run_prog(src, fill);
    }
    printf("op %-22s %016llx\n", ops[o].fmt, (unsigned long long)s_h);
  }
}

// noise and fbm over lattice-cache sequences, with a uniform seed (the
// strip loops' cached path) and with a seed per cell.
static void lattice(void) {
  static const char *const progs[] = {
      "level = noise(A[i], B[i], u0)",
      "level = noise(A[i], B[i])",
      "level = fbm(A[i], B[i], 1, u0)",
      "level = fbm(A[i], B[i], 3, u0)",
      "level = fbm(A[i], B[i], 8, u0)",
      "level = noise(A[i] * u1, B[i] * u1, u0)",
      "level = fbm(A[i], B[i], 4, C[i])",
      "level = noise(A[i], y * 0.25, u0) + noise(x * 0.3, B[i], u0)",
  };
  char src[512];
  for (size_t p = 0; p < sizeof progs / sizeof progs[0]; p++) {
    s_h = 1469598103934665603ull;
    snprintf(src, sizeof src, "%s%s\n", k_hdr, progs[p]);
    run_prog(src, fill_lattice);
    run_prog(src, fill_lattice);
    printf("lattice %-60s %016llx\n", progs[p], (unsigned long long)s_h);
  }
}

// The output stage: ramps (dithered), the nearest palette entry, keep, and
// layers including the grid itself.
static void outputs(void) {
  static const char *const outs[] = {
      "level = A[i]\ntone = B[i]\nbgtone = C[i]\nkeep = D[i]\n",
      "r = A[i]\ng = B[i]\nb = C[i]\nlevel = D[i]\n",
      "r = A[i]\nkeep = (B[i] > 0.3) and (C[i] < 0.6)\nglyph = L0.glyph + L2.fg\nbg = L3.bg - L1.glyph\n",
      ("level = smooth(0.2, 0.8, noise(x * 0.37, y * 0.21 + u0)) * fbm(u * 3, v * 5, 4, 7)\n"
       "tone = exp(-dist(x, y, u1, u2) * 0.1)\nbgtone = sin(x * u3) * 0.5 + 0.5\n"),
  };
  char src[1024];
  s_h = 1469598103934665603ull;
  for (size_t k = 0; k < sizeof outs / sizeof outs[0]; k++) {
    snprintf(src, sizeof src, "%s%s", k_hdr, outs[k]);
    run_prog(src, fill);
  }
  printf("outputs %016llx\n", (unsigned long long)s_h);
}

int main(void) {
  maths();
  s_g = chargrid_create(W, H, 4, 4);
  s_lay = chargrid_create(40, 4, 4, 4);
  static const uint8_t ramp[] = {0, 10, 20, 30, 40, 50, 60};
  chargrid_set_ramp(s_g, ramp, 7, true);
  chargrid_set_color_ramp(s_g, ramp, 5, false);
  for (int i = 0; i < 40 * 4; i++) {
    s_lay->glyph[i] = (uint8_t)rnd();
    s_lay->fg[i] = (uint8_t)rnd();
    s_lay->bg[i] = (uint8_t)rnd();
  }
  operations();
  lattice();
  outputs();
  printf("%d programs\n", s_progs);
  chargrid_free(s_lay);
  chargrid_free(s_g);
  return 0;
}
