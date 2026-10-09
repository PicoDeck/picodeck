// Host unit tests for the chargrid shader (src/os/charshader*.c): compile
// errors and limits, the optimiser (folding, dead code, fusion) through
// charshader_info, and (Tasks 5-6) the VM against the scalar folder and the
// output rules.
#include "charshader_internal.h"
#include "check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_err[160];

static charshader_t *compile(const char *src) {
  return charshader_compile(src, strlen(src), s_err, sizeof s_err);
}

static charshader_info_t info_of(const char *src) {
  charshader_info_t i = {-1, -1, -1, -1, -1, -1};
  charshader_t *s = compile(src);
  if (!s) printf("compile failed: %s\n  %s\n", s_err, src);
  CHECK(s != NULL);
  if (s) charshader_info(s, &i);
  charshader_free(s);
  return i;
}

static void test_compile_errors(void) {
  static const struct {
    const char *src, *err;
  } k[] = {
      {"level = hme", "line 1:9: unknown name 'hme'"},
      {"a = 1\nlevel = a + hme * 2", "line 2:13: unknown name 'hme'"},
      {"level = (x +\n  hme)", "line 2:3: unknown name 'hme'"},
      {"level = 1 2", "line 1:11: expected a new line or ';'"},
      {"level = ", "line 1:9: expected an expression"},
      {"= 3", "line 1:1: expected a name or a declaration"},
      {"level 3", "line 1:7: expected '='"},
      {"level = (1 + 2", "line 1:15: expected ')'"},
      {"array a\nlevel = a[1", "line 2:12: expected ']'"},
      {"level = 3x", "line 1:9: malformed number"},
      {"level = 1e", "line 1:9: malformed number"},
      {"level = 0x", "line 1:9: malformed number"},
      {"level = 1 # 2", "line 1:11: unexpected character '#'"},
      {"level = 1 \x01", "line 1:11: unexpected byte 0x01"},
      {"level = 1 ~ 2", "line 1:11: unexpected character '~'"},
      {"level = foo(1)", "line 1:9: unknown function 'foo'"},
      {"uniform k\nlevel = k(1)", "line 2:9: 'k' is not a function"},
      {"level = sin", "line 1:9: 'sin' is a function"},
      {"sin = 1", "line 1:1: 'sin' is a function"},
      {"uniform sin", "line 1:9: 'sin' is a function"},
      {"x = 1", "line 1:1: 'x' is read-only"},
      {"uniform k\nk = 1", "line 2:1: 'k' is read-only"},
      {"uniform k, k", "line 1:12: 'k' is already declared"},
      {"a = 1\narray a", "line 2:7: 'a' is already declared"},
      {"uniform", "line 1:8: expected a name after 'uniform'"},
      {"layer 3", "line 1:7: expected a name after 'layer'"},
      {"level = x[1]", "line 1:9: 'x' is not an array"},
      {"level = q[1]", "line 1:9: unknown name 'q'"},
      {"level = x.fg", "line 1:9: 'x' is not a layer"},
      {"layer m\nlevel = m.alpha", "line 2:11: layers have .glyph, .fg and .bg"},
      {"layer m\nlevel = m.", "line 2:11: expected a field name after '.'"},
      {"array a\nlevel = a", "line 2:9: array 'a' needs an index"},
      {"layer m\nlevel = m", "line 2:9: layer 'm' needs .glyph, .fg or .bg"},
      {"level = sin(1, 2)", "line 1:9: 'sin' takes 1 argument"},
      {"level = min(1)", "line 1:9: 'min' takes 2 arguments"},
      {"level = hash(1)", "line 1:9: 'hash' takes 2 or 3 arguments"},
      {"level = fbm(x, y, 9)", "line 1:9: fbm octaves must be a constant 1-8"},
      {"uniform o\nlevel = fbm(x, y, o)", "line 2:9: fbm octaves must be a constant 1-8"},
      {"level = fbm(x, y, 2.5)", "line 1:9: fbm octaves must be a constant 1-8"},
      {"level = keep", "line 1:9: unknown name 'keep'"},
      {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa = 1", "line 1:1: name is longer than 31 characters"},
      {"level = ((((((((((((((((((1))))))))))))))))))", "line 1:25: expression nests more than 16 deep"},
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
    s_err[0] = '\0';
    charshader_t *s = compile(k[i].src);
    CHECK(s == NULL);
    charshader_free(s);
    if (strcmp(s_err, k[i].err) != 0) printf("  source: %s\n", k[i].src);
    CHECK_STR(s_err, k[i].err);
  }
  // A small buffer truncates; a NULL buffer is allowed.
  char tiny[8];
  CHECK(charshader_compile("level = hme", 11, tiny, sizeof tiny) == NULL);
  CHECK_STR(tiny, "line 1:");
  CHECK(charshader_compile("level = hme", 11, NULL, 0) == NULL);
}

static char *repeat(const char *head, const char *fmt, int n, const char *tail) {
  static char buf[16384];
  size_t k = (size_t)snprintf(buf, sizeof buf, "%s", head);
  for (int i = 0; i < n; i++) k += (size_t)snprintf(buf + k, sizeof buf - k, fmt, i, i);
  snprintf(buf + k, sizeof buf - k, "%s", tail);
  return buf;
}

static void test_limits(void) {
  // 16 uniforms, 8 arrays, 4 layers compile; one more fails.
  CHECK(info_of(repeat("uniform u", "%d, u", 15, "15\nlevel = u15")).uniforms == 16);
  CHECK(compile(repeat("uniform u", "%d, u", 16, "16\nlevel = u16")) == NULL);
  CHECK(strstr(s_err, "more than 16 uniforms") != NULL);
  CHECK(info_of(repeat("array a", "%d, a", 7, "7\nlevel = a7[0]")).arrays == 8);
  CHECK(compile(repeat("array a", "%d, a", 8, "8\nlevel = a8[0]")) == NULL);
  CHECK(strstr(s_err, "more than 8 arrays") != NULL);
  CHECK(info_of(repeat("layer l", "%d, l", 3, "3\nlevel = l3.fg")).layers == 4);
  CHECK(compile(repeat("layer l", "%d, l", 4, "4\nlevel = l4.fg")) == NULL);
  CHECK(strstr(s_err, "more than 4 layers") != NULL);
  // The source size.
  char *src = (char *)malloc(CS_MAX_SOURCE + 2);
  memset(src, ' ', CS_MAX_SOURCE + 1);
  memcpy(src, "level = 1 --", 12);
  charshader_t *s = charshader_compile(src, CS_MAX_SOURCE, s_err, sizeof s_err);
  CHECK(s != NULL);
  charshader_free(s);
  CHECK(charshader_compile(src, CS_MAX_SOURCE + 1, s_err, sizeof s_err) == NULL);
  CHECK_STR(s_err, "source is longer than 4096 bytes");
  free(src);
  // 255 instructions: x and 254 fused multiply-adds; one more fails.
  CHECK(info_of(repeat("level = x", " + x * 1.5%.0d", 254, "")).instructions == 255);
  CHECK(compile(repeat("level = x", " + x * 1.5%.0d", 255, "")) == NULL);
  CHECK_STR(s_err, "program needs more than 255 instructions");
  // 32 temporaries: 32 values live at once fit (the last reuses x's register,
  // freed at its last use); 40 do not.
  CHECK(info_of(repeat("", "t%d = sin(x + %d)\n", 32,
                       "level = t0+t1+t2+t3+t4+t5+t6+t7+t8+t9+t10+t11+t12+t13+t14+t15+t16+"
                       "t17+t18+t19+t20+t21+t22+t23+t24+t25+t26+t27+t28+t29+t30+t31")).registers == 32);
  CHECK(compile(repeat("", "t%d = sin(x + %d)\n", 40,
                       "level = t0+t1+t2+t3+t4+t5+t6+t7+t8+t9+t10+t11+t12+t13+t14+t15+t16+t17+"
                       "t18+t19+t20+t21+t22+t23+t24+t25+t26+t27+t28+t29+t30+t31+t32+t33+t34+"
                       "t35+t36+t37+t38+t39")) == NULL);
  CHECK_STR(s_err, "program needs more than 32 temporaries");
  // 237 distinct constants fit; 240 do not.
  CHECK(info_of(repeat("level = x", " + %d.5%.0d", 237, "")).scalars == CS_SC_CONST0 + 237);
  CHECK(compile(repeat("level = x", " + %d.5%.0d", 240, "")) == NULL);
  CHECK_STR(s_err, "program has more than 237 distinct constants");
}

static void test_folding(void) {
  // Constants fold away; an output that is a constant needs no instruction.
  charshader_info_t i = info_of("level = 1 + 2 * 3 - sin(0)");
  CHECK_EQ_INT(i.instructions, 0);
  CHECK_EQ_INT(i.registers, 0);
  // Dead code goes.
  CHECK_EQ_INT(info_of("a = x * 2 + sin(y)\nlevel = 1").instructions, 0);
  // No outputs at all: nothing to run.
  CHECK_EQ_INT(info_of("a = x * 2").instructions, 0);
  // x * k + c fuses into one multiply-add (plus the load of x).
  CHECK_EQ_INT(info_of("level = x * 0.5 + 0.25").instructions, 2);
  CHECK_EQ_INT(info_of("level = x * 0.5 - 0.25").instructions, 2);
  CHECK_EQ_INT(info_of("level = 0.25 + 0.5 * x").instructions, 2);
  // Division by a constant is a multiply.
  CHECK_EQ_INT(info_of("level = x / 4").instructions, 2);
  // A product used twice is not fused (it stays one MUL plus two users).
  CHECK_EQ_INT(info_of("p = x * 3\nlevel = p + 1\ntone = p").instructions, 3);
  // Loads are shared: x read three times is one load.
  CHECK_EQ_INT(info_of("level = sin(x) + cos(x) * x").instructions, 4);
  // Uniforms and cols/rows are scalars, not instructions.
  CHECK_EQ_INT(info_of("uniform t\nlevel = x * t + cols").instructions, 2);
  // Comments, semicolons and blank lines.
  CHECK_EQ_INT(info_of("-- a comment\n\n a = x ; level = a -- trailing\n").instructions, 1);
}

static void test_layer_count(void) {
  charshader_t *s = compile("layer a, b\nlevel = a.glyph + b.bg");
  CHECK(s != NULL);
  CHECK_EQ_INT(charshader_layer_count(s), 2);
  charshader_free(s);
  charshader_free(NULL);
}

// ── Task 5: the VM ───────────────────────────────────────────────────────────
static float s_vals[4096];
static const char *const k_uni[4] = {"k0", "k1", "k2", "k3"};

static bool same(float a, float b) { return a == b || (a != a && b != b); }

// Compiles src, sets uniforms k0..k3 from k4 (if given), runs it over the
// whole of g and leaves output `out` in s_vals. Returns the cells, or -1.
static int run_vals(const char *src, chargrid_t *g, int out, chargrid_t *const *layers, int nl,
                    const float *k4) {
  charshader_t *s = compile(src);
  if (!s) {
    printf("compile failed: %s\n  %s\n", s_err, src);
    CHECK(s != NULL);
    return -1;
  }
  if (k4)
    for (int i = 0; i < 4; i++) charshader_set_uniform(s, k_uni[i], k4[i]);
  const int n = charshader_run_values(s, g, 0, 0, g->cols, g->rows, layers, nl, out, s_vals);
  charshader_free(s);
  return n;
}

static float eval1(const char *e) {  // level = e on a 1x1 grid
  char src[256];
  snprintf(src, sizeof src, "level = %s", e);
  chargrid_t *g = chargrid_create(1, 1, 4, 4);
  s_vals[0] = -12345.0f;
  run_vals(src, g, CS_OUT_LEVEL, NULL, 0, NULL);
  chargrid_free(g);
  return s_vals[0];
}

static void test_precedence(void) {
  CHECK(eval1("2 + 3 * 4 ^ 2") == 50.0f);
  CHECK(eval1("-2 ^ 2") == -4.0f);
  CHECK(eval1("2 ^ 3 ^ 2") == 512.0f);
  CHECK(eval1("2 ^ -1") == 0.5f);
  CHECK(eval1("10 - 4 - 3") == 3.0f);
  CHECK(eval1("2 * 3 / 4") == 1.5f);
  CHECK(eval1("7 % 3") == 1.0f);
  CHECK(eval1("-7 % 3") == 2.0f);
  CHECK(eval1("1 + 2 < 4 and 3 > 2") == 1.0f);
  CHECK(eval1("not 0 == 1") == 1.0f);
  CHECK(eval1("not (0 == 1)") == 1.0f);
  CHECK(eval1("(1 or 0) * 5") == 5.0f);
  CHECK(eval1("2 and 3") == 1.0f);
  CHECK(eval1("0 or 0") == 0.0f);
  CHECK(eval1("1 < 2 == 1") == 1.0f);
  CHECK(eval1("3 >= 3") == 1.0f);
  CHECK(eval1("2 ~= 2") == 0.0f);
  CHECK(eval1("select(0, 1, 2)") == 2.0f);
  CHECK(eval1("select(0.5, 1, 2)") == 1.0f);
  CHECK(eval1("clamp(5, 0, 1)") == 1.0f);
  CHECK(eval1("mix(2, 4, 0.25)") == 2.5f);
  CHECK(eval1("step(0.5, 0.4)") == 0.0f);
  CHECK(eval1("smooth(0, 1, 0.5)") == 0.5f);
  CHECK(eval1("dist(0, 0, 3, 4)") == 5.0f);
  CHECK(eval1("min(3, 2) + max(3, 2)") == 5.0f);
  CHECK(eval1("abs(-2) + floor(-1.5) + ceil(1.2) + fract(-0.25)") == 2.75f);
  CHECK(eval1("0x10 + 1e2 + .5") == 116.5f);
  CHECK(eval1("hash(0, 0)") == 0.0f);
  CHECK(eval1("hash(1, 0)") == noise_hash(1, 0, 0));
  CHECK(eval1("noise(0.5, 0.5, 3)") == noise_value(0.5f, 0.5f, 3));
  CHECK(eval1("fbm(0.3, 0.7, 3)") == noise_fbm(0.3f, 0.7f, 3, 0));
}

// The operand forms the VM must agree on: a register (a fused multiply-add of
// x or y) or a uniform. Constants are written as (float)<double>, as the
// lexer converts them.
static const char *const k_reg_src[4] = {"(x * 0.37 - 5)", "(x * -0.21 + 2.5)",
                                         "(y * 0.9 - 0.7)", "(x * 0.13 + 0.3)"};
static float reg_val(int i, int x, int y) {
  switch (i) {
    case 0: return (float)x * (float)0.37 - (float)5;
    case 1: return (float)x * -(float)0.21 + (float)2.5;
    case 2: return (float)y * (float)0.9 - (float)0.7;
    default: return (float)x * (float)0.13 + (float)0.3;
  }
}

typedef struct {
  const char *name;
  int op, nargs;
  bool infix, swap;  // swap: a > b is b < a
} fn_t;

static const fn_t k_fns[] = {
    {"sin", CS_OP_SIN, 1, false, false},     {"cos", CS_OP_COS, 1, false, false},
    {"abs", CS_OP_ABS, 1, false, false},     {"floor", CS_OP_FLOOR, 1, false, false},
    {"ceil", CS_OP_CEIL, 1, false, false},   {"fract", CS_OP_FRACT, 1, false, false},
    {"sqrt", CS_OP_SQRT, 1, false, false},   {"exp", CS_OP_EXP, 1, false, false},
    {"log", CS_OP_LOG, 1, false, false},     {"pow", CS_OP_POW, 2, false, false},
    {"min", CS_OP_MIN, 2, false, false},     {"max", CS_OP_MAX, 2, false, false},
    {"step", CS_OP_STEP, 2, false, false},   {"atan2", CS_OP_ATAN2, 2, false, false},
    {"clamp", CS_OP_CLAMP, 3, false, false}, {"mix", CS_OP_MIX, 3, false, false},
    {"smooth", CS_OP_SMOOTH, 3, false, false}, {"select", CS_OP_SELECT, 3, false, false},
    {"hash", CS_OP_HASH, 3, false, false},   {"noise", CS_OP_NOISE, 3, false, false},
    {"dist", CS_OP_DIST, 4, false, false},   {"+", CS_OP_ADD, 2, true, false},
    {"-", CS_OP_SUB, 2, true, false},        {"*", CS_OP_MUL, 2, true, false},
    {"/", CS_OP_DIV, 2, true, false},        {"%", CS_OP_MOD, 2, true, false},
    {"^", CS_OP_POW, 2, true, false},        {"<", CS_OP_LT, 2, true, false},
    {"<=", CS_OP_LE, 2, true, false},        {">", CS_OP_LT, 2, true, true},
    {">=", CS_OP_LE, 2, true, true},         {"==", CS_OP_EQ, 2, true, false},
    {"~=", CS_OP_NE, 2, true, false},        {"and", CS_OP_AND, 2, true, false},
    {"or", CS_OP_OR, 2, true, false},
};

// Every operation, in every register/uniform operand mix, over a grid whose
// width is not a multiple of the strip: the VM must equal the folder's
// scalar result bit for bit.
static void test_vm_matches_folder(void) {
  static const float k4[4] = {1.25f, -0.75f, 2.5f, 0.3f};
  chargrid_t *g = chargrid_create(37, 3, 4, 4);
  int bad = 0, cases = 0;
  char src[320];
  for (size_t f = 0; f < sizeof k_fns / sizeof k_fns[0]; f++) {
    const fn_t *fn = &k_fns[f];
    for (int m = 0; m < (1 << fn->nargs); m++) {
      const char *a[4];
      for (int i = 0; i < 4; i++) a[i] = ((m >> i) & 1) ? k_uni[i] : k_reg_src[i];
      int k = snprintf(src, sizeof src, "uniform k0, k1, k2, k3\nlevel = ");
      if (fn->infix)
        snprintf(src + k, sizeof src - (size_t)k, "%s %s %s", a[0], fn->name, a[1]);
      else if (fn->nargs == 1)
        snprintf(src + k, sizeof src - (size_t)k, "%s(%s)", fn->name, a[0]);
      else if (fn->nargs == 2)
        snprintf(src + k, sizeof src - (size_t)k, "%s(%s, %s)", fn->name, a[0], a[1]);
      else if (fn->nargs == 3)
        snprintf(src + k, sizeof src - (size_t)k, "%s(%s, %s, %s)", fn->name, a[0], a[1], a[2]);
      else
        snprintf(src + k, sizeof src - (size_t)k, "%s(%s, %s, %s, %s)", fn->name, a[0], a[1], a[2], a[3]);
      if (run_vals(src, g, CS_OUT_LEVEL, NULL, 0, k4) != 37 * 3) {
        bad++;
        continue;
      }
      cases++;
      for (int y = 0; y < 3; y++)
        for (int x = 0; x < 37; x++) {
          float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          for (int i = 0; i < fn->nargs; i++) v[i] = ((m >> i) & 1) ? k4[i] : reg_val(i, x, y);
          const float want = fn->swap ? cs_eval(fn->op, 0, v[1], v[0], 0.0f, 0.0f)
                                      : cs_eval(fn->op, 0, v[0], v[1], v[2], v[3]);
          if (!same(s_vals[y * 37 + x], want)) {
            if (bad < 10) printf("  %s at (%d,%d): %a, want %a\n", src, x, y, (double)s_vals[y * 37 + x], (double)want);
            bad++;
          }
        }
    }
  }
  // a * b + c in every operand mix (the MAD fast paths and the generic one),
  // and fbm with every operand form (octaves are constant).
  for (int m = 0; m < 8; m++) {
    const char *a[3];
    for (int i = 0; i < 3; i++) a[i] = ((m >> i) & 1) ? k_uni[i] : k_reg_src[i];
    for (int form = 0; form < 2; form++) {
      if (form == 0)
        snprintf(src, sizeof src, "uniform k0, k1, k2, k3\nlevel = %s * %s + %s", a[0], a[1], a[2]);
      else
        snprintf(src, sizeof src, "uniform k0, k1, k2, k3\nlevel = fbm(%s, %s, 3, %s)", a[0], a[1], a[2]);
      if (run_vals(src, g, CS_OUT_LEVEL, NULL, 0, k4) != 37 * 3) {
        bad++;
        continue;
      }
      cases++;
      for (int y = 0; y < 3; y++)
        for (int x = 0; x < 37; x++) {
          float v[3];
          for (int i = 0; i < 3; i++) v[i] = ((m >> i) & 1) ? k4[i] : reg_val(i, x, y);
          const float want = form == 0 ? cs_mad(v[0], v[1], v[2]) : cs_fbm3(v[0], v[1], v[2], 3);
          if (!same(s_vals[y * 37 + x], want)) {
            if (bad < 10) printf("  %s at (%d,%d)\n", src, x, y);
            bad++;
          }
        }
    }
  }
  CHECK_EQ_INT(bad, 0);
  CHECK_EQ_INT(cases, 174);
  chargrid_free(g);
}

static void test_inputs(void) {
  chargrid_t *g = chargrid_create(20, 3, 4, 4);
  for (int i = 0; i < 60; i++) {
    g->glyph[i] = (uint8_t)(i % 7);
    g->fg[i] = (uint8_t)(i % 11);
    g->bg[i] = (uint8_t)(i % 13);
  }
  static const struct {
    const char *src;
    int which;  // 0 x, 1 y, 2 u, 3 v, 4 cols, 5 rows, 6 planes, 7 assigned glyph
  } k[] = {{"level = x", 0},    {"level = y", 1},    {"level = u", 2},
           {"level = v", 3},    {"level = cols", 4}, {"level = rows", 5},
           {"level = glyph + fg * 16 + bg * 256", 6}, {"glyph = 7\nlevel = glyph + x", 7}};
  int bad = 0;
  for (size_t c = 0; c < sizeof k / sizeof k[0]; c++) {
    CHECK_EQ_INT(run_vals(k[c].src, g, CS_OUT_LEVEL, NULL, 0, NULL), 60);
    for (int y = 0; y < 3; y++)
      for (int x = 0; x < 20; x++) {
        const int i = y * 20 + x;
        float want;
        switch (k[c].which) {
          case 0: want = (float)x; break;
          case 1: want = (float)y; break;
          case 2: want = (float)x * (1.0f / 20.0f); break;
          case 3: want = (float)y * (1.0f / 3.0f); break;
          case 4: want = 20.0f; break;
          case 5: want = 3.0f; break;
          case 6: want = (float)(i % 7) + (float)(i % 11) * 16.0f + (float)(i % 13) * 256.0f; break;
          default: want = 7.0f + (float)x; break;
        }
        if (!same(s_vals[i], want)) bad++;
      }
  }
  CHECK_EQ_INT(bad, 0);
  // An output the program never assigns.
  CHECK_EQ_INT(run_vals("level = 1", g, CS_OUT_TONE, NULL, 0, NULL), -1);
  chargrid_free(g);
}

static void test_strips(void) {  // a sub-rect whose width is not a multiple of the strip
  chargrid_t *g = chargrid_create(53, 2, 4, 4);
  charshader_t *s = compile("level = x + y * 100");
  CHECK_EQ_INT(charshader_run_values(s, g, 3, 1, 47, 1, NULL, 0, CS_OUT_LEVEL, s_vals), 47);
  int bad = 0;
  for (int j = 0; j < 47; j++)
    if (s_vals[j] != (float)(3 + j + 100)) bad++;
  CHECK_EQ_INT(bad, 0);
  // Clipped: the rect hangs off three sides.
  CHECK_EQ_INT(charshader_run_values(s, g, -5, -1, 10, 9, NULL, 0, CS_OUT_LEVEL, s_vals), 5 * 2);
  CHECK(s_vals[0] == 0.0f && s_vals[4] == 4.0f && s_vals[5] == 100.0f);
  charshader_free(s);
  chargrid_free(g);
}

static void test_uniforms_and_arrays(void) {
  chargrid_t *g = chargrid_create(12, 1, 4, 4);
  charshader_t *s = compile("uniform t\narray a, b\nlevel = a[x * 0.5 - 1] + b[x] + t");
  CHECK(charshader_set_uniform(s, "t", 0.5f));
  CHECK(!charshader_set_uniform(s, "nope", 1.0f));
  float *a = charshader_array(s, "a", 4);
  CHECK(a != NULL);
  for (int i = 0; i < 4; i++) a[i] = (float)(10 * (i + 1));
  CHECK(charshader_array(s, "a", CS_MAX_ARRAY_LEN + 1) == NULL);
  CHECK(charshader_array(s, "a", -1) == NULL);
  CHECK(charshader_array(s, "zz", 1) == NULL);
  // Index floor(x / 2 - 1), clamped to 0..3; b was never set, so it reads 0.
  CHECK_EQ_INT(charshader_run_values(s, g, 0, 0, 12, 1, NULL, 0, CS_OUT_LEVEL, s_vals), 12);
  static const float want[12] = {10, 10, 10, 10, 20, 20, 30, 30, 40, 40, 40, 40};
  int bad = 0;
  for (int i = 0; i < 12; i++)
    if (s_vals[i] != want[i] + 0.5f) bad++;
  CHECK_EQ_INT(bad, 0);
  // Shrinking the array clamps to its new end.
  static const float two[2] = {1.0f, 2.0f};
  CHECK(charshader_set_array(s, "a", two, 2));
  charshader_run_values(s, g, 0, 0, 12, 1, NULL, 0, CS_OUT_LEVEL, s_vals);
  CHECK(s_vals[11] == 2.5f);
  charshader_free(s);
  // A NaN index reads entry 0.
  s = compile("array a\nlevel = a[0 / 0]");
  CHECK(charshader_set_array(s, "a", two, 2));
  charshader_run_values(s, g, 0, 0, 1, 1, NULL, 0, CS_OUT_LEVEL, s_vals);
  CHECK(s_vals[0] == 1.0f);
  charshader_free(s);
  chargrid_free(g);
}

static void test_layers(void) {
  chargrid_t *g = chargrid_create(20, 4, 4, 4), *small = chargrid_create(5, 2, 4, 4);
  for (int i = 0; i < 10; i++) small->fg[i] = (uint8_t)(i + 1);
  for (int i = 0; i < 80; i++) g->glyph[i] = (uint8_t)(i % 9);
  chargrid_t *lay[2] = {small, NULL};
  int bad = 0;
  CHECK_EQ_INT(run_vals("layer m\nlevel = m.fg", g, CS_OUT_LEVEL, lay, 1, NULL), 80);
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 20; x++)
      if (s_vals[y * 20 + x] != (x < 5 && y < 2 ? (float)(y * 5 + x + 1) : 0.0f)) bad++;
  // A NULL layer, and a layer that was not passed, read 0.
  run_vals("layer m, n\nlevel = m.fg + n.fg + 1", g, CS_OUT_LEVEL, lay + 1, 1, NULL);
  for (int i = 0; i < 80; i++)
    if (s_vals[i] != 1.0f) bad++;
  // The target grid as its own layer.
  lay[0] = g;
  run_vals("layer m\nlevel = m.glyph", g, CS_OUT_LEVEL, lay, 1, NULL);
  for (int i = 0; i < 80; i++)
    if (s_vals[i] != (float)(i % 9)) bad++;
  CHECK_EQ_INT(bad, 0);
  chargrid_free(small);
  chargrid_free(g);
}

int main(void) {
  test_compile_errors();
  test_limits();
  test_folding();
  test_layer_count();
  test_precedence();
  test_vm_matches_folder();
  test_inputs();
  test_strips();
  test_uniforms_and_arrays();
  test_layers();
  return check_report("test_charshader");
}
