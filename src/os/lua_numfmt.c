// Float formatting for Lua that does not depend on the C library.
// See lua_numfmt.h for why; cmake/picodeck_lua.cmake routes Lua's l_sprintf here.
//
// Method: a float32 is m * 2^e with m < 2^24 and -149 <= e <= 104, so its
// exact decimal value has at most 39 integer and 149 fraction digits. It is
// expanded exactly (m * 2^e, or m * 5^-e with the point moved -e places)
// in a base-10^9 bignum, then rounded half-to-even at the requested digit,
// which is what glibc does. Everything lives on the caller's stack (about
// 300 bytes); nothing is allocated.

#include "lua_numfmt.h"

// Lua's l_sprintf reaches picodeck_lua_sprintf only through the luaconf.h patch
// (cmake/picodeck_lua.cmake).  If a build defines PICODECK_LUA_SPRINTF but compiles
// against an unpatched luaconf.h, floats silently go through the C library
// again (pico_printf on the firmware): fail the build instead.
#if defined(PICODECK_LUA_SPRINTF)
#include "luaconf.h"
#if !defined(PICODECK_LUA_SPRINTF_PATCHED)
#error "PICODECK_LUA_SPRINTF is set but luaconf.h lacks the PicoDeck l_sprintf patch (run cmake: picodeck_patch_luaconf)"
#endif
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NUMFMT_MAX_PREC   150
#define NUMFMT_MAX_DIGITS 120  // exact digits of any float32: <= 112
#define NUMFMT_LIMBS      14   // base-1e9 limbs: m * 5^149 < 10^112
#define NUMFMT_BODY       (NUMFMT_MAX_PREC + 64)

enum {
  FL_MINUS = 1,
  FL_PLUS = 2,
  FL_SPACE = 4,
  FL_HASH = 8,
  FL_ZERO = 16,
};

typedef struct {
  int flags;
  int width;
  int prec;  // -1: not given
  char conv;
} numfmt_spec_t;

typedef struct {
  char *buf;
  size_t size;
  size_t len;  // full length, may exceed size
} numfmt_out_t;

// Exact decimal expansion of a finite value: digits d[0..n) without leading
// or trailing zeros, value = d0.d1d2... * 10^dexp. Zero is n == 0, dexp == 0.
typedef struct {
  char d[NUMFMT_MAX_DIGITS];
  int n;
  int dexp;
} numfmt_dec_t;

static void out_char(numfmt_out_t *o, char c) {
  if (o->len + 1 < o->size)
    o->buf[o->len] = c;
  o->len++;
}

static void out_rep(numfmt_out_t *o, char c, int n) {
  while (n-- > 0)
    out_char(o, c);
}

static void out_mem(numfmt_out_t *o, const char *s, size_t n) {
  while (n--)
    out_char(o, *s++);
}

static void out_finish(numfmt_out_t *o) {
  if (o->size > 0)
    o->buf[o->len < o->size ? o->len : o->size - 1] = '\0';
}

// limb[0..*nl) *= mult (mult <= 2^32).
static void big_mul(uint32_t *limb, int *nl, uint64_t mult) {
  uint64_t carry = 0;
  for (int i = 0; i < *nl; i++) {
    uint64_t t = (uint64_t)limb[i] * mult + carry;
    limb[i] = (uint32_t)(t % 1000000000u);
    carry = t / 1000000000u;
  }
  while (carry && *nl < NUMFMT_LIMBS) {
    limb[(*nl)++] = (uint32_t)(carry % 1000000000u);
    carry /= 1000000000u;
  }
}

// value = m * 2^e, m nonzero and < 2^24.
static void dec_from_parts(numfmt_dec_t *x, uint32_t m, int e) {
  static const uint32_t pow5[14] = {
      1u, 5u, 25u, 125u, 625u, 3125u, 15625u, 78125u, 390625u, 1953125u,
      9765625u, 48828125u, 244140625u, 1220703125u};
  uint32_t limb[NUMFMT_LIMBS];
  int nl = 1;
  int frac = 0;  // number of fraction digits in the integer we build
  limb[0] = m;   // m < 2^24 < 10^9
  if (e >= 0) {
    while (e > 0) {
      int k = e > 32 ? 32 : e;
      big_mul(limb, &nl, (uint64_t)1 << k);
      e -= k;
    }
  } else {
    frac = -e;  // m / 2^k == m * 5^k / 10^k
    for (int k = frac; k > 0;) {
      int s = k > 13 ? 13 : k;
      big_mul(limb, &nl, pow5[s]);
      k -= s;
    }
  }
  // Limbs to digits, most significant first.
  char tmp[NUMFMT_LIMBS * 9 + 1];
  int len = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)limb[nl - 1]);
  for (int i = nl - 2; i >= 0; i--) {
    uint32_t v = limb[i];
    for (int j = 8; j >= 0; j--) {
      tmp[len + j] = (char)('0' + v % 10);
      v /= 10;
    }
    len += 9;
  }
  x->dexp = len - frac - 1;
  while (len > 0 && tmp[len - 1] == '0')
    len--;
  if (len > NUMFMT_MAX_DIGITS)
    len = NUMFMT_MAX_DIGITS;  // unreachable for float32
  memcpy(x->d, tmp, (size_t)len);
  x->n = len;
}

