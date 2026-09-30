#include "lua_bridge_internal.h"
#include "../drivers/image_api.h"
#include "../fonts/font_registry.h"
#include "../drivers/image_preload.h"
#include "pico/time.h"
#include <math.h>

// ── picocalc.graphics.* ──────────────────────────────────────────────────────

static uint16_t s_graphics_color = COLOR_WHITE;
static uint16_t s_graphics_bg_color = COLOR_BLACK;

// ── Destroyed objects ────────────────────────────────────────────────────────
// __gc is not reachable from Lua (lb_register_type), but a finaliser that
// runs after an object's own (resurrection) can still hand it to a method.
// So every __gc leaves its object marked dead (an image's data is NULL, the
// other types set `destroyed`) and every check_* rejects a dead object with a
// Lua error. Finalisers use luaL_checkudata directly.
static lua_image_t *check_image(lua_State *L, int idx) {
  return lb_check_image(L, idx);
}

// ── Lifetime anchors ─────────────────────────────────────────────────────────
// An object that keeps a C pointer to another Lua object (a sprite's image, a
// tilemap's tileset, an animation loop's frames) also keeps that object in one
// of its user values, so the collector cannot free the target while the
// pointer is live. Every such pointer is assigned together with its anchor.
// valueidx 0 clears the slot. Slot numbers are listed with each type.
static void anchor_set(lua_State *L, int idx, int slot, int valueidx) {
  idx = lua_absindex(L, idx);
  if (valueidx)
    lua_pushvalue(L, valueidx);
  else
    lua_pushnil(L);
  lua_setiuservalue(L, idx, slot);
}

static int l_graphics_image_gc(lua_State *L) {
  lua_image_t *img = (lua_image_t *)luaL_checkudata(L, 1, GRAPHICS_IMAGE_MT);
  if (img->data) {
    umm_free(img->data);
    img->data = NULL;
  }
  return 0;
}

static int l_graphics_setColor(lua_State *L) {
  s_graphics_color = l_checkcolor(L, 1);
  return 0;
}

static int l_graphics_setBackgroundColor(lua_State *L) {
  s_graphics_bg_color = l_checkcolor(L, 1);
  return 0;
}

static int l_graphics_setTransparentColor(lua_State *L) {
  uint16_t color = 0;
  if (!lua_isnil(L, 1))
    color = l_checkcolor(L, 1);
  display_set_transparent_color(color);
  return 0;
}

static int l_graphics_getTransparentColor(lua_State *L) {
  uint16_t color = display_get_transparent_color();
  if (color == 0)
    lua_pushnil(L);
  else
    lua_pushinteger(L, color);
  return 1;
}

static int l_graphics_clear(lua_State *L) {
  uint16_t color =
      (lua_gettop(L) >= 1) ? l_checkcolor(L, 1) : s_graphics_bg_color;
  display_clear(color);
  return 0;
}

static int l_graphics_image_new(lua_State *L) {
  int w = lb_checkint(L, 1);
  int h = lb_checkint(L, 2);

  pc_image_t *loaded = image_new_blank(w, h);
  if (!loaded)
    return luaL_error(L, "out of memory allocating image");

  lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  img->w = loaded->w;
  img->h = loaded->h;
  img->data = loaded->data;        // steal ownership of pixel data
  img->transparent_color = 0;
  loaded->data = NULL;             // prevent image_free from freeing pixels
  umm_free(loaded);                // free just the temp struct

  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  return 1;
}

static int l_graphics_image_load(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  if (!fs_sandbox_check(L, path, false)) {
    return luaL_error(L, "access denied");
  }

  pc_image_t *loaded = image_load(path);
  if (!loaded)
    return luaL_error(L, "failed to load image: %s", path);

  // Steal pixel data from the loaded pc_image_t into a Lua-managed userdata.
  // Lua owns the struct; only the pixel buffer is in PSRAM and freed by __gc.
  lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  img->w = loaded->w;
  img->h = loaded->h;
  img->data = loaded->data;        // steal ownership of pixel data
  img->transparent_color = 0;
  loaded->data = NULL;             // prevent image_free from freeing pixels
  umm_free(loaded);                // free just the temp struct

  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  return 1;
}

static int l_graphics_image_getSize(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  lua_pushinteger(L, img->w);
  lua_pushinteger(L, img->h);
  return 2;
}

static int l_graphics_image_copy(lua_State *L) {
  lua_image_t *src = check_image(L, 1);
  lua_image_t *dst = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  dst->w = src->w;
  dst->h = src->h;
  dst->transparent_color = src->transparent_color;
  dst->data = (uint16_t *)umm_malloc(dst->w * dst->h * sizeof(uint16_t));
  if (!dst->data)
    return luaL_error(L, "out of memory allocating image copy");
  memcpy(dst->data, src->data, dst->w * dst->h * sizeof(uint16_t));
  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  return 1;
}

// Reads the srcRect table at stack index idx into sx, sy, sw, sh. It is either
// named {x=, y=, w=, h=} or positional {x, y, w, h}; a named field wins over the
// same array slot, and a missing one defaults to 0, 0, image width, image
// height. Errors name the field as "srcRect.x". Used by img:draw and
// img:drawStretched.
static void read_src_rect(lua_State *L, int idx, const lua_image_t *img,
                          int *sx, int *sy, int *sw, int *sh) {
  static const char *const names[4] = {"x", "y", "w", "h"};
  static const char *const whats[4] = {"srcRect.x", "srcRect.y", "srcRect.w",
                                       "srcRect.h"};
  int def[4] = {0, 0, img->w, img->h};
  int v[4];
  for (int i = 0; i < 4; i++) {
    lua_getfield(L, idx, names[i]);
    if (lua_isnil(L, -1)) {  // not named: fall back to the array slot
      lua_pop(L, 1);
      lua_rawgeti(L, idx, i + 1);
    }
    v[i] = (int)lb_optint_at(L, -1, idx, whats[i], def[i]);
    lua_pop(L, 1);
  }
  *sx = v[0]; *sy = v[1]; *sw = v[2]; *sh = v[3];
}

static int l_graphics_image_draw(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);

  bool flip_x = false;
  bool flip_y = false;
  if (lua_istable(L, 4)) {
    lua_getfield(L, 4, "flipX");
    flip_x = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "flipY");
    flip_y = lua_toboolean(L, -1);
    lua_pop(L, 1);
  } else if (lua_isboolean(L, 4)) {
    flip_x = lua_toboolean(L, 4);
  }

  int sx = 0, sy = 0, sw = img->w, sh = img->h;
  if (lua_istable(L, 5)) read_src_rect(L, 5, img, &sx, &sy, &sw, &sh);

  display_draw_image_partial(x, y, img->w, img->h, img->data, sx, sy, sw, sh,
                             flip_x, flip_y, img->transparent_color);
  return 0;
}

static int l_graphics_image_drawAnchored(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  double ax = luaL_checknumber(L, 4);
  double ay = luaL_checknumber(L, 5);

  x -= (int)(img->w * ax);
  y -= (int)(img->h * ay);

  display_draw_image_partial(x, y, img->w, img->h, img->data, 0, 0, img->w,
                             img->h, false, false, img->transparent_color);
  return 0;
}

static int l_graphics_image_drawTiled(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  int rect_w = lb_checkint(L, 4);
  int rect_h = lb_checkint(L, 5);

  for (int ty = 0; ty < rect_h; ty += img->h) {
    for (int tx = 0; tx < rect_w; tx += img->w) {
      int draw_w = (tx + img->w > rect_w) ? (rect_w - tx) : img->w;
      int draw_h = (ty + img->h > rect_h) ? (rect_h - ty) : img->h;
      display_draw_image_partial(x + tx, y + ty, img->w, img->h, img->data, 0,
                                 0, draw_w, draw_h, false, false,
                                 img->transparent_color);
    }
  }

  return 0;
}

static int l_graphics_image_getMetadata(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  lua_newtable(L);
  lua_pushinteger(L, img->w);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, img->h);
  lua_setfield(L, -2, "height");
  if (img->transparent_color) {
    lua_pushinteger(L, img->transparent_color);
    lua_setfield(L, -2, "transparentColor");
  }
  lua_pushstring(L, "psram");
  lua_setfield(L, -2, "storage");
  return 1;
}

// Largest scaled destination edge, in pixels, that drawScaled accepts. The
// firmware draws it with TGX blitScaledRotated, whose rasteriser is documented
// for viewports up to 4096 px at the configured 6 subpixel bits
// (third_party/tgx Rasterizer.h); beyond it the edge functions overflow int32
// and a screen-covering result can draw nothing. It still rejects a
// destination size passed as the scale (182 for a 96 px image = 17472 px).
#define IMAGE_SCALE_MAX_DST 4096.0f
// drawScaledNN draws through disp_blit_scaled, which does its arithmetic in
// int64 over the clipped span only, so it keeps a wider cap.
#define IMAGE_SCALE_NN_MAX_DST 16384.0f

// Shared scale check. `what` names the call for the message; the text says
// "scale multiplier" because passing dst_w/dst_h (as the native
// graphics->drawScaled takes) is the common mistake.
static void check_image_scale(lua_State *L, const lua_image_t *img, double scale,
                              const char *what, float max_dst) {
  double edge = (double)(img->w > img->h ? img->w : img->h) * scale;
  if (!isfinite(scale) || scale <= 0 || edge > max_dst)
    luaL_error(L,
               "%s: the argument is a scale multiplier (2 = twice the size), "
               "not dst_w/dst_h; it must be finite, > 0 and give at most %d px, "
               "got %f for a %dx%d image",
               what, (int)max_dst, scale, img->w, img->h);
}

// img:drawScaled(x, y, scale [, angle]): scale is a MULTIPLIER (the native
// graphics->drawScaled takes dst_w/dst_h instead).
static int l_graphics_image_drawScaled(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  double scale = luaL_checknumber(L, 4);
  check_image_scale(L, img, scale, "drawScaled", IMAGE_SCALE_MAX_DST);
  float angle = lb_optfloat(L, 5, 0.0f);

  display_draw_image_scaled(x, y, img->w, img->h, img->data, (float)scale,
                            angle, img->transparent_color);
  return 0;
}

// img:drawScaledNN(x, y, scale): integer multiplier, nearest neighbour.
static int l_graphics_image_drawScaledNN(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  int scale = lb_checkint(L, 4);
  check_image_scale(L, img, (double)scale, "drawScaledNN",
                    IMAGE_SCALE_NN_MAX_DST);

  int dst_w = img->w * scale;
  int dst_h = img->h * scale;

  display_draw_image_scaled_nn(x, y, img->data, img->w, img->h, dst_w, dst_h,
                               img->transparent_color);
  return 0;
}

// img:drawStretched(x, y, w, h [, srcRect]) — nearest-neighbour stretch of the
// image (or of srcRect, clamped to the image) to w x h. srcRect: read_src_rect.
static int l_graphics_image_drawStretched(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = (int)lb_checkint(L, 2);
  int y = (int)lb_checkint(L, 3);
  int w = (int)lb_checkint(L, 4);
  int h = (int)lb_checkint(L, 5);
  int sx = 0, sy = 0, sw = img->w, sh = img->h;
  if (!lua_isnoneornil(L, 6)) {
    luaL_checktype(L, 6, LUA_TTABLE);
    read_src_rect(L, 6, img, &sx, &sy, &sw, &sh);
  }
  if (w <= 0 || h <= 0) return 0;
  display_draw_image_stretched(x, y, w, h, img->data, img->w, img->h, sx, sy, sw,
                               sh, img->transparent_color);
  return 0;
}

static int l_graphics_image_setTransparentColor(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  if (lua_isnil(L, 2) || lua_isnone(L, 2))
    img->transparent_color = 0;
  else
    img->transparent_color = (uint16_t)luaL_checkinteger(L, 2);
  return 0;
}

static int l_graphics_image_getTransparentColor(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  if (img->transparent_color == 0)
    lua_pushnil(L);
  else
    lua_pushinteger(L, img->transparent_color);
  return 1;
}

// ── Pixel access ─────────────────────────────────────────────────────────────
// Image pixels are host-order RGB565, the values every colour argument takes
// (only the framebuffer is byte-swapped: the blitters swap as they write), so
// getPixel/setPixel convert nothing. The bulk strings are row-major
// little-endian RGB565, two bytes a pixel (string.pack "<I2"): that is the
// pixel buffer's own layout, so they are copied a row at a time. Writes do
// not invalidate anything: draws, the colour key, spritesheets, tilemaps,
// animation loops, display.applyEffect("blend") and gfx3d read the pixels
// live, but sprite:setSourceRect copies its rectangle into frame_data (and
// sprite:copy copies that copy), so such a sprite keeps drawing and
// alphaCollision-testing the old pixels until setSourceRect runs again.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "image pixel strings assume a little-endian host"
#endif

// The pixel (x, y) at args idx, idx + 1 (quantities); raises unless it lies
// inside the image.
static void image_check_pixel(lua_State *L, const lua_image_t *img, int idx,
                              int *x, int *y) {
  *x = (int)lb_checkint(L, idx);
  *y = (int)lb_checkint(L, idx + 1);
  if (*x < 0 || *x >= img->w || *y < 0 || *y >= img->h)
    luaL_error(L, "pixel (%d, %d) outside the %dx%d image", *x, *y, img->w,
               img->h);
}

// The rectangle x, y, w, h at args idx..idx + 3 (quantities), or the whole
// image when all four are absent. Raises unless it lies inside the image; a
// width or height of 0 is an empty rectangle.
static void image_check_rect(lua_State *L, const lua_image_t *img, int idx,
                             int *x, int *y, int *w, int *h) {
  if (lua_isnoneornil(L, idx) && lua_isnoneornil(L, idx + 1) &&
      lua_isnoneornil(L, idx + 2) && lua_isnoneornil(L, idx + 3)) {
    *x = 0;
    *y = 0;
    *w = img->w;
    *h = img->h;
    return;
  }
  *x = (int)lb_checkint(L, idx);
  *y = (int)lb_checkint(L, idx + 1);
  *w = (int)lb_checkint(L, idx + 2);
  *h = (int)lb_checkint(L, idx + 3);
  if (*x < 0 || *y < 0 || *w < 0 || *h < 0 || *x > img->w - *w ||
      *y > img->h - *h)
    luaL_error(L, "rectangle (%d, %d, %d, %d) outside the %dx%d image", *x, *y,
               *w, *h, img->w, img->h);
}

// img:getPixel(x, y) -> RGB565 colour
static int l_graphics_image_getPixel(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x, y;
  image_check_pixel(L, img, 2, &x, &y);
  lua_pushinteger(L, img->data[y * img->w + x]);
  return 1;
}

// img:setPixel(x, y, color)
static int l_graphics_image_setPixel(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x, y;
  image_check_pixel(L, img, 2, &x, &y);
  uint16_t color = l_checkcolor(L, 4);
  img->data[y * img->w + x] = color;
  return 0;
}

// img:getPixels([x, y, w, h]) -> string of w * h little-endian RGB565 pixels
static int l_graphics_image_getPixels(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x, y, w, h;
  image_check_rect(L, img, 2, &x, &y, &w, &h);
  if (w == 0 || h == 0) {
    lua_pushliteral(L, "");
    return 1;
  }
  size_t row = (size_t)w * sizeof(uint16_t);
  if (w == img->w) {  // whole rows are contiguous: one copy, no buffer
    lua_pushlstring(L, (const char *)&img->data[y * img->w], row * h);
    return 1;
  }
  luaL_Buffer b;
  char *out = luaL_buffinitsize(L, &b, row * h);
  for (int r = 0; r < h; r++)
    memcpy(out + r * row, &img->data[(y + r) * img->w + x], row);
  luaL_pushresultsize(&b, row * h);
  return 1;
}

// img:setPixels(data [, x, y, w, h]): data holds exactly w * h little-endian
// RGB565 pixels (w * h * 2 bytes), row-major; the default rect is the image.
static int l_graphics_image_setPixels(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  if (lua_type(L, 2) != LUA_TSTRING)
    return luaL_typeerror(L, 2, "string");
  size_t len;
  const char *data = lua_tolstring(L, 2, &len);
  int x, y, w, h;
  image_check_rect(L, img, 3, &x, &y, &w, &h);
  size_t row = (size_t)w * sizeof(uint16_t);
  if (len != row * (size_t)h)
    return luaL_error(L, "pixel data is %d bytes, expected %d (%dx%d pixels)",
                      (int)len, (int)(row * h), w, h);
  for (int r = 0; r < h; r++)
    memcpy(&img->data[(y + r) * img->w + x], data + r * row, row);
  return 0;
}

static const luaL_Reg l_graphics_image_methods[] = {
    {"getSize", l_graphics_image_getSize},
    {"copy", l_graphics_image_copy},
    {"draw", l_graphics_image_draw},
    {"drawAnchored", l_graphics_image_drawAnchored},
    {"drawTiled", l_graphics_image_drawTiled},
    {"drawScaled", l_graphics_image_drawScaled},
    {"drawScaledNN", l_graphics_image_drawScaledNN},
    {"drawStretched", l_graphics_image_drawStretched},
    {"setTransparentColor", l_graphics_image_setTransparentColor},
    {"getTransparentColor", l_graphics_image_getTransparentColor},
    {"getMetadata", l_graphics_image_getMetadata},
    {"getPixel", l_graphics_image_getPixel},
    {"setPixel", l_graphics_image_setPixel},
    {"getPixels", l_graphics_image_getPixels},
    {"setPixels", l_graphics_image_setPixels},
    {NULL, NULL}};

// loadFromBuffer(string) or loadFromBuffer(qmibuf, [len]): the only
// buffers accepted are a Lua string and a sys.qmiPsramAlloc handle, whose
// length is checked against the handle's size. Any other userdata was once
// read as raw bytes (an 8-byte handle read `len` bytes past its end).
static int l_graphics_image_loadFromBuffer(lua_State *L) {
  size_t len;
  const uint8_t *data;

  if (lua_type(L, 1) == LUA_TSTRING) {
    data = (const uint8_t *)lua_tolstring(L, 1, &len);
  } else {
    qmi_buf_t *b = (qmi_buf_t *)luaL_testudata(L, 1, QMI_BUF_MT);
    if (!b)
      return luaL_typeerror(L, 1, "string or qmibuf");
    if (!b->p)
      return luaL_error(L, "loadFromBuffer: qmibuf freed");
    lua_Integer n = luaL_optinteger(L, 2, (lua_Integer)b->size);
    luaL_argcheck(L, n >= 0 && (uint64_t)n <= (uint64_t)b->size, 2,
                  "length exceeds the buffer");
    data = b->p;
    len = (size_t)n;
  }

  if (!data || len < 16) {
    return luaL_error(L, "buffer too small or invalid");
  }

  bool is_bmp = (data[0] == 'B' && data[1] == 'M');
  bool is_jpeg = (data[0] == 0xFF && data[1] == 0xD8);
  bool is_png = (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
                 data[3] == 0x47);
  bool is_gif = (data[0] == 'G' && data[1] == 'I' && data[2] == 'F');

  image_decode_result_t res = {0, 0, NULL};
  bool success = false;
  const char *err_msg = "unsupported image format";

  if (is_bmp) {
    success = decode_bmp_buffer(data, len, &res);
    err_msg = "BMP decoding failed";
  } else if (is_jpeg) {
    success = decode_jpeg_buffer(data, len, &res);
    err_msg = "JPEG decoding failed";
  } else if (is_png) {
    success = decode_png_buffer(data, len, &res);
    err_msg = "PNG decoding failed";
  } else if (is_gif) {
    success = decode_gif_buffer(data, len, &res);
    err_msg = "GIF decoding failed";
  }

  if (success && res.data) {
    lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
    img->w = res.w;
    img->h = res.h;
    img->data = res.data;
    img->transparent_color = 0;
    luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
    return 1;
  }

  return luaL_error(L, err_msg);
}

