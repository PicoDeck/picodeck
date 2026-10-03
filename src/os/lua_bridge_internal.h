#pragma once
#include "lua_bridge.h"

// All original lua_bridge.c includes to be shared
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/http.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../drivers/tcp.h"
#include "../drivers/wifi.h"
#include "../os/clock.h"
#include "../os/config.h"
#include "../os/appconfig.h"
#include "../os/os.h"
#include "../os/screenshot.h"
#include "../os/system_menu.h"
#include "../os/ui.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "lua_strbuf.h"  // string results built in place (bulk calls)
#include "lua_fastarg.h"  // one-call argument checks (lb_checkudata, lb_checkint)

#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/umm_malloc/src/umm_malloc.h"
#include "image_decoders.h"

// Shared image userdata layout — used by both graphics and display bridges
typedef struct {
    int w;
    int h;
    uint16_t *data;
    uint16_t  transparent_color;  // 0 = disabled
} lua_image_t;

// luaL_checkudata / luaL_testudata, same results and errors, without the
// registry lookup by name on every call: each type's metatable pointer is
// remembered per VM (lua_bridge.c). Use these for every bridge type.
void *lb_checkudata(lua_State *L, int idx, const char *tname);
void *lb_testudata(lua_State *L, int idx, const char *tname);

// Integer keys of the table at absolute index `idx`, read and written as
// lua_rawgeti + lua_tonumber / lua_tointeger and lua_push* + lua_rawseti do
// (same values, same conversions), but in place for keys in the table's
// array part (picodeck_lua_array_*, lua_fastarg.h). For bulk loops over
// number sequences: particles, wireframe vertices, playfields.
typedef struct {
  lua_State *L;
  int idx;
  picodeck_lua_array_t a;
} lb_array_t;

static inline void lb_array_begin(lua_State *L, int idx, lb_array_t *t) {
  t->L = L;
  t->idx = lua_absindex(L, idx);
  picodeck_lua_array_of(L, t->idx, &t->a);
}

static inline float lb_array_number(lb_array_t *t, lua_Integer i) {
  float v;
  if (picodeck_lua_array_getnum(&t->a, i, &v)) return v;
  lua_rawgeti(t->L, t->idx, i);
  v = (float)lua_tonumber(t->L, -1);
  lua_pop(t->L, 1);
  return v;
}

static inline lua_Integer lb_array_integer(lb_array_t *t, lua_Integer i) {
  lua_Integer v;
  if (picodeck_lua_array_getint(&t->a, i, &v)) return v;
  lua_rawgeti(t->L, t->idx, i);
  v = lua_tointeger(t->L, -1);
  lua_pop(t->L, 1);
  return v;
}

// A standard-API set may resize the table: the view is refreshed after it.
static inline void lb_array_set_number(lb_array_t *t, lua_Integer i, float v) {
  if (picodeck_lua_array_setnum(&t->a, i, v)) return;
  lua_pushnumber(t->L, v);
  lua_rawseti(t->L, t->idx, i);
  picodeck_lua_array_of(t->L, t->idx, &t->a);
}

static inline void lb_array_set_integer(lb_array_t *t, lua_Integer i,
                                        lua_Integer v) {
  if (picodeck_lua_array_setint(&t->a, i, v)) return;
  lua_pushinteger(t->L, v);
  lua_rawseti(t->L, t->idx, i);
  picodeck_lua_array_of(t->L, t->idx, &t->a);
}

static inline void lb_array_set_nil(lb_array_t *t, lua_Integer i) {
  if (picodeck_lua_array_setnil(&t->a, i)) return;
  lua_pushnil(t->L);
  lua_rawseti(t->L, t->idx, i);
  picodeck_lua_array_of(t->L, t->idx, &t->a);
}

#define GRAPHICS_IMAGE_MT "picocalc.graphics.image"

