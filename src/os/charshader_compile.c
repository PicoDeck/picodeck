// The chargrid shader compiler (charshader.h). One pass of recursive descent
// builds SSA nodes, folding constants as it goes; then dead nodes are
// dropped, a * b + c is fused, registers are allocated by linear scan, and
// the bytecode and its scalar table are written into the shader's one block.
// Errors longjmp out of the parser with "line L:C: message". All working
// state is one temporary allocation; recursion is capped (CS_MAX_NEST)
// because Lua-called C runs on the 64 KB app stack.
#include "charshader_internal.h"

#include <float.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CS_MAX_NODES 512
#define CS_MAX_NAMES 128
#define CS_MAX_NEST 16

_Static_assert(CS_MAX_SCALARS - CS_SC_CONST0 == 237, "the constants message says 237");

enum { VK_CONST, VK_SCALAR, VK_NODE };
typedef struct {
  uint8_t kind;
  uint16_t i;  // VK_SCALAR: the slot; VK_NODE: the node
  float k;     // VK_CONST
} val_t;

typedef struct {
  uint8_t op, aux, nargs, reg;
  bool live;
  uint16_t uses;
  int16_t last;  // the last live node reading it; CS_MAX_NODES when an output
  val_t arg[4];
} node_t;

enum { NK_VAR, NK_INPUT, NK_UNIFORM, NK_ARRAY, NK_LAYER };
enum { IN_X, IN_Y, IN_U, IN_V, IN_COLS, IN_ROWS };

typedef struct {
  char name[CS_MAX_NAME + 1];
  uint8_t kind, slot;
  int8_t plane;  // glyph, fg, bg: the plane read before any assignment; else -1
  bool has_val, assigned;
  val_t val;
} name_t;

enum {
  T_EOF = 256, T_NL, T_NAME, T_NUM, T_UNIFORM, T_ARRAY, T_LAYER, T_AND, T_OR, T_NOT,
  T_LE, T_GE, T_EQ, T_NE
};

typedef struct {
  const char *src;
  size_t len, pos, line_start;
  int line, depth, nest;
  int tok, tok_line, tok_col;
  float num;
  char word[CS_MAX_NAME + 1];
  node_t nodes[CS_MAX_NODES];
  int nnodes;
  // Memoised loads (node + 1; 0 = none): x y u v, the three planes, the layer fields.
  int load[4 + 3 + CS_MAX_LAYERS * 3];
  name_t names[CS_MAX_NAMES];
  int nnames, nuniforms, narrays, nlayers;
  char uniform_names[CS_MAX_UNIFORMS][CS_MAX_NAME + 1];
  char array_names[CS_MAX_ARRAYS][CS_MAX_NAME + 1];
  float consts[CS_MAX_SCALARS];
  int nconsts;
  char *err;
  size_t err_len;
  jmp_buf fail;
} comp_t;

typedef struct {
  const char *name;
  uint8_t op, min, max;
} func_t;

static const func_t k_funcs[] = {
    {"sin", CS_OP_SIN, 1, 1},     {"cos", CS_OP_COS, 1, 1},       {"abs", CS_OP_ABS, 1, 1},
    {"floor", CS_OP_FLOOR, 1, 1}, {"ceil", CS_OP_CEIL, 1, 1},     {"fract", CS_OP_FRACT, 1, 1},
    {"sqrt", CS_OP_SQRT, 1, 1},   {"exp", CS_OP_EXP, 1, 1},       {"log", CS_OP_LOG, 1, 1},
    {"pow", CS_OP_POW, 2, 2},     {"min", CS_OP_MIN, 2, 2},       {"max", CS_OP_MAX, 2, 2},
    {"clamp", CS_OP_CLAMP, 3, 3}, {"mix", CS_OP_MIX, 3, 3},       {"smooth", CS_OP_SMOOTH, 3, 3},
    {"step", CS_OP_STEP, 2, 2},   {"select", CS_OP_SELECT, 3, 3}, {"dist", CS_OP_DIST, 4, 4},
    {"atan2", CS_OP_ATAN2, 2, 2}, {"hash", CS_OP_HASH, 2, 3},     {"noise", CS_OP_NOISE, 2, 3},
    {"fbm", CS_OP_FBM, 3, 4},
};

static const char *const k_out_names[CS_OUT_COUNT] = {"keep", "glyph", "level", "fg", "r",
                                                    "g",    "b",     "tone",  "bg", "bgtone"};

