// picocalc.gfx3d — Lua binding for the gfx3d renderer (src/os/gfx3d.c).
// One context per app, allocated on first use and freed by lua_close through
// a registry-anchored userdata. draw*() transform at once and keep no mesh
// pointer, so a mesh collected before endScene() is harmless; sprite images
// are anchored in a registry table until the next beginScene().
#include "lua_bridge_internal.h"
#include "gfx3d.h"
#include "../drivers/display.h"
#include "../drivers/display_raster.h"
#include "lauxlib.h"
#include "pico/time.h"
#include <math.h>

#define MESH_MT "picocalc.gfx3d.mesh"
#define CTX_MT "picocalc.gfx3d.context"
#define CTX_KEY "picodeck.gfx3d.ctx"
#define SPRITES_KEY "picodeck.gfx3d.sprites"

typedef struct {
  gfx3d_mesh_t *m;  // NULL once collected
} mesh_ud_t;

typedef struct {
  gfx3d_t *g;  // NULL once collected
} ctx_ud_t;

static gfx3d_t *s_g;  // this app's context, NULL until first use
static bool s_in_scene;
static bool s_closing;  // the live context was just freed (lua_close order)
static int s_nsprites;
static uint32_t s_us_geom, s_us_raster;

static uint32_t now_us(void) {
  return (uint32_t)to_us_since_boot(get_absolute_time());
}

// ── Context ─────────────────────────────────────────────────────────────────

static int l_ctx_gc(lua_State *L) {
  ctx_ud_t *u = (ctx_ud_t *)luaL_checkudata(L, 1, CTX_MT);
  if (u->g) {
    if (u->g == s_g) {  // the app's live context: the VM is closing
      s_g = NULL;
      s_in_scene = false;
      s_closing = true;
    }
    gfx3d_free(u->g);
    u->g = NULL;
  }
  return 0;
}

static gfx3d_t *ctx(lua_State *L) {
  if (s_closing) {
    luaL_error(L, "gfx3d: the app is closing");
    return NULL;
  }
  if (s_g) return s_g;
  ctx_ud_t *u = (ctx_ud_t *)lua_newuserdatauv(L, sizeof *u, 0);
  u->g = NULL;
  luaL_setmetatable(L, CTX_MT);
  u->g = gfx3d_new(FB_WIDTH, FB_HEIGHT);
  if (!u->g) {
    luaL_error(L, "gfx3d: out of memory");
    return NULL;
  }
  lua_setfield(L, LUA_REGISTRYINDEX, CTX_KEY);
  s_g = u->g;
  return s_g;
}

static void need_scene(lua_State *L, const char *fn) {
  if (!s_in_scene) luaL_error(L, "gfx3d.%s: call beginScene() first", fn);
}

// ── Meshes ──────────────────────────────────────────────────────────────────

static mesh_ud_t *check_mesh(lua_State *L, int idx) {
  mesh_ud_t *u = (mesh_ud_t *)luaL_checkudata(L, idx, MESH_MT);
  if (!u->m) luaL_error(L, "attempt to use a freed mesh");
  return u;
}

static int l_mesh_gc(lua_State *L) {
  mesh_ud_t *u = (mesh_ud_t *)luaL_checkudata(L, 1, MESH_MT);
  if (u->m) {
    gfx3d_mesh_free(u->m);
    u->m = NULL;
  }
  return 0;
}

static int l_mesh_getInfo(lua_State *L) {
  const gfx3d_mesh_t *m = check_mesh(L, 1)->m;
  lua_pushinteger(L, m->nverts);
  lua_pushinteger(L, m->ntris);
  lua_pushnumber(L, m->center[0]);
  lua_pushnumber(L, m->center[1]);
  lua_pushnumber(L, m->center[2]);
  lua_pushnumber(L, m->radius);
  return 6;
}

// Element i (1-based) of the table at t as a finite float.
static float tab_float(lua_State *L, int t, int i, int arg, const char *what) {
  lua_rawgeti(L, t, i);
  int ok;
  lua_Number n = lua_tonumberx(L, -1, &ok);
  lua_pop(L, 1);
  if (!ok)
    luaL_argerror(L, arg, lua_pushfstring(L, "%s[%d]: number expected", what, i));
  if (!isfinite(n))
    luaL_argerror(L, arg,
                  lua_pushfstring(L, "%s[%d]: number is NaN or infinite", what, i));
  return (float)n;
}