// The live image at idx, or a Lua error (wrong type, or pixels freed by its
// finaliser: data is NULL only after __gc). Every image argument goes
// through this, never a bare lb_checkudata.
static inline lua_image_t *lb_check_image(lua_State *L, int idx) {
  lua_image_t *img = (lua_image_t *)lb_checkudata(L, idx, GRAPHICS_IMAGE_MT);
  if (!img->data)
    luaL_error(L, "attempt to use a freed image");
  return img;
}

// sys.qmiPsramAlloc buffer handle (lua_bridge_sys.c): a full userdata that
// owns a umm_malloc block. p is NULL once freed. Check it with
// lb_checkudata/lb_testudata(L, idx, QMI_BUF_MT), never lua_touserdata.
#define QMI_BUF_MT "picocalc.sys.qmibuf"
typedef struct {
    uint8_t *p;
    size_t size;
} qmi_buf_t;

uint16_t l_checkcolor(lua_State *L, int idx);
// Integer "quantity" arguments (coordinates, sizes, durations, volumes...):
// accept any finite number and round floats to nearest (ties toward +inf);
// NaN/inf and floats beyond +-2^24 raise an argument error. See lua_bridge.c.
lua_Integer lb_checkint(lua_State *L, int idx);
lua_Integer lb_optint(lua_State *L, int idx, lua_Integer def);
// Same rules for a value already on the stack at idx (a table field or array
// entry): errors name argument `arg` and start with `what` ("field 'x'"),
// instead of reporting a meaningless negative index like "#-1".
lua_Integer lb_checkint_at(lua_State *L, int idx, int arg, const char *what);
lua_Integer lb_optint_at(lua_State *L, int idx, int arg, const char *what,
                         lua_Integer def);
float lb_checkfloat(lua_State *L, int idx);
float lb_optfloat(lua_State *L, int idx, float def);
void lua_bridge_gfx3d_init(lua_State *L);
// Clamps v to [lo, hi]. The +-2^24 bound above does not protect sinks
// narrower than that: uint8_t volumes/colour channels and unsigned
// positions clamp instead of wrapping.
static inline lua_Integer lb_clamp_int(lua_Integer v, lua_Integer lo,
                                       lua_Integer hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
bool fs_sandbox_check(lua_State *L, const char *path, bool write);
void http_lua_fire_pending(lua_State *L);
void tcp_lua_fire_pending(lua_State *L);
extern bool s_screenshot_pending;

void register_subtable(lua_State *L, const char *name, const luaL_Reg *funcs);
// Every bridge type registers through this (see lua_bridge.c): methods in a
// separate __index table, metamethods only in the metatable, metatable
// locked. Each type's check_* accessor must also reject a destroyed object.
void lb_register_type(lua_State *L, const char *mtname,
                      const luaL_Reg *methods, const luaL_Reg *meta);

void lua_bridge_display_init(lua_State *L);
void lua_bridge_input_init(lua_State *L);
void lua_bridge_gamepad_init(lua_State *L);
void lua_bridge_sys_init(lua_State *L);
void lua_bridge_fs_init(lua_State *L);
void lua_bridge_network_init(lua_State *L);
void lua_bridge_config_init(lua_State *L);
void lua_bridge_appconfig_init(lua_State *L);
void lua_bridge_perf_init(lua_State *L);
void lua_bridge_graphics_init(lua_State *L);
void lua_bridge_ui_init(lua_State *L);
void lua_bridge_audio_init(lua_State *L);
void lua_bridge_sound_init(lua_State *L);
void lua_bridge_sound_poll(lua_State *L);
void lua_bridge_repl_init(lua_State *L);
void lua_bridge_video_init(lua_State *L);
void lua_bridge_tcp_init(lua_State *L);
void lua_bridge_crypto_init(lua_State *L);
void lua_bridge_mod_init(lua_State *L);
void lua_bridge_json_init(lua_State *L);
void lua_bridge_require_init(lua_State *L);

// Shared JSON codec — game.save is built on these so the firmware carries one
// JSON implementation rather than several hand-rolled ones.
void lua_json_encode_push(lua_State *L, int idx, int indent);
bool lua_json_decode_push(lua_State *L, const char *s, size_t len,
                          const char **err);