// Rounds x to `keep` significant digits (keep may be <= 0), half to even.
static void dec_round(numfmt_dec_t *x, int keep) {
  if (keep >= x->n)
    return;
  if (keep < 0) {  // value < 0.1 unit of the last kept place
    x->n = 0;
    x->dexp = 0;
    return;
  }
  char cut = x->d[keep];
  bool up;
  if (cut != '5') {
    up = cut > '5';
  } else if (keep + 1 < x->n) {
    up = true;  // n excludes trailing zeros, so something nonzero follows
  } else {
    up = keep > 0 && ((x->d[keep - 1] - '0') & 1);  // exact tie: to even
  }
  x->n = keep;
  if (up) {
    int i = keep - 1;
    while (i >= 0 && x->d[i] == '9')
      i--;
    if (i < 0) {  // 999 -> 1000, or nothing kept -> 1 at the next place
      x->d[0] = '1';
      x->n = 1;
      x->dexp++;
      return;
    }
    x->d[i]++;
    x->n = i + 1;
  }
  while (x->n > 0 && x->d[x->n - 1] == '0')
    x->n--;
  if (x->n == 0)
    x->dexp = 0;
}

static char dec_digit(const numfmt_dec_t *x, int i) {
  return (i >= 0 && i < x->n) ? x->d[i] : '0';
}

// %e body (no sign): d.ddde+XX
static int body_e(char *b, numfmt_dec_t *x, int prec, bool hash, bool upper) {
  int n = 0;
  dec_round(x, prec + 1);
  b[n++] = dec_digit(x, 0);
  if (prec > 0 || hash)
    b[n++] = '.';
  for (int i = 1; i <= prec; i++)
    b[n++] = dec_digit(x, i);
  int ex = x->n ? x->dexp : 0;
  b[n++] = upper ? 'E' : 'e';
  b[n++] = ex < 0 ? '-' : '+';
  if (ex < 0)
    ex = -ex;
  if (ex >= 100)
    b[n++] = (char)('0' + ex / 100);
  b[n++] = (char)('0' + ex / 10 % 10);
  b[n++] = (char)('0' + ex % 10);
  return n;
}

// %f body (no sign): ddd.ddd
static int body_f(char *b, numfmt_dec_t *x, int prec, bool hash) {
  int n = 0;
  if (x->n)
    dec_round(x, x->dexp + 1 + prec);
  if (x->dexp < 0 || x->n == 0) {
    b[n++] = '0';
  } else {
    for (int i = 0; i <= x->dexp; i++)
      b[n++] = dec_digit(x, i);
  }
  if (prec > 0 || hash)
    b[n++] = '.';
  for (int j = 1; j <= prec; j++)
    b[n++] = x->n ? dec_digit(x, x->dexp + j) : '0';
  return n;
}

// Removes trailing zeros (and a bare point) from the fraction of a %g body.
static int strip_g(char *b, int n) {
  int end = n;  // end of the mantissa
  for (int i = 0; i < n; i++)
    if (b[i] == 'e' || b[i] == 'E') {
      end = i;
      break;
    }
  bool point = false;
  for (int i = 0; i < end; i++)
    if (b[i] == '.')
      point = true;
  if (!point)
    return n;
  int k = end;
  while (k > 0 && b[k - 1] == '0')
    k--;
  if (k > 0 && b[k - 1] == '.')
    k--;
  memmove(b + k, b + end, (size_t)(n - end));
  return n - (end - k);
}

