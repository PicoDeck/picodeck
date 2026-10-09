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

// The code under test allocates here (cg_test_alloc.h): malloc, counted,
// refused while s_refuse_alloc is set.
static int s_allocs;
static bool s_refuse_alloc;
void *cg_test_malloc(size_t n) {
  s_allocs++;
  return s_refuse_alloc ? NULL : malloc(n);
}

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
  // 119 names of the program's own (variables, outputs, uniforms, arrays,
  // layers; x, y, u, v, cols, rows, glyph, fg and bg are not counted).
  CHECK(info_of(repeat("", "a%d = 1\n", 118, "level = x")).instructions == 1);
  CHECK(compile(repeat("", "a%d = 1\n", 119, "level = x")) == NULL);
  CHECK(strstr(s_err, ": more than 119 names") != NULL);
  // 512 operations as parsed, before unused ones are dropped: x and 511
  // multiplies compile (to nothing); one more is too long.
  CHECK(info_of(repeat("", "t=x*x;", 511, "level=1")).instructions == 0);
  CHECK(compile(repeat("", "t=x*x;", 512, "level=1")) == NULL);
  CHECK(strstr(s_err, ": program is too long") != NULL);
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

// noise and fbm with a scalar seed run the strip loops' lattice cache
// (noise_loop: a lattice cell's corners, and y's weight, are reused while
// they repeat, starting from cell (0, 0)'s corners). Coordinates come from
// arrays, per cell, in runs of 8: repeats, +-0 alternation, NaN, jumps
// between cells, small steps across cell edges, cell (0, 0), a constant y,
// infinities and huge values; and each strip's first lane alternately in
// cell (0, 0), out of it, and NaN. Over the whole grid and a rect that
// starts mid-strip, the VM must equal cs_noise3 and cs_fbm3 cell by cell.
static void test_noise_lattice_cache(void) {
  enum { C = 75, R = 3, N = C * R };  // strips of 32, 32 and 11 per row
  static float ax[N], ay[N];
  const float nan = CS_NAN, inf = CS_INF;
  for (int i = 0; i < N; i++) {
    const int k = i % 8, x = i % C;
    float px, py;
    switch ((i / 8) % 8) {
      case 0: px = 3.25f, py = -1.75f; break;
      case 1: px = (k & 1) ? -0.0f : 0.0f, py = (k & 2) ? -0.0f : 0.0f; break;
      case 2: px = (k & 1) ? nan : 0.4f, py = k % 3 == 0 ? nan : 0.6f; break;
      case 3: px = (k & 1) ? 1000.5f : -7.25f, py = (k & 1) ? 0.5f : 12.75f; break;
      case 4: px = 0.9f + 0.05f * (float)k, py = 1.95f + 0.02f * (float)k; break;
      case 5: px = 0.1f * (float)k, py = 0.05f * (float)k; break;
      case 6: px = (float)(k % 3) + 0.5f, py = 2.5f; break;
      default: px = (k & 1) ? inf : -3e9f, py = (k & 1) ? -inf : 1e38f; break;
    }
    if (x == 0 || x == 5 || x == 32 || x == 37 || x == 64 || x == 69) {  // strip starts
      const int row = i / C;
      px = row == 0 ? 0.5f : row == 1 ? 5.5f : nan;
      py = row == 0 ? 0.25f : row == 1 ? 0.25f : nan;
    }
    ax[i] = px;
    ay[i] = py;
  }
  chargrid_t *g = chargrid_create(C, R, 4, 4);
  static const char *const progs[] = {"noise(A[i], B[i], s)", "fbm(A[i], B[i], 1, s)",
                                      "fbm(A[i], B[i], 3, s)", "fbm(A[i], B[i], 8, s)"};
  static const int octaves[] = {0, 1, 3, 8};
  static const float seeds[] = {0.0f, 7.0f, -123.5f};
  char src[160];
  int bad = 0, cells = 0;
  for (int p = 0; p < 4; p++)
    for (int sd = 0; sd < 3; sd++)
      for (int rect = 0; rect < 2; rect++) {
        snprintf(src, sizeof src, "uniform s\narray A, B\ni = x + y * cols\nlevel = %s", progs[p]);
        charshader_t *s = compile(src);
        CHECK(s != NULL);
        if (!s) continue;
        CHECK(charshader_set_array(s, "A", ax, N));
        CHECK(charshader_set_array(s, "B", ay, N));
        CHECK(charshader_set_uniform(s, "s", seeds[sd]));
        const int x0 = rect ? 5 : 0, w = C - x0;
        CHECK_EQ_INT(charshader_run_values(s, g, x0, 0, w, R, NULL, 0, CS_OUT_LEVEL, s_vals), w * R);
        for (int y = 0; y < R; y++)
          for (int x = x0; x < C; x++) {
            const int i = y * C + x;
            const float want = p ? cs_fbm3(ax[i], ay[i], seeds[sd], octaves[p])
                                 : cs_noise3(ax[i], ay[i], seeds[sd]);
            const float got = s_vals[y * w + (x - x0)];
            cells++;
            if (!same(got, want)) {
              if (bad < 10) printf("  %s, seed %g, cell %d: %a, want %a\n", progs[p], (double)seeds[sd], i, (double)got, (double)want);
              bad++;
            }
          }
        charshader_free(s);
      }
  CHECK_EQ_INT(bad, 0);
  CHECK_EQ_INT(cells, 4 * 3 * (C + C - 5) * R);
  chargrid_free(g);
}