// Element i (1-based) of the table at t as an exact integer.
static lua_Integer tab_int(lua_State *L, int t, int i, int arg, const char *what) {
  lua_rawgeti(L, t, i);
  int ok;
  lua_Integer v = lua_tointegerx(L, -1, &ok);
  lua_pop(L, 1);
  if (!ok)
    luaL_argerror(L, arg, lua_pushfstring(L, "%s[%d]: integer expected", what, i));
  return v;
}

// newMesh(verts, tris, colors [, flags])
static int l_gfx3d_newMesh(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  const lua_Integer nv3 = (lua_Integer)lua_rawlen(L, 1);
  const lua_Integer nt3 = (lua_Integer)lua_rawlen(L, 2);
  if (nv3 < 3 || nv3 % 3 || nv3 / 3 > GFX3D_MAX_VERTS)
    return luaL_argerror(L, 1, lua_pushfstring(L,
        "expected 3..%d coordinates, a multiple of 3", 3 * GFX3D_MAX_VERTS));
  if (nt3 < 3 || nt3 % 3 || nt3 / 3 > GFX3D_MAX_TRIS)
    return luaL_argerror(L, 2, lua_pushfstring(L,
        "expected 3..%d vertex indices, a multiple of 3", 3 * GFX3D_MAX_TRIS));
  const int nv = (int)(nv3 / 3), nt = (int)(nt3 / 3);
  const bool one_color = lua_type(L, 3) == LUA_TNUMBER;
  if (!one_color) {
    luaL_checktype(L, 3, LUA_TTABLE);
    if ((lua_Integer)lua_rawlen(L, 3) != nt)
      return luaL_argerror(L, 3, "expected one colour per triangle");
  }
  const int ft = lua_type(L, 4);
  if (ft != LUA_TNONE && ft != LUA_TNIL && ft != LUA_TNUMBER && ft != LUA_TTABLE)
    return luaL_typeerror(L, 4, "nil, integer or table");
  if (ft == LUA_TTABLE && (lua_Integer)lua_rawlen(L, 4) != nt)
    return luaL_argerror(L, 4, "expected one flags value per triangle");

  // The userdata owns the mesh from here, so an argument error below frees it.
  mesh_ud_t *u = (mesh_ud_t *)lua_newuserdatauv(L, sizeof *u, 0);
  u->m = NULL;
  luaL_setmetatable(L, MESH_MT);
  u->m = gfx3d_mesh_alloc(nv, nt);
  if (!u->m) return luaL_error(L, "gfx3d.newMesh: out of memory");
  gfx3d_mesh_t *m = u->m;
  for (int i = 0; i < 3 * nv; i++) m->xyz[i] = tab_float(L, 1, i + 1, 1, "verts");
  for (int i = 0; i < 3 * nt; i++) {
    lua_Integer v = tab_int(L, 2, i + 1, 2, "tris");
    if (v < 1 || v > nv)
      return luaL_argerror(L, 2, lua_pushfstring(L,
          "tris[%d]: vertex %d out of range 1..%d", i + 1, (int)v, nv));
    m->idx[i] = (uint16_t)(v - 1);
  }
  const lua_Integer c0 = one_color ? luaL_checkinteger(L, 3) : 0;
  const lua_Integer f0 = ft == LUA_TNUMBER ? luaL_checkinteger(L, 4) : 0;
  for (int t = 0; t < nt; t++) {
    lua_Integer c = one_color ? c0 : tab_int(L, 3, t + 1, 3, "colors");
    if (c < 0 || c > 0xFFFF)
      return luaL_argerror(L, 3, "colour out of range 0..65535");
    m->color[t] = (uint16_t)c;
    lua_Integer f = ft == LUA_TTABLE ? tab_int(L, 4, t + 1, 4, "flags") : f0;
    if (f < 0 || f > (lua_Integer)GFX3D_FLAG_MASK)
      return luaL_argerror(L, 4,
          "flags must combine DOUBLE_SIDED, UNLIT and NO_FOG");
    m->flags[t] = (uint8_t)f;
  }
  const char *err = gfx3d_mesh_finish(m);
  if (err) return luaL_error(L, "gfx3d.newMesh: %s", err);
  return 1;
}

