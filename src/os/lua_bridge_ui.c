#include "lua_bridge_internal.h"
#include "../../third_party/lua-5.4/src/lauxlib.h"
#include "ui_widgets.h"
#include "text_wrap.h"
#include "toast.h"
#include "umm_malloc.h"

// ── picocalc.ui.*
// ─────────────────────────────────────────────────────────────

static int l_ui_drawHeader(lua_State *L) {
  const char *title = luaL_checkstring(L, 1);
  lua_pushinteger(L, ui_draw_header(title));
  return 1;
}

static int l_ui_drawFooter(lua_State *L) {
  const char *left = luaL_optstring(L, 1, NULL);
  const char *right = luaL_optstring(L, 2, NULL);
  ui_draw_footer(left, right);
  return 0;
}

static int l_ui_drawTabs(lua_State *L) {
  int y = (int)lb_checkint(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);  // tabs array
  int active_index = (int)luaL_checkinteger(L, 3);
  int prev_key = (int)luaL_optinteger(L, 4, 0);
  int next_key = (int)luaL_optinteger(L, 5, 0);

  // Convert to 0-based index for C
  active_index--;

  // Count tabs
  int count = (int)lua_rawlen(L, 2);
  if (count == 0) {
    lua_pushinteger(L, active_index + 1); // Return 1-based
    lua_pushinteger(L, 0);
    return 2;
  }

  // Allocate tab strings array
  const char **tabs = (const char **)umm_malloc(sizeof(char *) * count);
  if (!tabs) {
    return luaL_error(L, "Failed to allocate memory for tabs");
  }

  // Extract tab strings from Lua table
  for (int i = 0; i < count; i++) {
    lua_rawgeti(L, 2, i + 1);  // Lua arrays are 1-indexed
    tabs[i] = lua_tostring(L, -1);
    lua_pop(L, 1);
  }

  // Check for key presses if keys specified
  if (prev_key > 0 || next_key > 0) {
    int pressed = kbd_get_buttons_pressed();
    if (prev_key > 0 && (pressed & prev_key)) {
      active_index--;
      if (active_index < 0)
        active_index = count - 1;
    }
    if (next_key > 0 && (pressed & next_key)) {
      active_index++;
      if (active_index >= count)
        active_index = 0;
    }
  }

  // Draw tabs
  int height = ui_draw_tabs(tabs, count, active_index, y);

  umm_free(tabs);

  // Return new active index (1-based) and height consumed
  lua_pushinteger(L, active_index + 1);
  lua_pushinteger(L, height);
  return 2;
}