// ── Task 6: outputs ──────────────────────────────────────────────────────────
static int s_run_cells;

static void run_src(const char *src, chargrid_t *g) {
  charshader_t *s = compile(src);
  if (!s) printf("compile failed: %s\n  %s\n", s_err, src);
  CHECK(s != NULL);
  s_run_cells = s ? charshader_run(s, g, 0, 0, g->cols, g->rows, NULL, 0) : -1;
  charshader_free(s);
}

static void check_row(const uint8_t *plane, const uint8_t *want, int n, const char *what) {
  for (int i = 0; i < n; i++)
    if (plane[i] != want[i]) {
      printf("  %s[%d] = %d, want %d\n", what, i, plane[i], want[i]);
      CHECK(plane[i] == want[i]);
      return;
    }
}

static void test_outputs_direct(void) {
  chargrid_t *g = chargrid_create(6, 1, 4, 4);
  g->dirty = false;
  run_src("glyph = x * 50.4\nfg = x - 2.5\nbg = 300", g);
  CHECK_EQ_INT(s_run_cells, 6);
  check_row(g->glyph, (const uint8_t[]){0, 50, 101, 151, 202, 252}, 6, "glyph");  // rounded
  check_row(g->fg, (const uint8_t[]){0, 0, 0, 1, 2, 3}, 6, "fg");               // clamped at 0
  check_row(g->bg, (const uint8_t[]){255, 255, 255, 255, 255, 255}, 6, "bg");   // clamped at 255
  CHECK(g->dirty);
  // keep (>= 0.5) leaves cells alone; unassigned planes stay.
  run_src("keep = x < 3\nglyph = 9", g);
  check_row(g->glyph, (const uint8_t[]){0, 50, 101, 9, 9, 9}, 6, "glyph");
  check_row(g->fg, (const uint8_t[]){0, 0, 0, 1, 2, 3}, 6, "fg");
  chargrid_free(g);
}

static void test_outputs_ramps(void) {
  chargrid_t *g = chargrid_create(6, 1, 4, 4);
  chargrid_set_ramp(g, (const uint8_t[]){0, 1, 2, 3, 4}, 5, false);
  chargrid_set_color_ramp(g, (const uint8_t[]){10, 20}, 2, false);
  run_src("level = x / 5", g);  // level drives glyph and, with no tone, fg
  check_row(g->glyph, (const uint8_t[]){0, 1, 2, 2, 3, 4}, 6, "glyph");
  check_row(g->fg, (const uint8_t[]){10, 10, 10, 20, 20, 20}, 6, "fg");
  run_src("level = x / 5\ntone = 1 - x / 5", g);  // tone beats level for fg
  check_row(g->fg, (const uint8_t[]){20, 20, 20, 10, 10, 10}, 6, "fg");
  run_src("tone = 1\nfg = 7", g);  // fg beats tone
  check_row(g->fg, (const uint8_t[]){7, 7, 7, 7, 7, 7}, 6, "fg");
  run_src("bgtone = x / 5", g);
  check_row(g->bg, (const uint8_t[]){10, 10, 10, 20, 20, 20}, 6, "bg");
  // Dithered ramps step per cell exactly as chargrid_ramp_step does.
  chargrid_t *d = chargrid_create(16, 4, 4, 4);
  chargrid_set_ramp(d, (const uint8_t[]){0, 1, 2, 3, 4}, 5, true);
  run_src("level = 0.37", d);
  int bad = 0;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 16; x++)
      if (d->glyph[y * 16 + x] != chargrid_ramp_step(0.37f, 5, true, x, y)) bad++;
  CHECK_EQ_INT(bad, 0);
  // No ramp set: level and tone leave the planes alone.
  chargrid_t *e = chargrid_create(2, 1, 4, 4);
  run_src("level = 1\ntone = 1", e);
  CHECK(e->glyph[0] == 0 && e->fg[0] == CG_DEFAULT_FG);
  chargrid_free(e);
  chargrid_free(d);
  chargrid_free(g);
}

