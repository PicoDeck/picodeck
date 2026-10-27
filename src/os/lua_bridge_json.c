// picocalc.json — JSON encode/decode for Lua apps.
//
// Added because there was no JSON anywhere in the Lua SDK: every app that
// needed structured persistence hand-rolled a serializer (apps/calculator,
// apps/picoforge, apps/ssh, ...), the launcher carries its own minimal parser,
// and game.save's flat encoder silently turned nested tables into `null`.
//
// Design notes:
//
//  * Both directions are ITERATIVE where recursion depth would track input
//    size, and depth-capped at JSON_MAX_DEPTH otherwise. Lua is built with
//    LUAI_MAXSTACK=1000 and runs on a fixed 64 KB C stack (lua_runner.c), so
//    a deeply nested document must produce a clean Lua error rather than a
//    stack-limit hard fault.
//
//  * decode returns `nil, errmsg` (never raises) so callers can handle bad
//    input from the network or a corrupted SD file without pcall. encode DOES
//    raise, because its failure modes are programmer errors (cycle, unsupported
//    type) rather than untrusted data.
//
//  * Lua has one table type for both arrays and objects. We treat a table as an
//    array iff it has at least one element and its integer keys are exactly
//    1..n with no other keys. An empty table encodes as `{}`.
//
//  * json.null is a unique sentinel userdata. Storing a real nil in a table
//    erases the key, so round-tripping `{"a":null}` needs a stand-in.

#include "lua_bridge_internal.h"
#include "lua_numfmt.h"
#include <math.h>

#define JSON_MAX_DEPTH 32

// ── json.null sentinel ───────────────────────────────────────────────────────

#define JSON_NULL_MT "picocalc.json.null"

static int json_null_tostring(lua_State *L) {
  lua_pushstring(L, "json.null");
  return 1;
}

// Pushes the singleton null sentinel onto the stack.
static void json_push_null(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, JSON_NULL_MT);
}

static bool json_is_null(lua_State *L, int idx) {
  if (lua_type(L, idx) != LUA_TUSERDATA) return false;
  idx = lua_absindex(L, idx);
  json_push_null(L);
  bool same = lua_rawequal(L, idx, -1);
  lua_pop(L, 1);
  return same;
}

// ── Encoder ──────────────────────────────────────────────────────────────────
//
// The output is accumulated in a buffer of our own rather than in a
// luaL_Buffer, because lauxlib's buffer cannot be used the way an encoder needs
// to use it. Its storage lives in a stack slot that every add call expects to
// find at the TOP of the stack (prepbuffsize(B, sz, -1)), whereas the encoder
// must keep the value it is currently encoding above the buffer for the
// duration, plus that value's key while walking a table.
//
// While the output fitted in LUAL_BUFFERSIZE nothing went wrong, which is why
// this survived review: that is 16 * sizeof(void*) * sizeof(lua_Number), so 256
// bytes on the RP2350 (LUA_32BITS makes lua_Number a float) and 512 in the
// simulator. The first add past it removed the value being encoded instead of
// lauxlib's own placeholder and put the buffer's box in its stack slot; the
// encoder's matching pop then closed that box, freeing the storage still being
// written to. The result was a corrupt string, "cannot encode a userdata value"
// once the box was reached as if it were a value, or a fault in resizebox.
// Neurogram's records.json crossed 256 bytes at about the seventh puzzle and so
// wiped the player's progress on the next load.
//
// The storage is owned by a userdata box, so raising a Lua error (cycle,
// unsupported type, out of memory) still unwinds without leaking it, and it is
// allocated through the Lua allocator, which is the PSRAM umm heap here rather
// than the ~29KB SRAM heap that plain malloc draws from.

#define JSON_ENC_MIN_CAP 256
#define JSON_BOX_MT "picocalc.json.buf"

typedef struct {
  char  *p;
  size_t n;         // bytes written
  size_t cap;       // bytes allocated
} json_box_t;

typedef struct {
  lua_State *L;
  json_box_t *box;
  int boxidx;       // absolute stack index of the box userdata
  int indent;       // 0 = compact
} json_enc_t;

static void enc_value(lua_State *L, json_enc_t *e, int idx, int depth);

static void *enc_realloc(lua_State *L, void *p, size_t osize, size_t nsize) {
  void *ud;
  lua_Alloc allocf = lua_getallocf(L, &ud);
  return allocf(ud, p, osize, nsize);
}

