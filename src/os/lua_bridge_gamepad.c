#include "lua_bridge_internal.h"
#include "gamepad.h"
#include "../drivers/keyboard.h"

// ── picocalc.gamepad.* ───────────────────────────────────────────────────────
// The logical gamepad (PAD_* buttons aliased to keys; kbd_event_queue.h). Its
// masks are updated by input.update(), like getButtons*, and the keys behind
// it keep reporting through picocalc.input.

static int l_gamepad_getButtons(lua_State *L) {
  lua_pushinteger(L, kbd_get_pad());
  return 1;
}

static int l_gamepad_getButtonsPressed(lua_State *L) {
  lua_pushinteger(L, kbd_get_pad_pressed());
  return 1;
}

static int l_gamepad_getButtonsReleased(lua_State *L) {
  lua_pushinteger(L, kbd_get_pad_released());
  return 1;
}

// getLabel(btn [, slot]) -> the bound key's name ("F4", "Del", "W") or nil
// when that slot is unbound. btn is one PAD_* constant; slot 0 = primary
// (default), 1 = alternate. Both are identifiers: exact integers.
static int l_gamepad_getLabel(lua_State *L) {
  lua_Integer btn = luaL_checkinteger(L, 1);
  lua_Integer slot = luaL_optinteger(L, 2, 0);
  luaL_argcheck(L, btn > 0 && btn <= PAD_SELECT && (btn & (btn - 1)) == 0, 1,
                "expected one PAD_* button");
  luaL_argcheck(L, slot == 0 || slot == 1, 2,
                "slot must be 0 (primary) or 1 (alternate)");
  const char *label = gamepad_get_label((uint32_t)btn, (int)slot);
  if (label)
    lua_pushstring(L, label);
  else
    lua_pushnil(L);
  return 1;
}

static const luaL_Reg l_gamepad_lib[] = {
    {"getButtons", l_gamepad_getButtons},
    {"getButtonsPressed", l_gamepad_getButtonsPressed},
    {"getButtonsReleased", l_gamepad_getButtonsReleased},
    {"getLabel", l_gamepad_getLabel},
    {NULL, NULL}};

void lua_bridge_gamepad_init(lua_State *L) {
  register_subtable(L, "gamepad", l_gamepad_lib);
  lua_getfield(L, -1, "gamepad");
  lua_pushinteger(L, PAD_UP); lua_setfield(L, -2, "PAD_UP");
  lua_pushinteger(L, PAD_DOWN); lua_setfield(L, -2, "PAD_DOWN");
  lua_pushinteger(L, PAD_LEFT); lua_setfield(L, -2, "PAD_LEFT");
  lua_pushinteger(L, PAD_RIGHT); lua_setfield(L, -2, "PAD_RIGHT");
  lua_pushinteger(L, PAD_A); lua_setfield(L, -2, "PAD_A");
  lua_pushinteger(L, PAD_B); lua_setfield(L, -2, "PAD_B");
  lua_pushinteger(L, PAD_X); lua_setfield(L, -2, "PAD_X");
  lua_pushinteger(L, PAD_Y); lua_setfield(L, -2, "PAD_Y");
  lua_pushinteger(L, PAD_L); lua_setfield(L, -2, "PAD_L");
  lua_pushinteger(L, PAD_R); lua_setfield(L, -2, "PAD_R");
  lua_pushinteger(L, PAD_START); lua_setfield(L, -2, "PAD_START");
  lua_pushinteger(L, PAD_SELECT); lua_setfield(L, -2, "PAD_SELECT");
  lua_pop(L, 1);
}
