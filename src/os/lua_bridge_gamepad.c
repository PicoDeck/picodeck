#include "lua_bridge_internal.h"
#include "gamepad.h"
#include "../drivers/keyboard.h"
#include "btn_repeat.h"

// ── picocalc.gamepad.* ───────────────────────────────────────────────────────
// The logical gamepad (PAD_* buttons aliased to keys; kbd_event_queue.h; AXIS_*
// sticks and triggers, drivers/pad_source.h). Its masks and axes are updated by
// input.update(), like getButtons*, and the keys behind it keep reporting
// through picocalc.input.

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

// getAxis(axis) -> -1.0 to 1.0 (triggers 0.0 to 1.0), as of the last
// input.update(). axis is one AXIS_* constant: an exact integer.
static int l_gamepad_getAxis(lua_State *L) {
  lua_Integer axis = luaL_checkinteger(L, 1);
  luaL_argcheck(L, axis >= AXIS_LX && axis <= AXIS_RT, 1,
                "expected one AXIS_* constant");
  lua_pushnumber(L, (lua_Number)kbd_get_pad_axis((int)axis));
  return 1;
}

// The D-pad, A and B share their bits with BTN_UP..BTN_ESC, so a mask of
// them reads the same in either space (system/lib/panels.lua ORs them into
// its key masks, and comics name keys as BTN_* constants).
_Static_assert(PAD_UP == BTN_UP && PAD_DOWN == BTN_DOWN &&
                   PAD_LEFT == BTN_LEFT && PAD_RIGHT == BTN_RIGHT &&
                   PAD_A == BTN_ENTER && PAD_B == BTN_ESC,
               "PAD_UP..PAD_B match BTN_UP..BTN_ESC");

#define PAD_DPAD (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)

// The gamepad's repeat clock, worked out once per kbd_poll: every call in
// one frame returns the same mask, and the clock advances once.
static btn_repeat_t s_rep;
static uint32_t s_rep_mask, s_rep_poll;
static bool s_rep_valid;

void lb_gamepad_repeat_reset(void) {
  btn_repeat_reset(&s_rep);
  s_rep_valid = false;
}

static uint32_t pad_repeated(void) {
  uint32_t poll = kbd_poll_count();
  if (!s_rep_valid || poll != s_rep_poll) {
    uint32_t delay, rate;
    lb_input_repeat_timing(&delay, &rate);
    s_rep_mask = btn_repeat_step(&s_rep, kbd_get_pad(), kbd_get_pad_pressed(),
                                 to_ms_since_boot(get_absolute_time()), delay,
                                 rate);
    s_rep_poll = poll;
    s_rep_valid = true;
  }
  return s_rep_mask;
}

// getButtonsRepeated() -> this frame's presses plus a repeat edge for each
// button held past input.setRepeat's delay, then every rate ms.
static int l_gamepad_getButtonsRepeated(lua_State *L) {
  lua_pushinteger(L, pad_repeated());
  return 1;
}

// getNavPressed() -> what a menu or list acts on: the D-pad with auto-repeat,
// the other buttons' press edges, and A for an Enter press, B for an Esc
// press, each only while that key is no button's key.
static int l_gamepad_getNavPressed(lua_State *L) {
  uint32_t nav =
      (pad_repeated() & PAD_DPAD) | (kbd_get_pad_pressed() & ~PAD_DPAD);
  uint32_t keys = kbd_get_buttons_pressed();
  const kbd_padmap_t *map = kbd_get_pad_map();
  if ((keys & BTN_ENTER) && !kbd_padmap_has_key(map, KEY_ENTER))
    nav |= PAD_A;
  if ((keys & BTN_ESC) && !kbd_padmap_has_key(map, KEY_ESC))
    nav |= PAD_B;
  lua_pushinteger(L, nav);
  return 1;
}

static const luaL_Reg l_gamepad_lib[] = {
    {"getButtons", l_gamepad_getButtons},
    {"getButtonsPressed", l_gamepad_getButtonsPressed},
    {"getButtonsReleased", l_gamepad_getButtonsReleased},
    {"getButtonsRepeated", l_gamepad_getButtonsRepeated},
    {"getNavPressed", l_gamepad_getNavPressed},
    {"getLabel", l_gamepad_getLabel},
    {"getAxis", l_gamepad_getAxis},
    {NULL, NULL}};

void lua_bridge_gamepad_init(lua_State *L) {
  lb_gamepad_repeat_reset();
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
  lua_pushinteger(L, AXIS_LX); lua_setfield(L, -2, "AXIS_LX");
  lua_pushinteger(L, AXIS_LY); lua_setfield(L, -2, "AXIS_LY");
  lua_pushinteger(L, AXIS_RX); lua_setfield(L, -2, "AXIS_RX");
  lua_pushinteger(L, AXIS_RY); lua_setfield(L, -2, "AXIS_RY");
  lua_pushinteger(L, AXIS_LT); lua_setfield(L, -2, "AXIS_LT");
  lua_pushinteger(L, AXIS_RT); lua_setfield(L, -2, "AXIS_RT");
  lua_pop(L, 1);
}