// ── errors ───────────────────────────────────────────────────────────────────
static void fail_at(comp_t *c, int line, int col, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 4, 5)));
static void fail_at(comp_t *c, int line, int col, const char *fmt, ...) {
  const int n = snprintf(c->err, c->err_len, "line %d:%d: ", line, col);
  if (n >= 0 && (size_t)n < c->err_len) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err + n, c->err_len - (size_t)n, fmt, ap);
    va_end(ap);
  }
  longjmp(c->fail, 1);
}
#define FAIL(...) fail_at(c, c->tok_line, c->tok_col, __VA_ARGS__)

static void fail_plain(comp_t *c, const char *msg) __attribute__((noreturn));
static void fail_plain(comp_t *c, const char *msg) {
  snprintf(c->err, c->err_len, "%s", msg);
  longjmp(c->fail, 1);
}

// ── lexer ────────────────────────────────────────────────────────────────────
static bool is_alpha(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static bool is_digit(int ch) { return ch >= '0' && ch <= '9'; }
static int peekc(const comp_t *c, size_t k) {
  return c->pos + k < c->len ? (unsigned char)c->src[c->pos + k] : -1;
}

static void lex_number(comp_t *c) {
  const size_t start = c->pos;
  if (peekc(c, 0) == '0' && (peekc(c, 1) == 'x' || peekc(c, 1) == 'X')) {
    c->pos += 2;
    uint32_t v = 0;
    int digits = 0;
    for (;;) {
      const int h = peekc(c, 0);
      const int d = is_digit(h) ? h - '0'
                    : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                    : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
      if (d < 0) break;
      if (v > 0x0FFFFFFFu) FAIL("malformed number");
      v = v * 16u + (uint32_t)d;
      digits++;
      c->pos++;
    }
    if (!digits || is_alpha(peekc(c, 0)) || peekc(c, 0) == '.') FAIL("malformed number");
    c->num = (float)v;
    c->tok = T_NUM;
    return;
  }
  while (is_digit(peekc(c, 0))) c->pos++;
  if (peekc(c, 0) == '.') {
    c->pos++;
    while (is_digit(peekc(c, 0))) c->pos++;
  }
  if (peekc(c, 0) == 'e' || peekc(c, 0) == 'E') {
    c->pos++;
    if (peekc(c, 0) == '+' || peekc(c, 0) == '-') c->pos++;
    if (!is_digit(peekc(c, 0))) FAIL("malformed number");
    while (is_digit(peekc(c, 0))) c->pos++;
  }
  if (is_alpha(peekc(c, 0)) || peekc(c, 0) == '.') FAIL("malformed number");
  char buf[64];
  const size_t n = c->pos - start;
  if (n >= sizeof buf) FAIL("malformed number");
  memcpy(buf, c->src + start, n);
  buf[n] = '\0';
  const double d = strtod(buf, NULL);
  c->num = d > (double)FLT_MAX ? CS_INF : (float)d;
  c->tok = T_NUM;
}

static void next(comp_t *c) {
  for (;;) {  // spaces, comments, and newlines inside ( ) and [ ]
    const int ch = peekc(c, 0);
    if (ch == ' ' || ch == '\t' || ch == '\r') {
      c->pos++;
    } else if (ch == '-' && peekc(c, 1) == '-') {
      while (c->pos < c->len && c->src[c->pos] != '\n') c->pos++;
    } else if (ch == '\n' && c->depth > 0) {
      c->pos++;
      c->line++;
      c->line_start = c->pos;
    } else {
      break;
    }
  }
  c->tok_line = c->line;
  c->tok_col = (int)(c->pos - c->line_start) + 1;
  const int ch = peekc(c, 0);
  if (ch < 0) {
    c->tok = T_EOF;
    return;
  }
  if (ch == '\n') {
    c->pos++;
    c->line++;
    c->line_start = c->pos;
    c->tok = T_NL;
    return;
  }
  if (is_alpha(ch)) {
    size_t n = 0;
    while (is_alpha(peekc(c, 0)) || is_digit(peekc(c, 0))) {
      if (n == CS_MAX_NAME) FAIL("name is longer than %d characters", CS_MAX_NAME);
      c->word[n++] = c->src[c->pos++];
    }
    c->word[n] = '\0';
    static const struct {
      const char *w;
      int tok;
    } kw[] = {{"uniform", T_UNIFORM}, {"array", T_ARRAY}, {"layer", T_LAYER},
              {"and", T_AND},         {"or", T_OR},       {"not", T_NOT}};
    c->tok = T_NAME;
    for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++)
      if (!strcmp(c->word, kw[i].w)) c->tok = kw[i].tok;
    return;
  }
  if (is_digit(ch) || (ch == '.' && is_digit(peekc(c, 1)))) {
    lex_number(c);
    return;
  }
  c->pos++;
  const int nx = peekc(c, 0);
  switch (ch) {
    case '<':
      if (nx == '=') { c->pos++; c->tok = T_LE; return; }
      break;
    case '>':
      if (nx == '=') { c->pos++; c->tok = T_GE; return; }
      break;
    case '=':
      if (nx == '=') { c->pos++; c->tok = T_EQ; return; }
      break;
    case '~':
      if (nx == '=') { c->pos++; c->tok = T_NE; return; }
      FAIL("unexpected character '~'");
    case '(':
    case '[':
      c->depth++;
      break;
    case ')':
    case ']':
      if (c->depth > 0) c->depth--;
      break;
    case '+': case '-': case '*': case '/': case '%': case '^': case ',': case '.': case ';':
      break;
    default:
      if (ch >= 0x21 && ch <= 0x7E) FAIL("unexpected character '%c'", ch);
      FAIL("unexpected byte 0x%02X", (unsigned)ch);
  }
  c->tok = ch;
}