// ── Camera, projection, light, fog, sky ─────────────────────────────────────

static int l_gfx3d_setViewport(lua_State *L) {
  // int64_t locals: x, y, w, h are each any 32-bit integer the app passes,
  // so x + w / y + h (and the negative-origin fixups below) must not
  // overflow a 32-bit int before the clamp brings them back into range.
  int64_t x = lb_checkint(L, 1), y = lb_checkint(L, 2);
  int64_t w = lb_checkint(L, 3), h = lb_checkint(L, 4);
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > FB_WIDTH) w = FB_WIDTH - x;
  if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
  if (w < 1 || h < 1)
    return luaL_error(L, "gfx3d.setViewport: the viewport must overlap the screen");
  gfx3d_set_viewport(ctx(L), (int)x, (int)y, (int)w, (int)h);
  return 0;
}

static int l_gfx3d_setProjection(lua_State *L) {
  const float fov = lb_checkfloat(L, 1), zn = lb_checkfloat(L, 2);
  const float zf = lb_checkfloat(L, 3);
  if (!gfx3d_set_projection(ctx(L), fov, zn, zf))
    return luaL_error(L,
        "gfx3d.setProjection: need 0.01 <= fovY <= 3.0 and 0 < near < far");
  return 0;
}

static int l_gfx3d_setCamera(lua_State *L) {
  const float p[3] = {lb_checkfloat(L, 1), lb_checkfloat(L, 2), lb_checkfloat(L, 3)};
  gfx3d_set_camera(ctx(L), p, lb_checkfloat(L, 4), lb_checkfloat(L, 5),
                   lb_checkfloat(L, 6));
  return 0;
}

static int l_gfx3d_lookAt(lua_State *L) {
  const float e[3] = {lb_checkfloat(L, 1), lb_checkfloat(L, 2), lb_checkfloat(L, 3)};
  const float t[3] = {lb_checkfloat(L, 4), lb_checkfloat(L, 5), lb_checkfloat(L, 6)};
  const float u[3] = {lb_optfloat(L, 7, 0.0f), lb_optfloat(L, 8, 1.0f),
                      lb_optfloat(L, 9, 0.0f)};
  if (!gfx3d_look_at(ctx(L), e, t, u))
    return luaL_error(L, "gfx3d.lookAt: eye and target must differ, and up "
                         "must not be parallel to the view");
  return 0;
}

static int l_gfx3d_setLight(lua_State *L) {
  const float dx = lb_checkfloat(L, 1), dy = lb_checkfloat(L, 2);
  const float dz = lb_checkfloat(L, 3), amb = lb_optfloat(L, 4, 0.25f);
  if (!gfx3d_set_light(ctx(L), dx, dy, dz, amb))
    return luaL_error(L, "gfx3d.setLight: the direction must not be zero");
  return 0;
}

static int l_gfx3d_setFog(lua_State *L) {
  if (lua_isnoneornil(L, 1)) {
    gfx3d_set_fog(ctx(L), false, 0.0f, 0.0f, 0);
    return 0;
  }
  const float fn = lb_checkfloat(L, 1), ff = lb_checkfloat(L, 2);
  const uint16_t c = l_checkcolor(L, 3);
  if (!(fn >= 0.0f) || !(ff > fn))
    return luaL_error(L, "gfx3d.setFog: need 0 <= near < far");
  gfx3d_set_fog(ctx(L), true, fn, ff, c);
  return 0;
}

static int l_gfx3d_setSky(lua_State *L) {
  if (lua_isnoneornil(L, 1)) {
    gfx3d_set_sky(ctx(L), NULL, NULL, 0);
    return 0;
  }
  luaL_checktype(L, 1, LUA_TTABLE);
  const int n = (int)lua_rawlen(L, 1);
  if (n < 1 || n > GFX3D_MAX_SKY_BANDS)
    return luaL_argerror(L, 1, "expected 1 to 8 bands");
  float ang[GFX3D_MAX_SKY_BANDS];
  uint16_t col[GFX3D_MAX_SKY_BANDS];
  for (int i = 0; i < n; i++) {
    lua_rawgeti(L, 1, i + 1);
    if (!lua_istable(L, -1))
      return luaL_argerror(L, 1, lua_pushfstring(L,
          "band %d: expected {angle, color}", i + 1));
    const int b = lua_gettop(L);
    ang[i] = tab_float(L, b, 1, 1, "band angle");
    const lua_Integer c = tab_int(L, b, 2, 1, "band color");
    if (c < 0 || c > 0xFFFF) return luaL_argerror(L, 1, "band colour out of range");
    col[i] = (uint16_t)c;
    lua_pop(L, 1);
  }
  if (!gfx3d_set_sky(ctx(L), ang, col, n))
    return luaL_argerror(L, 1,
        "band angles must increase and lie within +-89 degrees");
  return 0;
}