// picocalc.ui.textInput([prompt [, default]]) → string | nil
static int l_ui_textInput(lua_State *L) {
  const char *prompt      = luaL_optstring(L, 1, NULL);
  const char *default_val = luaL_optstring(L, 2, NULL);
  char buf[128];
  if (ui_text_input(prompt, default_val, buf, sizeof(buf))) {
    lua_pushstring(L, buf);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

// picocalc.ui.confirm(message) → bool
static int l_ui_confirm(lua_State *L) {
  const char *message = luaL_checkstring(L, 1);
  lua_pushboolean(L, ui_confirm(message));
  return 1;
}

static int l_ui_drawSpinner(lua_State *L) {
  int cx = (int)lb_checkint(L, 1);
  int cy = (int)lb_checkint(L, 2);
  int r = (int)lb_optint(L, 3, 8);
  int frame = (int)lb_optint(L, 4, 0);
  ui_draw_spinner(cx, cy, r, frame);
  return 0;
}

static int l_ui_splash(lua_State *L) {
  const char *status  = luaL_optstring(L, 1, NULL);
  const char *subtext = luaL_optstring(L, 2, NULL);
  ui_draw_splash(status, subtext);
  return 0;
}

// ── Widget rendering primitives ──────────────────────────────────────────────

// picocalc.ui.drawPanel(x, y, w, h, [title])
static int l_ui_drawPanel(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  int w = (int)lb_checkint(L, 3);
  int h = (int)lb_checkint(L, 4);
  const char *title = luaL_optstring(L, 5, NULL);
  ui_widget_panel(x, y, w, h, title);
  return 0;
}

// picocalc.ui.drawTextField(x, y, w, text, cursor_pos, scroll_offset, focused, show_cursor)
static int l_ui_drawTextField(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  int w = (int)lb_checkint(L, 3);
  size_t text_len = 0;
  const char *text = luaL_checklstring(L, 4, &text_len);
  int cursor_pos   = (int)lb_checkint(L, 5);
  int scroll_off   = (int)lb_checkint(L, 6);
  bool focused     = lua_toboolean(L, 7);
  bool show_cursor = lua_toboolean(L, 8);
  ui_widget_textfield(x, y, w, text, (int)text_len, cursor_pos, scroll_off,
                      focused, show_cursor);
  return 0;
}

// picocalc.ui.drawTextArea(x, y, w, h, text, segments_table, scroll_y,
//                          cursor_row, cursor_col, focused, show_cursor)
static int l_ui_drawTextArea(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  int w = (int)lb_checkint(L, 3);
  int h = (int)lb_checkint(L, 4);
  size_t text_len = 0;
  const char *text = luaL_checklstring(L, 5, &text_len);
  luaL_checktype(L, 6, LUA_TTABLE);
  int scroll_y    = (int)lb_checkint(L, 7);
  int cursor_row  = (int)lb_checkint(L, 8);
  int cursor_col  = (int)lb_checkint(L, 9);
  bool focused    = lua_toboolean(L, 10);
  bool show_cursor = lua_toboolean(L, 11);

  // Extract segments from Lua table (1-indexed) to C array (0-indexed values)
  int num_segments = (int)lua_rawlen(L, 6);
  // Use stack allocation for small segment counts, umm_malloc for large
  int stack_segs[128];
  int *segments = stack_segs;
  if (num_segments > 128) {
    segments = (int *)umm_malloc(sizeof(int) * num_segments);
    if (!segments) return luaL_error(L, "out of memory for segments");
  }
  for (int i = 0; i < num_segments; i++) {
    lua_rawgeti(L, 6, i + 1);
    segments[i] = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }

  int visible_rows = (h - 6) / 8;  // FH=8, 3px top/bottom padding
  if (visible_rows < 1) visible_rows = 1;

  ui_widget_textarea(x, y, w, h, text, (int)text_len,
                     segments, num_segments, scroll_y, visible_rows,
                     cursor_row, cursor_col, focused, show_cursor);

  if (segments != stack_segs) umm_free(segments);
  return 0;
}

// picocalc.ui.drawListItem(x, y, w, text, selected, focused)
static int l_ui_drawListItem(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  int w = (int)lb_checkint(L, 3);
  const char *text = luaL_checkstring(L, 4);
  bool selected = lua_toboolean(L, 5);
  bool focused  = lua_toboolean(L, 6);
  ui_widget_list_item(x, y, w, text, selected, focused);
  return 0;
}

// picocalc.ui.drawProgress(x, y, w, h, progress, [fill_color])
static int l_ui_drawProgress(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  int w = (int)lb_checkint(L, 3);
  int h = (int)lb_checkint(L, 4);
  float prog = (float)luaL_checknumber(L, 5);
  uint16_t fill = lua_isnoneornil(L, 6) ? COLOR_GREEN : l_checkcolor(L, 6);
  ui_widget_progress(x, y, w, h, prog, fill, UW_BORDER);
  return 0;
}

// picocalc.ui.drawCheckbox(x, y, checked, focused)
static int l_ui_drawCheckbox(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  bool checked = lua_toboolean(L, 3);
  bool focused = lua_toboolean(L, 4);
  ui_widget_checkbox(x, y, checked, focused);
  return 0;
}

// picocalc.ui.drawRadio(x, y, selected, focused)
static int l_ui_drawRadio(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  bool selected = lua_toboolean(L, 3);
  bool focused  = lua_toboolean(L, 4);
  ui_widget_radio(x, y, selected, focused);
  return 0;
}

// picocalc.ui.drawDivider(x, y, w, [color])
static int l_ui_drawDivider(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  int w = (int)lb_checkint(L, 3);
  uint16_t color = lua_isnoneornil(L, 4) ? UW_BORDER : l_checkcolor(L, 4);
  ui_widget_divider(x, y, w, color);
  return 0;
}

// picocalc.ui.drawToast(y, text, [bg_color])
static int l_ui_drawToast(lua_State *L) {
  int y = (int)lb_checkint(L, 1);
  const char *text = luaL_checkstring(L, 2);
  uint16_t bg = (lua_gettop(L) >= 3) ? (uint16_t)luaL_checkinteger(L, 3) : 0;
  ui_widget_toast(y, text, bg);
  return 0;
}

// picocalc.ui.toast(text, [style]) — push a system toast notification
// style: picocalc.ui.TOAST_INFO (default), TOAST_SUCCESS, TOAST_WARNING, TOAST_ERROR
static int l_ui_toast(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  uint8_t style = (lua_gettop(L) >= 2) ? (uint8_t)luaL_checkinteger(L, 2) : TOAST_STYLE_INFO;
  toast_push(text, style);
  return 0;
}

// picocalc.ui.drawButton(x, y, w, label, focused, [pressed])
static int l_ui_drawButton(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  int w = (int)lb_checkint(L, 3);
  const char *label = luaL_checkstring(L, 4);
  bool focused = lua_toboolean(L, 5);
  bool pressed = lua_toboolean(L, 6);
  ui_widget_button(x, y, w, label, focused, pressed);
  return 0;
}

// picocalc.ui.wrapText(text, max_cols) → {segment_offsets...}, num_rows
static int l_ui_wrapText(lua_State *L) {
  size_t text_len = 0;
  const char *text = luaL_checklstring(L, 1, &text_len);
  int max_cols = (int)lb_checkint(L, 2);

  // Max segments: worst case is 1 segment per character
  int max_segs = (int)(text_len / (max_cols > 0 ? max_cols : 1)) + 2;
  if (max_segs > 512) max_segs = 512;

  int stack_segs[128];
  int *segments = stack_segs;
  if (max_segs > 128) {
    segments = (int *)umm_malloc(sizeof(int) * max_segs);
    if (!segments) return luaL_error(L, "out of memory for wrap segments");
  }

  int num_rows = text_wrap_segments(text, (int)text_len, max_cols,
                                    segments, max_segs);

  // Build Lua table of segment offsets (0-based char offsets)
  lua_createtable(L, num_rows, 0);
  for (int i = 0; i < num_rows; i++) {
    lua_pushinteger(L, segments[i]);
    lua_rawseti(L, -2, i + 1);
  }

  lua_pushinteger(L, num_rows);

  if (segments != stack_segs) umm_free(segments);
  return 2;
}

static const luaL_Reg l_ui_lib[] = {{"drawHeader",    l_ui_drawHeader},
                                    {"drawFooter",    l_ui_drawFooter},
                                    {"drawTabs",      l_ui_drawTabs},
                                    {"textInput",     l_ui_textInput},
                                    {"confirm",       l_ui_confirm},
                                    {"drawSpinner",   l_ui_drawSpinner},
                                    {"splash",        l_ui_splash},
                                    {"drawPanel",     l_ui_drawPanel},
                                    {"drawTextField", l_ui_drawTextField},
                                    {"drawTextArea",  l_ui_drawTextArea},
                                    {"drawListItem",  l_ui_drawListItem},
                                    {"drawProgress",  l_ui_drawProgress},
                                    {"drawCheckbox",  l_ui_drawCheckbox},
                                    {"drawRadio",     l_ui_drawRadio},
                                    {"drawDivider",   l_ui_drawDivider},
                                    {"drawToast",     l_ui_drawToast},
                                    {"toast",         l_ui_toast},
                                    {"drawButton",    l_ui_drawButton},
                                    {"wrapText",      l_ui_wrapText},
                                    {NULL, NULL}};


void lua_bridge_ui_init(lua_State *L) {
  register_subtable(L, "ui", l_ui_lib);

  // Toast style constants: picocalc.ui.TOAST_INFO, etc.
  lua_getfield(L, -1, "ui");
  lua_pushinteger(L, TOAST_STYLE_INFO);    lua_setfield(L, -2, "TOAST_INFO");
  lua_pushinteger(L, TOAST_STYLE_SUCCESS); lua_setfield(L, -2, "TOAST_SUCCESS");
  lua_pushinteger(L, TOAST_STYLE_WARNING); lua_setfield(L, -2, "TOAST_WARNING");
  lua_pushinteger(L, TOAST_STYLE_ERROR);   lua_setfield(L, -2, "TOAST_ERROR");
  lua_pop(L, 1);
}