static int json_box_gc(lua_State *L) {
  json_box_t *bx = (json_box_t *)lua_touserdata(L, 1);
  if (bx && bx->p) {
    enc_realloc(L, bx->p, bx->cap, 0);
    bx->p = NULL;
    bx->cap = 0;
    bx->n = 0;
  }
  return 0;
}

// Pushes the box that owns the output and points e at it.
static void enc_open(lua_State *L, json_enc_t *e, int indent) {
  json_box_t *bx = (json_box_t *)lua_newuserdatauv(L, sizeof(json_box_t), 0);
  bx->p = NULL;
  bx->n = 0;
  bx->cap = 0;
  if (luaL_newmetatable(L, JSON_BOX_MT)) {
    lua_pushcfunction(L, json_box_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);

  e->L = L;
  e->box = bx;
  e->boxidx = lua_gettop(L);
  e->indent = (indent < 0) ? 0 : (indent > 8 ? 8 : indent);
}

// Replaces the box with the finished string.
static void enc_close(json_enc_t *e) {
  lua_State *L = e->L;
  json_box_t *bx = e->box;

  lua_pushlstring(L, bx->p ? bx->p : "", bx->n);

  // Freed here rather than at the next GC step: encode is usually followed
  // immediately by a write that wants the heap back.
  if (bx->p) {
    enc_realloc(L, bx->p, bx->cap, 0);
    bx->p = NULL;
    bx->cap = 0;
    bx->n = 0;
  }
  lua_remove(L, e->boxidx);
}

static void enc_reserve(json_enc_t *e, size_t extra) {
  json_box_t *bx = e->box;
  if (bx->cap - bx->n >= extra) return;

  size_t need = bx->n + extra;
  size_t cap = bx->cap ? bx->cap : JSON_ENC_MIN_CAP;
  while (cap < need) {
    if (cap > (size_t)-1 / 2) { cap = need; break; }
    cap *= 2;
  }

  // The Lua allocator leaves the old block alone when it cannot satisfy the
  // request, so the box still owns it and __gc will still free it.
  char *np = (char *)enc_realloc(e->L, bx->p, bx->cap, cap);
  if (!np)
    luaL_error(e->L, "json.encode: out of memory (%I bytes)", (lua_Integer)cap);
  bx->p = np;
  bx->cap = cap;
}

static void enc_addlstring(json_enc_t *e, const char *s, size_t len) {
  if (len == 0) return;
  enc_reserve(e, len);
  memcpy(e->box->p + e->box->n, s, len);
  e->box->n += len;
}

static void enc_addstring(json_enc_t *e, const char *s) {
  enc_addlstring(e, s, strlen(s));
}

static void enc_addchar(json_enc_t *e, char c) {
  enc_reserve(e, 1);
  e->box->p[e->box->n++] = c;
}

static void enc_newline_indent(json_enc_t *e, int depth) {
  if (e->indent <= 0) return;
  enc_addchar(e, '\n');
  int n = e->indent * depth;
  for (int i = 0; i < n; i++) enc_addchar(e, ' ');
}

static void enc_string(json_enc_t *e, const char *s, size_t len) {
  // Most strings need no escaping, so ask for the whole thing up front and let
  // the escapes grow the buffer further only when they actually appear.
  enc_reserve(e, len + 2);

  enc_addchar(e, '"');
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  enc_addstring(e, "\\\""); break;
      case '\\': enc_addstring(e, "\\\\"); break;
      case '\b': enc_addstring(e, "\\b");  break;
      case '\f': enc_addstring(e, "\\f");  break;
      case '\n': enc_addstring(e, "\\n");  break;
      case '\r': enc_addstring(e, "\\r");  break;
      case '\t': enc_addstring(e, "\\t");  break;
      default:
        if (c < 0x20) {
          // Control characters must be escaped; \u00XX is the portable form.
          char tmp[7];
          snprintf(tmp, sizeof(tmp), "\\u%04x", c);
          enc_addstring(e, tmp);
        } else {
          // Bytes >= 0x20 pass through verbatim, which keeps valid UTF-8
          // intact without needing to decode it.
          enc_addchar(e, (char)c);
        }
        break;
    }
  }
  enc_addchar(e, '"');
}

