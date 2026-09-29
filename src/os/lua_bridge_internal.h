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

#define GRAPHICS_IMAGE_MT "picocalc.graphics.image"

// The live image at idx, or a Lua error (wrong type, or pixels freed by its
// finaliser: data is NULL only after __gc). Every image argument goes
// through this, never a bare luaL_checkudata.
static inline lua_image_t *lb_check_image(lua_State *L, int idx) {
  lua_image_t *img = (lua_image_t *)luaL_checkudata(L, idx, GRAPHICS_IMAGE_MT);
  if (!img->data)
    luaL_error(L, "attempt to use a freed image");
  return img;
}

// sys.qmiPsramAlloc buffer handle (lua_bridge_sys.c): a full userdata that
// owns a umm_malloc block. p is NULL once freed. Check it with
// luaL_checkudata/luaL_testudata(L, idx, QMI_BUF_MT), never lua_touserdata.
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