static void expect(comp_t *c, int tok, const char *what) {
  if (c->tok != tok) FAIL("expected %s", what);
  next(c);
}

// ── names ────────────────────────────────────────────────────────────────────
static name_t *find(comp_t *c, const char *w) {
  for (int i = 0; i < c->nnames; i++)
    if (!strcmp(c->names[i].name, w)) return &c->names[i];
  return NULL;
}

static const func_t *find_func(const char *w) {
  for (size_t i = 0; i < sizeof k_funcs / sizeof k_funcs[0]; i++)
    if (!strcmp(k_funcs[i].name, w)) return &k_funcs[i];
  return NULL;
}

static name_t *add_name(comp_t *c, const char *w, int kind, int slot) {
  if (c->nnames == CS_MAX_NAMES) FAIL("more than %d names", CS_MAX_NAMES);
  name_t *n = &c->names[c->nnames++];
  memset(n, 0, sizeof *n);
  snprintf(n->name, sizeof n->name, "%s", w);
  n->kind = (uint8_t)kind;
  n->slot = (uint8_t)slot;
  n->plane = -1;
  return n;
}

// ── values and nodes ─────────────────────────────────────────────────────────
static val_t konst(float k) {
  const val_t v = {VK_CONST, 0, k};
  return v;
}
static val_t scalar(int slot) {
  const val_t v = {VK_SCALAR, (uint16_t)slot, 0.0f};
  return v;
}

// Swapping operands is exact for these (min and max are not: NaN picks a side).
static bool commutative(int op) {
  return op == CS_OP_ADD || op == CS_OP_MUL || op == CS_OP_EQ || op == CS_OP_NE ||
         op == CS_OP_AND || op == CS_OP_OR;
}

// A node for op over args[0..nargs) (args always has 4 initialised entries),
// or the folded constant.
static val_t make(comp_t *c, int op, int aux, int nargs, const val_t *args) {
  bool all_const = nargs > 0 && op != CS_OP_ARRAY;
  for (int i = 0; i < nargs; i++) all_const = all_const && args[i].kind == VK_CONST;
  if (all_const) return konst(cs_eval(op, aux, args[0].k, args[1].k, args[2].k, args[3].k));
  val_t a[4] = {args[0], args[1], args[2], args[3]};
  if (op == CS_OP_DIV && a[1].kind == VK_CONST) {  // x / k is x * (1 / k)
    op = CS_OP_MUL;
    a[1] = konst(1.0f / a[1].k);
  }
  if (commutative(op) && a[0].kind != VK_NODE && a[1].kind == VK_NODE) {
    const val_t t = a[0];
    a[0] = a[1];
    a[1] = t;
  }
  if (c->nnodes == CS_MAX_NODES) FAIL("program is too long");
  node_t *n = &c->nodes[c->nnodes];
  memset(n, 0, sizeof *n);
  n->op = (uint8_t)op;
  n->aux = (uint8_t)aux;
  n->nargs = (uint8_t)nargs;
  memcpy(n->arg, a, sizeof a);
  const val_t v = {VK_NODE, (uint16_t)c->nnodes++, 0.0f};
  return v;
}

