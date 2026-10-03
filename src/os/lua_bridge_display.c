#include "lua_bridge_internal.h"
#include "os_overlay.h"
#include "../fonts/font_registry.h"

// ── picocalc.display.* ───────────────────────────────────────────────────────

// ── picocalc.display.getBackBuffer() → framebuffer handle ────────────────────
// The Lua counterpart of the C display->getBackBuffer() pointer. Lua gets no
// raw pointer (nothing in the sandbox can dereference one), so the handle
// carries the capability instead: bulk read/write of the back buffer in
// host-order RGB565, with no intermediate image. Single pixels stay on
// display.setPixel()/getPixel(), which these deliberately do not mirror.
//
// The handle refers to "the back buffer" by role, not by address, so it stays
// correct across a flush() (which swaps the buffers under it) — the same
// contract display_set_pixel()/getPixel() have. Fetch it again after the
// system menu closes if you are holding one across that.
#define FB_MT "picocalc.display.framebuffer"

static int l_fb_width(lua_State *L) {
  lb_checkudata(L, 1, FB_MT);
  lua_pushinteger(L, FB_WIDTH);
  return 1;
}

static int l_fb_height(lua_State *L) {
  lb_checkudata(L, 1, FB_MT);
  lua_pushinteger(L, FB_HEIGHT);
  return 1;
}

// Reads the rectangle arguments at idx..idx+3 and raises unless it is a
// non-empty rectangle wholly inside the screen. w and h are bounded by the
// screen BEFORE they are multiplied, so the byte count cannot overflow and a
// huge request fails here, not in an allocation. Returns the byte count.
static size_t fb_check_rect(lua_State *L, int idx, int *x, int *y, int *w,
                            int *h) {
  *x = (int)lb_checkint(L, idx);
  *y = (int)lb_checkint(L, idx + 1);
  *w = (int)lb_checkint(L, idx + 2);
  *h = (int)lb_checkint(L, idx + 3);
  if (*w <= 0 || *h <= 0)
    return (size_t)luaL_error(L, "rectangle (%d, %d, %d, %d) has no area",
                              *x, *y, *w, *h);
  if (*w > FB_WIDTH || *h > FB_HEIGHT || *x < 0 || *y < 0 ||
      *x > FB_WIDTH - *w || *y > FB_HEIGHT - *h)
    return (size_t)luaL_error(L, "rectangle (%d, %d, %d, %d) outside the screen",
                              *x, *y, *w, *h);
  return (size_t)*w * (size_t)*h * 2u;
}

// fb:getPixels(x, y, w, h) → string (host-order RGB565, row-major)
static int l_fb_get_pixels(lua_State *L) {
  lb_checkudata(L, 1, FB_MT);
  int x, y, w, h;
  size_t n = fb_check_rect(L, 2, &x, &y, &w, &h);
  picodeck_lua_strbuf_t sb;  // read straight into the result string
  void *buf = picodeck_lua_strbuf_init(L, &sb, n);
  if (!display_get_pixels_block(x, y, w, h, buf, n))
    return luaL_error(L, "rectangle (%d, %d, %d, %d) outside the screen",
                      x, y, w, h);
  picodeck_lua_strbuf_push(L, &sb, n);
  return 1;
}

// fb:setPixels(data, x, y, w, h) → true when anything was drawn, false when
// the rectangle is on screen but wholly outside the clip rect. A rectangle
// that is not inside the screen raises, as getPixels does.
static int l_fb_set_pixels(lua_State *L) {
  lb_checkudata(L, 1, FB_MT);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);
  int x, y, w, h;
  size_t n = fb_check_rect(L, 3, &x, &y, &w, &h);
  if (len != n)
    return luaL_error(L, "pixel data is %I bytes, expected %d (%dx%d pixels)",
                      (lua_Integer)len, (int)n, w, h);
  lua_pushboolean(L, display_set_pixels_block(x, y, w, h, data, len));
  return 1;
}

static const luaL_Reg l_fb_methods[] = {
    {"width",     l_fb_width},
    {"height",    l_fb_height},
    {"getPixels", l_fb_get_pixels},
    {"setPixels", l_fb_set_pixels},
    {NULL, NULL}};