static void test_outputs_nonfinite(void) {
  chargrid_t *g = chargrid_create(6, 1, 4, 4);
  chargrid_set_ramp(g, (const uint8_t[]){5, 6, 7}, 3, false);
  run_src("glyph = 0 / 0\nfg = 1 / 0\nbg = -1 / 0", g);
  check_row(g->glyph, (const uint8_t[]){0, 0, 0, 0, 0, 0}, 6, "glyph");
  check_row(g->fg, (const uint8_t[]){255, 255, 255, 255, 255, 255}, 6, "fg");
  check_row(g->bg, (const uint8_t[]){0, 0, 0, 0, 0, 0}, 6, "bg");
  run_src("glyph = (x - x) / (x - x)\nfg = log(x - 3)\nbg = exp(x * 100)", g);
  check_row(g->glyph, (const uint8_t[]){0, 0, 0, 0, 0, 0}, 6, "glyph");
  check_row(g->fg, (const uint8_t[]){0, 0, 0, 0, 0, 1}, 6, "fg");  // NaN, -inf, 0, 0.69
  check_row(g->bg, (const uint8_t[]){1, 255, 255, 255, 255, 255}, 6, "bg");
  run_src("level = sqrt(x - 3)\nr = 0 / 0\ng = 1 / 0\nb = -1 / 0", g);  // NaN level: ramp entry 0
  check_row(g->glyph, (const uint8_t[]){5, 5, 5, 5, 7, 7}, 6, "glyph");
  chargrid_free(g);
}

static void test_outputs_constant(void) {
  chargrid_t *g = chargrid_create(4, 2, 4, 4);
  run_src("fg = 300", g);
  CHECK(g->fg[0] == 255 && g->fg[7] == 255);
  g->dirty = false;
  run_src("a = 1", g);  // no outputs: nothing written, not dirty
  CHECK_EQ_INT(s_run_cells, 8);
  CHECK(!g->dirty);
  run_src("keep = 1\nglyph = 5", g);  // everything kept
  CHECK(g->glyph[0] == 0 && g->glyph[7] == 0);
  chargrid_free(g);
}

static void test_nearest(void) {
  chargrid_t *g = chargrid_create(4, 1, 4, 4);
  for (int i = 0; i < 256; i++) g->palette[i] = 0x0000;
  g->palette[7] = 0xF800;    // red
  g->palette[9] = 0x07E0;    // green
  g->palette[200] = 0x001F;  // blue
  chargrid_palette_changed(g);
  run_src("r = x == 1\ng = x == 2\nb = x == 3", g);
  check_row(g->fg, (const uint8_t[]){0, 7, 9, 200}, 4, "fg");  // black: the lowest index
  CHECK(g->nearest != NULL);
  CHECK_EQ_INT(chargrid_nearest(g, 1.0f, 0.0f, 0.0f), 7);
  CHECK_EQ_INT(chargrid_nearest(g, NAN, 2.0f, -1.0f), 9);  // NaN 0, clamped to 1 and 0
  run_src("r = 1", g);  // g and b unassigned: 0
  check_row(g->fg, (const uint8_t[]){7, 7, 7, 7}, 4, "fg");
  chargrid_free(g);
}