// getInfo(path) — parse just the image header, no pixel decode.
// Returns {width=, height=, format=} or nil, error.
static int l_graphics_image_getInfo(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false))
    return luaL_error(L, "access denied");

  int w = 0, h = 0;
  const char *fmt = image_probe(path, &w, &h);
  if (!fmt)
    return luaL_error(L, "unrecognized or truncated image: %s", path);

  lua_newtable(L);
  lua_pushinteger(L, w);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, h);
  lua_setfield(L, -2, "height");
  lua_pushstring(L, fmt);
  lua_setfield(L, -2, "format");
  return 1;
}

// loadRegion(path, x, y, w, h) — load an image and keep only the given
// sub-rectangle (clamped to the image bounds).
static int l_graphics_image_loadRegion(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  int rx = lb_checkint(L, 2);
  int ry = lb_checkint(L, 3);
  int rw = lb_checkint(L, 4);
  int rh = lb_checkint(L, 5);

  if (!fs_sandbox_check(L, path, false))
    return luaL_error(L, "access denied");

  pc_image_t *loaded = image_load(path);
  if (!loaded)
    return luaL_error(L, "failed to load image: %s", path);

  // Clamp region to image bounds
  if (rx < 0) { rw += rx; rx = 0; }
  if (ry < 0) { rh += ry; ry = 0; }
  if (rx + rw > loaded->w) rw = loaded->w - rx;
  if (ry + rh > loaded->h) rh = loaded->h - ry;
  if (rw <= 0 || rh <= 0) {
    image_free(loaded);
    return luaL_error(L, "region outside image bounds");
  }

  uint16_t *crop = (uint16_t *)umm_malloc((size_t)rw * rh * sizeof(uint16_t));
  if (!crop) {
    image_free(loaded);
    return luaL_error(L, "out of memory");
  }
  for (int y = 0; y < rh; y++)
    memcpy(&crop[y * rw], &loaded->data[(ry + y) * loaded->w + rx],
           (size_t)rw * sizeof(uint16_t));
  image_free(loaded);

  lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  img->w = rw;
  img->h = rh;
  img->data = crop;
  img->transparent_color = 0;
  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  return 1;
}

// loadScaled(path, w, h) — load an image and resample it to w×h (bilinear).
static int l_graphics_image_loadScaled(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  int dw = lb_checkint(L, 2);
  int dh = lb_checkint(L, 3);

  if (!fs_sandbox_check(L, path, false))
    return luaL_error(L, "access denied");
  if (dw <= 0 || dh <= 0 || dw > 2048 || dh > 2048)
    return luaL_error(L, "invalid target size");

  pc_image_t *loaded = image_load(path);
  if (!loaded)
    return luaL_error(L, "failed to load image: %s", path);

  uint16_t *out = (uint16_t *)umm_malloc((size_t)dw * dh * sizeof(uint16_t));
  if (!out) {
    image_free(loaded);
    return luaL_error(L, "out of memory");
  }

  const int sw = loaded->w, sh = loaded->h;
  for (int y = 0; y < dh; y++) {
    // Source coordinate (half-pixel centred), clamped to edge pixels
    float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
    if (fy < 0) fy = 0;
    int y0 = (int)fy;
    int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
    float wy = fy - (float)y0;

    for (int x = 0; x < dw; x++) {
      float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
      if (fx < 0) fx = 0;
      int x0 = (int)fx;
      int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
      float wx = fx - (float)x0;

      uint16_t c00 = loaded->data[y0 * sw + x0];
      uint16_t c10 = loaded->data[y0 * sw + x1];
      uint16_t c01 = loaded->data[y1 * sw + x0];
      uint16_t c11 = loaded->data[y1 * sw + x1];

      float w00 = (1.0f - wx) * (1.0f - wy);
      float w10 = wx * (1.0f - wy);
      float w01 = (1.0f - wx) * wy;
      float w11 = wx * wy;

      int r = (int)(w00 * ((c00 >> 11) & 0x1F) + w10 * ((c10 >> 11) & 0x1F) +
                    w01 * ((c01 >> 11) & 0x1F) + w11 * ((c11 >> 11) & 0x1F) + 0.5f);
      int g = (int)(w00 * ((c00 >> 5) & 0x3F) + w10 * ((c10 >> 5) & 0x3F) +
                    w01 * ((c01 >> 5) & 0x3F) + w11 * ((c11 >> 5) & 0x3F) + 0.5f);
      int b = (int)(w00 * (c00 & 0x1F) + w10 * (c10 & 0x1F) +
                    w01 * (c01 & 0x1F) + w11 * (c11 & 0x1F) + 0.5f);

      out[y * dw + x] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
  image_free(loaded);

  lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  img->w = dw;
  img->h = dh;
  img->data = out;
  img->transparent_color = 0;
  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  return 1;
}

static int l_graphics_image_getSupportedFormats(lua_State *L) {
  lua_newtable(L);
  lua_pushstring(L, "BMP");
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, "JPEG");
  lua_rawseti(L, -2, 2);
  lua_pushstring(L, "PNG");
  lua_rawseti(L, -2, 3);
  lua_pushstring(L, "GIF");
  lua_rawseti(L, -2, 4);
  return 1;
}

// ── Async preloading (Core 1) ────────────────────────────────────────────────

static int l_graphics_image_preload(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false))
    return luaL_error(L, "access denied");
  lua_pushboolean(L, image_preload_start(path));
  return 1;
}

static int l_graphics_image_poll_preload(lua_State *L) {
  bool ready = false;
  pc_image_t *loaded = image_preload_poll(&ready);
  if (loaded) {
    lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
    img->w = loaded->w;
    img->h = loaded->h;
    img->data = loaded->data;
    img->transparent_color = 0;
    loaded->data = NULL;
    umm_free(loaded);
    luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  } else {
    lua_pushnil(L);
  }
  lua_pushboolean(L, ready);
  return 2;
}

static int l_graphics_image_cancel_preload(lua_State *L) {
  (void)L;
  image_preload_cancel();
  return 0;
}

static const luaL_Reg l_graphics_image_lib[] = {
    {"new", l_graphics_image_new},
    {"load", l_graphics_image_load},
    {"loadFromBuffer", l_graphics_image_loadFromBuffer},
    {"getInfo", l_graphics_image_getInfo},
    {"loadRegion", l_graphics_image_loadRegion},
    {"loadScaled", l_graphics_image_loadScaled},
    {"getSupportedFormats", l_graphics_image_getSupportedFormats},
    {"preload", l_graphics_image_preload},
    {"pollPreload", l_graphics_image_poll_preload},
    {"cancelPreload", l_graphics_image_cancel_preload},
    {NULL, NULL}};

// drawGrid(x, y, cell_w, cell_h, cols, rows, color)
// Draws a grid of cols×rows cells in a single C call.
// Replaces cols*rows individual drawRect calls from Lua.
static int l_graphics_drawGrid(lua_State *L) {
  int x      = lb_checkint(L, 1);
  int y      = lb_checkint(L, 2);
  int cell_w = lb_checkint(L, 3);
  int cell_h = lb_checkint(L, 4);
  int cols   = lb_checkint(L, 5);
  int rows   = lb_checkint(L, 6);
  uint16_t color = l_checkcolor(L, 7);
  int total_w = cols * cell_w;
  int total_h = rows * cell_h;
  for (int r = 0; r <= rows; r++)
    display_fill_rect(x, y + r * cell_h, total_w, 1, color);
  for (int c = 0; c <= cols; c++)
    display_fill_rect(x + c * cell_w, y, 1, total_h, color);
  return 0;
}

// fillBorderedRect(x, y, w, h, fill_color, border_color)
// Fills a rectangle then draws a 1-pixel border over it in one C call.
// Replaces a fillRect + drawRect pair from Lua.
static int l_graphics_fillBorderedRect(lua_State *L) {
  int x = lb_checkint(L, 1);
  int y = lb_checkint(L, 2);
  int w = lb_checkint(L, 3);
  int h = lb_checkint(L, 4);
  uint16_t fill   = l_checkcolor(L, 5);
  uint16_t border = l_checkcolor(L, 6);
  display_fill_rect(x, y, w, h, fill);
  display_draw_rect(x, y, w, h, border);
  return 0;
}

// drawPlayfield(playfield, x, y, block_size, cols, rows, grid_color)
//
// playfield    : 2-D Lua table playfield[row][col] — values are RGB565 color
//                integers for filled cells, nil for empty cells.
//                row 1 = top, indexed 1..rows; col 1 = left, indexed 1..cols.
// x, y         : pixel coordinate of the top-left corner of the field
// block_size   : size of each cell in pixels (square)
// cols, rows   : field dimensions (number of columns and rows)
// grid_color   : RGB565 color for the grid lines and block borders
//
// In a single C call this function:
//   1. Draws the grid background (same as drawGrid)
//   2. Iterates every cell; for non-nil values draws a fillBorderedRect
//
// Replaces the Lua draw_playfield() loop which made up to 200 Lua→C calls
// per frame (one fillBorderedRect per filled cell) plus the drawGrid call.
static int l_graphics_drawPlayfield(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  int      ox         = lb_checkint(L, 2);
  int      oy         = lb_checkint(L, 3);
  int      block_size = lb_checkint(L, 4);
  int      cols       = lb_checkint(L, 5);
  int      rows       = lb_checkint(L, 6);
  uint16_t grid_color = l_checkcolor(L, 7);

  if (rows <= 0 || cols <= 0) return 0;

  int total_w = cols * block_size;
  int total_h = rows * block_size;

  // ── 1. Draw grid lines ────────────────────────────────────────────────────
  for (int r = 0; r <= rows; r++)
    display_fill_rect(ox, oy + r * block_size, total_w, 1, grid_color);
  for (int c = 0; c <= cols; c++)
    display_fill_rect(ox + c * block_size, oy, 1, total_h, grid_color);

  // ── 2. Draw filled cells ──────────────────────────────────────────────────
  for (int row = 1; row <= rows; row++) {
    lua_rawgeti(L, 1, row);          // push playfield[row]
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    int py = oy + (row - 1) * block_size;
    for (int col = 1; col <= cols; col++) {
      lua_rawgeti(L, -1, col);       // push playfield[row][col]
      if (!lua_isnil(L, -1)) {
        uint16_t fill = (uint16_t)lua_tointeger(L, -1);
        int px = ox + (col - 1) * block_size;
        display_fill_rect(px, py, block_size, block_size, fill);
        display_draw_rect(px, py, block_size, block_size, grid_color);
      }
      lua_pop(L, 1);                 // pop playfield[row][col]
    }
    lua_pop(L, 1);                   // pop playfield[row]
  }

  return 0;
}

// updateDrawParticles(flat_array, delta_s) -> live_count
//
// flat_array is a Lua sequence with 6 values per particle:
//   { x, y, vx, vy, life_ms, color,  x, y, vx, vy, life_ms, color, ... }
//
// In a single C call this function:
//   1. Updates each particle's position (x += vx*dt, y += vy*dt)
//   2. Decrements its lifetime (life -= dt*1000)
//   3. Draws a pixel for every particle that is still alive
//   4. Compacts live particles to the front of the array (removes dead ones)
//
// Replaces per-frame Lua loops that otherwise cost one Lua->C call per pixel.
static int l_graphics_updateDrawParticles(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  float dt       = (float)luaL_checknumber(L, 2);
  float life_dec = dt * 1000.0f;

  int n     = (int)lua_rawlen(L, 1); // total values in flat array
  int write = 1;                     // compaction write cursor (1-indexed)

  for (int base = 1; base <= n; base += 6) {
    lua_rawgeti(L, 1, base);     float x    = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 1); float y    = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 2); float vx   = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 3); float vy   = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 4); float life = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 5); uint16_t color = (uint16_t)lua_tointeger(L, -1); lua_pop(L, 1);

    x    += vx * dt;
    y    += vy * dt;
    life -= life_dec;

    if (life > 0.0f) {
      display_set_pixel((int)x, (int)y, color);
      lua_pushnumber(L,  x);     lua_rawseti(L, 1, write++);
      lua_pushnumber(L,  y);     lua_rawseti(L, 1, write++);
      lua_pushnumber(L,  vx);    lua_rawseti(L, 1, write++);
      lua_pushnumber(L,  vy);    lua_rawseti(L, 1, write++);
      lua_pushnumber(L,  life);  lua_rawseti(L, 1, write++);
      lua_pushinteger(L, color); lua_rawseti(L, 1, write++);
    }
  }

  // Clear dead-particle slots at the tail so lua_rawlen stays correct.
  for (int i = write; i <= n; i++) {
    lua_pushnil(L);
    lua_rawseti(L, 1, i);
  }

  lua_pushinteger(L, (write - 1) / 6); // return live particle count
  return 1;
}

// draw3DWireframe(verts_flat, edges_flat, angleX, angleY, angleZ,
//                 scx, scy, fov, edge_color [, vert_color, vert_size])
//
// verts_flat  : flat Lua sequence {x1,y1,z1, x2,y2,z2, ...}  (n/3 vertices)
// edges_flat  : flat Lua sequence {a1,b1, a2,b2, ...}         (1-based indices)
// angleX/Y/Z  : rotation angles in radians, applied in X→Y→Z order
// scx, scy    : screen centre pixels
// fov         : perspective distance (e.g. 200)
// edge_color  : RGB565 line colour
// vert_color  : (optional) RGB565 dot colour; dots omitted when absent/0
// vert_size   : (optional) dot size in pixels (default 3)
//
// Computes the combined rotation matrix M = Rz*Ry*Rx once per call (6 trig
// ops), then transforms every vertex with 9 muls + 6 adds, projects to screen,
// and draws all edges and vertex dots — all in a single Lua→C call.
static int l_graphics_draw3DWireframe(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  float aX  = (float)luaL_checknumber(L, 3);
  float aY  = (float)luaL_checknumber(L, 4);
  float aZ  = (float)luaL_checknumber(L, 5);
  int   scx = lb_checkint(L, 6);
  int   scy = lb_checkint(L, 7);
  float fov = (float)luaL_checknumber(L, 8);
  uint16_t edge_color = l_checkcolor(L, 9);
  uint16_t vert_color = (lua_gettop(L) >= 10) ? l_checkcolor(L, 10) : 0;
  int vert_size       = (lua_gettop(L) >= 11) ? (int)lb_checkint(L, 11) : 3;

  // Build combined rotation matrix M = Rz(aZ) * Ry(aY) * Rx(aX).
  // Applying M*v is equivalent to rotateX then rotateY then rotateZ.
  float cX = cosf(aX), sX = sinf(aX);
  float cY = cosf(aY), sY = sinf(aY);
  float cZ = cosf(aZ), sZ = sinf(aZ);

  float m00 = cZ*cY,               m01 = cZ*sY*sX - sZ*cX,  m02 = cZ*sY*cX + sZ*sX;
  float m10 = sZ*cY,               m11 = sZ*sY*sX + cZ*cX,  m12 = sZ*sY*cX - cZ*sX;
  float m20 = -sY,                 m21 = cY*sX,              m22 = cY*cX;

  // Transform + project all vertices into screen-space integer coords.
  int n_verts = (int)lua_rawlen(L, 1) / 3;
  if (n_verts > 64) n_verts = 64;
  int px[64], py[64];

  for (int i = 0; i < n_verts; i++) {
    lua_rawgeti(L, 1, i*3 + 1); float vx = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, i*3 + 2); float vy = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, i*3 + 3); float vz = (float)lua_tonumber(L, -1); lua_pop(L, 1);

    float rx = m00*vx + m01*vy + m02*vz;
    float ry = m10*vx + m11*vy + m12*vz;
    float rz = m20*vx + m21*vy + m22*vz;

    float scale = fov / (fov + rz);
    px[i] = (int)(scx + rx * scale);
    py[i] = (int)(scy + ry * scale);
  }

  // Draw edges.
  int n_edges_flat = (int)lua_rawlen(L, 2);
  for (int i = 1; i <= n_edges_flat; i += 2) {
    lua_rawgeti(L, 2, i);     int a = (int)lua_tointeger(L, -1) - 1; lua_pop(L, 1);
    lua_rawgeti(L, 2, i + 1); int b = (int)lua_tointeger(L, -1) - 1; lua_pop(L, 1);
    if (a >= 0 && a < n_verts && b >= 0 && b < n_verts)
      display_draw_line(px[a], py[a], px[b], py[b], edge_color);
  }

  // Draw vertex dots (optional).
  if (vert_color && vert_size > 0) {
    int half = vert_size / 2;
    for (int i = 0; i < n_verts; i++)
      display_fill_rect(px[i] - half, py[i] - half, vert_size, vert_size, vert_color);
  }

  return 0;
}

// ── Tilemap type (used by sprites and tilemap API) ─────────────────────────
#define GRAPHICS_TILEMAP_MT "picocalc.graphics.tilemap"
#define TILEMAP_MAX_WIDTH 128
#define TILEMAP_MAX_HEIGHT 128
#define TILEMAP_UV_TILESET 1  // user value: the image behind ->tileset

typedef struct {
  lua_image_t *tileset;  // tileset image (spritesheet atlas)
  uint16_t *tiles;       // 2D tile index array (umm_malloc'd)
  int map_w, map_h;      // map dimensions in tiles
  int tile_w, tile_h;    // tile size in pixels
  int tiles_per_row;     // tiles per row in the tileset image
  bool destroyed;        // set by __gc
} lua_tilemap_t;

static lua_tilemap_t *check_tilemap(lua_State *L, int idx) {
  lua_tilemap_t *tm = (lua_tilemap_t *)luaL_checkudata(L, idx, GRAPHICS_TILEMAP_MT);
  if (tm->destroyed)
    luaL_error(L, "attempt to use a destroyed tilemap");
  return tm;
}

// Draw visible tiles from a tilemap at the given scroll offset
static void tilemap_draw(lua_tilemap_t *tm, int scroll_x, int scroll_y) {
  if (!tm || !tm->tileset || !tm->tiles || !tm->tileset->data) return;

  int tw = tm->tile_w;
  int th = tm->tile_h;
  int tpr = tm->tiles_per_row;
  if (tpr <= 0) return;

  int start_col = scroll_x / tw;
  int start_row = scroll_y / th;
  int end_col = (scroll_x + 320 + tw - 1) / tw;
  int end_row = (scroll_y + 320 + th - 1) / th;

  if (start_col < 0) start_col = 0;
  if (start_row < 0) start_row = 0;
  if (end_col > tm->map_w) end_col = tm->map_w;
  if (end_row > tm->map_h) end_row = tm->map_h;

  for (int row = start_row; row < end_row; row++) {
    for (int col = start_col; col < end_col; col++) {
      uint16_t tile_idx = tm->tiles[row * tm->map_w + col];
      if (tile_idx == 0) continue; // 0 = empty/transparent

      int idx = tile_idx - 1;
      int src_col = idx % tpr;
      int src_row = idx / tpr;
      int src_x = src_col * tw;
      int src_y = src_row * th;

      int dst_x = col * tw - scroll_x;
      int dst_y = row * th - scroll_y;

      display_draw_image_partial(dst_x, dst_y, tm->tileset->w, tm->tileset->h,
                                  tm->tileset->data, src_x, src_y,
                                  tw, th,
                                  false, false, 0);
    }
  }
}

// ── Font type (used by sprites, graphics text, and font API) ────────────────
#define GRAPHICS_FONT_MT "picocalc.graphics.font"

typedef struct {
  int font_id;        // registry id; >= FONT_REGISTRY_BUILTIN when owned
  bool owned;         // true when this object loaded the slot and must free it
  char name[64];      // built-in name or the path it was loaded from
  bool destroyed;     // set by __gc (an owned slot is unloaded then)
} lua_font_t;