static val_t bin(comp_t *c, int op, val_t a, val_t b) {
  const val_t args[4] = {a, b, konst(0.0f), konst(0.0f)};
  return make(c, op, 0, 2, args);
}

static val_t load(comp_t *c, int op, int aux, int memo) {
  if (c->load[memo]) {
    const val_t v = {VK_NODE, (uint16_t)(c->load[memo] - 1), 0.0f};
    return v;
  }
  const val_t none[4] = {konst(0.0f), konst(0.0f), konst(0.0f), konst(0.0f)};
  const val_t v = make(c, op, aux, 0, none);
  c->load[memo] = v.i + 1;
  return v;
}

// ── expressions ──────────────────────────────────────────────────────────────
static val_t expr(comp_t *c);
static val_t unary(comp_t *c);

static void enter(comp_t *c) {
  if (++c->nest > CS_MAX_NEST) FAIL("expression nests more than %d deep", CS_MAX_NEST);
}

static val_t call(comp_t *c, const char *fname, int line, int col) {
  const func_t *f = find_func(fname);
  if (!f) {
    if (find(c, fname)) fail_at(c, line, col, "'%s' is not a function", fname);
    fail_at(c, line, col, "unknown function '%s'", fname);
  }
  next(c);  // '('
  val_t args[4] = {konst(0.0f), konst(0.0f), konst(0.0f), konst(0.0f)};
  int n = 0;
  if (c->tok != ')') {
    for (;;) {
      const val_t v = expr(c);
      if (n < 4) args[n] = v;
      n++;
      if (c->tok != ',') break;
      next(c);
    }
  }
  expect(c, ')', "')'");
  if (n < f->min || n > f->max) {
    if (f->min == f->max)
      fail_at(c, line, col, "'%s' takes %d argument%s", fname, f->min, f->min == 1 ? "" : "s");
    fail_at(c, line, col, "'%s' takes %d or %d arguments", fname, f->min, f->max);
  }
  if (f->op == CS_OP_FBM) {
    const float o = args[2].k;
    if (args[2].kind != VK_CONST || o != cs_floor(o) || o < 1.0f || o > (float)NOISE_MAX_OCTAVES)
      fail_at(c, line, col, "fbm octaves must be a constant 1-%d", NOISE_MAX_OCTAVES);
    const val_t a[4] = {args[0], args[1], n == 4 ? args[3] : konst(0.0f), konst(0.0f)};
    return make(c, CS_OP_FBM, (int)o, 3, a);
  }
  if (f->op == CS_OP_HASH || f->op == CS_OP_NOISE) return make(c, f->op, 0, 3, args);  // seed 0 if absent
  return make(c, f->op, 0, f->max, args);
}

static val_t read_name(comp_t *c, name_t *nm, const char *word, int line, int col) {
  if (!nm) {
    if (find_func(word)) fail_at(c, line, col, "'%s' is a function", word);
    fail_at(c, line, col, "unknown name '%s'", word);
  }
  switch (nm->kind) {
    case NK_INPUT:
      if (nm->slot == IN_COLS) return scalar(CS_SC_COLS);
      if (nm->slot == IN_ROWS) return scalar(CS_SC_ROWS);
      return load(c, CS_OP_X + nm->slot, 0, nm->slot);  // IN_X..IN_V match CS_OP_X..CS_OP_V
    case NK_UNIFORM:
      return scalar(CS_SC_UNIFORM0 + nm->slot);
    case NK_ARRAY:
      fail_at(c, line, col, "array '%s' needs an index", word);
    case NK_LAYER:
      fail_at(c, line, col, "layer '%s' needs .glyph, .fg or .bg", word);
    default:  // NK_VAR
      if (!nm->has_val) {  // glyph, fg or bg before any assignment: the plane
        nm->val = load(c, CS_OP_PLANE, nm->plane, 4 + nm->plane);
        nm->has_val = true;
      }
      return nm->val;
  }
}