// %a body without the 0x prefix: h.hhhp+d. v is finite.
static int body_a(char *b, uint32_t bits, int prec, bool hash, bool upper) {
  const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  uint32_t exp_bits = (bits >> 23) & 0xFF;
  uint32_t mant = bits & 0x7FFFFF;
  int n = 0;
  int lead;
  int e2;
  uint64_t f52;  // fraction as a double's 52-bit field
  if (exp_bits == 0 && mant == 0) {
    lead = 0;
    e2 = 0;
    f52 = 0;
  } else {
    if (exp_bits == 0) {  // subnormal float: normal once promoted to double
      int s = 0;
      while (!(mant & 0x800000)) {
        mant <<= 1;
        s++;
      }
      e2 = -126 - s;
      mant &= 0x7FFFFF;
    } else {
      e2 = (int)exp_bits - 127;
    }
    lead = 1;
    f52 = (uint64_t)mant << 29;
  }
  int digits = 13;
  if (prec >= 0 && prec < 13) {
    int drop = 4 * (13 - prec);
    uint64_t rem = f52 & (((uint64_t)1 << drop) - 1);
    uint64_t half = (uint64_t)1 << (drop - 1);
    f52 >>= drop;
    // Ties to even on the last digit kept: at precision 0 that is the
    // leading digit (0x1.8p+0 -> 0x2p+0, as glibc prints).
    bool odd = prec > 0 ? (f52 & 1) != 0 : (lead & 1) != 0;
    if (rem > half || (rem == half && odd))
      f52++;
    if (f52 >> (4 * prec)) {  // carried into the leading digit: 0x2p+0
      lead++;
      f52 &= ((uint64_t)1 << (4 * prec)) - 1;
    }
    digits = prec;
  } else if (prec < 0) {
    while (digits > 0 && ((f52 >> (4 * (13 - digits))) & 0xF) == 0)
      digits--;
    f52 >>= 4 * (13 - digits);
  }
  b[n++] = hex[lead];
  int shown = prec > digits ? prec : digits;
  if (shown > 0 || hash)
    b[n++] = '.';
  for (int i = 0; i < digits; i++)
    b[n++] = hex[(f52 >> (4 * (digits - 1 - i))) & 0xF];
  for (int i = digits; i < shown; i++)
    b[n++] = '0';
  n += snprintf(b + n, 8, "%c%+d", upper ? 'P' : 'p', e2);
  return n;
}

static void fmt_float(numfmt_out_t *o, const numfmt_spec_t *sp, float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof(bits));
  bool upper = sp->conv == 'A' || sp->conv == 'E' || sp->conv == 'F' ||
               sp->conv == 'G';
  bool hash = (sp->flags & FL_HASH) != 0;
  int prec = sp->prec > NUMFMT_MAX_PREC ? NUMFMT_MAX_PREC : sp->prec;
  uint32_t exp_bits = (bits >> 23) & 0xFF;
  bool nan = exp_bits == 0xFF && (bits & 0x7FFFFF);
  bool finite = exp_bits != 0xFF;
  bool neg = (bits >> 31) && !nan;

  char body[NUMFMT_BODY];
  int blen;
  const char *prefix = "";
  if (!finite) {
    memcpy(body, nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"), 3);
    blen = 3;
  } else if (sp->conv == 'a' || sp->conv == 'A') {
    prefix = upper ? "0X" : "0x";
    blen = body_a(body, bits, prec, hash, upper);
  } else {
    numfmt_dec_t x;
    x.n = 0;
    x.dexp = 0;
    uint32_t mant = bits & 0x7FFFFF;
    if (exp_bits != 0)
      dec_from_parts(&x, mant | 0x800000, (int)exp_bits - 150);
    else if (mant != 0)
      dec_from_parts(&x, mant, -149);
    if (prec < 0)
      prec = 6;
    switch (sp->conv) {
      case 'e': case 'E':
        blen = body_e(body, &x, prec, hash, upper);
        break;
      case 'f': case 'F':
        blen = body_f(body, &x, prec, hash);
        break;
      default: {  // g, G
        int p = prec == 0 ? 1 : prec;
        numfmt_dec_t r = x;
        dec_round(&r, p);
        int ex = r.n ? r.dexp : 0;
        if (ex < p && ex >= -4)
          blen = body_f(body, &x, p - 1 - ex, hash);
        else
          blen = body_e(body, &x, p - 1, hash, upper);
        if (!hash)
          blen = strip_g(body, blen);
        break;
      }
    }
  }

  char sign = neg ? '-' : (sp->flags & FL_PLUS) ? '+' : (sp->flags & FL_SPACE) ? ' ' : 0;
  size_t plen = strlen(prefix);
  int total = (sign ? 1 : 0) + (int)plen + blen;
  int pad = sp->width > total ? sp->width - total : 0;
  if (sp->flags & FL_MINUS) {
    if (sign) out_char(o, sign);
    out_mem(o, prefix, plen);
    out_mem(o, body, (size_t)blen);
    out_rep(o, ' ', pad);
  } else if ((sp->flags & FL_ZERO) && finite) {
    if (sign) out_char(o, sign);
    out_mem(o, prefix, plen);
    out_rep(o, '0', pad);
    out_mem(o, body, (size_t)blen);
  } else {
    out_rep(o, ' ', pad);
    if (sign) out_char(o, sign);
    out_mem(o, prefix, plen);
    out_mem(o, body, (size_t)blen);
  }
}