static lua_font_t *check_font(lua_State *L, int idx) {
  lua_font_t *f = (lua_font_t *)luaL_checkudata(L, idx, GRAPHICS_FONT_MT);
  if (f->destroyed)
    luaL_error(L, "attempt to use a destroyed font");
  return f;
}

// Resolve the pc_font_t a Lua call should measure with: the font object at
// `idx` when one was passed, else the display's active font.
static const pc_font_t *font_for_arg(lua_State *L, int idx) {
  if (lua_gettop(L) >= idx && lua_isuserdata(L, idx)) {
    const pc_font_t *f = font_registry_get(check_font(L, idx)->font_id);
    if (f) return f;
  }
  return display_get_active_font();
}

// Run `fn` with the display font temporarily switched to the object at
// `idx` (if any). Every draw path below uses this so the swap/restore
// logic exists once.
#define WITH_FONT_ARG(L, idx, body) do {                       \
    int prev_font_ = display_get_font();                      \
    if (lua_gettop(L) >= (idx) && lua_isuserdata(L, (idx)))   \
      display_set_font(check_font(L, (idx))->font_id);        \
    body;                                                     \
    display_set_font(prev_font_);                             \
  } while (0)

// Shared word-wrap loop. `emit(x, y, line, user)` is called per line with
// x already adjusted for alignment (0 left, 1 center, 2 right) within rw.
// Returns the number of lines emitted. Stops when the next line would not
// fit in rh.
static int wrap_lines(const pc_font_t *f, const char *text,
                      int rx, int ry, int rw, int rh, int alignment,
                      void (*emit)(int x, int y, const char *line, void *user),
                      void *user) {
  int fh = f->height;
  int y = ry, lines = 0;
  const char *p = text;
  while (*p && (y + fh <= ry + rh)) {
    int n = font_wrap_line(f, p, rw);
    char line[128];
    if (n > 127) n = 127;
    memcpy(line, p, (size_t)n);
    line[n] = '\0';
    int tw = font_text_width(f, line);
    int x = rx;
    if (alignment == 1) x = rx + (rw - tw) / 2;
    else if (alignment == 2) x = rx + rw - tw;
    emit(x, y, line, user);
    lines++;
    y += fh;
    p += n;
    if (*p == ' ') p++;
    if (*p == '\n') p++;
  }
  return lines;
}

typedef struct { uint16_t fg, bg; } emit_fb_t;
static void emit_to_display(int x, int y, const char *line, void *user) {
  emit_fb_t *e = (emit_fb_t *)user;
  display_draw_text(x, y, line, e->fg, e->bg);
}

typedef struct { uint16_t *buf; int w, h; uint16_t fg, bg; } emit_buf_t;
static void emit_to_buffer(int x, int y, const char *line, void *user) {
  emit_buf_t *e = (emit_buf_t *)user;
  display_draw_text_to_buffer(e->buf, e->w, e->h, x, y, line, e->fg, e->bg);
}

// ── Sprite System ───────────────────────────────────────────────────────────────

#define GRAPHICS_SPRITE_MT "picocalc.graphics.sprite"
#define MAX_SPRITES 256

// Sprite user values: the Lua objects behind the image, stencil and tilemap
// pointers (anchor_set). Create sprites with new_sprite_ud().
#define SPRITE_UV_IMAGE   1
#define SPRITE_UV_STENCIL 2
#define SPRITE_UV_TILEMAP 3
#define SPRITE_NUV        3

typedef struct {
  int x, y;
  int width, height;
  int center_x, center_y;
  float scale, scale_y;
  float rotation;
  int z_index;
  bool visible;
  bool updates_enabled;
  bool always_redraw;
  bool redraws_on_image_change;
  bool dirty;
  int tag;
  bool ignores_draw_offset;
  bool opaque;
  bool flip_x, flip_y;
  int bounds_x, bounds_y, bounds_w, bounds_h;
  bool collisions_enabled;
  int collide_x, collide_y, collide_w, collide_h;
  int clip_x, clip_y, clip_w, clip_h;
  bool has_clip;
  int group_mask;
  int collides_with_mask;
  lua_image_t *image;
  uint16_t *frame_data;  // extracted frame pixels (NULL = use full image)
  int frame_w, frame_h;  // dimensions of extracted frame
  int scale_nn;         // integer scale for NN scaling
  bool use_nn_scaling;  // use NN instead of bilinear
  uint16_t transparent_color;  // 0 = use global setting
  lua_image_t *stencil;        // stencil image (deferred masking)
  uint8_t stencil_pattern[8];  // 8x8 dither stencil pattern
  bool has_stencil_pattern;    // true if stencil_pattern is active
  void *tilemap;               // lua_tilemap_t* if set (renders tilemap instead of image)
  bool destroyed;              // set by __gc
} lua_sprite_t;

static lua_sprite_t *s_sprites[MAX_SPRITES];
static int s_sprite_count = 0;

static uint8_t s_global_stencil[8];
static bool s_has_global_stencil = false;

static lua_sprite_t *check_sprite(lua_State *L, int idx) {
  lua_sprite_t *s = (lua_sprite_t *)luaL_checkudata(L, idx, GRAPHICS_SPRITE_MT);
  if (s->destroyed)
    luaL_error(L, "attempt to use a destroyed sprite");
  return s;
}

// ── Display list ─────────────────────────────────────────────────────────────
// s_sprites[] is the draw order. The registry table keyed by the address of
// s_sprites maps lightuserdata(sprite) -> sprite userdata, so a listed sprite
// is always reachable from Lua and is never collected while displayed; it is
// also how enumeration (getAllSprites, query*, collisions) hands out the real
// sprite objects. The two change together, only through these helpers:
// add() anchors, remove()/removeSprites()/removeAll() release.
static int sprite_list_find(const lua_sprite_t *s) {
  for (int i = 0; i < s_sprite_count; i++)
    if (s_sprites[i] == s) return i;
  return -1;
}

// Add the sprite at idx (idempotent). False when the list is full.
static bool sprite_list_add(lua_State *L, int idx) {
  lua_sprite_t *s = check_sprite(L, idx);
  if (sprite_list_find(s) >= 0) return true;
  if (s_sprite_count >= MAX_SPRITES) return false;
  idx = lua_absindex(L, idx);
  lua_rawgetp(L, LUA_REGISTRYINDEX, s_sprites);
  lua_pushvalue(L, idx);
  lua_rawsetp(L, -2, s);  // anchor first: this may raise out-of-memory
  lua_pop(L, 1);
  s_sprites[s_sprite_count++] = s;
  return true;
}

static void sprite_list_unlink(int i) {
  for (int j = i; j < s_sprite_count - 1; j++)
    s_sprites[j] = s_sprites[j + 1];
  s_sprite_count--;
}

static void sprite_list_remove(lua_State *L, lua_sprite_t *s) {
  int i = sprite_list_find(s);
  if (i < 0) return;
  sprite_list_unlink(i);
  lua_rawgetp(L, LUA_REGISTRYINDEX, s_sprites);
  lua_pushnil(L);
  lua_rawsetp(L, -2, s);
  lua_pop(L, 1);
}

static void sprite_list_clear(lua_State *L) {
  // The new anchor table first: if it raises (out of memory) the list and
  // its anchors are both left as they were, never an empty list over a
  // table that still pins every old sprite.
  lua_newtable(L);
  lua_rawsetp(L, LUA_REGISTRYINDEX, s_sprites);
  s_sprite_count = 0;
}

// Push the userdata of a listed sprite (nil if it is not in the list).
// Enumeration hands out these real objects, never a proxy or a light
// userdata: a 4-byte proxy with the sprite metatable let any method read
// and write past its allocation.
static void push_sprite(lua_State *L, const lua_sprite_t *s) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, s_sprites);
  lua_rawgetp(L, -1, s);
  lua_remove(L, -2);
}

// Push a new, zeroed sprite userdata with its anchor slots (no metatable yet).
static lua_sprite_t *new_sprite_ud(lua_State *L) {
  lua_sprite_t *s = (lua_sprite_t *)lua_newuserdatauv(L, sizeof(lua_sprite_t),
                                                       SPRITE_NUV);
  memset(s, 0, sizeof(*s));
  return s;
}

static int l_sprite_new(lua_State *L);
static int l_sprite_add(lua_State *L);
static int l_sprite_remove(lua_State *L);
static int l_sprite_update(lua_State *L);
static int l_sprite_setImage(lua_State *L);
static int l_sprite_getImage(lua_State *L);
static int l_sprite_moveTo(lua_State *L);
static int l_sprite_moveBy(lua_State *L);
static int l_sprite_getPosition(lua_State *L);
static int l_sprite_setZIndex(lua_State *L);
static int l_sprite_getZIndex(lua_State *L);
static int l_sprite_setVisible(lua_State *L);
static int l_sprite_isVisible(lua_State *L);
static int l_sprite_setCenter(lua_State *L);
static int l_sprite_getCenter(lua_State *L);
static int l_sprite_getCenterPoint(lua_State *L);
static int l_sprite_setSize(lua_State *L);
static int l_sprite_getSize(lua_State *L);
static int l_sprite_setScale(lua_State *L);
static int l_sprite_getScale(lua_State *L);
static int l_sprite_setRotation(lua_State *L);
static int l_sprite_setScaleNN(lua_State *L);
static int l_sprite_setTransparentColor(lua_State *L);
static int l_sprite_getRotation(lua_State *L);
static int l_sprite_copy(lua_State *L);
static int l_sprite_setUpdatesEnabled(lua_State *L);
static int l_sprite_updatesEnabled(lua_State *L);
static int l_sprite_setTag(lua_State *L);
static int l_sprite_getTag(lua_State *L);
static int l_sprite_setImageDrawMode(lua_State *L);
static int l_sprite_setImageFlip(lua_State *L);
static int l_sprite_getImageFlip(lua_State *L);
static int l_sprite_setIgnoresDrawOffset(lua_State *L);
static int l_sprite_setBounds(lua_State *L);
static int l_sprite_getBounds(lua_State *L);
static int l_sprite_getBoundsRect(lua_State *L);
static int l_sprite_setOpaque(lua_State *L);
static int l_sprite_isOpaque(lua_State *L);
static int l_sprite_draw(lua_State *L);
static int l_sprite_updateSingle(lua_State *L);
static int l_sprite_setBackgroundDrawingCallback(lua_State *L);
static int l_sprite_index(lua_State *L);
static int l_sprite_newindex(lua_State *L);
static int l_sprite_addSprite(lua_State *L);
static int l_sprite_removeSprite(lua_State *L);
static int l_sprite_getAllSprites(lua_State *L);
static int l_sprite_spriteCount(lua_State *L);
static int l_sprite_removeAll(lua_State *L);
static int l_sprite_removeSprites(lua_State *L);
static int l_sprite_performOnAllSprites(lua_State *L);
static int l_sprite_setCollisionsEnabled(lua_State *L);
static int l_sprite_collisionsEnabled(lua_State *L);
static int l_sprite_setCollideRect(lua_State *L);
static int l_sprite_getCollideRect(lua_State *L);
static int l_sprite_getCollideBounds(lua_State *L);
static int l_sprite_clearCollideRect(lua_State *L);
static int l_sprite_overlappingSprites(lua_State *L);
static int l_sprite_allOverlappingSprites(lua_State *L);
static int l_sprite_setGroups(lua_State *L);
static int l_sprite_setCollidesWithGroups(lua_State *L);
static int l_sprite_setGroupMask(lua_State *L);
static int l_sprite_getGroupMask(lua_State *L);
static int l_sprite_setCollidesWithGroupsMask(lua_State *L);
static int l_sprite_getCollidesWithGroupsMask(lua_State *L);
static int l_sprite_resetGroupMask(lua_State *L);
static int l_sprite_resetCollidesWithGroupsMask(lua_State *L);
static int l_sprite_checkCollisions(lua_State *L);
static int l_sprite_querySpritesAtPoint(lua_State *L);
static int l_sprite_querySpritesInRect(lua_State *L);
static int l_sprite_setClipRect(lua_State *L);
static int l_sprite_clearClipRect(lua_State *L);
static int l_sprite_setAlwaysRedraw(lua_State *L);
static int l_sprite_getAlwaysRedraw(lua_State *L);
static int l_sprite_markDirty(lua_State *L);
static int l_sprite_addDirtyRect(lua_State *L);
static int l_sprite_setRedrawsOnImageChange(lua_State *L);
static int l_sprite_querySpritesAlongLine(lua_State *L);
static int l_sprite_querySpriteInfoAlongLine(lua_State *L);
static int l_sprite_moveWithCollisions(lua_State *L);
static int l_sprite_collisionResponse(lua_State *L);
static int l_sprite_setStencilImage(lua_State *L);
static int l_sprite_clearStencil(lua_State *L);
static int l_sprite_setStencilPattern(lua_State *L);
static int l_sprite_alphaCollision(lua_State *L);
static int l_sprite_setClipRectsInRange(lua_State *L);
static int l_sprite_clearClipRectsInRange(lua_State *L);
static int l_sprite_addEmptyCollisionSprite(lua_State *L);
static int l_graphics_setStencilPattern(lua_State *L);

static int l_sprite_gc(lua_State *L) {
  lua_sprite_t *s = (lua_sprite_t *)luaL_checkudata(L, 1, GRAPHICS_SPRITE_MT);
  // A listed sprite is anchored, so this only happens at lua_close. Unlink
  // it anyway: s_sprites[] must never hold a freed sprite.
  int i = sprite_list_find(s);
  if (i >= 0) sprite_list_unlink(i);
  if (s->frame_data) {
    umm_free(s->frame_data);
    s->frame_data = NULL;
  }
  s->image = NULL;
  s->stencil = NULL;
  s->tilemap = NULL;
  s->has_stencil_pattern = false;
  s->destroyed = true;  // never re-listed: s_sprites[] would outlive it
  return 0;
}

// The pixels a sprite draws and their real dimensions: the extracted frame
// if there is one, else the image. Sprite width/height are bounds only
// (setSize and sprite.width accept anything) and are never a source stride:
// a sprite sized past its image used to read past the image's pixels.
static const uint16_t *sprite_pixels(const lua_sprite_t *s, int *w, int *h) {
  if (s->frame_data) {
    *w = s->frame_w;
    *h = s->frame_h;
    return s->frame_data;
  }
  if (s->image && s->image->data) {
    *w = s->image->w;
    *h = s->image->h;
    return s->image->data;
  }
  *w = *h = 0;
  return NULL;
}

// Draw a sprite's pixels at (x, y) with its scale/rotation/flip settings.
static void sprite_draw_at(const lua_sprite_t *s, int x, int y) {
  int src_w, src_h;
  const uint16_t *data = sprite_pixels(s, &src_w, &src_h);
  if (!data || src_w <= 0 || src_h <= 0) return;
  if (s->use_nn_scaling && s->scale_nn > 1) {
    display_draw_image_scaled_nn(x, y, data, src_w, src_h, src_w * s->scale_nn,
                                 src_h * s->scale_nn, s->transparent_color);
  } else if (s->rotation != 0.0f || s->scale != 1.0f || s->scale_y != 1.0f) {
    display_draw_image_scaled(x, y, src_w, src_h, data, s->scale, s->rotation,
                              s->transparent_color);
  } else {
    display_draw_image_partial(x, y, src_w, src_h, data, 0, 0, src_w, src_h,
                               s->flip_x, s->flip_y, s->transparent_color);
  }
}

static int l_sprite_new(lua_State *L) {
  // Resolve the argument BEFORE pushing the sprite: with no argument, index 1
  // would otherwise be the new userdata itself.
  lua_image_t *img = lua_isnoneornil(L, 1) ? NULL : check_image(L, 1);
  lua_sprite_t *s = new_sprite_ud(L);
  s->x = 0;
  s->y = 0;
  s->width = 0;
  s->height = 0;
  s->center_x = 0;
  s->center_y = 0;
  s->scale = 1.0f;
  s->scale_y = 1.0f;
  s->rotation = 0.0f;
  s->z_index = 0;
  s->visible = true;
  s->updates_enabled = true;
  s->always_redraw = false;
  s->redraws_on_image_change = true;
  s->dirty = false;
  s->tag = 0;
  s->ignores_draw_offset = false;
  s->opaque = true;
  s->flip_x = false;
  s->flip_y = false;
  s->bounds_x = 0;
  s->bounds_y = 0;
  s->bounds_w = 0;
  s->bounds_h = 0;
  s->collisions_enabled = false;
  s->collide_x = 0;
  s->collide_y = 0;
  s->collide_w = 0;
  s->collide_h = 0;
  s->clip_x = 0;
  s->clip_y = 0;
  s->clip_w = 0;
  s->clip_h = 0;
  s->has_clip = false;
  s->group_mask = 0;
  s->collides_with_mask = 0;
  s->image = NULL;
  s->frame_data = NULL;
  s->frame_w = 0;
  s->frame_h = 0;
  s->scale_nn = 1;
  s->use_nn_scaling = false;
  s->transparent_color = 0;  // use global setting
  s->stencil = NULL;
  memset(s->stencil_pattern, 0, sizeof(s->stencil_pattern));
  s->has_stencil_pattern = false;
  s->tilemap = NULL;

  if (img) {
    s->image = img;
    s->width = img->w;
    s->height = img->h;
    anchor_set(L, -1, SPRITE_UV_IMAGE, 1);
  }

  luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
  return 1;
}

static int l_sprite_add(lua_State *L) {
  if (!sprite_list_add(L, 1))
    return luaL_error(L, "max sprites reached");
  return 0;
}

static int l_sprite_remove(lua_State *L) {
  sprite_list_remove(L, check_sprite(L, 1));
  return 0;
}

static int l_sprite_update(lua_State *L) {
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    // Tilemap sprites: draw tilemap at sprite position as scroll offset
    if (s->updates_enabled && s->visible && s->tilemap) {
      lua_tilemap_t *tm = (lua_tilemap_t *)s->tilemap;
      tilemap_draw(tm, -s->x, -s->y);
      continue;
    }
    if (s->updates_enabled && s->visible && s->image)
      sprite_draw_at(s, s->x, s->y);
  }
  return 0;
}

static int l_sprite_setImage(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  // Changing the image invalidates any extracted frame
  if (s->frame_data) {
    umm_free(s->frame_data);
    s->frame_data = NULL;
    s->frame_w = 0;
    s->frame_h = 0;
  }
  if (lua_isnil(L, 2)) {
    s->image = NULL;
    s->width = 0;
    s->height = 0;
    anchor_set(L, 1, SPRITE_UV_IMAGE, 0);
    return 0;
  }
  s->image = check_image(L, 2);
  anchor_set(L, 1, SPRITE_UV_IMAGE, 2);
  s->width = s->image->w;
  s->height = s->image->h;
  if (lua_isboolean(L, 3))
    s->flip_x = lua_toboolean(L, 3);
  if (lua_isnumber(L, 4))
    s->scale = (float)lua_tonumber(L, 4);
  if (lua_isnumber(L, 5))
    s->scale_y = (float)lua_tonumber(L, 5);
  return 0;
}

static int l_sprite_getImage(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (!s->image)
    return 0;
  lua_getiuservalue(L, 1, SPRITE_UV_IMAGE);  // the image object itself
  return 1;
}

static int l_sprite_moveTo(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->x = lb_checkint(L, 2);
  s->y = lb_checkint(L, 3);
  return 0;
}

static int l_sprite_moveBy(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->x += lb_checkint(L, 2);
  s->y += lb_checkint(L, 3);
  return 0;
}

static int l_sprite_getPosition(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->x);
  lua_pushinteger(L, s->y);
  return 2;
}

static int l_sprite_setZIndex(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->z_index = lb_checkint(L, 2);
  return 0;
}

static int l_sprite_getZIndex(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->z_index);
  return 1;
}

static int l_sprite_setVisible(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->visible = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_isVisible(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->visible);
  return 1;
}

static int l_sprite_setCenter(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->center_x = lb_checkint(L, 2);
  s->center_y = lb_checkint(L, 3);
  return 0;
}