static val_t primary(comp_t *c) {
  if (c->tok == T_NUM) {
    const float k = c->num;
    next(c);
    return konst(k);
  }
  if (c->tok == '(') {
    next(c);
    const val_t v = expr(c);
    expect(c, ')', "')'");
    return v;
  }
  if (c->tok != T_NAME) FAIL("expected an expression");
  char word[CS_MAX_NAME + 1];
  memcpy(word, c->word, sizeof word);
  const int line = c->tok_line, col = c->tok_col;
  next(c);
  if (c->tok == '(') return call(c, word, line, col);
  name_t *nm = find(c, word);
  if (c->tok == '[') {
    if (!nm) fail_at(c, line, col, "unknown name '%s'", word);
    if (nm->kind != NK_ARRAY) fail_at(c, line, col, "'%s' is not an array", word);
    next(c);
    const val_t idx = expr(c);
    expect(c, ']', "']'");
    const val_t a[4] = {idx, konst(0.0f), konst(0.0f), konst(0.0f)};
    return make(c, CS_OP_ARRAY, nm->slot, 1, a);
  }
  if (c->tok == '.') {
    if (!nm) fail_at(c, line, col, "unknown name '%s'", word);
    if (nm->kind != NK_LAYER) fail_at(c, line, col, "'%s' is not a layer", word);
    next(c);
    if (c->tok != T_NAME) FAIL("expected a field name after '.'");
    const int plane = !strcmp(c->word, "glyph") ? CG_PLANE_GLYPH
                      : !strcmp(c->word, "fg")  ? CG_PLANE_FG
                      : !strcmp(c->word, "bg")  ? CG_PLANE_BG : -1;
    if (plane < 0) FAIL("layers have .glyph, .fg and .bg");
    next(c);
    return load(c, CS_OP_LAYER, nm->slot * 4 + plane, 7 + nm->slot * 3 + plane);
  }
  return read_name(c, nm, word, line, col);
}

// The recursion that source text can drive (brackets, calls and indexes
// through expr, chains of prefix operators, chains of ^) counts against
// CS_MAX_NEST; one level of brackets costs one.
static val_t power(comp_t *c) {  // right associative; binds tighter than unary minus on its left
  const val_t a = primary(c);
  if (c->tok != '^') return a;
  enter(c);
  next(c);
  const val_t b = unary(c);
  c->nest--;
  return bin(c, CS_OP_POW, a, b);
}

static val_t unary(comp_t *c) {
  if (c->tok != '-' && c->tok != T_NOT) return power(c);
  const bool neg = c->tok == '-';
  enter(c);
  next(c);
  const val_t a = unary(c);
  c->nest--;
  return neg ? bin(c, CS_OP_MUL, a, konst(-1.0f))  // x * -1 is exactly -x
             : bin(c, CS_OP_EQ, a, konst(0.0f));
}

static val_t mul(comp_t *c) {
  val_t a = unary(c);
  for (;;) {
    const int t = c->tok;
    if (t != '*' && t != '/' && t != '%') return a;
    next(c);
    const val_t b = unary(c);
    a = bin(c, t == '*' ? CS_OP_MUL : t == '/' ? CS_OP_DIV : CS_OP_MOD, a, b);
  }
}

static val_t add(comp_t *c) {
  val_t a = mul(c);
  for (;;) {
    const int t = c->tok;
    if (t != '+' && t != '-') return a;
    next(c);
    const val_t b = mul(c);
    a = bin(c, t == '+' ? CS_OP_ADD : CS_OP_SUB, a, b);
  }
}

static val_t cmp(comp_t *c) {
  val_t a = add(c);
  for (;;) {
    const int t = c->tok;
    if (t != '<' && t != '>' && t != T_LE && t != T_GE && t != T_EQ && t != T_NE) return a;
    next(c);
    const val_t b = add(c);
    switch (t) {
      case '<': a = bin(c, CS_OP_LT, a, b); break;
      case '>': a = bin(c, CS_OP_LT, b, a); break;
      case T_LE: a = bin(c, CS_OP_LE, a, b); break;
      case T_GE: a = bin(c, CS_OP_LE, b, a); break;
      case T_EQ: a = bin(c, CS_OP_EQ, a, b); break;
      default: a = bin(c, CS_OP_NE, a, b); break;
    }
  }
}

static val_t and_(comp_t *c) {
  val_t a = cmp(c);
  while (c->tok == T_AND) {
    next(c);
    const val_t b = cmp(c);
    a = bin(c, CS_OP_AND, a, b);
  }
  return a;
}

