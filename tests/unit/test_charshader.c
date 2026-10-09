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

int main(void) {
  test_compile_errors();
  test_limits();
  test_folding();
  test_layer_count();
  return check_report("test_charshader");
}
