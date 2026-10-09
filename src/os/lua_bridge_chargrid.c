// picocalc.chargrid: character-cell grids over src/os/chargrid.h, and picocalc.noise (noise.h). Grids are drawn by
// one C call (specs/2026-10-09-chargrid-design.md). Cell positions, pixel
// positions and sizes are quantities (lb_checkint: floats round); glyph
// codes, palette indices, colours, styles and font ids are exact integers.
// A grid owns one umm block; nothing in it points at another object (fonts
// are copied into the glyph cache, blits copy at once).
#include "lua_bridge_internal.h"

#include "../fonts/font_registry.h"
#include "chargrid.h"
#include "noise.h"
#include "lauxlib.h"
#include "pico/time.h"

#define GRID_MT "picocalc.chargrid.grid"

typedef struct {
  chargrid_t *g;  // NULL once collected
  int last_cells;
  uint32_t last_us;
} grid_ud_t;

static grid_ud_t *check_ud(lua_State *L, int idx) {
  grid_ud_t *u = (grid_ud_t *)lb_checkudata(L, idx, GRID_MT);
  if (!u->g) luaL_error(L, "attempt to use a freed chargrid");
  return u;
}

static chargrid_t *check_grid(lua_State *L, int idx) { return check_ud(L, idx)->g; }

static int qint(lua_State *L, int idx) { return (int)lb_checkint(L, idx); }

static int check_range(lua_State *L, int idx, int lo, int hi, const char *what) {
  const lua_Integer v = luaL_checkinteger(L, idx);
  if (v < lo || v > hi)
    luaL_argerror(L, idx, lua_pushfstring(L, "%s must be %d-%d", what, lo, hi));
  return (int)v;
}

// A palette index; def when nil or none.
static int opt_index(lua_State *L, int idx, int def, const char *what) {
  return lua_isnoneornil(L, idx) ? def : check_range(L, idx, 0, 255, what);
}

// A glyph: a code 0-255, or a one-character string mapped like text.
static int check_glyph(lua_State *L, int idx) {
  if (lua_type(L, idx) == LUA_TSTRING) {
    size_t len;
    const char *s = lua_tolstring(L, idx, &len);
    uint8_t code = 0;
    if (len == 0 || (size_t)chargrid_utf8_code(s, len, &code) != len)
      luaL_argerror(L, idx, "one character expected");
    return code;
  }
  return check_range(L, idx, 0, 255, "glyph");
}

static int opt_glyph(lua_State *L, int idx, int def) {
  return lua_isnoneornil(L, idx) ? def : check_glyph(L, idx);
}

// Entry i (0-based) of the table at idx: an integer 0-255.
static uint8_t table_byte(lua_State *L, int idx, size_t i, const char *what) {
  lua_rawgeti(L, idx, (lua_Integer)i + 1);
  int ok;
  const lua_Integer v = lua_tointegerx(L, -1, &ok);
  lua_pop(L, 1);
  if (!ok || v < 0 || v > 255)
    luaL_argerror(L, idx,
                  lua_pushfstring(L, "%s entry %d must be an integer 0-255", what, (int)i + 1));
  return (uint8_t)v;
}

// ── plane arguments: a string, a table, an integer (fill) or nil (keep) ─────
enum { PA_KEEP, PA_FILL, PA_BYTES, PA_TABLE };
typedef struct {
  int kind, idx, fill;
  const uint8_t *s;
  size_t n;
  lb_array_t arr;
  const char *what;
} plane_arg_t;

static void plane_arg(lua_State *L, int idx, const char *what, plane_arg_t *p) {
  p->idx = idx;
  p->what = what;
  p->n = 0;
  p->fill = 0;
  p->s = NULL;
  switch (lua_type(L, idx)) {
  case LUA_TNONE:
  case LUA_TNIL: p->kind = PA_KEEP; break;
  case LUA_TNUMBER:
    p->kind = PA_FILL;
    p->fill = check_range(L, idx, 0, 255, what);
    break;
  case LUA_TSTRING:
    p->kind = PA_BYTES;
    p->s = (const uint8_t *)lua_tolstring(L, idx, &p->n);
    break;
  case LUA_TTABLE:
    p->kind = PA_TABLE;
    p->n = lua_rawlen(L, idx);
    lb_array_begin(L, idx, &p->arr);
    break;
  default: luaL_typeerror(L, idx, "string, table, integer or nil");
  }
}