static int l_sprite_getCenter(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->center_x);
  lua_pushinteger(L, s->center_y);
  return 2;
}

static int l_sprite_getCenterPoint(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_createtable(L, 2, 0);
  lua_pushinteger(L, s->center_x);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, s->center_y);
  lua_rawseti(L, -2, 2);
  return 1;
}

static int l_sprite_setSize(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->width = lb_checkint(L, 2);
  s->height = lb_checkint(L, 3);
  return 0;
}

static int l_sprite_getSize(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->width);
  lua_pushinteger(L, s->height);
  return 2;
}

static int l_sprite_setScale(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->scale = (float)luaL_checknumber(L, 2);
  s->scale_y = lua_isnumber(L, 3) ? (float)lua_tonumber(L, 3) : s->scale;
  return 0;
}

static int l_sprite_getScale(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushnumber(L, s->scale);
  lua_pushnumber(L, s->scale_y);
  return 2;
}

static int l_sprite_setRotation(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->rotation = (float)luaL_checknumber(L, 2);
  if (lua_isnumber(L, 3))
    s->scale = (float)lua_tonumber(L, 3);
  if (lua_isnumber(L, 4))
    s->scale_y = (float)lua_tonumber(L, 4);
  return 0;
}

static int l_sprite_getRotation(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushnumber(L, s->rotation);
  return 1;
}

static int l_sprite_setScaleNN(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int scale = lb_checkint(L, 2);
  if (scale <= 0)
    return luaL_error(L, "scale must be positive integer");
  
  s->scale_nn = scale;
  s->use_nn_scaling = true;
  return 0;
}

static int l_sprite_setTransparentColor(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (lua_isnil(L, 2)) {
    s->transparent_color = 0;  // use global setting
  } else {
    s->transparent_color = l_checkcolor(L, 2);
  }
  return 0;
}

static int l_sprite_copy(lua_State *L) {
  lua_sprite_t *src = check_sprite(L, 1);
  lua_sprite_t *dst = new_sprite_ud(L);
  memcpy(dst, src, sizeof(lua_sprite_t));
  // The copy shares the image/stencil/tilemap, so it anchors them too.
  for (int slot = 1; slot <= SPRITE_NUV; slot++) {
    lua_getiuservalue(L, 1, slot);
    lua_setiuservalue(L, -2, slot);
  }
  // Deep-copy extracted frame data so each sprite owns its buffer
  if (src->frame_data && src->frame_w > 0 && src->frame_h > 0) {
    int sz = src->frame_w * src->frame_h * sizeof(uint16_t);
    dst->frame_data = (uint16_t *)umm_malloc(sz);
    if (dst->frame_data)
      memcpy(dst->frame_data, src->frame_data, sz);
    else
      dst->frame_data = NULL;
  }
  luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
  return 1;
}

static int l_sprite_setSourceRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int sx = lb_checkint(L, 2);
  int sy = lb_checkint(L, 3);
  int sw = lb_checkint(L, 4);
  int sh = lb_checkint(L, 5);

  if (!s->image) return 0;
  // The image is anchored, but a finaliser that runs after the image's own
  // can still get here with its pixels freed.
  if (!s->image->data)
    return luaL_error(L, "attempt to use a freed image");

  // Clamp to image bounds
  if (sx < 0) sx = 0;
  if (sy < 0) sy = 0;
  if (sx + sw > s->image->w) sw = s->image->w - sx;
  if (sy + sh > s->image->h) sh = s->image->h - sy;
  if (sw <= 0 || sh <= 0) return 0;

  if (s->frame_data) {
    umm_free(s->frame_data);
    s->frame_data = NULL;
  }

  s->frame_data = (uint16_t *)umm_malloc(sw * sh * sizeof(uint16_t));
  if (!s->frame_data) return 0;

  for (int row = 0; row < sh; row++)
    for (int col = 0; col < sw; col++)
      s->frame_data[row * sw + col] =
          s->image->data[(sy + row) * s->image->w + (sx + col)];

  s->frame_w = sw;
  s->frame_h = sh;
  return 0;
}

static int l_sprite_clearSourceRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (s->frame_data) {
    umm_free(s->frame_data);
    s->frame_data = NULL;
    s->frame_w = 0;
    s->frame_h = 0;
  }
  return 0;
}

static int l_sprite_setUpdatesEnabled(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->updates_enabled = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_updatesEnabled(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->updates_enabled);
  return 1;
}

static int l_sprite_setTag(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->tag = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_getTag(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->tag);
  return 1;
}

static int l_sprite_setImageDrawMode(lua_State *L) {
  return 0;
}

static int l_sprite_setImageFlip(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->flip_x = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_getImageFlip(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->flip_x);
  return 1;
}

static int l_sprite_setIgnoresDrawOffset(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->ignores_draw_offset = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_setBounds(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    lua_getfield(L, 2, "w");
    lua_getfield(L, 2, "h");
    s->bounds_x = lb_optint_at(L, -4, 2, "field 'x'", 0);
    s->bounds_y = lb_optint_at(L, -3, 2, "field 'y'", 0);
    s->bounds_w = lb_optint_at(L, -2, 2, "field 'w'", 0);
    s->bounds_h = lb_optint_at(L, -1, 2, "field 'h'", 0);
    lua_pop(L, 4);
  } else {
    s->bounds_x = lb_checkint(L, 2);
    s->bounds_y = lb_checkint(L, 3);
    s->bounds_w = lb_checkint(L, 4);
    s->bounds_h = lb_checkint(L, 5);
  }
  return 0;
}

static int l_sprite_getBounds(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->bounds_x);
  lua_pushinteger(L, s->bounds_y);
  lua_pushinteger(L, s->bounds_w);
  lua_pushinteger(L, s->bounds_h);
  return 4;
}

static int l_sprite_getBoundsRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_createtable(L, 4, 0);
  lua_pushinteger(L, s->bounds_x);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, s->bounds_y);
  lua_rawseti(L, -2, 2);
  lua_pushinteger(L, s->bounds_w);
  lua_rawseti(L, -2, 3);
  lua_pushinteger(L, s->bounds_h);
  lua_rawseti(L, -2, 4);
  return 1;
}

static int l_sprite_setOpaque(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->opaque = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_isOpaque(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->opaque);
  return 1;
}

static int l_sprite_draw(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int x = lb_optint(L, 2, s->x);
  int y = lb_optint(L, 3, s->y);
  
  if (!s->visible || !s->image)
    return 0;
  sprite_draw_at(s, x, y);
  return 0;
}

static int l_sprite_updateSingle(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (s->updates_enabled && s->visible && s->image)
    sprite_draw_at(s, s->x, s->y);
  return 0;
}

static int l_sprite_setBackgroundDrawingCallback(lua_State *L) {
  return 0;
}

static int l_sprite_index(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (!strcmp(key, "x")) {
    lua_pushinteger(L, s->x);
  } else if (!strcmp(key, "y")) {
    lua_pushinteger(L, s->y);
  } else if (!strcmp(key, "width")) {
    lua_pushinteger(L, s->width);
  } else if (!strcmp(key, "height")) {
    lua_pushinteger(L, s->height);
  } else if (!strcmp(key, "z")) {
    lua_pushinteger(L, s->z_index);
  } else if (!strcmp(key, "visible")) {
    lua_pushboolean(L, s->visible);
  } else if (!strcmp(key, "scale")) {
    lua_pushnumber(L, s->scale);
  } else if (!strcmp(key, "rotation")) {
    lua_pushnumber(L, s->rotation);
  } else if (!strcmp(key, "tag")) {
    lua_pushinteger(L, s->tag);
  } else if (!strcmp(key, "image")) {
    if (s->image)
      lua_getiuservalue(L, 1, SPRITE_UV_IMAGE);
    else
      lua_pushnil(L);
  } else if (!strcmp(key, "scale_nn")) {
    lua_pushinteger(L, s->scale_nn);
  } else {
    // Methods only (upvalue 1, from lb_register_type), never the metatable:
    // sprite:__gc() must not reach the finaliser.
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
  }
  return 1;
}

static int l_sprite_newindex(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (!strcmp(key, "x")) {
    s->x = lb_checkint(L, 3);
  } else if (!strcmp(key, "y")) {
    s->y = lb_checkint(L, 3);
  } else if (!strcmp(key, "width")) {
    s->width = lb_checkint(L, 3);
  } else if (!strcmp(key, "height")) {
    s->height = lb_checkint(L, 3);
  } else if (!strcmp(key, "z")) {
    s->z_index = lb_checkint(L, 3);
  } else if (!strcmp(key, "visible")) {
    s->visible = lua_toboolean(L, 3);
  } else if (!strcmp(key, "scale")) {
    s->scale = (float)luaL_checknumber(L, 3);
  } else if (!strcmp(key, "rotation")) {
    s->rotation = (float)luaL_checknumber(L, 3);
  } else if (!strcmp(key, "tag")) {
    s->tag = luaL_checkinteger(L, 3);
  }
  return 0;
}

// Forward declarations for tilemap/text sprite functions (defined after spritesheet)
static int l_sprite_setTilemap(lua_State *L);
static int l_sprite_addWallSprites(lua_State *L);
static int l_sprite_spriteWithText(lua_State *L);

static const luaL_Reg l_sprite_methods[] = {
    {"setImage", l_sprite_setImage},
    {"getImage", l_sprite_getImage},
    {"add", l_sprite_add},
    {"remove", l_sprite_remove},
    {"moveTo", l_sprite_moveTo},
    {"moveBy", l_sprite_moveBy},
    {"getPosition", l_sprite_getPosition},
    {"setZIndex", l_sprite_setZIndex},
    {"getZIndex", l_sprite_getZIndex},
    {"setVisible", l_sprite_setVisible},
    {"isVisible", l_sprite_isVisible},
    {"setCenter", l_sprite_setCenter},
    {"getCenter", l_sprite_getCenter},
    {"getCenterPoint", l_sprite_getCenterPoint},
    {"setSize", l_sprite_setSize},
    {"getSize", l_sprite_getSize},
    {"setScale", l_sprite_setScale},
    {"getScale", l_sprite_getScale},
    {"setScaleNN", l_sprite_setScaleNN},
    {"setTransparentColor", l_sprite_setTransparentColor},
    {"setRotation", l_sprite_setRotation},
    {"getRotation", l_sprite_getRotation},
    {"copy", l_sprite_copy},
    {"setSourceRect", l_sprite_setSourceRect},
    {"clearSourceRect", l_sprite_clearSourceRect},
    {"setUpdatesEnabled", l_sprite_setUpdatesEnabled},
    {"updatesEnabled", l_sprite_updatesEnabled},
    {"setTag", l_sprite_setTag},
    {"getTag", l_sprite_getTag},
    {"setImageDrawMode", l_sprite_setImageDrawMode},
    {"setImageFlip", l_sprite_setImageFlip},
    {"getImageFlip", l_sprite_getImageFlip},
    {"setIgnoresDrawOffset", l_sprite_setIgnoresDrawOffset},
    {"setBounds", l_sprite_setBounds},
    {"getBounds", l_sprite_getBounds},
    {"getBoundsRect", l_sprite_getBoundsRect},
    {"setOpaque", l_sprite_setOpaque},
    {"isOpaque", l_sprite_isOpaque},
    {"setBackgroundDrawingCallback", l_sprite_setBackgroundDrawingCallback},
    {"draw", l_sprite_draw},
    {"update", l_sprite_updateSingle},
    {"setCollisionsEnabled", l_sprite_setCollisionsEnabled},
    {"collisionsEnabled", l_sprite_collisionsEnabled},
    {"setCollideRect", l_sprite_setCollideRect},
    {"getCollideRect", l_sprite_getCollideRect},
    {"getCollideBounds", l_sprite_getCollideBounds},
    {"clearCollideRect", l_sprite_clearCollideRect},
    {"overlappingSprites", l_sprite_overlappingSprites},
    {"allOverlappingSprites", l_sprite_allOverlappingSprites},
    {"setGroups", l_sprite_setGroups},
    {"setCollidesWithGroups", l_sprite_setCollidesWithGroups},
    {"setGroupMask", l_sprite_setGroupMask},
    {"getGroupMask", l_sprite_getGroupMask},
    {"setCollidesWithGroupsMask", l_sprite_setCollidesWithGroupsMask},
    {"getCollidesWithGroupsMask", l_sprite_getCollidesWithGroupsMask},
    {"resetGroupMask", l_sprite_resetGroupMask},
    {"resetCollidesWithGroupsMask", l_sprite_resetCollidesWithGroupsMask},
    {"checkCollisions", l_sprite_checkCollisions},
    {"moveWithCollisions", l_sprite_moveWithCollisions},
    {"collisionResponse", l_sprite_collisionResponse},
    {"setClipRect", l_sprite_setClipRect},
    {"clearClipRect", l_sprite_clearClipRect},
    {"setAlwaysRedraw", l_sprite_setAlwaysRedraw},
    {"getAlwaysRedraw", l_sprite_getAlwaysRedraw},
    {"markDirty", l_sprite_markDirty},
    {"addDirtyRect", l_sprite_addDirtyRect},
    {"setRedrawsOnImageChange", l_sprite_setRedrawsOnImageChange},
    {"setStencilImage", l_sprite_setStencilImage},
    {"clearStencil", l_sprite_clearStencil},
    {"setStencilPattern", l_sprite_setStencilPattern},
    {"alphaCollision", l_sprite_alphaCollision},
    {"setTilemap", l_sprite_setTilemap},
    {NULL, NULL}};

static int l_sprite_addSprite(lua_State *L) {
  return l_sprite_add(L);
}

static int l_sprite_removeSprite(lua_State *L) {
  return l_sprite_remove(L);
}

static int l_sprite_getAllSprites(lua_State *L) {
  lua_createtable(L, s_sprite_count, 0);
  for (int i = 0; i < s_sprite_count; i++) {
    push_sprite(L, s_sprites[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_sprite_spriteCount(lua_State *L) {
  lua_pushinteger(L, s_sprite_count);
  return 1;
}

static int l_sprite_removeAll(lua_State *L) {
  sprite_list_clear(L);
  return 0;
}

static int l_sprite_removeSprites(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  int remove_count = (int)lua_rawlen(L, 1);
  
  for (int r = 1; r <= remove_count; r++) {
    lua_rawgeti(L, 1, r);
    lua_sprite_t *target = check_sprite(L, -1);
    lua_pop(L, 1);
    sprite_list_remove(L, target);
  }
  return 0;
}

// performOnAllSprites(fn): fn(sprite) for each listed sprite. It iterates a
// snapshot (getAllSprites), so fn may add or remove sprites; an error in fn
// propagates to the caller.
static int l_sprite_performOnAllSprites(lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_settop(L, 1);
  l_sprite_getAllSprites(L);  // index 2
  int n = (int)lua_rawlen(L, 2);
  for (int i = 1; i <= n; i++) {
    lua_pushvalue(L, 1);
    lua_rawgeti(L, 2, i);
    lua_call(L, 1, 0);
  }
  return 0;
}

static int l_sprite_setCollisionsEnabled(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collisions_enabled = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_collisionsEnabled(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->collisions_enabled);
  return 1;
}

static int l_sprite_setCollideRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    lua_getfield(L, 2, "w");
    lua_getfield(L, 2, "h");
    s->collide_x = lb_optint_at(L, -4, 2, "field 'x'", 0);
    s->collide_y = lb_optint_at(L, -3, 2, "field 'y'", 0);
    s->collide_w = lb_optint_at(L, -2, 2, "field 'w'", s->width);
    s->collide_h = lb_optint_at(L, -1, 2, "field 'h'", s->height);
    lua_pop(L, 4);
  } else {
    s->collide_x = lb_checkint(L, 2);
    s->collide_y = lb_checkint(L, 3);
    s->collide_w = lb_checkint(L, 4);
    s->collide_h = lb_checkint(L, 5);
  }
  return 0;
}

static int l_sprite_getCollideRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->collide_x);
  lua_pushinteger(L, s->collide_y);
  lua_pushinteger(L, s->collide_w);
  lua_pushinteger(L, s->collide_h);
  return 4;
}

static int l_sprite_getCollideBounds(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int bx = s->x + s->collide_x;
  int by = s->y + s->collide_y;
  lua_pushinteger(L, bx);
  lua_pushinteger(L, by);
  lua_pushinteger(L, s->collide_w);
  lua_pushinteger(L, s->collide_h);
  return 4;
}

static int l_sprite_clearCollideRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collide_x = 0;
  s->collide_y = 0;
  s->collide_w = s->width;
  s->collide_h = s->height;
  return 0;
}

// Returns the effective on-screen pixel dimensions of a sprite.
// Uses frame dimensions if a source rect has been extracted, otherwise the
// full image dimensions, then applies NN or bilinear scaling.
static void sprite_visual_size(const lua_sprite_t *s, int *w, int *h) {
  int bw = s->frame_w > 0 ? s->frame_w : s->width;
  int bh = s->frame_h > 0 ? s->frame_h : s->height;
  if (s->use_nn_scaling && s->scale_nn > 1) {
    *w = bw * s->scale_nn;
    *h = bh * s->scale_nn;
  } else {
    *w = (int)(bw * s->scale);
    *h = (int)(bh * s->scale_y);
  }
}

static bool spritesOverlap(lua_sprite_t *a, lua_sprite_t *b) {
  if (!a->collisions_enabled || !b->collisions_enabled) return false;

  // Use explicit collide rect if set; otherwise derive from visual size
  int aw, ah, bw, bh;
  if (a->collide_w > 0) { aw = a->collide_w; ah = a->collide_h; }
  else                  { sprite_visual_size(a, &aw, &ah); }
  if (b->collide_w > 0) { bw = b->collide_w; bh = b->collide_h; }
  else                  { sprite_visual_size(b, &bw, &bh); }

  int ax = a->x + a->collide_x;
  int ay = a->y + a->collide_y;
  int bx = b->x + b->collide_x;
  int by = b->y + b->collide_y;

  return (ax < bx + bw && ax + aw > bx &&
          ay < by + bh && ay + ah > by);
}

static int l_sprite_overlappingSprites(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *other = s_sprites[i];
    if (other != s && spritesOverlap(s, other)) {
      push_sprite(L, other);
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

static int l_sprite_allOverlappingSprites(lua_State *L) {
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *a = s_sprites[i];
    if (!a->collisions_enabled) continue;
    
    for (int j = i + 1; j < s_sprite_count; j++) {
      lua_sprite_t *b = s_sprites[j];
      if (!b->collisions_enabled) continue;
      
      if (spritesOverlap(a, b)) {
        lua_createtable(L, 2, 0);

        push_sprite(L, a);
        lua_rawseti(L, -2, 1);

        push_sprite(L, b);
        lua_rawseti(L, -2, 2);

        lua_rawseti(L, -2, ++count);
      }
    }
  }
  return 1;
}

static int l_sprite_setGroups(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->group_mask = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_setCollidesWithGroups(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collides_with_mask = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_setGroupMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->group_mask = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_getGroupMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->group_mask);
  return 1;
}

static int l_sprite_setCollidesWithGroupsMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collides_with_mask = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_getCollidesWithGroupsMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->collides_with_mask);
  return 1;
}

static int l_sprite_resetGroupMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->group_mask = 0;
  return 0;
}

static int l_sprite_resetCollidesWithGroupsMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collides_with_mask = 0;
  return 0;
}

static int l_sprite_checkCollisions(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int px, py;
  
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    px = lb_optint_at(L, -2, 2, "field 'x'", 0);
    py = lb_optint_at(L, -1, 2, "field 'y'", 0);
    lua_pop(L, 2);
  } else {
    px = lb_checkint(L, 2);
    py = lb_checkint(L, 3);
  }
  
  int sx = s->x + s->collide_x;
  int sy = s->y + s->collide_y;
  
  bool hit = (px >= sx && px < sx + s->collide_w &&
              py >= sy && py < sy + s->collide_h);
  lua_pushboolean(L, hit);
  return 1;
}