// ── Scene ───────────────────────────────────────────────────────────────────

static void clear_sprites(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, SPRITES_KEY);
  for (int i = 1; i <= s_nsprites; i++) {
    lua_pushnil(L);
    lua_rawseti(L, -2, i);
  }
  lua_pop(L, 1);
  s_nsprites = 0;
}

static int l_gfx3d_beginScene(lua_State *L) {
  gfx3d_t *g = ctx(L);
  const bool clear = !lua_isnoneornil(L, 1);
  gfx3d_begin(g, clear, clear ? l_checkcolor(L, 1) : 0);
  clear_sprites(L);
  s_us_geom = s_us_raster = 0;
  s_in_scene = true;
  return 0;
}

// Optional trailing (scale, bias, sortAsOne) starting at argument `opt`.
static void draw_instance(lua_State *L, const gfx3d_mesh_t *m, const float rot[9],
                          const float pos[3], int opt) {
  const float scale = lb_optfloat(L, opt, 1.0f);
  const float bias = lb_optfloat(L, opt + 1, 0.0f);
  const bool one = lua_toboolean(L, opt + 2);
  if (!(scale > 0.0f)) luaL_argerror(L, opt, "scale must be positive");
  const uint32_t t0 = now_us();
  gfx3d_draw(s_g, m, rot, pos, scale, bias, one);
  s_us_geom += now_us() - t0;
}

// draw(mesh, x, y, z, yaw, pitch, roll [, scale [, bias [, sortAsOne]]])
static int l_gfx3d_draw(lua_State *L) {
  need_scene(L, "draw");
  const gfx3d_mesh_t *m = check_mesh(L, 1)->m;
  const float pos[3] = {lb_checkfloat(L, 2), lb_checkfloat(L, 3), lb_checkfloat(L, 4)};
  float rot[9];
  gfx3d_rot_euler(rot, lb_checkfloat(L, 5), lb_checkfloat(L, 6), lb_checkfloat(L, 7));
  draw_instance(L, m, rot, pos, 8);
  return 0;
}

// drawBasis(mesh, x, y, z, fx, fy, fz, ux, uy, uz [, scale [, bias [, sortAsOne]]])
static int l_gfx3d_drawBasis(lua_State *L) {
  need_scene(L, "drawBasis");
  const gfx3d_mesh_t *m = check_mesh(L, 1)->m;
  const float pos[3] = {lb_checkfloat(L, 2), lb_checkfloat(L, 3), lb_checkfloat(L, 4)};
  const float f[3] = {lb_checkfloat(L, 5), lb_checkfloat(L, 6), lb_checkfloat(L, 7)};
  const float u[3] = {lb_checkfloat(L, 8), lb_checkfloat(L, 9), lb_checkfloat(L, 10)};
  float rot[9];
  if (!gfx3d_rot_basis(rot, f, u))
    return luaL_error(L, "gfx3d.drawBasis: forward and up must be non-zero "
                         "and not parallel");
  draw_instance(L, m, rot, pos, 11);
  return 0;
}