static uint8_t plane_at(lua_State *L, plane_arg_t *p, size_t i) {
  if (p->kind == PA_BYTES) return p->s[i];
  if (p->kind == PA_FILL) return (uint8_t)p->fill;
  lua_Integer v;
  if (picodeck_lua_array_getint(&p->arr.a, (lua_Integer)i + 1, &v) && v >= 0 && v <= 255)
    return (uint8_t)v;
  return table_byte(L, p->idx, i, p->what);
}

// The shared length of the sequence arguments, or dflt when there are none.
static size_t seq_len(lua_State *L, const plane_arg_t *p, int n, size_t dflt) {
  bool have = false;
  size_t len = dflt;
  for (int k = 0; k < n; k++) {
    if (p[k].kind != PA_BYTES && p[k].kind != PA_TABLE) continue;
    if (have && p[k].n != len) luaL_error(L, "glyphs, fgs and bgs have different lengths");
    len = p[k].n;
    have = true;
  }
  return len;
}

// Writes a w x h rectangle of cells at (x, y) from the three plane arguments
// (row-major sequences of w*h entries), clipped to the grid.
static void write_rect(lua_State *L, chargrid_t *g, plane_arg_t *p, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  g->dirty = true;  // before the loop: plane_at can raise mid-write with cells already changed
  const int64_t r_lo = y < 0 ? -(int64_t)y : 0;
  int64_t r_hi = (int64_t)g->rows - y;
  if (r_hi > h) r_hi = h;
  for (int64_t r = r_lo; r < r_hi; r++) {
    int first;
    const int vis = chargrid_clip_span(g, x, (int)(y + r), w, &first);
    if (vis <= 0) break;
    const size_t cell = (size_t)(y + r) * (size_t)g->cols + (size_t)(x + first);
    for (int pl = 0; pl < 3; pl++) {
      plane_arg_t *a = &p[pl];
      if (a->kind == PA_KEEP) continue;
      uint8_t *dst = chargrid_plane(g, pl) + cell;
      if (a->kind == PA_FILL) {
        memset(dst, a->fill, (size_t)vis);
        continue;
      }
      const size_t src = (size_t)r * (size_t)w + (size_t)first;
      if (a->kind == PA_BYTES) {
        memcpy(dst, a->s + src, (size_t)vis);
        continue;
      }
      for (int k = 0; k < vis; k++) dst[k] = plane_at(L, a, src + (size_t)k);
    }
  }
  }

static const pc_font_t *check_font(lua_State *L, int idx) {
  const lua_Integer id = luaL_checkinteger(L, idx);
  const pc_font_t *f = font_registry_get((int)id);
  if (!f) luaL_argerror(L, idx, lua_pushfstring(L, "font %d is not loaded", (int)id));
  return f;
}

// ── module ───────────────────────────────────────────────────────────────────
static int l_chargrid_new(lua_State *L) {
  const int cols = qint(L, 1), rows = qint(L, 2);
  const pc_font_t *f = lua_isnoneornil(L, 3) ? NULL : check_font(L, 3);
  int cw, ch;
  if (lua_isnoneornil(L, 4) && lua_isnoneornil(L, 5)) {
    if (!f) return luaL_error(L, "chargrid.new: cellW and cellH are required without a font");
    cw = f->max_width;
    ch = f->height;
  } else {
    cw = qint(L, 4);
    ch = qint(L, 5);
  }
  const char *why = chargrid_check_size(cols, rows, cw, ch);
  if (why) return luaL_error(L, "chargrid.new: %s", why);
  grid_ud_t *u = (grid_ud_t *)lua_newuserdatauv(L, sizeof(grid_ud_t), 0);
  u->g = NULL;
  u->last_cells = 0;
  u->last_us = 0;
  luaL_setmetatable(L, GRID_MT);
  u->g = chargrid_create(cols, rows, cw, ch);
  if (!u->g) return luaL_error(L, "chargrid.new: out of memory");
  if (f) chargrid_set_font(u->g, f);
  return 1;
}