static int l_sprite_querySpritesAtPoint(lua_State *L) {
  int px, py;
  
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "x");
    lua_getfield(L, 1, "y");
    px = lb_optint_at(L, -2, 1, "field 'x'", 0);
    py = lb_optint_at(L, -1, 1, "field 'y'", 0);
    lua_pop(L, 2);
  } else {
    px = lb_checkint(L, 1);
    py = lb_checkint(L, 2);
  }
  
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    int sx = s->x + s->collide_x;
    int sy = s->y + s->collide_y;
    
    if (px >= sx && px < sx + s->collide_w &&
        py >= sy && py < sy + s->collide_h) {
      push_sprite(L, s);
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

static int l_sprite_querySpritesInRect(lua_State *L) {
  int rx, ry, rw, rh;
  
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "x");
    lua_getfield(L, 1, "y");
    lua_getfield(L, 1, "w");
    lua_getfield(L, 1, "h");
    rx = lb_optint_at(L, -4, 1, "field 'x'", 0);
    ry = lb_optint_at(L, -3, 1, "field 'y'", 0);
    rw = lb_optint_at(L, -2, 1, "field 'w'", 320);
    rh = lb_optint_at(L, -1, 1, "field 'h'", 320);
    lua_pop(L, 4);
  } else {
    rx = lb_checkint(L, 1);
    ry = lb_checkint(L, 2);
    rw = lb_checkint(L, 3);
    rh = lb_checkint(L, 4);
  }
  
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    int sx = s->x + s->collide_x;
    int sy = s->y + s->collide_y;
    
    if (sx < rx + rw && sx + s->collide_w > rx &&
        sy < ry + rh && sy + s->collide_h > ry) {
      push_sprite(L, s);
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

// Collision response types matching Playdate SDK:
// "slide"   = slide along the collision surface
// "freeze"  = stop at the collision point
// "overlap" = pass through (report collision but don't resolve)
// "bounce"  = reflect off the collision surface
#define COLLISION_SLIDE   1
#define COLLISION_FREEZE  2
#define COLLISION_OVERLAP 3
#define COLLISION_BOUNCE  4

// Helper: get collision bounds for a sprite at position (px, py)
static void sprite_collide_bounds_at(const lua_sprite_t *s, int px, int py,
                                      int *ox, int *oy, int *ow, int *oh) {
  int cw, ch;
  if (s->collide_w > 0) { cw = s->collide_w; ch = s->collide_h; }
  else                   { sprite_visual_size(s, &cw, &ch); }
  *ox = px + s->collide_x;
  *oy = py + s->collide_y;
  *ow = cw;
  *oh = ch;
}

// Helper: check if sprite `s` at position (sx, sy) overlaps any other sprite
// in the active list, respecting group masks.
static bool sprite_overlaps_any_at(lua_sprite_t *s, int sx, int sy,
                                    lua_sprite_t **hit_out) {
  int ax, ay, aw, ah;
  sprite_collide_bounds_at(s, sx, sy, &ax, &ay, &aw, &ah);

  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *other = s_sprites[i];
    if (other == s || !other->collisions_enabled) continue;
    // Group mask check: if both masks are non-zero, require overlap
    if (s->collides_with_mask && other->group_mask &&
        !(s->collides_with_mask & other->group_mask)) continue;

    int bx, by, bw, bh;
    sprite_collide_bounds_at(other, other->x, other->y, &bx, &by, &bw, &bh);
    if (ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by) {
      if (hit_out) *hit_out = other;
      return true;
    }
  }
  return false;
}

// sprite:moveWithCollisions(goalX, goalY)
// Returns: actualX, actualY, collisions
// Collisions is a table of {sprite, other, type, normal, touch, move}
static int l_sprite_moveWithCollisions(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int goalX, goalY;

  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    goalX = lb_optint_at(L, -2, 2, "field 'x'", s->x);
    goalY = lb_optint_at(L, -1, 2, "field 'y'", s->y);
    lua_pop(L, 2);
  } else {
    goalX = lb_checkint(L, 2);
    goalY = lb_checkint(L, 3);
  }

  if (!s->collisions_enabled) {
    s->x = goalX;
    s->y = goalY;
    lua_pushinteger(L, goalX);
    lua_pushinteger(L, goalY);
    lua_newtable(L);
    return 3;
  }

  int startX = s->x, startY = s->y;
  int dx = goalX - startX;
  int dy = goalY - startY;

  lua_createtable(L, 0, 0); // collisions table
  int coll_count = 0;

  // Try X movement first, then Y (axis-separated resolution)
  int finalX = startX, finalY = startY;

  // --- X axis ---
  if (dx != 0) {
    lua_sprite_t *hit = NULL;
    if (!sprite_overlaps_any_at(s, goalX, startY, &hit)) {
      finalX = goalX;
    } else {
      int step = (dx > 0) ? 1 : -1;
      int best = startX;
      // Simple sweep: step pixel by pixel (capped at 320 iterations max)
      int steps = abs(dx);
      if (steps > FB_WIDTH) steps = FB_WIDTH;
      for (int i = 1; i <= steps; i++) {
        int testX = startX + step * i;
        if (sprite_overlaps_any_at(s, testX, startY, &hit)) {
          // Record collision
          coll_count++;
          lua_createtable(L, 0, 6);

          // sprite (self)
          lua_pushvalue(L, 1);
          lua_setfield(L, -2, "sprite");
          // other
          push_sprite(L, hit);
          lua_setfield(L, -2, "other");
          // type (default: slide)
          lua_pushinteger(L, COLLISION_SLIDE);
          lua_setfield(L, -2, "type");
          // normal
          lua_createtable(L, 0, 2);
          lua_pushinteger(L, dx > 0 ? -1 : 1);
          lua_setfield(L, -2, "x");
          lua_pushinteger(L, 0);
          lua_setfield(L, -2, "y");
          lua_setfield(L, -2, "normal");
          // touch point
          lua_createtable(L, 0, 2);
          lua_pushinteger(L, best);
          lua_setfield(L, -2, "x");
          lua_pushinteger(L, startY);
          lua_setfield(L, -2, "y");
          lua_setfield(L, -2, "touch");

          lua_rawseti(L, -2, coll_count);
          break;
        }
        best = testX;
      }
      finalX = best;
    }
  }

  // --- Y axis ---
  if (dy != 0) {
    lua_sprite_t *hit = NULL;
    if (!sprite_overlaps_any_at(s, finalX, goalY, &hit)) {
      finalY = goalY;
    } else {
      int step = (dy > 0) ? 1 : -1;
      int best = startY;
      int steps = abs(dy);
      if (steps > FB_HEIGHT) steps = FB_HEIGHT;
      for (int i = 1; i <= steps; i++) {
        int testY = startY + step * i;
        if (sprite_overlaps_any_at(s, finalX, testY, &hit)) {
          coll_count++;
          lua_createtable(L, 0, 6);

          lua_pushvalue(L, 1);
          lua_setfield(L, -2, "sprite");
          push_sprite(L, hit);
          lua_setfield(L, -2, "other");
          lua_pushinteger(L, COLLISION_SLIDE);
          lua_setfield(L, -2, "type");
          lua_createtable(L, 0, 2);
          lua_pushinteger(L, 0);
          lua_setfield(L, -2, "x");
          lua_pushinteger(L, dy > 0 ? -1 : 1);
          lua_setfield(L, -2, "y");
          lua_setfield(L, -2, "normal");
          lua_createtable(L, 0, 2);
          lua_pushinteger(L, finalX);
          lua_setfield(L, -2, "x");
          lua_pushinteger(L, best);
          lua_setfield(L, -2, "y");
          lua_setfield(L, -2, "touch");

          lua_rawseti(L, -2, coll_count);
          break;
        }
        best = testY;
      }
      finalY = best;
    }
  }

  s->x = finalX;
  s->y = finalY;

  lua_pushinteger(L, finalX);
  lua_pushinteger(L, finalY);
  lua_pushvalue(L, -3); // push the collisions table
  lua_remove(L, -4);    // remove the original table position
  return 3;
}

// sprite:collisionResponse(other) — returns response type string
// Default is "slide". Apps can override this method in Lua.
static int l_sprite_collisionResponse(lua_State *L) {
  (void)check_sprite(L, 1); // self
  // Default response is "slide"
  lua_pushstring(L, "slide");
  return 1;
}

static int l_sprite_setClipRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    lua_getfield(L, 2, "w");
    lua_getfield(L, 2, "h");
    s->clip_x = lb_optint_at(L, -4, 2, "field 'x'", 0);
    s->clip_y = lb_optint_at(L, -3, 2, "field 'y'", 0);
    s->clip_w = lb_optint_at(L, -2, 2, "field 'w'", s->width);
    s->clip_h = lb_optint_at(L, -1, 2, "field 'h'", s->height);
    lua_pop(L, 4);
    s->has_clip = true;
  } else if (lua_gettop(L) >= 5) {
    s->clip_x = lb_checkint(L, 2);
    s->clip_y = lb_checkint(L, 3);
    s->clip_w = lb_checkint(L, 4);
    s->clip_h = lb_checkint(L, 5);
    s->has_clip = true;
  } else {
    s->has_clip = false;
  }
  return 0;
}

static int l_sprite_clearClipRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->has_clip = false;
  return 0;
}

static int l_sprite_setAlwaysRedraw(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->always_redraw = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_getAlwaysRedraw(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->always_redraw);
  return 1;
}

static int l_sprite_markDirty(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->dirty = true;
  return 0;
}

static int l_sprite_addDirtyRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  (void)s;
  return 0;
}

static int l_sprite_setRedrawsOnImageChange(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->redraws_on_image_change = lua_toboolean(L, 2);
  return 0;
}

static bool sprite_line_line_intersect(int x1, int y1, int x2, int y2,
                                       int x3, int y3, int x4, int y4) {
  int denom = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
  if (denom == 0) return false;
  
  float t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / (float)denom;
  float u = -((x1 - x2) * (y1 - y3) - (y1 - y2) * (x1 - x3)) / (float)denom;
  
  return (t >= 0 && t <= 1 && u >= 0 && u <= 1);
}

static bool sprite_line_intersect(int x1, int y1, int x2, int y2, 
                                  int rx, int ry, int rw, int rh) {
  if (rw <= 0 || rh <= 0) return false;
  
  int left = rx;
  int right = rx + rw;
  int top = ry;
  int bottom = ry + rh;
  
  if ((x1 >= left && x1 <= right && y1 >= top && y1 <= bottom) ||
      (x2 >= left && x2 <= right && y2 >= top && y2 <= bottom)) {
    return true;
  }
  
  if (sprite_line_line_intersect(x1, y1, x2, y2, left, top, right, top)) return true;
  if (sprite_line_line_intersect(x1, y1, x2, y2, left, bottom, right, bottom)) return true;
  if (sprite_line_line_intersect(x1, y1, x2, y2, left, top, left, bottom)) return true;
  if (sprite_line_line_intersect(x1, y1, x2, y2, right, top, right, bottom)) return true;
  
  return false;
}

static int l_sprite_querySpritesAlongLine(lua_State *L) {
  int x1, y1, x2, y2;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "x1"); x1 = lb_checkint_at(L, -1, 1, "field 'x1'"); lua_pop(L, 1);
    lua_getfield(L, 1, "y1"); y1 = lb_checkint_at(L, -1, 1, "field 'y1'"); lua_pop(L, 1);
    lua_getfield(L, 1, "x2"); x2 = lb_checkint_at(L, -1, 1, "field 'x2'"); lua_pop(L, 1);
    lua_getfield(L, 1, "y2"); y2 = lb_checkint_at(L, -1, 1, "field 'y2'"); lua_pop(L, 1);
  } else {
    x1 = lb_checkint(L, 1);
    y1 = lb_checkint(L, 2);
    x2 = lb_checkint(L, 3);
    y2 = lb_checkint(L, 4);
  }
  
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    int sx = s->x;
    int sy = s->y;
    int sw = s->width > 0 ? s->width : s->collide_w;
    int sh = s->height > 0 ? s->height : s->collide_h;
    
    if (sprite_line_intersect(x1, y1, x2, y2, sx, sy, sw, sh)) {
      push_sprite(L, s);
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

static bool sprite_line_rect_intersection(int x1, int y1, int x2, int y2,
                                           int rx, int ry, int rw, int rh,
                                           int *out_x, int *out_y) {
  if (rw <= 0 || rh <= 0) return false;
  
  int left = rx;
  int right = rx + rw;
  int top = ry;
  int bottom = ry + rh;
  
  int ix = 0, iy = 0;
  bool found = false;
  float min_dist = 1e9f;
  
  int edges[4][4] = {
    {left, top, right, top},
    {left, bottom, right, bottom},
    {left, top, left, bottom},
    {right, top, right, bottom}
  };
  
  for (int i = 0; i < 4; i++) {
    int ex1 = edges[i][0], ey1 = edges[i][1], ex2 = edges[i][2], ey2 = edges[i][3];
    int denom = (x1 - x2) * (ey1 - ey2) - (y1 - y2) * (ex1 - ex2);
    if (denom == 0) continue;
    float t = ((x1 - ex1) * (ey1 - ey2) - (y1 - ey1) * (ex1 - ex2)) / (float)denom;
    float u = -((x1 - x2) * (y1 - ey1) - (y1 - y2) * (x1 - ex1)) / (float)denom;
    if (t >= 0 && t <= 1 && u >= 0 && u <= 1) {
      int px = (int)(x1 + t * (x2 - x1));
      int py = (int)(y1 + t * (y2 - y1));
      float d = (px - x1) * (px - x1) + (py - y1) * (py - y1);
      if (d < min_dist) { min_dist = d; ix = px; iy = py; found = true; }
    }
  }
  
  if (found) {
    *out_x = ix;
    *out_y = iy;
  }
  return found;
}

static int l_sprite_querySpriteInfoAlongLine(lua_State *L) {
  int x1, y1, x2, y2;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "x1"); x1 = lb_checkint_at(L, -1, 1, "field 'x1'"); lua_pop(L, 1);
    lua_getfield(L, 1, "y1"); y1 = lb_checkint_at(L, -1, 1, "field 'y1'"); lua_pop(L, 1);
    lua_getfield(L, 1, "x2"); x2 = lb_checkint_at(L, -1, 1, "field 'x2'"); lua_pop(L, 1);
    lua_getfield(L, 1, "y2"); y2 = lb_checkint_at(L, -1, 1, "field 'y2'"); lua_pop(L, 1);
  } else {
    x1 = lb_checkint(L, 1);
    y1 = lb_checkint(L, 2);
    x2 = lb_checkint(L, 3);
    y2 = lb_checkint(L, 4);
  }
  
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    int sx = s->x;
    int sy = s->y;
    int sw = s->width > 0 ? s->width : s->collide_w;
    int sh = s->height > 0 ? s->height : s->collide_h;
    
    int ix, iy;
    if (sprite_line_rect_intersection(x1, y1, x2, y2, sx, sy, sw, sh, &ix, &iy)) {
      lua_createtable(L, 0, 0);
      push_sprite(L, s);
      lua_setfield(L, -2, "sprite");
      lua_pushinteger(L, ix);
      lua_setfield(L, -2, "x");
      lua_pushinteger(L, iy);
      lua_setfield(L, -2, "y");
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

// ── Stencil Methods ──────────────────────────────────────────────────────────

static int l_sprite_setStencilImage(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->stencil = check_image(L, 2);
  anchor_set(L, 1, SPRITE_UV_STENCIL, 2);
  // arg 3 (tile) is ignored — reserved for future use
  return 0;
}

static int l_sprite_clearStencil(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->stencil = NULL;
  anchor_set(L, 1, SPRITE_UV_STENCIL, 0);
  s->has_stencil_pattern = false;
  return 0;
}

// 8x8 ordered (Bayer) dither matrix
static const uint8_t s_bayer8x8[8][8] = {
  { 0,  32,  8, 40,  2, 34, 10, 42},
  {48,  16, 56, 24, 50, 18, 58, 26},
  {12,  44,  4, 36, 14, 46,  6, 38},
  {60,  28, 52, 20, 62, 30, 54, 22},
  { 3,  35, 11, 43,  1, 33,  9, 41},
  {51,  19, 59, 27, 49, 17, 57, 25},
  {15,  47,  7, 39, 13, 45,  5, 37},
  {63,  31, 55, 23, 61, 29, 53, 21}
};

static int l_sprite_setStencilPattern(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  float level = (float)luaL_checknumber(L, 2);
  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;
  // ditherType (arg 3) is ignored for now

  int threshold = (int)(level * 64.0f);
  for (int row = 0; row < 8; row++) {
    uint8_t bits = 0;
    for (int col = 0; col < 8; col++) {
      if ((int)s_bayer8x8[row][col] < threshold)
        bits |= (1 << col);
    }
    s->stencil_pattern[row] = bits;
  }
  s->has_stencil_pattern = true;
  return 0;
}

// Global graphics.setStencilPattern({row1, row2, ..., row8})
static int l_graphics_setStencilPattern(lua_State *L) {
  if (lua_isnil(L, 1)) {
    s_has_global_stencil = false;
    return 0;
  }
  luaL_checktype(L, 1, LUA_TTABLE);
  for (int i = 0; i < 8; i++) {
    lua_rawgeti(L, 1, i + 1);
    s_global_stencil[i] = (uint8_t)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
  }
  s_has_global_stencil = true;
  return 0;
}

// ── Alpha (Pixel-Perfect) Collision ─────────────────────────────────────────

static int l_sprite_alphaCollision(lua_State *L) {
  lua_sprite_t *a = check_sprite(L, 1);
  lua_sprite_t *b = check_sprite(L, 2);

  // Pixel data with its real dimensions; a sprite without pixels uses its
  // bounds for the AABB fallback. Never the sprite size as a stride.
  int a_w, a_h, b_w, b_h;
  const uint16_t *a_data = sprite_pixels(a, &a_w, &a_h);
  const uint16_t *b_data = sprite_pixels(b, &b_w, &b_h);
  if (!a_data) { a_w = a->width; a_h = a->height; }
  if (!b_data) { b_w = b->width; b_h = b->height; }

  // If either sprite has no image data, fall back to AABB overlap
  if (!a_data || !b_data || a_w <= 0 || a_h <= 0 || b_w <= 0 || b_h <= 0) {
    // Simple AABB overlap check
    bool overlap = !(a->x + a_w <= b->x || b->x + b_w <= a->x ||
                     a->y + a_h <= b->y || b->y + b_h <= a->y);
    lua_pushboolean(L, overlap);
    return 1;
  }

  // If either sprite is scaled or rotated, fall back to AABB
  if (a->rotation != 0.0f || b->rotation != 0.0f ||
      (a->scale != 1.0f && !a->use_nn_scaling) ||
      (b->scale != 1.0f && !b->use_nn_scaling) ||
      a->scale_y != 1.0f || b->scale_y != 1.0f) {
    bool overlap = !(a->x + a_w <= b->x || b->x + b_w <= a->x ||
                     a->y + a_h <= b->y || b->y + b_h <= a->y);
    lua_pushboolean(L, overlap);
    return 1;
  }

  // Compute AABB overlap region in screen coordinates
  int overlap_left   = (a->x > b->x) ? a->x : b->x;
  int overlap_top    = (a->y > b->y) ? a->y : b->y;
  int overlap_right  = ((a->x + a_w) < (b->x + b_w)) ? (a->x + a_w) : (b->x + b_w);
  int overlap_bottom = ((a->y + a_h) < (b->y + b_h)) ? (a->y + a_h) : (b->y + b_h);

  if (overlap_left >= overlap_right || overlap_top >= overlap_bottom) {
    lua_pushboolean(L, false);
    return 1;
  }

  uint16_t a_trans = a->transparent_color ? a->transparent_color : display_get_transparent_color();
  uint16_t b_trans = b->transparent_color ? b->transparent_color : display_get_transparent_color();

  for (int sy = overlap_top; sy < overlap_bottom; sy++) {
    for (int sx = overlap_left; sx < overlap_right; sx++) {
      // Map screen coord to sprite A's local coord
      int a_lx = sx - a->x;
      int a_ly = sy - a->y;
      if (a->flip_x) a_lx = a_w - 1 - a_lx;
      if (a->flip_y) a_ly = a_h - 1 - a_ly;

      // Map screen coord to sprite B's local coord
      int b_lx = sx - b->x;
      int b_ly = sy - b->y;
      if (b->flip_x) b_lx = b_w - 1 - b_lx;
      if (b->flip_y) b_ly = b_h - 1 - b_ly;

      uint16_t a_px = a_data[a_ly * a_w + a_lx];
      uint16_t b_px = b_data[b_ly * b_w + b_lx];

      bool a_opaque = (a_trans == 0) || (a_px != a_trans);
      bool b_opaque = (b_trans == 0) || (b_px != b_trans);

      if (a_opaque && b_opaque) {
        lua_pushboolean(L, true);
        return 1;
      }
    }
  }

  lua_pushboolean(L, false);
  return 1;
}

// ── Clip Rects In Range (class-level) ───────────────────────────────────────

static int l_sprite_setClipRectsInRange(lua_State *L) {
  int clip_x, clip_y, clip_w, clip_h, startz, endz;

  if (lua_istable(L, 1)) {
    // Table variant: ({x, y, w, h}, startz, endz)
    lua_getfield(L, 1, "x"); clip_x = lb_checkint_at(L, -1, 1, "field 'x'"); lua_pop(L, 1);
    lua_getfield(L, 1, "y"); clip_y = lb_checkint_at(L, -1, 1, "field 'y'"); lua_pop(L, 1);
    lua_getfield(L, 1, "w"); clip_w = lb_checkint_at(L, -1, 1, "field 'w'"); lua_pop(L, 1);
    lua_getfield(L, 1, "h"); clip_h = lb_checkint_at(L, -1, 1, "field 'h'"); lua_pop(L, 1);
    startz = lb_checkint(L, 2);
    endz   = lb_checkint(L, 3);
  } else {
    // Numeric variant: (x, y, w, h, startz, endz)
    clip_x = lb_checkint(L, 1);
    clip_y = lb_checkint(L, 2);
    clip_w = lb_checkint(L, 3);
    clip_h = lb_checkint(L, 4);
    startz = lb_checkint(L, 5);
    endz   = lb_checkint(L, 6);
  }

  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    if (s->z_index >= startz && s->z_index <= endz) {
      s->clip_x = clip_x;
      s->clip_y = clip_y;
      s->clip_w = clip_w;
      s->clip_h = clip_h;
      s->has_clip = true;
    }
  }
  return 0;
}