// The handle is stateless, so one shared instance is built at registration
// and every getBackBuffer() hands back that same object.
#define FB_SINGLETON "picocalc.display.framebuffer.singleton"

static int l_display_get_back_buffer(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, FB_SINGLETON);
  return 1;
}

// Called from lua_bridge_display_init(), once per VM.
static void display_register_fb(lua_State *L) {
  lb_register_type(L, FB_MT, l_fb_methods, NULL);
  lua_getfield(L, LUA_REGISTRYINDEX, FB_SINGLETON);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newuserdatauv(L, 1, 0);   // no state, no __gc
    luaL_setmetatable(L, FB_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, FB_SINGLETON);
  }
  lua_pop(L, 1);
}

static int l_display_clear(lua_State *L) {
  uint16_t color = (lua_gettop(L) >= 1) ? l_checkcolor(L, 1) : COLOR_BLACK;
  display_clear(color);
  return 0;
}

static int l_display_setPixel(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  uint16_t c = l_checkcolor(L, 3);
  display_set_pixel(x, y, c);
  return 0;
}

// getPixel(x, y): the colour at (x, y) in the frame being drawn (the back
// buffer) as the RGB565 value setPixel takes (the driver undoes the panel
// byte swap). Off-screen coordinates raise; the clip rect does not apply.
static int l_display_getPixel(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT)
    return luaL_error(L, "pixel (%d, %d) outside the screen", x, y);
  lua_pushinteger(L, display_get_pixel(x, y));
  return 1;
}