// ── methods ──────────────────────────────────────────────────────────────────
static int l_grid_gc(lua_State *L) {
  grid_ud_t *u = (grid_ud_t *)lb_checkudata(L, 1, GRID_MT);
  if (u->g) {
    chargrid_free(u->g);
    u->g = NULL;
  }
  return 0;
}

static int l_grid_getSize(lua_State *L) {
  const chargrid_t *g = check_grid(L, 1);
  lua_pushinteger(L, g->cols);
  lua_pushinteger(L, g->rows);
  return 2;
}

static int l_grid_getCellSize(lua_State *L) {
  const chargrid_t *g = check_grid(L, 1);
  lua_pushinteger(L, g->cell_w);
  lua_pushinteger(L, g->cell_h);
  return 2;
}

static int l_grid_setFont(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  chargrid_set_font(g, check_font(L, 2));
  return 0;
}

static int l_grid_defineGlyph(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int code = check_range(L, 2, 1, 255, "code");
  luaL_checktype(L, 3, LUA_TTABLE);
  if ((int)lua_rawlen(L, 3) != g->cell_h)
    return luaL_argerror(L, 3, lua_pushfstring(L, "rows must have %d rows (the cell height)", g->cell_h));
  uint16_t rows[CG_MAX_CELL_H];
  const lua_Integer max = (1 << g->cell_w) - 1;
  for (int r = 0; r < g->cell_h; r++) {
    lua_rawgeti(L, 3, r + 1);
    int ok;
    const lua_Integer v = lua_tointegerx(L, -1, &ok);
    lua_pop(L, 1);
    if (!ok || v < 0 || v > max)
      return luaL_argerror(L, 3, lua_pushfstring(L, "row %d must be an integer 0-%d", r + 1, (int)max));
    rows[r] = (uint16_t)v;
  }
  chargrid_define_glyph(g, code, rows);
  return 0;
}