static void enc_number(lua_State *L, json_enc_t *e, int idx) {
  char tmp[40];
  if (lua_isinteger(L, idx)) {
    lua_Integer v = lua_tointeger(L, idx);
    snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
  } else {
    lua_Number v = lua_tonumber(L, idx);
    // JSON has no way to spell NaN or Infinity. Emitting them would produce a
    // document no parser accepts, so fail loudly instead.
    if (isnan(v) || isinf(v))
      luaL_error(L, "json.encode: cannot encode %s", isnan(v) ? "NaN" : "Infinity");
    // lua_Number is a float (LUA_32BITS). Emit the shortest form that decodes
    // back to the same value: 7 digits reads cleanly (0.1, not
    // 0.100000001490116), 9 always round-trips a float. picodeck_numfmt_float,
    // not snprintf: the firmware's pico_printf %g keeps trailing zeros.
    static const char *const specs[] = {"%.7g", "%.8g", "%.9g"};
    for (int i = 0; i < 3; i++) {
      picodeck_numfmt_float(tmp, sizeof(tmp), specs[i], v);
      if ((lua_Number)strtod(tmp, NULL) == v)
        break;
    }
    // A whole float ("2" from 2.0) would decode as an integer; keep it a
    // float so math.type survives the round trip.
    if (!strpbrk(tmp, ".eEn"))
      strcat(tmp, ".0");
  }
  enc_addstring(e, tmp);
}

// Returns the array length n if the table at idx is a clean 1..n sequence,
// or -1 if it should be encoded as an object.
static lua_Integer enc_array_len(lua_State *L, int idx) {
  lua_Integer n = 0;
  lua_Integer count = 0;

  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    count++;
    if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
      lua_Integer k = lua_tointeger(L, -2);
      if (k >= 1) {
        if (k > n) n = k;
      } else {
        lua_pop(L, 2);
        return -1;  // zero or negative index -> object
      }
    } else {
      lua_pop(L, 2);
      return -1;    // non-integer key -> object
    }
    lua_pop(L, 1);
  }

  if (count == 0) return 0;      // empty table -> []  (see note below)
  if (n != count) return -1;     // sparse -> object, so no key is lost
  return n;
}

static void enc_table(lua_State *L, json_enc_t *e, int idx, int depth) {
  // Each level keeps a key and a value on the stack; a C function is only
  // guaranteed LUA_MINSTACK slots, so grow it (writes past the end corrupt
  // the heap).
  luaL_checkstack(L, 6, "json.encode: nesting too deep");
  if (depth >= JSON_MAX_DEPTH)
    luaL_error(L, "json.encode: nesting deeper than %d (cycle?)", JSON_MAX_DEPTH);

  lua_Integer alen = enc_array_len(L, idx);

  if (alen >= 0) {
    // An empty Lua table is ambiguous; {} is the safer default because a table
    // used as a map is far more common in config/save data than an empty list,
    // and [] would decode back as a table that then re-encodes as {}.
    if (alen == 0) { enc_addstring(e, "{}"); return; }

    enc_addchar(e, '[');
    for (lua_Integer i = 1; i <= alen; i++) {
      if (i > 1) enc_addchar(e, ',');
      enc_newline_indent(e, depth + 1);
      lua_rawgeti(L, idx, i);
      enc_value(L, e, lua_gettop(L), depth + 1);
      lua_pop(L, 1);
    }
    enc_newline_indent(e, depth);
    enc_addchar(e, ']');
    return;
  }

  enc_addchar(e, '{');
  bool first = true;
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    int ktype = lua_type(L, -2);
    if (ktype != LUA_TSTRING && ktype != LUA_TNUMBER) {
      // Skip keys JSON cannot name rather than emitting something invalid.
      lua_pop(L, 1);
      continue;
    }

    if (!first) enc_addchar(e, ',');
    first = false;
    enc_newline_indent(e, depth + 1);

    if (ktype == LUA_TSTRING) {
      size_t klen;
      const char *k = lua_tolstring(L, -2, &klen);
      enc_string(e, k, klen);
    } else {
      // Numeric key: JSON object keys must be strings, so quote it.
      // lua_tolstring on the key would convert it in place and break lua_next,
      // so format from a copy.
      lua_pushvalue(L, -2);
      size_t klen;
      const char *k = lua_tolstring(L, -1, &klen);
      enc_string(e, k, klen);
      lua_pop(L, 1);
    }

    enc_addchar(e, ':');
    if (e->indent > 0) enc_addchar(e, ' ');
    enc_value(L, e, lua_gettop(L), depth + 1);
    lua_pop(L, 1);
  }
  enc_newline_indent(e, depth);
  enc_addchar(e, '}');
}