static val_t expr(comp_t *c) {
  enter(c);
  val_t a = and_(c);
  while (c->tok == T_OR) {
    next(c);
    const val_t b = and_(c);
    a = bin(c, CS_OP_OR, a, b);
  }
  c->nest--;
  return a;
}

// ── statements ───────────────────────────────────────────────────────────────
static void declare(comp_t *c) {
  const int kind = c->tok == T_UNIFORM ? NK_UNIFORM : c->tok == T_ARRAY ? NK_ARRAY : NK_LAYER;
  const char *kw = kind == NK_UNIFORM ? "uniform" : kind == NK_ARRAY ? "array" : "layer";
  next(c);
  for (;;) {
    if (c->tok != T_NAME) FAIL("expected a name after '%s'", kw);
    if (find_func(c->word)) FAIL("'%s' is a function", c->word);
    if (find(c, c->word)) FAIL("'%s' is already declared", c->word);
    if (kind == NK_UNIFORM) {
      if (c->nuniforms == CS_MAX_UNIFORMS) FAIL("more than %d uniforms", CS_MAX_UNIFORMS);
      snprintf(c->uniform_names[c->nuniforms], CS_MAX_NAME + 1, "%s", c->word);
      add_name(c, c->word, NK_UNIFORM, c->nuniforms++);
    } else if (kind == NK_ARRAY) {
      if (c->narrays == CS_MAX_ARRAYS) FAIL("more than %d arrays", CS_MAX_ARRAYS);
      snprintf(c->array_names[c->narrays], CS_MAX_NAME + 1, "%s", c->word);
      add_name(c, c->word, NK_ARRAY, c->narrays++);
    } else {
      if (c->nlayers == CS_MAX_LAYERS) FAIL("more than %d layers", CS_MAX_LAYERS);
      add_name(c, c->word, NK_LAYER, c->nlayers++);
    }
    next(c);
    if (c->tok != ',') return;
    next(c);
  }
}

static void statement(comp_t *c) {
  if (c->tok == T_UNIFORM || c->tok == T_ARRAY || c->tok == T_LAYER) {
    declare(c);
    return;
  }
  if (c->tok != T_NAME) FAIL("expected a name or a declaration");
  char word[CS_MAX_NAME + 1];
  memcpy(word, c->word, sizeof word);
  const int line = c->tok_line, col = c->tok_col;
  next(c);
  if (c->tok != '=') FAIL("expected '='");
  next(c);
  if (find_func(word)) fail_at(c, line, col, "'%s' is a function", word);
  name_t *nm = find(c, word);
  if (nm && nm->kind != NK_VAR) fail_at(c, line, col, "'%s' is read-only", word);
  const val_t v = expr(c);  // before binding: a = a + 1 reads the old a
  if (!nm) nm = add_name(c, word, NK_VAR, 0);
  nm->val = v;
  nm->has_val = true;
  nm->assigned = true;
}

static void program(comp_t *c) {
  next(c);
  for (;;) {
    while (c->tok == T_NL || c->tok == ';') next(c);
    if (c->tok == T_EOF) return;
    statement(c);
    if (c->tok != T_NL && c->tok != ';' && c->tok != T_EOF) FAIL("expected a new line or ';'");
  }
}

// ── optimisation, allocation, emission ───────────────────────────────────────
// a * b + c (and a * b - k) where the product has no other use: one MAD.
static void fuse(comp_t *c) {
  for (int i = 0; i < c->nnodes; i++) {
    node_t *n = &c->nodes[i];
    if (!n->live || (n->op != CS_OP_ADD && n->op != CS_OP_SUB)) continue;
    for (int s = 0; s < 2; s++) {
      if (n->op == CS_OP_SUB && (s == 1 || n->arg[1].kind != VK_CONST)) break;
      const val_t m = n->arg[s];
      if (m.kind != VK_NODE) continue;
      node_t *p = &c->nodes[m.i];
      if (p->op != CS_OP_MUL || p->uses != 1) continue;
      const val_t addend = n->op == CS_OP_SUB ? konst(-n->arg[1].k) : n->arg[1 - s];
      n->op = CS_OP_MAD;
      n->nargs = 3;
      n->arg[0] = p->arg[0];
      n->arg[1] = p->arg[1];
      n->arg[2] = addend;
      p->live = false;  // its operands' use counts carry over to n
      break;
    }
  }
}