static int l_sprite_clearClipRectsInRange(lua_State *L) {
  int startz = lb_checkint(L, 1);
  int endz   = lb_checkint(L, 2);

  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    if (s->z_index >= startz && s->z_index <= endz) {
      s->has_clip = false;
    }
  }
  return 0;
}

// ── Empty Collision Sprite (class-level) ────────────────────────────────────

static int l_sprite_addEmptyCollisionSprite(lua_State *L) {
  int x, y, w, h;

  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "x"); x = lb_checkint_at(L, -1, 1, "field 'x'"); lua_pop(L, 1);
    lua_getfield(L, 1, "y"); y = lb_checkint_at(L, -1, 1, "field 'y'"); lua_pop(L, 1);
    lua_getfield(L, 1, "w"); w = lb_checkint_at(L, -1, 1, "field 'w'"); lua_pop(L, 1);
    lua_getfield(L, 1, "h"); h = lb_checkint_at(L, -1, 1, "field 'h'"); lua_pop(L, 1);
  } else {
    x = lb_checkint(L, 1);
    y = lb_checkint(L, 2);
    w = lb_checkint(L, 3);
    h = lb_checkint(L, 4);
  }

  if (s_sprite_count >= MAX_SPRITES)
    return luaL_error(L, "max sprites reached");

  lua_sprite_t *s = new_sprite_ud(L);
  s->x = x;
  s->y = y;
  s->width = w;
  s->height = h;
  s->scale = 1.0f;
  s->scale_y = 1.0f;
  s->scale_nn = 1;
  s->visible = false;
  s->collisions_enabled = true;
  s->collide_x = 0;
  s->collide_y = 0;
  s->collide_w = w;
  s->collide_h = h;
  s->updates_enabled = true;
  s->redraws_on_image_change = true;
  s->opaque = true;

  luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
  sprite_list_add(L, -1);  // room was checked above

  return 1;  // return the sprite userdata
}

static const luaL_Reg l_sprite_lib[] = {
    {"new", l_sprite_new},
    {"addSprite", l_sprite_addSprite},
    {"removeSprite", l_sprite_removeSprite},
    {"update", l_sprite_update},
    {"getAllSprites", l_sprite_getAllSprites},
    {"spriteCount", l_sprite_spriteCount},
    {"removeAll", l_sprite_removeAll},
    {"removeSprites", l_sprite_removeSprites},
    {"performOnAllSprites", l_sprite_performOnAllSprites},
    {"querySpritesAtPoint", l_sprite_querySpritesAtPoint},
    {"querySpritesInRect", l_sprite_querySpritesInRect},
    {"querySpritesAlongLine", l_sprite_querySpritesAlongLine},
    {"querySpriteInfoAlongLine", l_sprite_querySpriteInfoAlongLine},
    {"setClipRectsInRange", l_sprite_setClipRectsInRange},
    {"clearClipRectsInRange", l_sprite_clearClipRectsInRange},
    {"addEmptyCollisionSprite", l_sprite_addEmptyCollisionSprite},
    {"addWallSprites", l_sprite_addWallSprites},
    {"spriteWithText", l_sprite_spriteWithText},
    {NULL, NULL}};

// ── Spritesheet System ─────────────────────────────────────────────────────────

#define GRAPHICS_SPRITESHEET_MT "picocalc.graphics.spritesheet"
#define MAX_FRAMES 64
#define SPRITESHEET_UV_IMAGE 1  // user value: the image behind ->image

typedef struct {
  lua_image_t *image;
  int frame_count;
  int frame_x[MAX_FRAMES];
  int frame_y[MAX_FRAMES];
  int frame_w[MAX_FRAMES];
  int frame_h[MAX_FRAMES];
  bool destroyed;  // set by __gc
} lua_spritesheet_t;

static lua_spritesheet_t *check_spritesheet(lua_State *L, int idx) {
  lua_spritesheet_t *ss =
      (lua_spritesheet_t *)luaL_checkudata(L, idx, GRAPHICS_SPRITESHEET_MT);
  if (ss->destroyed)
    luaL_error(L, "attempt to use a destroyed spritesheet");
  return ss;
}

static int l_spritesheet_gc(lua_State *L) {
  lua_spritesheet_t *ss =
      (lua_spritesheet_t *)luaL_checkudata(L, 1, GRAPHICS_SPRITESHEET_MT);
  ss->image = NULL;
  ss->destroyed = true;
  return 0;
}

static int l_spritesheet_new(lua_State *L) {
  lua_image_t *img = NULL;
  if (lua_isuserdata(L, 1)) {
    img = check_image(L, 1);
  }
  
  lua_spritesheet_t *ss = (lua_spritesheet_t *)lua_newuserdatauv(L, sizeof(lua_spritesheet_t), 1);
  ss->image = img;
  ss->frame_count = 0;
  ss->destroyed = false;
  if (img) anchor_set(L, -1, SPRITESHEET_UV_IMAGE, 1);
  
  luaL_setmetatable(L, GRAPHICS_SPRITESHEET_MT);
  return 1;
}

static int l_spritesheet_newGrid(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int cols = lb_checkint(L, 2);
  int rows = lb_checkint(L, 3);
  int frame_w = lb_checkint(L, 4);
  int frame_h = lb_checkint(L, 5);
  
  lua_spritesheet_t *ss = (lua_spritesheet_t *)lua_newuserdatauv(L, sizeof(lua_spritesheet_t), 1);
  ss->image = img;
  ss->frame_count = 0;
  ss->destroyed = false;
  anchor_set(L, -1, SPRITESHEET_UV_IMAGE, 1);
  
  int x = 0, y = 0;
  for (int r = 0; r < rows && ss->frame_count < MAX_FRAMES; r++) {
    for (int c = 0; c < cols && ss->frame_count < MAX_FRAMES; c++) {
      ss->frame_x[ss->frame_count] = x;
      ss->frame_y[ss->frame_count] = y;
      ss->frame_w[ss->frame_count] = frame_w;
      ss->frame_h[ss->frame_count] = frame_h;
      ss->frame_count++;
      x += frame_w;
    }
    x = 0;
    y += frame_h;
  }
  
  luaL_setmetatable(L, GRAPHICS_SPRITESHEET_MT);
  return 1;
}

static int l_spritesheet_addFrame(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  if (ss->frame_count >= MAX_FRAMES)
    return luaL_error(L, "max frames reached");
  
  int idx = ss->frame_count;
  ss->frame_x[idx] = lb_checkint(L, 2);
  ss->frame_y[idx] = lb_checkint(L, 3);
  ss->frame_w[idx] = lb_checkint(L, 4);
  ss->frame_h[idx] = lb_checkint(L, 5);
  ss->frame_count++;
  
  lua_pushinteger(L, idx);
  return 1;
}

static int l_spritesheet_getFrameCount(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  lua_pushinteger(L, ss->frame_count);
  return 1;
}

static int l_spritesheet_getFrame(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  int idx = lb_checkint(L, 2);
  if (idx < 0 || idx >= ss->frame_count)
    return 0;
  
  lua_createtable(L, 4, 0);
  lua_pushinteger(L, ss->frame_x[idx]);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, ss->frame_y[idx]);
  lua_rawseti(L, -2, 2);
  lua_pushinteger(L, ss->frame_w[idx]);
  lua_rawseti(L, -2, 3);
  lua_pushinteger(L, ss->frame_h[idx]);
  lua_rawseti(L, -2, 4);
  return 1;
}

static int l_spritesheet_getImage(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  if (!ss->image) return 0;
  lua_getiuservalue(L, 1, SPRITESHEET_UV_IMAGE);
  return 1;
}

static int l_spritesheet_drawFrame(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  int frame_idx = lb_checkint(L, 2);
  int x = lb_checkint(L, 3);
  int y = lb_checkint(L, 4);
  
  if (!ss->image || frame_idx < 0 || frame_idx >= ss->frame_count)
    return 0;
  // The image is anchored, but a finaliser that runs after the image's own
  // can still get here with its pixels freed.
  if (!ss->image->data)
    return luaL_error(L, "attempt to use a freed image");
  
  bool flip = lua_toboolean(L, 5);
  
  display_draw_image_partial(x, y, ss->image->w, ss->image->h,
                            ss->image->data,
                            ss->frame_x[frame_idx], ss->frame_y[frame_idx],
                            ss->frame_w[frame_idx], ss->frame_h[frame_idx],
                            flip, false, 0);
  return 0;
}

static const luaL_Reg l_spritesheet_methods[] = {
    {"addFrame", l_spritesheet_addFrame},
    {"getFrameCount", l_spritesheet_getFrameCount},
    {"getFrame", l_spritesheet_getFrame},
    {"getImage", l_spritesheet_getImage},
    {"drawFrame", l_spritesheet_drawFrame},
    {NULL, NULL}};

static const luaL_Reg l_spritesheet_lib[] = {
    {"new", l_spritesheet_new},
    {"newGrid", l_spritesheet_newGrid},
    {NULL, NULL}};

// ── Tilemap Lua API ───────────────────────────────────────────────────────────

// tilemap.new(image, tileWidth, tileHeight)
static int l_tilemap_new(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int tw = lb_checkint(L, 2);
  int th = lb_checkint(L, 3);

  if (tw <= 0 || th <= 0) return luaL_error(L, "tile size must be positive");

  lua_tilemap_t *tm = (lua_tilemap_t *)lua_newuserdatauv(L, sizeof(lua_tilemap_t), 1);
  tm->tileset = img;
  anchor_set(L, -1, TILEMAP_UV_TILESET, 1);
  tm->tile_w = tw;
  tm->tile_h = th;
  tm->tiles_per_row = img->w / tw;
  tm->map_w = 0;
  tm->map_h = 0;
  tm->tiles = NULL;
  tm->destroyed = false;

  luaL_setmetatable(L, GRAPHICS_TILEMAP_MT);
  return 1;
}

// tilemap:setSize(width, height) — allocate the tile grid
static int l_tilemap_setSize(lua_State *L) {
  lua_tilemap_t *tm = check_tilemap(L, 1);
  int w = lb_checkint(L, 2);
  int h = lb_checkint(L, 3);
  if (w <= 0 || h <= 0 || w > TILEMAP_MAX_WIDTH || h > TILEMAP_MAX_HEIGHT)
    return luaL_error(L, "tilemap size out of range (max %dx%d)", TILEMAP_MAX_WIDTH, TILEMAP_MAX_HEIGHT);

  if (tm->tiles) umm_free(tm->tiles);
  size_t sz = (size_t)w * h * sizeof(uint16_t);
  tm->tiles = (uint16_t *)umm_malloc(sz);
  if (!tm->tiles) return luaL_error(L, "out of memory for tilemap");
  memset(tm->tiles, 0, sz);
  tm->map_w = w;
  tm->map_h = h;
  return 0;
}

// tilemap:setTileAtPosition(x, y, tileIndex) — 1-based tile index, 0=empty
static int l_tilemap_setTile(lua_State *L) {
  lua_tilemap_t *tm = check_tilemap(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  int tile = luaL_checkinteger(L, 4);

  if (!tm->tiles || x < 0 || x >= tm->map_w || y < 0 || y >= tm->map_h)
    return 0;
  tm->tiles[y * tm->map_w + x] = (uint16_t)tile;
  return 0;
}

// tilemap:getTileAtPosition(x, y) → tileIndex
static int l_tilemap_getTile(lua_State *L) {
  lua_tilemap_t *tm = check_tilemap(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);

  if (!tm->tiles || x < 0 || x >= tm->map_w || y < 0 || y >= tm->map_h) {
    lua_pushinteger(L, 0);
    return 1;
  }
  lua_pushinteger(L, tm->tiles[y * tm->map_w + x]);
  return 1;
}

// tilemap:getSize() → width, height (in tiles)
static int l_tilemap_getSize(lua_State *L) {
  lua_tilemap_t *tm = check_tilemap(L, 1);
  lua_pushinteger(L, tm->map_w);
  lua_pushinteger(L, tm->map_h);
  return 2;
}

// tilemap:getTileSize() → tileWidth, tileHeight
static int l_tilemap_getTileSize(lua_State *L) {
  lua_tilemap_t *tm = check_tilemap(L, 1);
  lua_pushinteger(L, tm->tile_w);
  lua_pushinteger(L, tm->tile_h);
  return 2;
}

// tilemap:getPixelSize() → pixelWidth, pixelHeight
static int l_tilemap_getPixelSize(lua_State *L) {
  lua_tilemap_t *tm = check_tilemap(L, 1);
  lua_pushinteger(L, tm->map_w * tm->tile_w);
  lua_pushinteger(L, tm->map_h * tm->tile_h);
  return 2;
}

// tilemap:draw(scrollX, scrollY)
static int l_tilemap_draw(lua_State *L) {
  lua_tilemap_t *tm = check_tilemap(L, 1);
  int sx = lb_optint(L, 2, 0);
  int sy = lb_optint(L, 3, 0);
  tilemap_draw(tm, sx, sy);
  return 0;
}

static int l_tilemap_gc(lua_State *L) {
  lua_tilemap_t *tm = (lua_tilemap_t *)luaL_checkudata(L, 1, GRAPHICS_TILEMAP_MT);
  if (tm->tiles) {
    umm_free(tm->tiles);
    tm->tiles = NULL;
  }
  tm->tileset = NULL;
  tm->destroyed = true;
  return 0;
}

static const luaL_Reg l_tilemap_methods[] = {
    {"setSize", l_tilemap_setSize},
    {"setTileAtPosition", l_tilemap_setTile},
    {"getTileAtPosition", l_tilemap_getTile},
    {"getSize", l_tilemap_getSize},
    {"getTileSize", l_tilemap_getTileSize},
    {"getPixelSize", l_tilemap_getPixelSize},
    {"draw", l_tilemap_draw},
    {NULL, NULL}};

static const luaL_Reg l_tilemap_lib[] = {
    {"new", l_tilemap_new},
    {NULL, NULL}};

// ── sprite:setTilemap(tilemap) ──────────────────────────────────────────────
static int l_sprite_setTilemap(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (lua_isnil(L, 2)) {
    s->tilemap = NULL;
    anchor_set(L, 1, SPRITE_UV_TILEMAP, 0);
    return 0;
  }
  lua_tilemap_t *tm = check_tilemap(L, 2);
  s->tilemap = tm;
  anchor_set(L, 1, SPRITE_UV_TILEMAP, 2);
  s->width = tm->map_w * tm->tile_w;
  s->height = tm->map_h * tm->tile_h;
  return 0;
}

// sprite.addWallSprites(tilemap, wallIDs, [xOffset], [yOffset])
// wallIDs is a table of tile indices that should have collision
static int l_sprite_addWallSprites(lua_State *L) {
  lua_tilemap_t *tm = check_tilemap(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  int x_off = (int)lb_optint(L, 3, 0);
  int y_off = (int)lb_optint(L, 4, 0);

  if (!tm->tiles) return 0;

  int count = 0;
  for (int row = 0; row < tm->map_h; row++) {
    for (int col = 0; col < tm->map_w; col++) {
      uint16_t tile = tm->tiles[row * tm->map_w + col];
      if (tile == 0) continue;

      // Check if this tile is in the wallIDs table
      bool is_wall = false;
      int tlen = (int)lua_rawlen(L, 2);
      for (int i = 1; i <= tlen; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_tointeger(L, -1) == tile) is_wall = true;
        lua_pop(L, 1);
        if (is_wall) break;
      }

      if (is_wall && s_sprite_count < MAX_SPRITES) {
        // Create an invisible collision sprite at this tile position
        lua_sprite_t *ws = new_sprite_ud(L);
        ws->x = col * tm->tile_w + x_off;
        ws->y = row * tm->tile_h + y_off;
        ws->width = tm->tile_w;
        ws->height = tm->tile_h;
        ws->scale = 1.0f;
        ws->scale_y = 1.0f;
        ws->visible = false;
        ws->updates_enabled = false;
        ws->collisions_enabled = true;
        ws->collide_w = tm->tile_w;
        ws->collide_h = tm->tile_h;
        ws->group_mask = 1;
        ws->collides_with_mask = 0xFFFF;
        luaL_setmetatable(L, GRAPHICS_SPRITE_MT);

        // Anchor in the display list, then pop: up to MAX_SPRITES walls,
        // and a C function is only guaranteed LUA_MINSTACK (20) slots.
        sprite_list_add(L, -1);  // room was checked above
        lua_pop(L, 1);
        count++;
      }
    }
  }

  lua_pushinteger(L, count);
  return 1;
}

// Defined with the other text renderers below; used by spriteWithText here.
static uint16_t *render_text_image(lua_State *L, const char *text, int w, int h,
                                   uint16_t bg, int font_idx);

// sprite.spriteWithText(text, maxWidth, maxHeight, [bgColor], [font])
static int l_sprite_spriteWithText(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  int max_w = lb_checkint(L, 2);
  int max_h = lb_checkint(L, 3);
  uint16_t bg = (lua_gettop(L) >= 4 && !lua_isnil(L, 4))
                    ? l_checkcolor(L, 4)
                    : s_graphics_bg_color;

  uint16_t *pixels = render_text_image(L, text, max_w, max_h, bg, 5);
  if (!pixels) {
    lua_pushnil(L);
    return 1;
  }

  // Create image userdata
  lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  img->w = max_w;
  img->h = max_h;
  img->data = pixels;
  img->transparent_color = 0;
  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);

  int img_idx = lua_gettop(L);

  // Create sprite with this image
  lua_sprite_t *s = new_sprite_ud(L);
  s->image = img;
  anchor_set(L, -1, SPRITE_UV_IMAGE, img_idx);
  s->width = max_w;
  s->height = max_h;
  s->scale = 1.0f;
  s->scale_y = 1.0f;
  s->visible = true;
  s->updates_enabled = true;
  s->opaque = true;
  s->redraws_on_image_change = true;
  luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
  return 1;  // the sprite (it anchors the image below it)
}