static void enc_value(lua_State *L, json_enc_t *e, int idx, int depth) {
  idx = lua_absindex(L, idx);

  if (json_is_null(L, idx)) { enc_addstring(e, "null"); return; }

  switch (lua_type(L, idx)) {
    case LUA_TNIL:
      enc_addstring(e, "null");
      break;
    case LUA_TBOOLEAN:
      enc_addstring(e, lua_toboolean(L, idx) ? "true" : "false");
      break;
    case LUA_TNUMBER:
      enc_number(L, e, idx);
      break;
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, idx, &len);
      enc_string(e, s, len);
      break;
    }
    case LUA_TTABLE:
      enc_table(L, e, idx, depth);
      break;
    default:
      luaL_error(L, "json.encode: cannot encode a %s value",
                 luaL_typename(L, idx));
  }
}

// json.encode(value [, opts])   opts = { indent = n }
static int l_json_encode(lua_State *L) {
  int indent = 0;

  if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
    lua_getfield(L, 2, "indent");
    if (!lua_isnil(L, -1)) indent = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
  }

  lua_json_encode_push(L, 1, indent);
  return 1;
}

// ── Decoder ──────────────────────────────────────────────────────────────────

typedef struct {
  const char *s;
  size_t len;
  size_t pos;
  const char *err;
} json_dec_t;

static bool dec_value(lua_State *L, json_dec_t *d, int depth);

