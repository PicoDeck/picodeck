#include "lua_bridge_internal.h"
#include "perf.h"
#include "ui.h"

// ── picocalc.perf.* ──────────────────────────────────────────────────────────
// Performance monitoring utilities for apps

// Start timing a frame. Call at the beginning of your game loop.
static int l_perf_beginFrame(lua_State *L) {
  (void)L;
  perf_begin_frame();
  return 0;
}

// End timing a frame and calculate FPS. Call at the end of your game loop.
static int l_perf_endFrame(lua_State *L) {
  perf_end_frame();
  return 0;
}

// Get current FPS (averaged over recent frames)
static int l_perf_getFPS(lua_State *L) {
  lua_pushinteger(L, perf_get_fps());
  return 1;
}

// Get last frame time in milliseconds
static int l_perf_getFrameTime(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)perf_get_frame_time());
  return 1;
}

// Convenience: draw FPS counter at specified position with color coding.
// Default: top-right, right-aligned 8px from the edge, just below the
// standard header (ui_draw_header) so it never covers its status icons.
static int l_perf_drawFPS(lua_State *L) {
  int fps = perf_get_fps();
  char buf[16];
  snprintf(buf, sizeof(buf), "FPS: %d", fps);

  int x = (int)lb_optint(L, 1, FB_WIDTH - 8 - display_text_width(buf));
  int y = (int)lb_optint(L, 2, UI_HEADER_H + 1 + 3);

  // Color code: green >= 55, yellow >= 30, red < 30
  uint16_t color = (fps >= 55)   ? COLOR_GREEN
                   : (fps >= 30) ? COLOR_YELLOW
                                 : COLOR_RED;

  display_draw_text(x, y, buf, color, COLOR_BLACK);
  return 0;
}

// Set target FPS for automatic frame pacing (0 = no limit)
static int l_perf_setTargetFPS(lua_State *L) {
  int fps = (int)lb_checkint(L, 1);
  if (fps < 0)
    fps = 0;
  perf_set_target_fps((uint32_t)fps);
  return 0;
}

static const luaL_Reg l_perf_lib[] = {
    {"beginFrame", l_perf_beginFrame}, {"endFrame", l_perf_endFrame},
    {"getFPS", l_perf_getFPS},         {"getFrameTime", l_perf_getFrameTime},
    {"drawFPS", l_perf_drawFPS},       {"setTargetFPS", l_perf_setTargetFPS},
    {NULL, NULL}};


void lua_bridge_perf_init(lua_State *L) {
  perf_init();
  register_subtable(L, "perf", l_perf_lib);
}