// drawList(list [, bias [, start]]): every mesh in the array `list`, as it
// is (world coordinates, no transform), from list[start] (default 1) round
// to list[start - 1]. One Lua->C call for a whole static scene: each draw()
// costs a Lua->C call, ~20-65 us of overhead on the device.
static int l_gfx3d_drawList(lua_State *L) {
  need_scene(L, "drawList");
  luaL_checktype(L, 1, LUA_TTABLE);
  const float bias = lb_optfloat(L, 2, 0.0f);
  const lua_Integer n = luaL_len(L, 1);
  const lua_Integer start = lb_optint(L, 3, 1);
  if (n == 0) return 0;
  if (start < 1 || start > n) return luaL_argerror(L, 3, "start must be in 1..#list");
  static const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  static const float origin[3] = {0, 0, 0};
  uint32_t geom = 0;
  for (lua_Integer k = 0; k < n; k++) {
    const lua_Integer i = (start - 1 + k) % n + 1;
    lua_rawgeti(L, 1, i);
    mesh_ud_t *u = (mesh_ud_t *)luaL_testudata(L, -1, MESH_MT);
    if (!u || !u->m) {
      s_us_geom += geom;
      return luaL_error(L, "gfx3d.drawList: list[%d] is not a live mesh", (int)i);
    }
    const uint32_t t0 = now_us();
    gfx3d_draw(s_g, u->m, ident, origin, 1.0f, bias, false);
    geom += now_us() - t0;
    lua_pop(L, 1);
  }
  s_us_geom += geom;
  return 0;
}

static int l_gfx3d_drawBackground(lua_State *L) {
  need_scene(L, "drawBackground");
  const gfx3d_mesh_t *m = check_mesh(L, 1)->m;
  const uint32_t t0 = now_us();
  gfx3d_draw_background(s_g, m);
  s_us_geom += now_us() - t0;
  return 0;
}

// drawSprite(image, x, y, z, size [, sx, sy, sw, sh [, bias]])
static int l_gfx3d_drawSprite(lua_State *L) {
  need_scene(L, "drawSprite");
  lua_image_t *img = lb_check_image(L, 1);
  const float pos[3] = {lb_checkfloat(L, 2), lb_checkfloat(L, 3), lb_checkfloat(L, 4)};
  const float size = lb_checkfloat(L, 5);
  if (!(size > 0.0f)) return luaL_argerror(L, 5, "size must be positive");
  // int64_t locals: sx/sy/sw/sh are each any 32-bit integer the app passes,
  // so img->w - sx / img->h - sy (the default values and the clamps below)
  // must not overflow a 32-bit int before gfx3d_draw_sprite's own range
  // check narrows them.
  int64_t sx = lb_optint(L, 6, 0), sy = lb_optint(L, 7, 0);
  int64_t sw = lb_optint(L, 8, (lua_Integer)(img->w - sx));
  int64_t sh = lb_optint(L, 9, (lua_Integer)(img->h - sy));
  const float bias = lb_optfloat(L, 10, 0.0f);
  if (sx < 0) { sw += sx; sx = 0; }
  if (sy < 0) { sh += sy; sy = 0; }
  if (sw > img->w - sx) sw = img->w - sx;
  if (sh > img->h - sy) sh = img->h - sy;
  if (sw <= 0 || sh <= 0 || s_nsprites >= 0xFFFE) return 0;
  const int slot = s_nsprites + 1;
  const uint32_t t0 = now_us();
  const bool kept = gfx3d_draw_sprite(s_g, slot, pos, size, (int)sx, (int)sy,
                                      (int)sw, (int)sh, bias);
  s_us_geom += now_us() - t0;
  if (kept) {
    lua_getfield(L, LUA_REGISTRYINDEX, SPRITES_KEY);
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, slot);
    lua_pop(L, 1);
    s_nsprites = slot;
  }
  return 0;
}

static bool resolve_sprite(void *ud, int slot, const uint16_t **data, int *w,
                           int *h, uint16_t *key) {
  lua_State *L = (lua_State *)ud;
  lua_getfield(L, LUA_REGISTRYINDEX, SPRITES_KEY);
  lua_rawgeti(L, -1, slot);
  lua_image_t *img = (lua_image_t *)luaL_testudata(L, -1, GRAPHICS_IMAGE_MT);
  lua_pop(L, 2);  // the image stays anchored in the sprites table
  if (!img || !img->data) return false;
  *data = img->data;
  *w = img->w;
  *h = img->h;
  *key = img->transparent_color ? img->transparent_color
                                : display_get_transparent_color();
  return true;
}

static int l_gfx3d_endScene(lua_State *L) {
  need_scene(L, "endScene");
  display_raster_target_t d;
  display_get_raster_target(&d);
  const gfx3d_target_t t = {d.fb, d.stride, d.clip_x0, d.clip_y0, d.clip_x1,
                            d.clip_y1, d.swap, resolve_sprite, L};
  const uint32_t t0 = now_us();
  gfx3d_end(s_g, &t);
  s_us_raster = now_us() - t0;
  s_in_scene = false;
  return 0;
}