static int l_grid_setPalette(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  const int first = opt_index(L, 3, 0, "first");
  const size_t n = lua_rawlen(L, 2);
  if ((size_t)first + n > 256)
    return luaL_argerror(L, 2, "first + #colors must be at most 256");
  for (size_t k = 0; k < n; k++) {  // check every colour before writing any
    lua_rawgeti(L, 2, (lua_Integer)k + 1);
    int ok;
    const lua_Integer v = lua_tointegerx(L, -1, &ok);
    lua_pop(L, 1);
    if (!ok || v < 0 || v > 0xFFFF)
      return luaL_argerror(L, 2, lua_pushfstring(L, "colour %d must be an integer 0-65535", (int)k + 1));
  }
  for (size_t k = 0; k < n; k++) {
    lua_rawgeti(L, 2, (lua_Integer)k + 1);
    g->palette[(size_t)first + k] = (uint16_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  g->dirty = true;
  return 0;
}

static int l_grid_setColor(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int i = check_range(L, 2, 0, 255, "index");
  g->palette[i] = (uint16_t)check_range(L, 3, 0, 0xFFFF, "colour");
  g->dirty = true;
  return 0;
}

static int l_grid_getColor(lua_State *L) {
  const chargrid_t *g = check_grid(L, 1);
  lua_pushinteger(L, g->palette[check_range(L, 2, 0, 255, "index")]);
  return 1;
}

static int l_grid_setRamp(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  uint8_t codes[CG_RAMP_MAX];
  int n = 0;
  if (lua_type(L, 2) == LUA_TSTRING) {
    size_t len;
    const char *s = lua_tolstring(L, 2, &len);
    for (size_t at = 0; at < len;) {
      if (n == CG_RAMP_MAX) return luaL_argerror(L, 2, "a ramp needs 1-256 entries");
      uint8_t c;
      at += (size_t)chargrid_utf8_code(s + at, len - at, &c);
      codes[n++] = c;
    }
  } else {
    luaL_checktype(L, 2, LUA_TTABLE);
    const size_t len = lua_rawlen(L, 2);
    if (len > CG_RAMP_MAX) return luaL_argerror(L, 2, "a ramp needs 1-256 entries");
    for (size_t k = 0; k < len; k++) codes[n++] = table_byte(L, 2, k, "ramp");
  }
  if (n == 0) return luaL_argerror(L, 2, "a ramp needs 1-256 entries");
  chargrid_set_ramp(g, codes, n, lua_toboolean(L, 3));
  return 0;
}

static int l_grid_setColorRamp(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  const size_t len = lua_rawlen(L, 2);
  if (len == 0 || len > CG_RAMP_MAX) return luaL_argerror(L, 2, "a colour ramp needs 1-256 entries");
  uint8_t idx[CG_RAMP_MAX];
  for (size_t k = 0; k < len; k++) idx[k] = table_byte(L, 2, k, "colour ramp");
  chargrid_set_color_ramp(g, idx, (int)len, lua_toboolean(L, 3));
  return 0;
}

static int l_grid_clear(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int glyph = opt_glyph(L, 2, CG_EMPTY);
  const int fg = opt_index(L, 3, CG_DEFAULT_FG, "fg"), bg = opt_index(L, 4, CG_DEFAULT_BG, "bg");
  chargrid_fill(g, 0, 0, g->cols, g->rows, glyph, fg, bg);
  return 0;
}

static int l_grid_set(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int x = qint(L, 2), y = qint(L, 3), glyph = check_glyph(L, 4);
  chargrid_set(g, x, y, glyph, opt_index(L, 5, CG_KEEP, "fg"), opt_index(L, 6, CG_KEEP, "bg"));
  return 0;
}

static int l_grid_get(lua_State *L) {
  const chargrid_t *g = check_grid(L, 1);
  uint8_t c, f, b;
  chargrid_get(g, qint(L, 2), qint(L, 3), &c, &f, &b);
  lua_pushinteger(L, c);
  lua_pushinteger(L, f);
  lua_pushinteger(L, b);
  return 3;
}

static int l_grid_fill(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int x = qint(L, 2), y = qint(L, 3), w = qint(L, 4), h = qint(L, 5);
  chargrid_fill(g, x, y, w, h, opt_glyph(L, 6, CG_KEEP), opt_index(L, 7, CG_KEEP, "fg"),
                opt_index(L, 8, CG_KEEP, "bg"));
  return 0;
}

static int l_grid_setRow(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int y = qint(L, 2);
  plane_arg_t p[3];
  plane_arg(L, 3, "glyphs", &p[0]);
  plane_arg(L, 4, "fgs", &p[1]);
  plane_arg(L, 5, "bgs", &p[2]);
  const int x = (int)lb_optint(L, 6, 0);
  const int64_t rest = (int64_t)g->cols - x;
  size_t n = seq_len(L, p, 3, rest > 0 ? (size_t)rest : 0);
  if (n > INT32_MAX) n = INT32_MAX;
  write_rect(L, g, p, x, y, (int)n, 1);
  return 0;
}

static int l_grid_getRow(lua_State *L) {
  const chargrid_t *g = check_grid(L, 1);
  const int y = qint(L, 2), x = (int)lb_optint(L, 3, 0);
  int64_t n = lua_isnoneornil(L, 4) ? (int64_t)g->cols - x : lb_checkint(L, 4);
  if (n > INT32_MAX) n = INT32_MAX;
  int first = 0;
  const int vis = n > 0 ? chargrid_clip_span(g, x, y, (int)n, &first) : 0;
  if (vis <= 0) {
    for (int k = 0; k < 3; k++) lua_pushliteral(L, "");
    return 3;
  }
  const size_t cell = (size_t)y * (size_t)g->cols + (size_t)(x + first);
  lua_pushlstring(L, (const char *)g->glyph + cell, (size_t)vis);
  lua_pushlstring(L, (const char *)g->fg + cell, (size_t)vis);
  lua_pushlstring(L, (const char *)g->bg + cell, (size_t)vis);
  return 3;
}

static int l_grid_setRect(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int x = qint(L, 2), y = qint(L, 3), w = qint(L, 4), h = qint(L, 5);
  plane_arg_t p[3];
  plane_arg(L, 6, "glyphs", &p[0]);
  plane_arg(L, 7, "fgs", &p[1]);
  plane_arg(L, 8, "bgs", &p[2]);
  const int64_t cells = w > 0 && h > 0 ? (int64_t)w * h : 0;
  for (int k = 0; k < 3; k++)
    if ((p[k].kind == PA_BYTES || p[k].kind == PA_TABLE) && (int64_t)p[k].n != cells)
      return luaL_argerror(L, 6 + k, lua_pushfstring(L, "%s must have w*h = %d entries", p[k].what,
                                                     (int)(cells > INT32_MAX ? INT32_MAX : cells)));
  write_rect(L, g, p, x, y, w, h);
  return 0;
}

static int l_grid_shadeRow(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int y = qint(L, 2);
  const bool hl = !lua_isnoneornil(L, 3), ht = !lua_isnoneornil(L, 4);
  if (!hl && !ht) return luaL_argerror(L, 3, "levels or tones expected");
  if (hl) luaL_checktype(L, 3, LUA_TTABLE);
  if (ht) luaL_checktype(L, 4, LUA_TTABLE);
  if (hl && g->ramp_n == 0) return luaL_error(L, "shadeRow: no ramp (call setRamp first)");
  if (ht && g->cramp_n == 0) return luaL_error(L, "shadeRow: no colour ramp (call setColorRamp first)");
  const int x = (int)lb_optint(L, 5, 0);
  const size_t nl = hl ? lua_rawlen(L, 3) : 0, nt = ht ? lua_rawlen(L, 4) : 0;
  if (hl && ht && nl != nt) return luaL_error(L, "shadeRow: levels and tones have different lengths");
  const size_t n = hl ? nl : nt;
  int first = 0;
  const int vis = chargrid_clip_span(g, x, y, n > INT32_MAX ? INT32_MAX : (int)n, &first);
  lb_array_t la, ta;
  if (hl) lb_array_begin(L, 3, &la);
  if (ht) lb_array_begin(L, 4, &ta);
  const bool tone_from_level = hl && !ht && g->cramp_n > 0;
  for (int k = 0; k < vis; k++) {
    const lua_Integer i = (lua_Integer)first + k + 1;
    const float lv = hl ? lb_array_number(&la, i) : 0.0f;
    const float tn = ht ? lb_array_number(&ta, i) : lv;
    chargrid_shade(g, x + first + k, y, lv, hl, tn, ht || tone_from_level);
  }
  return 0;
}

static int l_grid_print(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int x = qint(L, 2), y = qint(L, 3);
  size_t len;
  const char *s = luaL_checklstring(L, 4, &len);
  lua_pushinteger(L, chargrid_print(g, x, y, s, len, opt_index(L, 5, CG_KEEP, "fg"),
                                    opt_index(L, 6, CG_KEEP, "bg")));
  return 1;
}

static int l_grid_printWrapped(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int x = qint(L, 2), y = qint(L, 3), w = qint(L, 4);
  size_t len;
  const char *s = luaL_checklstring(L, 5, &len);
  lua_pushinteger(L, chargrid_print_wrapped(g, x, y, w, 0, s, len, opt_index(L, 6, CG_KEEP, "fg"),
                                            opt_index(L, 7, CG_KEEP, "bg")));
  return 1;
}

static int l_grid_box(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int x = qint(L, 2), y = qint(L, 3), w = qint(L, 4), h = qint(L, 5);
  const int style = lua_isnoneornil(L, 6) ? CG_STYLE_SINGLE : check_range(L, 6, 0, 2, "style");
  chargrid_box(g, x, y, w, h, style, opt_index(L, 7, CG_KEEP, "fg"), opt_index(L, 8, CG_KEEP, "bg"));
  return 0;
}

static int l_grid_stamp(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int x = qint(L, 2), y = qint(L, 3);
  size_t len;
  const char *s = luaL_checklstring(L, 4, &len);
  chargrid_stamp(g, x, y, s, len, opt_index(L, 5, CG_KEEP, "fg"), opt_index(L, 6, CG_KEEP, "bg"),
                 opt_glyph(L, 7, ' '));
  return 0;
}

static int l_grid_blit(lua_State *L) {
  chargrid_t *dst = check_grid(L, 1);
  const chargrid_t *src = check_grid(L, 2);
  const int sx = qint(L, 3), sy = qint(L, 4), w = qint(L, 5), h = qint(L, 6);
  const int dx = qint(L, 7), dy = qint(L, 8);
  chargrid_blit(dst, src, sx, sy, w, h, dx, dy, opt_glyph(L, 9, CG_KEEP));
  return 0;
}

static int l_grid_scroll(lua_State *L) {
  chargrid_t *g = check_grid(L, 1);
  const int dx = qint(L, 2), dy = qint(L, 3);
  chargrid_scroll(g, dx, dy, opt_glyph(L, 4, CG_EMPTY), opt_index(L, 5, CG_DEFAULT_FG, "fg"),
                  opt_index(L, 6, CG_DEFAULT_BG, "bg"));
  return 0;
}

static int l_grid_draw(lua_State *L) {
  grid_ud_t *u = check_ud(L, 1);
  chargrid_t *g = u->g;
  const int px = qint(L, 2), py = qint(L, 3);
  const int sx = (int)lb_optint(L, 4, 0), sy = (int)lb_optint(L, 5, 0);
  const int w = (int)lb_optint(L, 6, g->cols), h = (int)lb_optint(L, 7, g->rows);
  const uint32_t t0 = (uint32_t)to_us_since_boot(get_absolute_time());
  u->last_cells = chargrid_draw(g, px, py, sx, sy, w, h);
  u->last_us = (uint32_t)to_us_since_boot(get_absolute_time()) - t0;
  return 0;
}

static int l_grid_isDirty(lua_State *L) {
  lua_pushboolean(L, check_grid(L, 1)->dirty);
  return 1;
}

static int l_grid_getStats(lua_State *L) {
  const grid_ud_t *u = check_ud(L, 1);
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, u->last_cells);
  lua_setfield(L, -2, "cells");
  lua_pushinteger(L, (lua_Integer)u->last_us);
  lua_setfield(L, -2, "us");
  return 1;
}

// ── picocalc.noise ───────────────────────────────────────────────────────────
// Coordinates are finite floats; seeds are exact integers (wrapped to 32
// bits); octaves an exact integer 1-8.
static int32_t opt_seed(lua_State *L, int idx) {
  return (int32_t)(uint32_t)luaL_optinteger(L, idx, 0);
}

static int l_noise_hash(lua_State *L) {
  const float x = lb_checkfloat(L, 1), y = lb_checkfloat(L, 2);
  lua_pushnumber(L, noise_hash(noise_lattice(x), noise_lattice(y), opt_seed(L, 3)));
  return 1;
}

static int l_noise_value(lua_State *L) {
  const float x = lb_checkfloat(L, 1), y = lb_checkfloat(L, 2);
  lua_pushnumber(L, noise_value(x, y, opt_seed(L, 3)));
  return 1;
}

static int l_noise_fbm(lua_State *L) {
  const float x = lb_checkfloat(L, 1), y = lb_checkfloat(L, 2);
  const int octaves = check_range(L, 3, 1, NOISE_MAX_OCTAVES, "octaves");
  lua_pushnumber(L, noise_fbm(x, y, octaves, opt_seed(L, 4)));
  return 1;
}

static const luaL_Reg l_noise_lib[] = {
    {"hash", l_noise_hash}, {"value", l_noise_value}, {"fbm", l_noise_fbm}, {NULL, NULL}};

static const luaL_Reg l_grid_methods[] = {
    {"getSize", l_grid_getSize},           {"getCellSize", l_grid_getCellSize},
    {"setFont", l_grid_setFont},           {"defineGlyph", l_grid_defineGlyph},
    {"setPalette", l_grid_setPalette},     {"setColor", l_grid_setColor},
    {"getColor", l_grid_getColor},         {"setRamp", l_grid_setRamp},
    {"setColorRamp", l_grid_setColorRamp}, {"clear", l_grid_clear},
    {"set", l_grid_set},                   {"get", l_grid_get},
    {"fill", l_grid_fill},                 {"setRow", l_grid_setRow},
    {"getRow", l_grid_getRow},             {"setRect", l_grid_setRect},
    {"shadeRow", l_grid_shadeRow},         {"print", l_grid_print},
    {"printWrapped", l_grid_printWrapped}, {"box", l_grid_box},
    {"stamp", l_grid_stamp},               {"blit", l_grid_blit},
    {"scroll", l_grid_scroll},             {"draw", l_grid_draw},
    {"isDirty", l_grid_isDirty},           {"getStats", l_grid_getStats},
    {NULL, NULL}};
static const luaL_Reg l_grid_meta[] = {{"__gc", l_grid_gc}, {NULL, NULL}};
static const luaL_Reg l_chargrid_lib[] = {{"new", l_chargrid_new}, {NULL, NULL}};

static const struct {
  const char *name;
  int value;
} k_consts[] = {
    {"EMPTY", CG_EMPTY},         {"DOT1", CG_DOT1},           {"DOT2", CG_DOT2},
    {"DOT3", CG_DOT3},           {"DOT4", CG_DOT4},           {"SHADE1", CG_SHADE1},
    {"SHADE2", CG_SHADE2},       {"SHADE3", CG_SHADE3},       {"SHADE4", CG_SHADE4},
    {"HALF_TOP", CG_HALF_TOP},   {"HALF_BOTTOM", CG_HALF_BOTTOM}, {"HALF_LEFT", CG_HALF_LEFT},
    {"HALF_RIGHT", CG_HALF_RIGHT}, {"BOX_H", CG_BOX_H},       {"BOX_V", CG_BOX_V},
    {"BOX_TL", CG_BOX_TL},       {"BOX_TR", CG_BOX_TR},       {"BOX_BL", CG_BOX_BL},
    {"BOX_BR", CG_BOX_BR},       {"BOX_LT", CG_BOX_LT},       {"BOX_RT", CG_BOX_RT},
    {"BOX_TT", CG_BOX_TT},       {"BOX_BT", CG_BOX_BT},       {"BOX_X", CG_BOX_X},
    {"TRI_UP", CG_TRI_UP},       {"TRI_RIGHT", CG_TRI_RIGHT}, {"TRI_DOWN", CG_TRI_DOWN},
    {"TRI_LEFT", CG_TRI_LEFT},   {"BOX_SINGLE", CG_STYLE_SINGLE}, {"BOX_BLOCK", CG_STYLE_BLOCK},
    {"BOX_ASCII", CG_STYLE_ASCII},
};

void lua_bridge_chargrid_init(lua_State *L) {
  lb_register_type(L, GRID_MT, l_grid_methods, l_grid_meta);
  register_subtable(L, "chargrid", l_chargrid_lib);
  lua_getfield(L, -1, "chargrid");
  for (size_t i = 0; i < sizeof k_consts / sizeof k_consts[0]; i++) {
    lua_pushinteger(L, k_consts[i].value);
    lua_setfield(L, -2, k_consts[i].name);
  }
  lua_pop(L, 1);
  register_subtable(L, "noise", l_noise_lib);
}