static int l_display_fillRect(lua_State *L) {
  display_fill_rect((int)lb_checkint(L, 1), (int)lb_checkint(L, 2),
                    (int)lb_checkint(L, 3), (int)lb_checkint(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawRect(lua_State *L) {
  display_draw_rect((int)lb_checkint(L, 1), (int)lb_checkint(L, 2),
                    (int)lb_checkint(L, 3), (int)lb_checkint(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawLine(lua_State *L) {
  display_draw_line((int)lb_checkint(L, 1), (int)lb_checkint(L, 2),
                    (int)lb_checkint(L, 3), (int)lb_checkint(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawCircle(lua_State *L) {
  display_draw_circle((int)lb_checkint(L, 1), (int)lb_checkint(L, 2),
                      (int)lb_checkint(L, 3), l_checkcolor(L, 4));
  return 0;
}

static int l_display_fillCircle(lua_State *L) {
  display_fill_circle((int)lb_checkint(L, 1), (int)lb_checkint(L, 2),
                      (int)lb_checkint(L, 3), l_checkcolor(L, 4));
  return 0;
}

static int l_display_setScrollArea(lua_State *L) {
  int top = (int)lb_checkint(L, 1);
  int height = (int)lb_checkint(L, 2);
  int bottom = (int)lb_checkint(L, 3);
  display_set_scroll_area(top, height, bottom);
  return 0;
}

static int l_display_setScrollOffset(lua_State *L) {
  display_set_scroll_offset((int)lb_checkint(L, 1));
  return 0;
}

// getScrollOffset() -> offset, writeCount
// Last offset written via setScrollOffset, plus the total number of
// register writes.  The OS resets the offset to 0 when it takes over the
// screen (system menu, app switch); apps driving hardware scroll poll both
// each frame — the count catches a foreign write even when the value
// happens to match what the app last set.
static int l_display_getScrollOffset(lua_State *L) {
  lua_pushinteger(L, display_get_scroll_offset());
  lua_pushinteger(L, (lua_Integer)display_get_scroll_offset_writes());
  return 2;
}

// drawText(x, y, text, fg [, bg])
//
// bg == false selects transparent background: only glyph pixels are written, so
// the text can sit over existing art. Any other value (including absent) keeps
// the original opaque behaviour with bg defaulting to black.
//
// `false` is the sentinel rather than nil because an explicit nil is
// indistinguishable from a nil-valued variable at the call site — so
// drawText(x, y, t, fg, someNilVar) would silently go transparent. Nothing
// passes false today, since l_checkcolor would have thrown on it.
static int l_display_drawText(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  const char *text = luaL_checkstring(L, 3);
  uint16_t fg = l_checkcolor(L, 4);

  if (lua_isboolean(L, 5) && !lua_toboolean(L, 5)) {
    lua_pushinteger(L, display_draw_text_transparent(x, y, text, fg));
    return 1;
  }

  uint16_t bg = (lua_gettop(L) >= 5) ? l_checkcolor(L, 5) : COLOR_BLACK;
  int width = display_draw_text(x, y, text, fg, bg);
  lua_pushinteger(L, width);
  return 1;
}

// fillHLine(y, x0, x1, color) — horizontal counterpart to fillVLine.
static int l_display_fillHLine(lua_State *L) {
  int y = (int)lb_checkint(L, 1);
  int x0 = (int)lb_checkint(L, 2);
  int x1 = (int)lb_checkint(L, 3);
  display_fill_hline(y, x0, x1, l_checkcolor(L, 4));
  return 0;
}

// fillTriangle(x0, y0, x1, y1, x2, y2, color)
// display_fill_triangle already existed in both display implementations and was
// simply never registered for Lua.
static int l_display_fillTriangle(lua_State *L) {
  int x0 = (int)lb_checkint(L, 1);
  int y0 = (int)lb_checkint(L, 2);
  int x1 = (int)lb_checkint(L, 3);
  int y1 = (int)lb_checkint(L, 4);
  int x2 = (int)lb_checkint(L, 5);
  int y2 = (int)lb_checkint(L, 6);
  display_fill_triangle(x0, y0, x1, y1, x2, y2, l_checkcolor(L, 7));
  return 0;
}

// setClipRect(x, y, w, h) — restrict all drawing primitives to a rect.
// clearClipRect() restores the full screen. getClipRect() returns x, y, w, h.
static int l_display_setClipRect(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y = (int)lb_checkint(L, 2);
  int w = (int)lb_checkint(L, 3);
  int h = (int)lb_checkint(L, 4);
  display_set_clip_rect(x, y, w, h);
  return 0;
}

static int l_display_getClipRect(lua_State *L) {
  int x, y, w, h;
  display_get_clip_rect(&x, &y, &w, &h);
  lua_pushinteger(L, x);
  lua_pushinteger(L, y);
  lua_pushinteger(L, w);
  lua_pushinteger(L, h);
  return 4;
}

static int l_display_clearClipRect(lua_State *L) {
  (void)L;
  display_clear_clip_rect();
  return 0;
}

// drawPlane(image, cam_x, cam_y, cam_z, angle, horizon_y, scale)
// Mode 7 perspective ground plane — see display_draw_plane().
static int l_display_drawPlane(lua_State *L) {
  lua_image_t *img = lb_check_image(L, 1);
  float cam_x = (float)luaL_checknumber(L, 2);
  float cam_y = (float)luaL_checknumber(L, 3);
  float cam_z = (float)luaL_checknumber(L, 4);
  float angle = (float)luaL_optnumber(L, 5, 0.0);
  int horizon_y = (int)lb_optint(L, 6, 120);
  float scale = (float)luaL_optnumber(L, 7, 1.0);
  display_draw_plane(img->data, img->w, img->h, cam_x, cam_y, cam_z, angle,
                     horizon_y, scale);
  return 0;
}

// Set by menu_lua_hook when a screenshot is requested.  Cleared and fired
// inside l_display_flush so the capture always happens on a complete frame.
bool s_screenshot_pending = false;

static int l_display_flush(lua_State *L) {
  os_overlay_draw(OS_PRESENT_FLUSH, 0, FB_HEIGHT - 1);  // toast, Show FPS
  display_flush();
  if (s_screenshot_pending) {
    s_screenshot_pending = false;
    screenshot_save();
  }
  // Once a frame, whatever the count hook's count: a draw loop that never
  // polls input runs a few instructions per frame, so after a compute phase
  // (count at its maximum) the next hook call could be seconds away and the
  // watchdog would fire. The gated pass (see lua_bridge.h) feeds it and
  // serves exit, the menu, callbacks and dev commands.
  lua_bridge_service_poll(L);
  return 0;
}

// flushRows(y0, y1) — push rows y0..y1 (inclusive) of the current draw buffer
// to the panel WITHOUT swapping buffers.  Mirrors l_display_flush's overlay
// pass and screenshot-pending hook so MCP screenshots still fire while an app
// sits in a partial-update idle loop.  The overlay pass draws only into the
// rows sent and pushes an overlay outside them as a small window (see
// os_overlay.h), so it never smears into the app's draw buffer.
static int l_display_flushRows(lua_State *L) {
  int y0 = (int)lb_checkint(L, 1);
  int y1 = (int)lb_checkint(L, 2);
  os_overlay_draw(OS_PRESENT_ROWS, y0, y1);
  display_flush_rows(y0, y1);
  if (s_screenshot_pending) {
    s_screenshot_pending = false;
    screenshot_save();
  }
  lua_bridge_service_poll(L);  // as l_display_flush
  return 0;
}

// flushRegion(y0, y1) — like flush() but only transfers rows y0..y1.  Swaps
// buffers and re-syncs the flushed band into the new back buffer (see
// display_flush_region).  Same overlay pass and screenshot hook as
// flushRows.
static int l_display_flushRegion(lua_State *L) {
  int y0 = (int)lb_checkint(L, 1);
  int y1 = (int)lb_checkint(L, 2);
  os_overlay_draw(OS_PRESENT_REGION, y0, y1);
  display_flush_region(y0, y1);
  if (s_screenshot_pending) {
    s_screenshot_pending = false;
    screenshot_save();
  }
  lua_bridge_service_poll(L);  // as l_display_flush
  return 0;
}

static int l_display_getWidth(lua_State *L) {
  lua_pushinteger(L, FB_WIDTH);
  return 1;
}

static int l_display_getHeight(lua_State *L) {
  lua_pushinteger(L, FB_HEIGHT);
  return 1;
}

static int l_display_setBrightness(lua_State *L) {
  display_set_brightness((uint8_t)lb_clamp_int(lb_checkint(L, 1), 0, 255));
  return 0;
}

static int l_display_textWidth(lua_State *L) {
  lua_pushinteger(L, display_text_width(luaL_checkstring(L, 1)));
  return 1;
}

static int l_display_setFont(lua_State *L) {
  display_set_font((int)luaL_checkinteger(L, 1));
  return 0;
}

static int l_display_getFont(lua_State *L) {
  lua_pushinteger(L, display_get_font());
  return 1;
}

static int l_display_getFontWidth(lua_State *L) {
  lua_pushinteger(L, display_get_font_width());
  return 1;
}

static int l_display_getFontHeight(lua_State *L) {
  lua_pushinteger(L, display_get_font_height());
  return 1;
}

// loadFont(path) -> id | nil, errstr. Sandbox-checked like image loading. Freed
// at app exit by the launcher, or earlier via unloadFont. The 8 loaded-font
// slots are shared with graphics.font.new; a full registry is logged by
// font_registry_load_ex and reported here. (The native loadFont returns -1.)
static int l_display_loadFont(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushnil(L);
    lua_pushfstring(L, "access denied: %s", path);
    return 2;
  }
  const char *why = "load failed";
  int id = font_registry_load_ex(path, &why);
  if (id < 0) {
    lua_pushnil(L);
    if (strcmp(why, FONT_REGISTRY_WHY_FULL) == 0)
      lua_pushfstring(L, "font registry full (all %d loaded-font slots are in "
                         "use; display.loadFont and graphics.font.new share "
                         "them): %s", FONT_REGISTRY_LOADED, path);
    else
      lua_pushfstring(L, "%s: %s", why, path);
    return 2;
  }
  lua_pushinteger(L, id);
  return 1;
}

static int l_display_unloadFont(lua_State *L) {
  int id = (int)luaL_checkinteger(L, 1);
  if (display_get_font() == id) display_set_font(0);
  font_registry_unload(id);
  return 0;
}

// Convenience: create RGB565 from r,g,b components
static int l_display_rgb(lua_State *L) {
  // Clamp before packing: 256 would carry into the neighbouring channel.
  int r = (int)lb_clamp_int(lb_checkint(L, 1), 0, 255);
  int g = (int)lb_clamp_int(lb_checkint(L, 2), 0, 255);
  int b = (int)lb_clamp_int(lb_checkint(L, 3), 0, 255);
  lua_pushinteger(L, RGB565(r, g, b));
  return 1;
}

// picocalc.display.applyEffect(name, ...)
// Dispatches to the appropriate effect function based on the string name.
static int l_display_applyEffect(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);

  if (strcmp(name, "invert") == 0) {
    display_effect_invert();
  } else if (strcmp(name, "darken") == 0) {
    uint8_t factor = (uint8_t)lb_clamp_int(lb_optint(L, 2, 128), 0, 255);
    display_effect_darken(factor);
  } else if (strcmp(name, "brighten") == 0) {
    uint8_t factor = (uint8_t)lb_clamp_int(lb_optint(L, 2, 128), 0, 255);
    display_effect_brighten(factor);
  } else if (strcmp(name, "tint") == 0) {
    uint8_t r = (uint8_t)lb_clamp_int(lb_checkint(L, 2), 0, 255);
    uint8_t g = (uint8_t)lb_clamp_int(lb_checkint(L, 3), 0, 255);
    uint8_t b = (uint8_t)lb_clamp_int(lb_checkint(L, 4), 0, 255);
    uint8_t strength = (uint8_t)lb_clamp_int(lb_optint(L, 5, 128), 0, 255);
    display_effect_tint(r, g, b, strength);
  } else if (strcmp(name, "fade") == 0) {
    uint8_t r = (uint8_t)lb_clamp_int(lb_checkint(L, 2), 0, 255);
    uint8_t g = (uint8_t)lb_clamp_int(lb_checkint(L, 3), 0, 255);
    uint8_t b = (uint8_t)lb_clamp_int(lb_checkint(L, 4), 0, 255);
    uint8_t factor = (uint8_t)lb_clamp_int(lb_optint(L, 5, 128), 0, 255);
    display_effect_tint(r, g, b, factor); // fade = tint toward target color
  } else if (strcmp(name, "grayscale") == 0) {
    display_effect_grayscale();
  } else if (strcmp(name, "blend") == 0) {
    // Arg 2: image userdata, Arg 3: alpha (0-255)
    lua_image_t *img = lb_check_image(L, 2);
    uint8_t alpha = (uint8_t)lb_clamp_int(lb_optint(L, 3, 128), 0, 255);
    display_effect_blend(img->data, img->w, img->h, alpha);
  } else if (strcmp(name, "palette") == 0) {
    // Arg 2: table of 256 RGB565 color values
    luaL_checktype(L, 2, LUA_TTABLE);
    int lut_size = (int)luaL_len(L, 2);
    if (lut_size <= 0 || lut_size > 256) {
      return luaL_error(L, "palette: table must have 1-256 entries");
    }
    // Allocate LUT on stack (256 * 2 = 512 bytes, fine for stack)
    uint16_t lut[256];
    for (int i = 0; i < lut_size; i++) {
      lua_rawgeti(L, 2, i + 1);
      lut[i] = (uint16_t)lua_tointeger(L, -1);
      lua_pop(L, 1);
    }
    display_effect_palette(lut, lut_size);
  } else if (strcmp(name, "dither") == 0) {
    uint8_t levels = (uint8_t)lb_clamp_int(lb_optint(L, 2, 4), 2, 32);
    display_effect_dither(levels);
  } else if (strcmp(name, "scanline") == 0) {
    uint8_t intensity = (uint8_t)lb_clamp_int(lb_optint(L, 2, 128), 0, 255);
    display_effect_scanline(intensity);
  } else if (strcmp(name, "posterize") == 0) {
    uint8_t levels = (uint8_t)lb_clamp_int(lb_optint(L, 2, 4), 2, 32);
    display_effect_posterize(levels);
  } else {
    return luaL_error(L, "unknown effect: %s", name);
  }
  return 0;
}

static int l_display_fillVLine(lua_State *L) {
  display_fill_vline((int)lb_checkint(L, 1), (int)lb_checkint(L, 2),
                     (int)lb_checkint(L, 3),
                     (uint16_t)luaL_checkinteger(L, 4));
  return 0;
}

static int l_display_drawTexturedColumn(lua_State *L) {
  int x = (int)lb_checkint(L, 1);
  int y0 = (int)lb_checkint(L, 2);
  int y1 = (int)lb_checkint(L, 3);
  lua_image_t *img = lb_check_image(L, 4);
  int tex_x = (int)lb_checkint(L, 5);
  int tex_y0 = (int)lb_checkint(L, 6);
  int tex_y1 = (int)lb_checkint(L, 7);
  display_draw_textured_column(x, y0, y1, img->data, img->w, img->h, tex_x,
                               tex_y0, tex_y1);
  return 0;
}

static int l_display_fillVLineGradient(lua_State *L) {
  display_fill_vline_gradient(
      (int)lb_checkint(L, 1), (int)lb_checkint(L, 2),
      (int)lb_checkint(L, 3), (uint16_t)luaL_checkinteger(L, 4),
      (uint16_t)luaL_checkinteger(L, 5));
  return 0;
}

static const luaL_Reg l_display_lib[] = {
    {"clear", l_display_clear},
    {"setPixel", l_display_setPixel},
    {"getPixel", l_display_getPixel},
    {"getBackBuffer", l_display_get_back_buffer},
    {"fillRect", l_display_fillRect},
    {"drawRect", l_display_drawRect},
    {"drawLine", l_display_drawLine},
    {"drawCircle", l_display_drawCircle},
    {"fillCircle", l_display_fillCircle},
    {"setScrollArea", l_display_setScrollArea},
    {"setScrollOffset", l_display_setScrollOffset},
    {"getScrollOffset", l_display_getScrollOffset},
    {"drawText", l_display_drawText},
    {"flush", l_display_flush},
    {"getWidth", l_display_getWidth},
    {"getHeight", l_display_getHeight},
    {"setBrightness", l_display_setBrightness},
    {"textWidth", l_display_textWidth},
    {"setFont", l_display_setFont},
    {"getFont", l_display_getFont},
    {"getFontWidth", l_display_getFontWidth},
    {"getFontHeight", l_display_getFontHeight},
    {"loadFont", l_display_loadFont},
    {"unloadFont", l_display_unloadFont},
    {"rgb", l_display_rgb},
    {"applyEffect", l_display_applyEffect},
    {"fillVLine", l_display_fillVLine},
    {"fillHLine", l_display_fillHLine},
    {"fillTriangle", l_display_fillTriangle},
    {"setClipRect", l_display_setClipRect},
    {"getClipRect", l_display_getClipRect},
    {"clearClipRect", l_display_clearClipRect},
    {"flushRows", l_display_flushRows},
    {"flushRegion", l_display_flushRegion},
    {"drawPlane", l_display_drawPlane},
    {"drawTexturedColumn", l_display_drawTexturedColumn},
    {"fillVLineGradient", l_display_fillVLineGradient},
    {NULL, NULL}};


void lua_bridge_display_init(lua_State *L) {
  // Fresh clip state per app launch, mirroring http_close_all() in
  // lua_bridge_register — a clip left behind by a previous app must not
  // silently truncate the next app's drawing.
  display_clear_clip_rect();

  display_register_fb(L);
  register_subtable(L, "display", l_display_lib);
  // Push colour constants into picocalc.display
  lua_getfield(L, -1, "display");
  lua_pushinteger(L, COLOR_BLACK);
  lua_setfield(L, -2, "BLACK");
  lua_pushinteger(L, COLOR_WHITE);
  lua_setfield(L, -2, "WHITE");
  lua_pushinteger(L, COLOR_RED);
  lua_setfield(L, -2, "RED");
  lua_pushinteger(L, COLOR_GREEN);
  lua_setfield(L, -2, "GREEN");
  lua_pushinteger(L, COLOR_BLUE);
  lua_setfield(L, -2, "BLUE");
  lua_pushinteger(L, COLOR_YELLOW);
  lua_setfield(L, -2, "YELLOW");
  lua_pushinteger(L, COLOR_CYAN);
  lua_setfield(L, -2, "CYAN");
  lua_pushinteger(L, COLOR_GRAY);
  lua_setfield(L, -2, "GRAY");
  // Font ID constants
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "FONT_6X8");
  lua_pushinteger(L, 1);
  lua_setfield(L, -2, "FONT_8X12");
  lua_pushinteger(L, 2);
  lua_setfield(L, -2, "FONT_SCIENTIFICA");
  lua_pushinteger(L, 3);
  lua_setfield(L, -2, "FONT_SCIENTIFICA_BOLD");
  lua_pop(L, 1);
}