// The nearest table cannot be allocated: every cell searches in full, and
// the allocation is tried once, not once per cell, until the palette
// changes.
static void test_nearest_alloc_failed(void) {
  chargrid_t *g = chargrid_create(4, 1, 4, 4);
  for (int i = 0; i < 256; i++) g->palette[i] = 0x0000;
  g->palette[7] = 0xF800;
  g->palette[9] = 0x07E0;
  g->palette[200] = 0x001F;
  chargrid_palette_changed(g);
  charshader_t *s = compile("r = x == 1\ng = x == 2\nb = x == 3");
  CHECK(s != NULL);
  s_refuse_alloc = true;
  const int before = s_allocs;
  CHECK_EQ_INT(charshader_run(s, g, 0, 0, 4, 1, NULL, 0), 4);
  check_row(g->fg, (const uint8_t[]){0, 7, 9, 200}, 4, "fg");
  CHECK(g->nearest == NULL);
  CHECK_EQ_INT(s_allocs - before, 1);
  memset(g->fg, 0, 4);
  charshader_run(s, g, 0, 0, 4, 1, NULL, 0);  // not tried again
  check_row(g->fg, (const uint8_t[]){0, 7, 9, 200}, 4, "fg");
  CHECK_EQ_INT(s_allocs - before, 1);
  s_refuse_alloc = false;
  chargrid_palette_changed(g);  // tried again, and now it works
  memset(g->fg, 0, 4);
  charshader_run(s, g, 0, 0, 4, 1, NULL, 0);
  check_row(g->fg, (const uint8_t[]){0, 7, 9, 200}, 4, "fg");
  CHECK_EQ_INT(s_allocs - before, 2);
  CHECK(g->nearest != NULL);
  charshader_free(s);
  chargrid_free(g);
}

static void test_nearest_invalidated(void) {
  chargrid_t *g = chargrid_create(1, 1, 4, 4);
  for (int i = 0; i < 256; i++) g->palette[i] = 0x0000;
  g->palette[7] = 0xF800;
  chargrid_palette_changed(g);
  run_src("r = 1", g);
  CHECK_EQ_INT(g->fg[0], 7);
  g->palette[7] = 0x0000;
  g->palette[50] = 0xF800;
  // The cache still answers 7 until the change is reported...
  CHECK_EQ_INT(chargrid_nearest(g, 1.0f, 0.0f, 0.0f), 7);
  g->dirty = false;
  chargrid_palette_changed(g);
  CHECK(g->dirty);
  // ...then it is rebuilt.
  run_src("r = 1", g);
  CHECK_EQ_INT(g->fg[0], 50);
  chargrid_free(g);
}

static void test_run_rect_clip(void) {
  chargrid_t *g = chargrid_create(10, 4, 4, 4);
  charshader_t *s = compile("glyph = glyph + 1");
  CHECK_EQ_INT(charshader_run(s, g, -3, -2, 6, 4, NULL, 0), 3 * 2);
  CHECK_EQ_INT(charshader_run(s, g, 8, 3, INT32_MAX, INT32_MAX, NULL, 0), 2);
  CHECK_EQ_INT(charshader_run(s, g, 0, 0, -1, 5, NULL, 0), 0);
  CHECK_EQ_INT(charshader_run(s, g, 10, 0, 5, 5, NULL, 0), 0);
  CHECK_EQ_INT(charshader_run(s, g, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, NULL, 0), 0);
  int ones = 0, total = 0;
  for (int i = 0; i < 40; i++) {
    ones += g->glyph[i] == 1;
    total += g->glyph[i];
  }
  CHECK_EQ_INT(ones, 8);
  CHECK_EQ_INT(total, 8);
  // The target as its own layer: each cell reads itself before it is written.
  charshader_t *t = compile("layer m\nglyph = m.glyph + 2");
  chargrid_t *self[1] = {g};
  charshader_run(t, g, 0, 0, 10, 4, self, 1);
  CHECK_EQ_INT(g->glyph[0], 3);   // run once before, then +2
  CHECK_EQ_INT(g->glyph[5], 2);   // never run before
  CHECK_EQ_INT(g->glyph[39], 3);
  charshader_free(t);
  charshader_free(s);
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
  test_noise_lattice_cache();
  test_outputs_direct();
  test_outputs_ramps();
  test_outputs_nonfinite();
  test_outputs_constant();
  test_nearest();
  test_nearest_alloc_failed();
  test_nearest_invalidated();
  test_run_rect_clip();
  return check_report("test_charshader");
}