static int const_slot(comp_t *c, float k) {
  const uint32_t b = cs_bits(k);
  for (int i = 0; i < c->nconsts; i++)
    if (cs_bits(c->consts[i]) == b) return CS_SC_CONST0 + i;
  if (CS_SC_CONST0 + c->nconsts == CS_MAX_SCALARS)
    fail_plain(c, "program has more than 237 distinct constants");
  c->consts[c->nconsts] = k;
  return CS_SC_CONST0 + c->nconsts++;
}

static uint8_t encode(comp_t *c, val_t v, int i, uint8_t *scal) {
  if (v.kind == VK_NODE) return c->nodes[v.i].reg;
  *scal |= (uint8_t)(1u << i);
  return (uint8_t)(v.kind == VK_SCALAR ? v.i : const_slot(c, v.k));
}

// Linear scan in program order: an operand's register is freed at its last
// use, before the result's is chosen, so a result may reuse it (every VM
// loop reads element j of its operands before writing element j).
static int allocate(comp_t *c, const val_t *out, uint16_t used) {
  for (int i = 0; i < c->nnodes; i++) c->nodes[i].last = -1;
  for (int i = 0; i < c->nnodes; i++) {
    const node_t *n = &c->nodes[i];
    if (!n->live) continue;
    for (int j = 0; j < n->nargs; j++)
      if (n->arg[j].kind == VK_NODE) c->nodes[n->arg[j].i].last = (int16_t)i;
  }
  for (int k = 0; k < CS_OUT_COUNT; k++)
    if (((used >> k) & 1u) && out[k].kind == VK_NODE) c->nodes[out[k].i].last = CS_MAX_NODES;
  bool busy[CS_MAX_REGS] = {false};
  int nregs = 0;
  for (int i = 0; i < c->nnodes; i++) {
    node_t *n = &c->nodes[i];
    if (!n->live) continue;
    for (int j = 0; j < n->nargs; j++)
      if (n->arg[j].kind == VK_NODE && c->nodes[n->arg[j].i].last == i)
        busy[c->nodes[n->arg[j].i].reg] = false;
    int r = 0;
    while (r < CS_MAX_REGS && busy[r]) r++;
    if (r == CS_MAX_REGS) fail_plain(c, "program needs more than 32 temporaries");
    busy[r] = true;
    n->reg = (uint8_t)r;
    if (r + 1 > nregs) nregs = r + 1;
  }
  return nregs;
}