// Forward declarations for text rendering functions (defined in font section)
static int l_graphics_drawText(lua_State *L);
static int l_graphics_drawTextAligned(lua_State *L);
static int l_graphics_drawTextInRect(lua_State *L);
static int l_graphics_getTextSize(lua_State *L);
static int l_graphics_getTextSizeForMaxWidth(lua_State *L);
static int l_graphics_imageWithText(lua_State *L);

static const luaL_Reg l_graphics_lib[] = {
    {"setColor", l_graphics_setColor},
    {"setBackgroundColor", l_graphics_setBackgroundColor},
    {"setTransparentColor", l_graphics_setTransparentColor},
    {"getTransparentColor", l_graphics_getTransparentColor},
    {"clear", l_graphics_clear},
    {"drawGrid", l_graphics_drawGrid},
    {"fillBorderedRect", l_graphics_fillBorderedRect},
    {"drawPlayfield", l_graphics_drawPlayfield},
    {"updateDrawParticles", l_graphics_updateDrawParticles},
    {"draw3DWireframe", l_graphics_draw3DWireframe},
    {"setStencilPattern", l_graphics_setStencilPattern},
    {"drawText", l_graphics_drawText},
    {"drawTextAligned", l_graphics_drawTextAligned},
    {"drawTextInRect", l_graphics_drawTextInRect},
    {"getTextSize", l_graphics_getTextSize},
    {"getTextSizeForMaxWidth", l_graphics_getTextSizeForMaxWidth},
    {"imageWithText", l_graphics_imageWithText},
    {NULL, NULL}};

// ── Animation System ─────────────────────────────────────────────────────────

#define GRAPHICS_ANIMATION_LOOP_MT "picocalc.graphics.animation.loop"
#define MAX_ANIMATION_LOOP_FRAMES 32

typedef struct {
  lua_image_t *frames[MAX_ANIMATION_LOOP_FRAMES];
  int frame_count;
  int current_frame;
  uint32_t interval_ms;
  uint32_t last_update_ms;
  bool looping;
  bool valid;
  bool destroyed;  // set by __gc
} lua_animation_loop_t;

static lua_animation_loop_t *check_animation_loop(lua_State *L, int idx) {
  lua_animation_loop_t *loop = (lua_animation_loop_t *)luaL_checkudata(
      L, idx, GRAPHICS_ANIMATION_LOOP_MT);
  if (loop->destroyed)
    luaL_error(L, "attempt to use a destroyed animation loop");
  return loop;
}

// User value 1 of a loop: a private table {[i] = image} holding the images
// behind frames[i-1] (a copy, so the caller may reuse or clear its table).
#define LOOP_UV_FRAMES 1

// Take loop frames from the table at tbl_idx (anything but an image is an
// empty frame, never cast) and anchor them in the loop at loop_idx.
static void loop_set_frames(lua_State *L, lua_animation_loop_t *loop,
                            int loop_idx, int tbl_idx) {
  loop_idx = lua_absindex(L, loop_idx);
  tbl_idx = lua_absindex(L, tbl_idx);
  int n = (int)lua_rawlen(L, tbl_idx);
  if (n > MAX_ANIMATION_LOOP_FRAMES) n = MAX_ANIMATION_LOOP_FRAMES;
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_rawgeti(L, tbl_idx, i + 1);
    loop->frames[i] = (lua_image_t *)luaL_testudata(L, -1, GRAPHICS_IMAGE_MT);
    if (loop->frames[i] && !loop->frames[i]->data)
      loop->frames[i] = NULL;  // a freed image is an empty frame too
    if (loop->frames[i])
      lua_rawseti(L, -2, i + 1);
    else
      lua_pop(L, 1);
  }
  lua_setiuservalue(L, loop_idx, LOOP_UV_FRAMES);
  loop->frame_count = n;
  loop->valid = (n > 0);
}

static int l_animation_loop_gc(lua_State *L) {
  lua_animation_loop_t *loop = (lua_animation_loop_t *)luaL_checkudata(
      L, 1, GRAPHICS_ANIMATION_LOOP_MT);
  loop->frame_count = 0;
  loop->valid = false;
  loop->destroyed = true;
  return 0;
}

static int l_animation_loop_new(lua_State *L) {
  // Same hazard as l_animation_blinker_new: capture the argument count before
  // lua_newuserdata pushes the object, or the object is counted as a trailing
  // argument. Here it failed silently rather than loudly — loop.new(ms, frames)
  // saw the userdata at index 2 instead of the frames table, so the animation
  // was created with zero frames and simply never drew.
  int top = lua_gettop(L);

  lua_animation_loop_t *loop = (lua_animation_loop_t *)lua_newuserdatauv(L, sizeof(lua_animation_loop_t), 1);
  loop->frame_count = 0;
  loop->current_frame = 0;
  loop->interval_ms = 100;
  loop->last_update_ms = to_ms_since_boot(get_absolute_time());
  loop->looping = true;
  loop->valid = false;
  loop->destroyed = false;

  if (top >= 1) {
    if (lua_isnumber(L, 1)) {
      loop->interval_ms = lb_checkint(L, 1);
    }
  }

  if (top >= 2 && lua_istable(L, 2))
    loop_set_frames(L, loop, -1, 2);

  if (top >= 3) {
    loop->looping = lua_toboolean(L, 3);
  }

  luaL_setmetatable(L, GRAPHICS_ANIMATION_LOOP_MT);
  return 1;
}

static int l_animation_loop_draw(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  bool flip = lua_toboolean(L, 4);

  if (!loop->valid || !loop->frames[loop->current_frame])
    return 0;

  // A frame's image is anchored, but a finaliser that runs after the
  // image's own can still get here with its pixels freed.
  lua_image_t *img = loop->frames[loop->current_frame];
  if (!img->data)
    return luaL_error(L, "attempt to use a freed image");
  display_draw_image_partial(x, y, img->w, img->h, img->data,
                            0, 0, img->w, img->h, flip, false, 0);
  return 0;
}

static int l_animation_loop_update(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  uint32_t now = to_ms_since_boot(get_absolute_time());

  if (!loop->valid)
    return 0;

  if (now - loop->last_update_ms >= loop->interval_ms) {
    loop->last_update_ms = now;
    loop->current_frame++;
    if (loop->current_frame >= loop->frame_count) {
      if (loop->looping) {
        loop->current_frame = 0;
      } else {
        loop->current_frame = loop->frame_count - 1;
      }
    }
  }
  return 0;
}

static int l_animation_loop_image(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  if (!loop->valid || !loop->frames[loop->current_frame])
    return 0;
  lua_getiuservalue(L, 1, LOOP_UV_FRAMES);
  lua_rawgeti(L, -1, loop->current_frame + 1);  // the image object itself
  return 1;
}

static int l_animation_loop_isValid(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  lua_pushboolean(L, loop->valid);
  return 1;
}

static int l_animation_loop_getFrameIndex(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  lua_pushinteger(L, loop->current_frame);
  return 1;
}

static int l_animation_loop_setImageTable(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->frame_count = 0;
  loop->current_frame = 0;
  loop->valid = false;

  if (!lua_istable(L, 2)) {
    anchor_set(L, 1, LOOP_UV_FRAMES, 0);
    return 0;
  }

  loop_set_frames(L, loop, 1, 2);
  loop->last_update_ms = to_ms_since_boot(get_absolute_time());
  return 0;
}

static int l_animation_loop_setInterval(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->interval_ms = lb_checkint(L, 2);
  return 0;
}

static int l_animation_loop_setLooping(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->looping = lua_toboolean(L, 2);
  return 0;
}

static int l_animation_loop_reset(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->current_frame = 0;
  loop->last_update_ms = to_ms_since_boot(get_absolute_time());
  return 0;
}

static const luaL_Reg l_animation_loop_methods[] = {
    {"draw", l_animation_loop_draw},
    {"update", l_animation_loop_update},
    {"image", l_animation_loop_image},
    {"isValid", l_animation_loop_isValid},
    {"getFrameIndex", l_animation_loop_getFrameIndex},
    {"setImageTable", l_animation_loop_setImageTable},
    {"setInterval", l_animation_loop_setInterval},
    {"setLooping", l_animation_loop_setLooping},
    {"reset", l_animation_loop_reset},
    {NULL, NULL}};

static const luaL_Reg l_animation_loop_lib[] = {
    {"new", l_animation_loop_new},
    {NULL, NULL}};

// ── Easing Functions ─────────────────────────────────────────────────────────

static float easing_linear(float t) {
  return t;
}

static float easing_sineIn(float t) {
  return sinf(t * (float)M_PI_2);
}

static float easing_sineOut(float t) {
  return 1.0f - sinf((1.0f - t) * (float)M_PI_2);
}

static float easing_sineInOut(float t) {
  return -(cosf((float)M_PI * t) - 1.0f) / 2.0f;
}

static float easing_quadIn(float t) {
  return t * t;
}

static float easing_quadOut(float t) {
  return 1.0f - (1.0f - t) * (1.0f - t);
}

static float easing_quadInOut(float t) {
  return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f;
}

static float easing_cubicIn(float t) {
  return t * t * t;
}

static float easing_cubicOut(float t) {
  return 1.0f - powf(1.0f - t, 3.0f);
}

static float easing_cubicInOut(float t) {
  return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

typedef float (*easing_fn)(float);

#define EASING_NAMES \
  "linear, sineIn, sineOut, sineInOut, quadIn, quadOut, quadInOut, cubicIn, " \
  "cubicOut, cubicInOut"

// NULL for a name that is not one of EASING_NAMES (the capitalised spellings
// "SineIn" etc. are also accepted); the caller raises.
static easing_fn get_easing_fn(const char *name) {
  if (!strcmp(name, "linear")) return easing_linear;
  if (!strcmp(name, "sineIn") || !strcmp(name, "SineIn")) return easing_sineIn;
  if (!strcmp(name, "sineOut") || !strcmp(name, "SineOut")) return easing_sineOut;
  if (!strcmp(name, "sineInOut") || !strcmp(name, "SineInOut")) return easing_sineInOut;
  if (!strcmp(name, "quadIn") || !strcmp(name, "QuadIn")) return easing_quadIn;
  if (!strcmp(name, "quadOut") || !strcmp(name, "QuadOut")) return easing_quadOut;
  if (!strcmp(name, "quadInOut") || !strcmp(name, "QuadInOut")) return easing_quadInOut;
  if (!strcmp(name, "cubicIn") || !strcmp(name, "CubicIn")) return easing_cubicIn;
  if (!strcmp(name, "cubicOut") || !strcmp(name, "CubicOut")) return easing_cubicOut;
  if (!strcmp(name, "cubicInOut") || !strcmp(name, "CubicInOut")) return easing_cubicInOut;
  return NULL;
}

// ── Animator ─────────────────────────────────────────────────────────────────

#define GRAPHICS_ANIMATOR_MT "picocalc.graphics.animator"

typedef struct {
  float start_value;
  float end_value;
  uint32_t duration_ms;
  uint32_t start_time_ms;
  float easing_amplitude;
  float easing_period;
  int repeat_count;
  int current_repeat;
  bool reverses;
  bool ended;
  easing_fn easing;
  bool destroyed;  // set by __gc
} lua_animator_t;

static lua_animator_t *check_animator(lua_State *L, int idx) {
  lua_animator_t *a = (lua_animator_t *)luaL_checkudata(L, idx, GRAPHICS_ANIMATOR_MT);
  if (a->destroyed)
    luaL_error(L, "attempt to use a destroyed animator");
  return a;
}

static int l_animator_gc(lua_State *L) {
  lua_animator_t *a = (lua_animator_t *)luaL_checkudata(L, 1, GRAPHICS_ANIMATOR_MT);
  a->ended = true;
  a->destroyed = true;
  return 0;
}

// Milliseconds since the animator started; 0 while a start delay is pending
// (start_time_ms is then in the future, and the unsigned difference would wrap
// to a huge elapsed time and end the animation at once).
static uint32_t animator_elapsed(const lua_animator_t *a, uint32_t now) {
  int32_t d = (int32_t)(now - a->start_time_ms);
  return d < 0 ? 0 : (uint32_t)d;
}

static int l_animator_new(lua_State *L) {
  // Capture the argument count before lua_newuserdata pushes the object, or
  // the object is counted as a trailing argument (see l_animation_loop_new).
  int top = lua_gettop(L);
  lua_Integer duration = lb_checkint(L, 1);
  float from = (float)luaL_checknumber(L, 2);
  float to = (float)luaL_checknumber(L, 3);
  const char *easing =
      (top >= 4 && !lua_isnil(L, 4)) ? luaL_checkstring(L, 4) : NULL;
  lua_Integer delay = (top >= 5 && !lua_isnil(L, 5)) ? lb_checkint(L, 5) : 0;

  easing_fn easing_f = easing_linear;
  if (easing) {
    easing_f = get_easing_fn(easing);
    if (!easing_f)
      return luaL_argerror(L, 4, lua_pushfstring(L, "unknown easing \"%s\" (expected one of: " EASING_NAMES ")", easing));
  }

  lua_animator_t *a = (lua_animator_t *)lua_newuserdata(L, sizeof(lua_animator_t));
  a->duration_ms = duration;
  a->start_value = from;
  a->end_value = to;
  a->start_time_ms = to_ms_since_boot(get_absolute_time()) + (uint32_t)delay;
  a->easing_amplitude = 1.0f;
  a->easing_period = 0.0f;
  a->repeat_count = 1;
  a->current_repeat = 0;
  a->reverses = false;
  a->ended = false;
  a->easing = easing_f;
  a->destroyed = false;

  luaL_setmetatable(L, GRAPHICS_ANIMATOR_MT);
  return 1;
}

static int l_animator_currentValue(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  if (a->ended) {
    lua_pushnumber(L, a->reverses ? a->start_value : a->end_value);
    return 1;
  }

  uint32_t now = to_ms_since_boot(get_absolute_time());
  uint32_t elapsed = animator_elapsed(a, now);
  float t = (float)elapsed / (float)a->duration_ms;

  if (t >= 1.0f) {
    if (a->current_repeat < a->repeat_count - 1) {
      a->current_repeat++;
      a->start_time_ms = now;
      t = 0.0f;
    } else if (a->reverses) {
      float tmp = a->start_value;
      a->start_value = a->end_value;
      a->end_value = tmp;
      a->start_time_ms = now;
      a->reverses = false;
      t = 0.0f;
    } else {
      t = 1.0f;
      a->ended = true;
    }
  }

  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  float eased = a->easing(t);
  float value = a->start_value + (a->end_value - a->start_value) * eased;
  lua_pushnumber(L, value);
  return 1;
}

static int l_animator_valueAtTime(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  uint32_t time_ms = (uint32_t)lb_clamp_int(lb_checkint(L, 2), 0, LUA_MAXINTEGER);

  float t = (float)time_ms / (float)a->duration_ms;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  float eased = a->easing(t);
  float value = a->start_value + (a->end_value - a->start_value) * eased;
  lua_pushnumber(L, value);
  return 1;
}

static int l_animator_progress(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  if (a->ended) {
    lua_pushnumber(L, 1.0f);
    return 1;
  }

  uint32_t now = to_ms_since_boot(get_absolute_time());
  uint32_t elapsed = animator_elapsed(a, now);
  float progress = (float)elapsed / (float)a->duration_ms;
  if (progress > 1.0f) progress = 1.0f;
  lua_pushnumber(L, progress);
  return 1;
}

static int l_animator_reset(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  a->start_time_ms = to_ms_since_boot(get_absolute_time());
  a->ended = false;
  a->current_repeat = 0;

  if (lua_isnumber(L, 2)) {
    a->duration_ms = lb_checkint(L, 2);
  }
  return 0;
}

static int l_animator_ended(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  lua_pushboolean(L, a->ended);
  return 1;
}

static int l_animator_index(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (!strcmp(key, "easingAmplitude")) {
    lua_pushnumber(L, a->easing_amplitude);
  } else if (!strcmp(key, "easingPeriod")) {
    lua_pushnumber(L, a->easing_period);
  } else if (!strcmp(key, "repeatCount")) {
    lua_pushinteger(L, a->repeat_count);
  } else if (!strcmp(key, "reverses")) {
    lua_pushboolean(L, a->reverses);
  } else {
    lua_pushvalue(L, 2);  // methods only (upvalue 1), as l_sprite_index
    lua_rawget(L, lua_upvalueindex(1));
  }
  return 1;
}

static int l_animator_newindex(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (!strcmp(key, "easingAmplitude")) {
    a->easing_amplitude = (float)luaL_checknumber(L, 3);
  } else if (!strcmp(key, "easingPeriod")) {
    a->easing_period = (float)luaL_checknumber(L, 3);
  } else if (!strcmp(key, "repeatCount")) {
    a->repeat_count = lb_checkint(L, 3);
  } else if (!strcmp(key, "reverses")) {
    a->reverses = lua_toboolean(L, 3);
  }
  return 0;
}

static const luaL_Reg l_animator_methods[] = {
    {"currentValue", l_animator_currentValue},
    {"valueAtTime", l_animator_valueAtTime},
    {"progress", l_animator_progress},
    {"reset", l_animator_reset},
    {"ended", l_animator_ended},
    {NULL, NULL}};

static const luaL_Reg l_animator_lib[] = {
    {"new", l_animator_new},
    {NULL, NULL}};

// ── Blinker ─────────────────────────────────────────────────────────────────

#define GRAPHICS_ANIMATION_BLINKER_MT "picocalc.graphics.animation.blinker"
#define MAX_BLINKERS 32

typedef struct {
  uint32_t on_duration_ms;
  uint32_t off_duration_ms;
  bool loop;
  int cycles;
  int current_cycle;
  uint32_t start_time_ms;
  bool running;
  bool state;
  bool destroyed;  // set by __gc; a dead blinker must never re-enter the list
} lua_animation_blinker_t;

static lua_animation_blinker_t *s_blinkers[MAX_BLINKERS];
static int s_blinker_count = 0;

static lua_animation_blinker_t *check_blinker(lua_State *L, int idx) {
  lua_animation_blinker_t *b = (lua_animation_blinker_t *)luaL_checkudata(
      L, idx, GRAPHICS_ANIMATION_BLINKER_MT);
  if (b->destroyed)
    luaL_error(L, "attempt to use a destroyed blinker");
  return b;
}

// s_blinkers[] (the updateAll/stopAll list) holds blinkers weakly: a
// blinker nobody references can't be observed, so it leaves the list when
// it is collected rather than being kept alive by it.
static void blinker_list_remove(lua_animation_blinker_t *b) {
  for (int i = 0; i < s_blinker_count; i++) {
    if (s_blinkers[i] == b) {
      for (int j = i; j < s_blinker_count - 1; j++)
        s_blinkers[j] = s_blinkers[j + 1];
      s_blinker_count--;
      return;
    }
  }
}

static int l_animation_blinker_gc(lua_State *L) {
  lua_animation_blinker_t *b = (lua_animation_blinker_t *)luaL_checkudata(
      L, 1, GRAPHICS_ANIMATION_BLINKER_MT);
  b->running = false;
  b->destroyed = true;
  blinker_list_remove(b);
  return 0;
}

static int l_animation_blinker_new(lua_State *L) {
  // Argument count MUST be read before lua_newuserdata: that call pushes the
  // new object onto the stack, so a later lua_gettop() counts the object as if
  // it were a trailing argument. Reading it after made every call fail —
  // blinker.new(420, 220, true) saw top == 4 and tried to read the userdata as
  // the integer `cycles`, and even blinker.new() with no arguments saw top == 1
  // and read the userdata as on_duration_ms. The constructor was unusable at
  // any arity, which is why nothing in the tree called it.
  int top = lua_gettop(L);

  lua_animation_blinker_t *b = (lua_animation_blinker_t *)lua_newuserdata(L, sizeof(lua_animation_blinker_t));
  b->on_duration_ms = 500;
  b->off_duration_ms = 500;
  b->loop = true;
  b->cycles = 0;
  b->current_cycle = 0;
  b->start_time_ms = to_ms_since_boot(get_absolute_time());
  b->running = false;
  b->state = true;
  b->destroyed = false;

  if (top >= 1) b->on_duration_ms = lb_checkint(L, 1);
  if (top >= 2) b->off_duration_ms = lb_checkint(L, 2);
  if (top >= 3) b->loop = lua_toboolean(L, 3);
  if (top >= 4) b->cycles = lb_checkint(L, 4);
  if (top >= 5) b->state = !lua_toboolean(L, 5);

  if (s_blinker_count < MAX_BLINKERS) {
    s_blinkers[s_blinker_count++] = b;
  }

  luaL_setmetatable(L, GRAPHICS_ANIMATION_BLINKER_MT);
  return 1;
}

static int l_animation_blinker_update(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);

  if (!b->running) {
    b->running = true;
    b->start_time_ms = to_ms_since_boot(get_absolute_time());
    b->current_cycle = 0;
    b->state = true;
    lua_pushboolean(L, b->state);
    return 1;
  }

  uint32_t now = to_ms_since_boot(get_absolute_time());
  uint32_t elapsed = now - b->start_time_ms;
  uint32_t cycle_time = b->on_duration_ms + b->off_duration_ms;

  if (cycle_time == 0) {
    lua_pushboolean(L, b->state);
    return 1;
  }

  uint32_t cycle_elapsed = elapsed % cycle_time;
  bool new_state = (cycle_elapsed < b->on_duration_ms);

  if (new_state != b->state) {
    b->state = new_state;
  }

  if (!b->loop && b->cycles > 0) {
    uint32_t total_cycles = elapsed / cycle_time;
    if (total_cycles >= (uint32_t)b->cycles) {
      b->running = false;
      b->state = false;
    }
  }

  lua_pushboolean(L, b->running ? b->state : false);
  return 1;
}