static int l_gfx3d_project(lua_State *L) {
  const float p[3] = {lb_checkfloat(L, 1), lb_checkfloat(L, 2), lb_checkfloat(L, 3)};
  float sx, sy, d;
  if (!gfx3d_project(ctx(L), p, &sx, &sy, &d)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushnumber(L, sx);
  lua_pushnumber(L, sy);
  lua_pushnumber(L, d);
  return 3;
}

// getStats([t]): the last frame's counters, into t (returned) when given, so
// a per-frame call need not allocate, else into a new table.
static int l_gfx3d_getStats(lua_State *L) {
  const gfx3d_stats_t *s = gfx3d_get_stats(ctx(L));
  if (lua_isnoneornil(L, 1)) {
    lua_createtable(L, 0, 8);
  } else {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
  }
  lua_pushinteger(L, (lua_Integer)s->tris_in);  lua_setfield(L, -2, "tris_in");
  lua_pushinteger(L, (lua_Integer)s->culled);   lua_setfield(L, -2, "culled");
  lua_pushinteger(L, (lua_Integer)s->clipped);  lua_setfield(L, -2, "clipped");
  lua_pushinteger(L, (lua_Integer)s->drawn);    lua_setfield(L, -2, "drawn");
  lua_pushinteger(L, (lua_Integer)s->sprites);  lua_setfield(L, -2, "sprites");
  lua_pushinteger(L, (lua_Integer)s->overflow); lua_setfield(L, -2, "overflow");
  lua_pushinteger(L, (lua_Integer)s_us_geom);   lua_setfield(L, -2, "us_geom");
  lua_pushinteger(L, (lua_Integer)s_us_raster); lua_setfield(L, -2, "us_raster");
  return 1;
}

static const luaL_Reg l_mesh_methods[] = {{"getInfo", l_mesh_getInfo}, {NULL, NULL}};
static const luaL_Reg l_mesh_meta[] = {{"__gc", l_mesh_gc}, {NULL, NULL}};
static const luaL_Reg l_ctx_methods[] = {{NULL, NULL}};
static const luaL_Reg l_ctx_meta[] = {{"__gc", l_ctx_gc}, {NULL, NULL}};

static const luaL_Reg l_gfx3d_lib[] = {
    {"newMesh", l_gfx3d_newMesh},
    {"setViewport", l_gfx3d_setViewport},
    {"setProjection", l_gfx3d_setProjection},
    {"setCamera", l_gfx3d_setCamera},
    {"lookAt", l_gfx3d_lookAt},
    {"setLight", l_gfx3d_setLight},
    {"setFog", l_gfx3d_setFog},
    {"setSky", l_gfx3d_setSky},
    {"beginScene", l_gfx3d_beginScene},
    {"draw", l_gfx3d_draw},
    {"drawBasis", l_gfx3d_drawBasis},
    {"drawList", l_gfx3d_drawList},
    {"drawBackground", l_gfx3d_drawBackground},
    {"drawSprite", l_gfx3d_drawSprite},
    {"endScene", l_gfx3d_endScene},
    {"project", l_gfx3d_project},
    {"getStats", l_gfx3d_getStats},
    {NULL, NULL}};

void lua_bridge_gfx3d_init(lua_State *L) {
  s_g = NULL;
  s_in_scene = false;
  s_closing = false;
  s_nsprites = 0;
  s_us_geom = s_us_raster = 0;
  lb_register_type(L, MESH_MT, l_mesh_methods, l_mesh_meta);
  lb_register_type(L, CTX_MT, l_ctx_methods, l_ctx_meta);
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, SPRITES_KEY);
  register_subtable(L, "gfx3d", l_gfx3d_lib);
  lua_getfield(L, -1, "gfx3d");
  lua_pushinteger(L, GFX3D_DOUBLE_SIDED);
  lua_setfield(L, -2, "DOUBLE_SIDED");
  lua_pushinteger(L, GFX3D_UNLIT);
  lua_setfield(L, -2, "UNLIT");
  lua_pushinteger(L, GFX3D_NO_FOG);
  lua_setfield(L, -2, "NO_FOG");
  lua_pop(L, 1);
}