static charshader_t *build(comp_t *c) {
  static const char *const inputs[] = {"x", "y", "u", "v", "cols", "rows"};
  for (int i = 0; i < 6; i++) add_name(c, inputs[i], NK_INPUT, i);
  static const char *const planes[] = {"glyph", "fg", "bg"};
  for (int p = 0; p < 3; p++) add_name(c, planes[p], NK_VAR, 0)->plane = (int8_t)p;
  program(c);
  val_t out[CS_OUT_COUNT];
  uint16_t used = 0;
  for (int k = 0; k < CS_OUT_COUNT; k++) {
    const name_t *nm = find(c, k_out_names[k]);
    out[k] = konst(0.0f);
    if (nm && nm->assigned) {
      out[k] = nm->val;
      used |= (uint16_t)(1u << k);
    }
  }
  // Liveness from the outputs back; then use counts; then fusion.
  for (int k = 0; k < CS_OUT_COUNT; k++)
    if (((used >> k) & 1u) && out[k].kind == VK_NODE) c->nodes[out[k].i].live = true;
  for (int i = c->nnodes - 1; i >= 0; i--) {
    const node_t *n = &c->nodes[i];
    if (!n->live) continue;
    for (int j = 0; j < n->nargs; j++)
      if (n->arg[j].kind == VK_NODE) c->nodes[n->arg[j].i].live = true;
  }
  for (int i = 0; i < c->nnodes; i++) {
    const node_t *n = &c->nodes[i];
    if (!n->live) continue;
    for (int j = 0; j < n->nargs; j++)
      if (n->arg[j].kind == VK_NODE) c->nodes[n->arg[j].i].uses++;
  }
  for (int k = 0; k < CS_OUT_COUNT; k++)
    if (((used >> k) & 1u) && out[k].kind == VK_NODE) c->nodes[out[k].i].uses++;
  fuse(c);
  int ninstr = 0;
  for (int i = 0; i < c->nnodes; i++) ninstr += c->nodes[i].live;
  if (ninstr > CS_MAX_INSTR) fail_plain(c, "program needs more than 255 instructions");
  // Every constant gets its slot before anything is allocated (no leak on failure).
  for (int i = 0; i < c->nnodes; i++) {
    const node_t *n = &c->nodes[i];
    if (!n->live) continue;
    for (int j = 0; j < n->nargs; j++)
      if (n->arg[j].kind == VK_CONST) const_slot(c, n->arg[j].k);
  }
  for (int k = 0; k < CS_OUT_COUNT; k++)
    if (((used >> k) & 1u) && out[k].kind == VK_CONST) const_slot(c, out[k].k);
  const int nregs = allocate(c, out, used);

  const size_t floats = (size_t)nregs * CS_STRIP + (size_t)CS_OUT_COUNT * CS_STRIP +
                        (size_t)c->narrays * CS_MAX_ARRAY_LEN;
  const size_t bytes = sizeof(charshader_t) + floats * sizeof(float) + (size_t)ninstr * sizeof(cs_ins_t);
  charshader_t *s = (charshader_t *)CG_MALLOC(bytes);
  if (!s) fail_plain(c, "out of memory");
  memset(s, 0, bytes);
  float *f = (float *)(s + 1);
  s->regs = f;
  f += (size_t)nregs * CS_STRIP;
  s->scratch = f;
  f += (size_t)CS_OUT_COUNT * CS_STRIP;
  s->arrays = f;
  f += (size_t)c->narrays * CS_MAX_ARRAY_LEN;
  s->ins = (cs_ins_t *)f;
  int k = 0;
  for (int i = 0; i < c->nnodes; i++) {
    const node_t *n = &c->nodes[i];
    if (!n->live) continue;
    cs_ins_t *in = &s->ins[k++];
    in->op = n->op;
    in->d = n->reg;
    in->aux = n->aux;
    for (int j = 0; j < n->nargs; j++) in->a[j] = encode(c, n->arg[j], j, &in->scal);
  }
  for (int o = 0; o < CS_OUT_COUNT; o++) {
    if (!((used >> o) & 1u)) continue;
    uint8_t sb = 0;
    s->out_ref[o] = encode(c, out[o], 0, &sb);
    if (sb) s->out_scal |= (uint16_t)(1u << o);
  }
  s->out_used = used;
  s->ninstr = ninstr;
  s->nregs = nregs;
  s->nscalars = CS_SC_CONST0 + c->nconsts;
  s->nuniforms = c->nuniforms;
  s->narrays = c->narrays;
  s->nlayers = c->nlayers;
  for (int i = 0; i < c->nconsts; i++) s->sc[CS_SC_CONST0 + i] = c->consts[i];
  memcpy(s->uniform_names, c->uniform_names, sizeof s->uniform_names);
  memcpy(s->array_names, c->array_names, sizeof s->array_names);
  return s;
}

charshader_t *charshader_compile(const char *src, size_t len, char *err, size_t err_len) {
  char dummy[1];
  if (!err || !err_len) {
    err = dummy;
    err_len = sizeof dummy;
  }
  err[0] = '\0';
  if (len > CS_MAX_SOURCE) {
    snprintf(err, err_len, "source is longer than %d bytes", CS_MAX_SOURCE);
    return NULL;
  }
  comp_t *c = (comp_t *)CG_MALLOC(sizeof *c);
  if (!c) {
    snprintf(err, err_len, "out of memory");
    return NULL;
  }
  memset(c, 0, sizeof *c);
  c->src = src;
  c->len = len;
  c->line = 1;
  c->err = err;
  c->err_len = err_len;
  charshader_t *volatile s = NULL;
  if (setjmp(c->fail) == 0) s = build(c);
  CG_FREE(c);
  return s;
}

void charshader_free(charshader_t *s) {
  if (s) CG_FREE(s);
}

int charshader_layer_count(const charshader_t *s) { return s->nlayers; }

void charshader_info(const charshader_t *s, charshader_info_t *out) {
  out->instructions = s->ninstr;
  out->registers = s->nregs;
  out->scalars = s->nscalars;
  out->uniforms = s->nuniforms;
  out->arrays = s->narrays;
  out->layers = s->nlayers;
}