static int l_animation_blinker_start(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);

  // Argument 1 is self; the optional durations/flags start at 2.
  int top = lua_gettop(L);
  if (top >= 2) b->on_duration_ms = lb_checkint(L, 2);
  if (top >= 3) b->off_duration_ms = lb_checkint(L, 3);
  if (top >= 4) b->loop = lua_toboolean(L, 4);
  if (top >= 5) b->cycles = lb_checkint(L, 5);
  if (top >= 6) b->state = !lua_toboolean(L, 6);

  b->start_time_ms = to_ms_since_boot(get_absolute_time());
  b->current_cycle = 0;
  b->running = true;
  b->state = true;
  return 0;
}

static int l_animation_blinker_startLoop(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  b->loop = true;
  b->cycles = 0;
  b->start_time_ms = to_ms_since_boot(get_absolute_time());
  b->current_cycle = 0;
  b->running = true;
  b->state = true;
  return 0;
}

static int l_animation_blinker_stop(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  b->running = false;
  return 0;
}

static int l_animation_blinker_remove(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  b->running = false;
  blinker_list_remove(b);
  return 0;
}

static int l_animation_blinker_updateAll(lua_State *L) {
  uint32_t now = to_ms_since_boot(get_absolute_time());

  for (int i = 0; i < s_blinker_count; i++) {
    lua_animation_blinker_t *b = s_blinkers[i];
    if (!b->running) continue;

    uint32_t elapsed = now - b->start_time_ms;
    uint32_t cycle_time = b->on_duration_ms + b->off_duration_ms;

    if (cycle_time == 0) continue;

    uint32_t cycle_elapsed = elapsed % cycle_time;
    bool new_state = (cycle_elapsed < b->on_duration_ms);

    if (new_state != b->state) {
      b->state = new_state;
    }

    if (!b->loop && b->cycles > 0) {
      uint32_t total_cycles = elapsed / cycle_time;
      if (total_cycles >= (uint32_t)b->cycles) {
        b->running = false;
        b->state = false;
      }
    }
  }
  return 0;
}

static int l_animation_blinker_stopAll(lua_State *L) {
  (void)L;
  for (int i = 0; i < s_blinker_count; i++) {
    s_blinkers[i]->running = false;
  }
  return 0;
}

static int l_animation_blinker_isRunning(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  lua_pushboolean(L, b->running);
  return 1;
}

static const luaL_Reg l_animation_blinker_methods[] = {
    {"update", l_animation_blinker_update},
    {"start", l_animation_blinker_start},
    {"startLoop", l_animation_blinker_startLoop},
    {"stop", l_animation_blinker_stop},
    {"remove", l_animation_blinker_remove},
    {"isRunning", l_animation_blinker_isRunning},
    {NULL, NULL}};

static const luaL_Reg l_animation_blinker_lib[] = {
    {"new", l_animation_blinker_new},
    {"updateAll", l_animation_blinker_updateAll},
    {"stopAll", l_animation_blinker_stopAll},
    {NULL, NULL}};

// ── picocalc.graphics.font.* ─────────────────────────────────────────────────
// lua_font_t, check_font, and GRAPHICS_FONT_MT are defined near the top of this file

static int l_font_new(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  int font_id = -1;
  bool owned = false;
  if      (strcmp(name, "6x8") == 0)              font_id = 0;
  else if (strcmp(name, "8x12") == 0)             font_id = 1;
  else if (strcmp(name, "scientifica") == 0)      font_id = 2;
  else if (strcmp(name, "scientifica-bold") == 0) font_id = 3;
  else {
    // A path has a '/' or ends in ".pfn"; anything else is a bare font name
    // that matched no built-in, and must not be blamed on the sandbox.
    size_t n = strlen(name);
    bool path_like = strchr(name, '/') != NULL ||
                     (n >= 4 && strcmp(name + n - 4, ".pfn") == 0);
    if (!path_like)
      return luaL_error(L, "no such built-in font '%s' (built-ins: 6x8, 8x12, "
                           "scientifica, scientifica-bold); to load a .pfn file "
                           "pass its absolute path", name);
    if (!fs_sandbox_check(L, name, false))
      return luaL_error(L, "access denied: %s", name);
    const char *why = "load failed";
    font_id = font_registry_load_ex(name, &why);
    if (font_id < 0) {
      if (strcmp(why, FONT_REGISTRY_WHY_FULL) == 0)
        return luaL_error(L, "font registry full (all %d loaded-font slots are "
                             "in use; display.loadFont and graphics.font.new "
                             "share them): %s", FONT_REGISTRY_LOADED, name);
      return luaL_error(L, "failed to load font %s: %s", name, why);
    }
    owned = true;
  }
  lua_font_t *f = (lua_font_t *)lua_newuserdata(L, sizeof(lua_font_t));
  f->font_id = font_id;
  f->owned = owned;
  f->destroyed = false;
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->name[sizeof(f->name) - 1] = '\0';
  luaL_setmetatable(L, GRAPHICS_FONT_MT);
  return 1;
}

static int l_font_gc(lua_State *L) {
  lua_font_t *f = (lua_font_t *)luaL_checkudata(L, 1, GRAPHICS_FONT_MT);
  if (f->owned) {
    if (display_get_font() == f->font_id) display_set_font(0);
    font_registry_unload(f->font_id);
    f->owned = false;
  }
  f->destroyed = true;
  return 0;
}

static int l_font_drawText(lua_State *L) {
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  const char *text = luaL_checkstring(L, 4);
  uint16_t fg = l_checkcolor(L, 5);
  uint16_t bg = (lua_gettop(L) >= 6) ? l_checkcolor(L, 6) : COLOR_BLACK;
  int width = 0;
  WITH_FONT_ARG(L, 1, width = display_draw_text(x, y, text, fg, bg));
  lua_pushinteger(L, width);
  return 1;
}

static int l_font_getHeight(lua_State *L) {
  lua_pushinteger(L, font_for_arg(L, 1)->height);
  return 1;
}

static int l_font_getWidth(lua_State *L) {
  lua_pushinteger(L, font_for_arg(L, 1)->max_width);
  return 1;
}

static int l_font_getTextWidth(lua_State *L) {
  const char *text = luaL_checkstring(L, 2);
  lua_pushinteger(L, font_text_width(font_for_arg(L, 1), text));
  return 1;
}

static int l_font_getName(lua_State *L) {
  lua_pushstring(L, check_font(L, 1)->name);
  return 1;
}

// font:drawTextAligned(x, y, text, alignment, fg, [bg])
// alignment: 0=left, 1=center, 2=right
static int l_font_drawTextAligned(lua_State *L) {
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  const char *text = luaL_checkstring(L, 4);
  int alignment = luaL_checkinteger(L, 5);
  uint16_t fg = l_checkcolor(L, 6);
  uint16_t bg = (lua_gettop(L) >= 7) ? l_checkcolor(L, 7) : COLOR_BLACK;
  int tw = font_text_width(font_for_arg(L, 1), text);
  if (alignment == 1) x -= tw / 2;
  else if (alignment == 2) x -= tw;
  WITH_FONT_ARG(L, 1, display_draw_text(x, y, text, fg, bg));
  return 0;
}

// font:drawTextInRect(x, y, w, h, text, [alignment], [fg], [bg])
// Word-wraps text within a bounding rect.
static int l_font_drawTextInRect(lua_State *L) {
  int rx = lb_checkint(L, 2);
  int ry = lb_checkint(L, 3);
  int rw = lb_checkint(L, 4);
  int rh = lb_checkint(L, 5);
  const char *text = luaL_checkstring(L, 6);
  int alignment = (int)luaL_optinteger(L, 7, 0);
  emit_fb_t e = {
    (lua_gettop(L) >= 8) ? l_checkcolor(L, 8) : s_graphics_color,
    (lua_gettop(L) >= 9) ? l_checkcolor(L, 9) : s_graphics_bg_color };
  const pc_font_t *f = font_for_arg(L, 1);
  WITH_FONT_ARG(L, 1, wrap_lines(f, text, rx, ry, rw, rh, alignment, emit_to_display, &e));
  return 0;
}

// ── Top-level graphics.drawText* functions ─────────────────────────────────

// graphics.drawText(text, x, y, [font])
static int l_graphics_drawText(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  int width = 0;
  WITH_FONT_ARG(L, 4, width = display_draw_text(x, y, text, s_graphics_color, s_graphics_bg_color));
  lua_pushinteger(L, width);
  return 1;
}

// graphics.drawTextAligned(text, x, y, alignment, [font])
static int l_graphics_drawTextAligned(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  int x = lb_checkint(L, 2);
  int y = lb_checkint(L, 3);
  int alignment = luaL_checkinteger(L, 4);
  int tw = font_text_width(font_for_arg(L, 5), text);
  if (alignment == 1) x -= tw / 2;
  else if (alignment == 2) x -= tw;
  WITH_FONT_ARG(L, 5, display_draw_text(x, y, text, s_graphics_color, s_graphics_bg_color));
  return 0;
}

// graphics.drawTextInRect(text, x, y, w, h, [alignment], [font])
static int l_graphics_drawTextInRect(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  int rx = lb_checkint(L, 2);
  int ry = lb_checkint(L, 3);
  int rw = lb_checkint(L, 4);
  int rh = lb_checkint(L, 5);
  int alignment = (int)luaL_optinteger(L, 6, 0);
  emit_fb_t e = { s_graphics_color, s_graphics_bg_color };
  const pc_font_t *f = font_for_arg(L, 7);
  WITH_FONT_ARG(L, 7, wrap_lines(f, text, rx, ry, rw, rh, alignment, emit_to_display, &e));
  return 0;
}

// graphics.getTextSize(text, [font]) -> width, height
static int l_graphics_getTextSize(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  const pc_font_t *f = font_for_arg(L, 2);
  lua_pushinteger(L, font_text_width(f, text));
  lua_pushinteger(L, f->height);
  return 2;
}

typedef struct { const pc_font_t *f; int max_w; } emit_measure_t;
static void emit_measure(int x, int y, const char *line, void *user) {
  (void)x; (void)y;
  emit_measure_t *e = (emit_measure_t *)user;
  int w = font_text_width(e->f, line);
  if (w > e->max_w) e->max_w = w;
}

// graphics.getTextSizeForMaxWidth(text, maxWidth, [font]) -> width, height
static int l_graphics_getTextSizeForMaxWidth(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  int max_width = lb_checkint(L, 2);
  const pc_font_t *f = font_for_arg(L, 3);
  emit_measure_t e = { f, 0 };
  int lines = 0;
  WITH_FONT_ARG(L, 3, lines = wrap_lines(f, text, 0, 0, max_width, 0x7FFF, 0, emit_measure, &e));
  lua_pushinteger(L, e.max_w);
  lua_pushinteger(L, lines * f->height);
  return 2;
}

// Shared body of imageWithText and spriteWithText: render wrapped text into
// a fresh PSRAM image. Returns the pixel buffer or NULL when out of memory.
static uint16_t *render_text_image(lua_State *L, const char *text, int w, int h,
                                   uint16_t bg, int font_idx) {
  // Resolve the font FIRST: font_for_arg -> check_font -> luaL_checkudata
  // longjmps on a wrong-type font argument, and anything allocated before
  // that point would be orphaned (up to 200KB of PSRAM).
  const pc_font_t *f = font_for_arg(L, font_idx);
  // Same 2048 cap as image.new and the decoders: w/h come straight from Lua,
  // and w=h=-1 made (size_t)w*h*2 wrap to a 2-byte allocation.
  if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
    luaL_error(L, "text image size %dx%d out of range (1..2048)", w, h);
  uint16_t *pixels = (uint16_t *)umm_malloc((size_t)w * h * sizeof(uint16_t));
  if (!pixels) return NULL;
  for (int i = 0; i < w * h; i++) pixels[i] = bg;
  emit_buf_t e = { pixels, w, h, s_graphics_color, bg };
  WITH_FONT_ARG(L, font_idx, wrap_lines(f, text, 0, 0, w, h, 0, emit_to_buffer, &e));
  return pixels;
}

// graphics.imageWithText(text, maxWidth, maxHeight, [bgColor], [font])
// Returns a graphics.image with the text rendered into it
static int l_graphics_imageWithText(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  int img_w = lb_checkint(L, 2);
  int img_h = lb_checkint(L, 3);
  uint16_t bg = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? l_checkcolor(L, 4) : s_graphics_bg_color;
  uint16_t *pixels = render_text_image(L, text, img_w, img_h, bg, 5);
  if (!pixels) {
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  img->w = img_w;
  img->h = img_h;
  img->data = pixels;
  img->transparent_color = 0;
  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  return 1;
}

static const luaL_Reg l_font_methods[] = {
    {"drawText", l_font_drawText},
    {"drawTextAligned", l_font_drawTextAligned},
    {"drawTextInRect", l_font_drawTextInRect},
    {"getHeight", l_font_getHeight},
    {"getWidth", l_font_getWidth},
    {"getTextWidth", l_font_getTextWidth},
    {"getName", l_font_getName},
    {NULL, NULL}};

static const luaL_Reg l_font_lib[] = {
    {"new", l_font_new},
    {NULL, NULL}};

void lua_bridge_graphics_init(lua_State *L) {
  sprite_list_clear(L);  // reset on each app launch (a fresh lua_State)
  s_blinker_count = 0;  // reset blinkers on each app launch
  s_has_global_stencil = false;
  memset(s_global_stencil, 0, sizeof(s_global_stencil));

  // Metatables (lb_register_type): methods in a separate __index table,
  // finalisers and the sprite/animator property accessors in the metatable.
  static const luaL_Reg image_meta[] = {
      {"__gc", l_graphics_image_gc}, {NULL, NULL}};
  static const luaL_Reg sprite_meta[] = {
      {"__index", l_sprite_index}, {"__newindex", l_sprite_newindex},
      {"__gc", l_sprite_gc}, {NULL, NULL}};
  static const luaL_Reg spritesheet_meta[] = {
      {"__gc", l_spritesheet_gc}, {NULL, NULL}};
  static const luaL_Reg loop_meta[] = {
      {"__gc", l_animation_loop_gc}, {NULL, NULL}};
  static const luaL_Reg animator_meta[] = {
      {"__index", l_animator_index}, {"__newindex", l_animator_newindex},
      {"__gc", l_animator_gc}, {NULL, NULL}};
  static const luaL_Reg blinker_meta[] = {
      {"__gc", l_animation_blinker_gc}, {NULL, NULL}};
  static const luaL_Reg font_meta[] = {
      {"__gc", l_font_gc}, {NULL, NULL}};
  static const luaL_Reg tilemap_meta[] = {
      {"__gc", l_tilemap_gc}, {NULL, NULL}};
  lb_register_type(L, GRAPHICS_IMAGE_MT, l_graphics_image_methods, image_meta);
  lb_register_type(L, GRAPHICS_SPRITE_MT, l_sprite_methods, sprite_meta);
  lb_register_type(L, GRAPHICS_SPRITESHEET_MT, l_spritesheet_methods,
                   spritesheet_meta);
  lb_register_type(L, GRAPHICS_ANIMATION_LOOP_MT, l_animation_loop_methods,
                   loop_meta);
  lb_register_type(L, GRAPHICS_ANIMATOR_MT, l_animator_methods, animator_meta);
  lb_register_type(L, GRAPHICS_ANIMATION_BLINKER_MT,
                   l_animation_blinker_methods, blinker_meta);
  lb_register_type(L, GRAPHICS_FONT_MT, l_font_methods, font_meta);
  lb_register_type(L, GRAPHICS_TILEMAP_MT, l_tilemap_methods, tilemap_meta);

  // Build picocalc.graphics table
  lua_newtable(L);
  luaL_setfuncs(L, l_graphics_lib, 0);

  lua_newtable(L);
  luaL_setfuncs(L, l_graphics_image_lib, 0);
  lua_setfield(L, -2, "image");

  lua_newtable(L);
  luaL_setfuncs(L, l_sprite_lib, 0);
  lua_setfield(L, -2, "sprite");

  lua_newtable(L);
  luaL_setfuncs(L, l_spritesheet_lib, 0);
  lua_setfield(L, -2, "spritesheet");

  lua_newtable(L);
  luaL_setfuncs(L, l_tilemap_lib, 0);
  lua_setfield(L, -2, "tilemap");

  lua_newtable(L);  // animation parent table

  lua_newtable(L);
  luaL_setfuncs(L, l_animation_loop_lib, 0);
  lua_setfield(L, -2, "loop");

  lua_newtable(L);
  luaL_setfuncs(L, l_animator_lib, 0);
  lua_setfield(L, -2, "animator");

  lua_newtable(L);
  luaL_setfuncs(L, l_animation_blinker_lib, 0);
  lua_setfield(L, -2, "blinker");

  lua_setfield(L, -2, "animation");  // graphics["animation"] = animation table

  lua_newtable(L);
  luaL_setfuncs(L, l_font_lib, 0);
  lua_setfield(L, -2, "font");

  lua_setfield(L, -2, "graphics");   // picocalc["graphics"] = graphics table
}