// Parses "%[flags][width][.prec][length]conv" at s. Returns the character
// after conv, or NULL if malformed. Length modifiers are skipped.
static const char *parse_spec(const char *s, numfmt_spec_t *sp) {
  if (*s++ != '%')
    return NULL;
  sp->flags = 0;
  sp->width = 0;
  sp->prec = -1;
  for (;; s++) {
    if (*s == '-') sp->flags |= FL_MINUS;
    else if (*s == '+') sp->flags |= FL_PLUS;
    else if (*s == ' ') sp->flags |= FL_SPACE;
    else if (*s == '#') sp->flags |= FL_HASH;
    else if (*s == '0') sp->flags |= FL_ZERO;
    else break;
  }
  while (*s >= '0' && *s <= '9' && sp->width < 10000)
    sp->width = sp->width * 10 + (*s++ - '0');
  if (*s == '.') {
    s++;
    sp->prec = 0;
    while (*s >= '0' && *s <= '9' && sp->prec < 10000)
      sp->prec = sp->prec * 10 + (*s++ - '0');
  }
  while (*s && strchr("hlLqjzt", *s))
    s++;
  if (!*s)
    return NULL;
  sp->conv = *s++;
  return s;
}

static bool is_float_conv(char c) {
  return c && strchr("aAeEfFgG", c) != NULL;
}

int picodeck_numfmt_float(char *buf, size_t size, const char *spec, float v) {
  numfmt_spec_t sp;
  const char *end = parse_spec(spec, &sp);
  if (!end || *end || !is_float_conv(sp.conv))
    return -1;
  numfmt_out_t o = {buf, size, 0};
  fmt_float(&o, &sp, v);
  out_finish(&o);
  return (int)o.len;
}

// Copies literal format text, turning "%%" into "%".
static void out_literal(numfmt_out_t *o, const char *s, const char *end) {
  while (s < end) {
    if (s[0] == '%' && s + 1 < end && s[1] == '%')
      s++;
    out_char(o, *s++);
  }
}

// "%d" of one int, with snprintf's return and truncation. Lua formats every
// integer it turns into text this way (lua_integer2str: tostring, `..`,
// lua_pushfstring %d/%I, and string.format's plain %d), always with an int
// argument (LUAI_UACINT: lua_Integer is 32-bit), so it skips the C library's
// vsnprintf, a third of a tostring(i) on the device.
static int fmt_int_d(char *buf, size_t size, int v) {
  char tmp[12];  // "-2147483648" is 11
  int n = 0;
  unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
  do {
    tmp[n++] = (char)('0' + u % 10u);
    u /= 10u;
  } while (u);
  if (v < 0) tmp[n++] = '-';
  if (size) {
    size_t k = (size_t)n < size - 1 ? (size_t)n : size - 1;
    for (size_t i = 0; i < k; i++) buf[i] = tmp[n - 1 - (int)i];
    buf[k] = '\0';
  }
  return n;
}

int picodeck_lua_sprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (fmt[0] == '%' && fmt[1] == 'd' && fmt[2] == '\0') {
    int r = fmt_int_d(buf, size, va_arg(ap, int));
    va_end(ap);
    return r;
  }
  const char *pct = NULL;
  for (const char *p = fmt; *p; p++) {
    if (*p != '%')
      continue;
    if (p[1] == '%') {
      p++;
      continue;
    }
    pct = p;
    break;
  }
  numfmt_spec_t sp;
  const char *after = pct ? parse_spec(pct, &sp) : NULL;
  int r;
  if (after && is_float_conv(sp.conv)) {
    double d = va_arg(ap, double);  // a lua_Number promoted: exact as float
    numfmt_out_t o = {buf, size, 0};
    out_literal(&o, fmt, pct);
    fmt_float(&o, &sp, (float)d);
    out_literal(&o, after, after + strlen(after));
    out_finish(&o);
    r = (int)o.len;
  } else {
    r = vsnprintf(buf, size, fmt, ap);
  }
  va_end(ap);
  return r;
}