static void dec_skip_ws(json_dec_t *d) {
  while (d->pos < d->len) {
    char c = d->s[d->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') d->pos++;
    else break;
  }
}

static bool dec_fail(json_dec_t *d, const char *msg) {
  if (!d->err) d->err = msg;
  return false;
}

static bool dec_literal(json_dec_t *d, const char *lit) {
  size_t n = strlen(lit);
  if (d->pos + n > d->len) return false;
  if (memcmp(d->s + d->pos, lit, n) != 0) return false;
  d->pos += n;
  return true;
}

// Appends a code point to buf as UTF-8.
static void dec_utf8(luaL_Buffer *buf, unsigned long cp) {
  if (cp < 0x80) {
    luaL_addchar(buf, (char)cp);
  } else if (cp < 0x800) {
    luaL_addchar(buf, (char)(0xC0 | (cp >> 6)));
    luaL_addchar(buf, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    luaL_addchar(buf, (char)(0xE0 | (cp >> 12)));
    luaL_addchar(buf, (char)(0x80 | ((cp >> 6) & 0x3F)));
    luaL_addchar(buf, (char)(0x80 | (cp & 0x3F)));
  } else {
    luaL_addchar(buf, (char)(0xF0 | (cp >> 18)));
    luaL_addchar(buf, (char)(0x80 | ((cp >> 12) & 0x3F)));
    luaL_addchar(buf, (char)(0x80 | ((cp >> 6) & 0x3F)));
    luaL_addchar(buf, (char)(0x80 | (cp & 0x3F)));
  }
}

static int dec_hex4(json_dec_t *d, unsigned long *out) {
  if (d->pos + 4 > d->len) return 0;
  unsigned long v = 0;
  for (int i = 0; i < 4; i++) {
    char c = d->s[d->pos + i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
    else return 0;
  }
  d->pos += 4;
  *out = v;
  return 1;
}

static bool dec_string(lua_State *L, json_dec_t *d) {
  if (d->pos >= d->len || d->s[d->pos] != '"')
    return dec_fail(d, "expected string");
  d->pos++;

  luaL_Buffer b;
  luaL_buffinit(L, &b);

  while (d->pos < d->len) {
    unsigned char c = (unsigned char)d->s[d->pos++];

    if (c == '"') { luaL_pushresult(&b); return true; }

    if (c != '\\') {
      if (c < 0x20) { luaL_pushresult(&b); lua_pop(L, 1);
                      return dec_fail(d, "control character in string"); }
      luaL_addchar(&b, (char)c);
      continue;
    }

    if (d->pos >= d->len) { luaL_pushresult(&b); lua_pop(L, 1);
                            return dec_fail(d, "truncated escape"); }

    char esc = d->s[d->pos++];
    switch (esc) {
      case '"':  luaL_addchar(&b, '"');  break;
      case '\\': luaL_addchar(&b, '\\'); break;
      case '/':  luaL_addchar(&b, '/');  break;
      case 'b':  luaL_addchar(&b, '\b'); break;
      case 'f':  luaL_addchar(&b, '\f'); break;
      case 'n':  luaL_addchar(&b, '\n'); break;
      case 'r':  luaL_addchar(&b, '\r'); break;
      case 't':  luaL_addchar(&b, '\t'); break;
      case 'u': {
        unsigned long cp;
        if (!dec_hex4(d, &cp)) { luaL_pushresult(&b); lua_pop(L, 1);
                                 return dec_fail(d, "bad \\u escape"); }
        // Surrogate pair: high surrogate must be followed by \uDC00-\uDFFF.
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (d->pos + 6 <= d->len && d->s[d->pos] == '\\' &&
              d->s[d->pos + 1] == 'u') {
            size_t save = d->pos;
            d->pos += 2;
            unsigned long lo;
            if (dec_hex4(d, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              d->pos = save;  // lone high surrogate; emit as-is
            }
          }
        }
        dec_utf8(&b, cp);
        break;
      }
      default:
        luaL_pushresult(&b);
        lua_pop(L, 1);
        return dec_fail(d, "unknown escape");
    }
  }

  luaL_pushresult(&b);
  lua_pop(L, 1);
  return dec_fail(d, "unterminated string");
}

static bool dec_number(lua_State *L, json_dec_t *d) {
  size_t start = d->pos;
  bool is_float = false;

  if (d->pos < d->len && (d->s[d->pos] == '-' || d->s[d->pos] == '+')) d->pos++;
  while (d->pos < d->len) {
    char c = d->s[d->pos];
    if (c >= '0' && c <= '9') { d->pos++; }
    else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
      is_float = true; d->pos++;
    } else break;
  }

  if (d->pos == start) return dec_fail(d, "expected number");

  // strtod/strtoll need a NUL-terminated copy; numbers are short so a small
  // stack buffer is enough, and an over-long run is malformed anyway.
  char tmp[64];
  size_t n = d->pos - start;
  if (n >= sizeof(tmp)) return dec_fail(d, "number too long");
  memcpy(tmp, d->s + start, n);
  tmp[n] = '\0';

  char *end = NULL;
  if (!is_float) {
    long long v = strtoll(tmp, &end, 10);
    if (end && *end == '\0') {
      // Keep integers as Lua integers so round-tripping does not turn 1 into
      // 1.0. LUA_32BITS=1 here, so lua_Integer is 32-bit: fall back to a float
      // if the value does not fit.
      if (v >= (long long)LUA_MININTEGER && v <= (long long)LUA_MAXINTEGER) {
        lua_pushinteger(L, (lua_Integer)v);
        return true;
      }
      lua_pushnumber(L, (lua_Number)v);
      return true;
    }
  }

  double dv = strtod(tmp, &end);
  if (!end || *end != '\0') return dec_fail(d, "malformed number");
  lua_pushnumber(L, (lua_Number)dv);
  return true;
}

static bool dec_array(lua_State *L, json_dec_t *d, int depth) {
  d->pos++;  // consume '['
  lua_newtable(L);
  lua_Integer i = 1;

  dec_skip_ws(d);
  if (d->pos < d->len && d->s[d->pos] == ']') { d->pos++; return true; }

  for (;;) {
    dec_skip_ws(d);
    if (!dec_value(L, d, depth + 1)) { lua_pop(L, 1); return false; }
    lua_rawseti(L, -2, i++);

    dec_skip_ws(d);
    if (d->pos >= d->len) { lua_pop(L, 1); return dec_fail(d, "unterminated array"); }
    if (d->s[d->pos] == ',') { d->pos++; continue; }
    if (d->s[d->pos] == ']') { d->pos++; return true; }
    lua_pop(L, 1);
    return dec_fail(d, "expected ',' or ']' in array");
  }
}

static bool dec_object(lua_State *L, json_dec_t *d, int depth) {
  d->pos++;  // consume '{'
  lua_newtable(L);

  dec_skip_ws(d);
  if (d->pos < d->len && d->s[d->pos] == '}') { d->pos++; return true; }

  for (;;) {
    dec_skip_ws(d);
    if (!dec_string(L, d)) { lua_pop(L, 1); return false; }

    dec_skip_ws(d);
    if (d->pos >= d->len || d->s[d->pos] != ':') {
      lua_pop(L, 2);
      return dec_fail(d, "expected ':' after object key");
    }
    d->pos++;

    dec_skip_ws(d);
    if (!dec_value(L, d, depth + 1)) { lua_pop(L, 2); return false; }
    lua_rawset(L, -3);

    dec_skip_ws(d);
    if (d->pos >= d->len) { lua_pop(L, 1); return dec_fail(d, "unterminated object"); }
    if (d->s[d->pos] == ',') { d->pos++; continue; }
    if (d->s[d->pos] == '}') { d->pos++; return true; }
    lua_pop(L, 1);
    return dec_fail(d, "expected ',' or '}' in object");
  }
}

static bool dec_value(lua_State *L, json_dec_t *d, int depth) {
  if (depth >= JSON_MAX_DEPTH) return dec_fail(d, "nesting too deep");
  // A table (and a key) per level: beyond LUA_MINSTACK without growing.
  if (!lua_checkstack(L, 4)) return dec_fail(d, "nesting too deep");

  dec_skip_ws(d);
  if (d->pos >= d->len) return dec_fail(d, "unexpected end of input");

  char c = d->s[d->pos];
  switch (c) {
    case '{': return dec_object(L, d, depth);
    case '[': return dec_array(L, d, depth);
    case '"': return dec_string(L, d);
    case 't':
      if (dec_literal(d, "true"))  { lua_pushboolean(L, 1); return true; }
      return dec_fail(d, "expected 'true'");
    case 'f':
      if (dec_literal(d, "false")) { lua_pushboolean(L, 0); return true; }
      return dec_fail(d, "expected 'false'");
    case 'n':
      if (dec_literal(d, "null"))  { json_push_null(L); return true; }
      return dec_fail(d, "expected 'null'");
    default:
      return dec_number(L, d);
  }
}

// json.decode(str) -> value, or nil, errmsg
static int l_json_decode(lua_State *L) {
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);

  json_dec_t d = { .s = s, .len = len, .pos = 0, .err = NULL };

  if (!dec_value(L, &d, 0)) {
    lua_pushnil(L);
    lua_pushfstring(L, "json.decode: %s at byte %d",
                    d.err ? d.err : "parse error", (int)d.pos + 1);
    return 2;
  }

  dec_skip_ws(&d);
  if (d.pos != d.len) {
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_pushfstring(L, "json.decode: trailing content at byte %d",
                    (int)d.pos + 1);
    return 2;
  }

  return 1;
}

// json.isNull(v) -> bool.  Saves apps from importing the sentinel to compare.
static int l_json_isNull(lua_State *L) {
  lua_pushboolean(L, json_is_null(L, 1));
  return 1;
}

// ── C entry points for other bridge modules ──────────────────────────────────
//
// game.save is built on these so there is exactly one JSON implementation in
// the firmware rather than a second hand-rolled one that drifts.

// Encodes the value at `idx` and pushes the resulting string.
// Raises a Lua error on cycles / unsupported types, like json.encode.
void lua_json_encode_push(lua_State *L, int idx, int indent) {
  json_enc_t e;
  // Resolved before the box goes on the stack, so a relative idx still refers
  // to the caller's value.
  idx = lua_absindex(L, idx);
  enc_open(L, &e, indent);
  enc_value(L, &e, idx, 0);
  enc_close(&e);
}

// Decodes `s` and pushes the value, returning true. On failure pushes nothing,
// returns false, and sets *err to a static description.
bool lua_json_decode_push(lua_State *L, const char *s, size_t len,
                          const char **err) {
  json_dec_t d = { .s = s, .len = len, .pos = 0, .err = NULL };
  if (!dec_value(L, &d, 0)) {
    if (err) *err = d.err ? d.err : "parse error";
    return false;
  }
  dec_skip_ws(&d);
  if (d.pos != d.len) {
    lua_pop(L, 1);
    if (err) *err = "trailing content";
    return false;
  }
  return true;
}

// ── Registration ─────────────────────────────────────────────────────────────

static const luaL_Reg l_json_lib[] = {
    {"encode", l_json_encode},
    {"decode", l_json_decode},
    {"isNull", l_json_isNull},
    {NULL, NULL}};

void lua_bridge_json_init(lua_State *L) {
  // Create the null sentinel once and stash it in the registry so every
  // decode returns the identical value and `v == picocalc.json.null` works.
  if (lua_getfield(L, LUA_REGISTRYINDEX, JSON_NULL_MT) == LUA_TNIL) {
    lua_pop(L, 1);
    lua_newuserdatauv(L, 1, 0);
    lua_newtable(L);
    lua_pushcfunction(L, json_null_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, JSON_NULL_MT);
    lua_setfield(L, -2, "__name");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, JSON_NULL_MT);
  }
  lua_pop(L, 1);

  register_subtable(L, "json", l_json_lib);

  lua_getfield(L, -1, "json");
  json_push_null(L);
  lua_setfield(L, -2, "null");
  lua_pop(L, 1);
}
