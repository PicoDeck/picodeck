#include "os_overlay.h"
#include "config.h"
#include "fps_counter.h"
#include "toast.h"
#include "ui.h"
#include "../drivers/display.h"

#include "pico/stdlib.h"
#include "umm_malloc.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// The counter box: "FPS: 999" in the 6x8 font plus 2px of padding, opaque
// black and fixed-width, so a shorter number never leaves stale digits in an
// app that does not redraw every pixel. The text sits 8px from the side and
// bottom edges; the top corners sit below the standard header, where
// perf.drawFPS() draws by default (text at y 24).
#define FPS_PAD      2
#define FPS_BOX_W    (48 + 2 * FPS_PAD)
#define FPS_BOX_H    (8 + 2 * FPS_PAD)
#define FPS_BOX_X_L  (8 - FPS_PAD)
#define FPS_BOX_X_R  (FB_WIDTH - 8 + FPS_PAD - FPS_BOX_W)
#define FPS_BOX_Y_T  (UI_HEADER_H + 1 + 3 - FPS_PAD)
#define FPS_BOX_Y_B  (FB_HEIGHT - 8 + FPS_PAD - FPS_BOX_H)

enum { OV_TOAST, OV_FPS, OV_COUNT };

// An overlay on screen: an id for its content (0 = nothing) and its
// rectangle, clipped to the screen.
typedef struct {
  uint32_t id;
  int16_t x, y, w, h;
} ov_rect_t;

// Static SRAM: 28 + 24 + 1 bytes (52 B of BSS on the device).
static fps_counter_t s_fps;
static ov_rect_t s_shown[OV_COUNT];  // what the panel shows of each overlay
static uint8_t s_fps_mode = OS_FPS_OFF;

// Off is stored as no key at all (a "0" from older builds also reads as Off).
static const char *const k_fps_keys[OS_FPS_MODES] = {NULL, "tr", "tl", "br", "bl"};
static const char *const k_fps_labels[OS_FPS_MODES] = {
    "Off", "Top right", "Top left", "Bottom right", "Bottom left"};

int os_overlay_fps_mode(void) {
  const char *v = config_get("show_fps");
  for (int m = OS_FPS_TOP_RIGHT; v && m < OS_FPS_MODES; m++)
    if (strcmp(v, k_fps_keys[m]) == 0)
      return m;
  return OS_FPS_OFF;
}

void os_overlay_reload(void) {
  s_fps_mode = (uint8_t)os_overlay_fps_mode();
  // Whatever drew over the app since (the system menu) replaced what the
  // panel showed: the next present puts the overlays up afresh.
  memset(s_shown, 0, sizeof(s_shown));
}

const char *os_overlay_fps_key(int mode) {
  return k_fps_keys[mode >= 0 && mode < OS_FPS_MODES ? mode : OS_FPS_OFF];
}

const char *os_overlay_fps_label(int mode) {
  return k_fps_labels[mode >= 0 && mode < OS_FPS_MODES ? mode : OS_FPS_OFF];
}

void os_overlay_app_start(void) {
  fps_counter_reset(&s_fps);
  os_overlay_reload();
}

void os_overlay_frame_tick(uint32_t now_ms) { fps_counter_tick(&s_fps, now_ms); }

static bool fps_right(void) {
  return s_fps_mode == OS_FPS_TOP_RIGHT || s_fps_mode == OS_FPS_BOTTOM_RIGHT;
}

static void set_rect(ov_rect_t *r, uint32_t id, int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > FB_WIDTH) w = FB_WIDTH - x;
  if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
  r->id = (w > 0 && h > 0) ? id : 0;
  r->x = (int16_t)x;
  r->y = (int16_t)y;
  r->w = (int16_t)w;
  r->h = (int16_t)h;
}

static bool same_rect(const ov_rect_t *a, const ov_rect_t *b) {
  return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

static bool in_rows(const ov_rect_t *r, int y0, int y1) {
  return r->y >= y0 && r->y + r->h - 1 <= y1;
}

static void draw_fps(const ov_rect_t *r, int value) {
  char buf[12];
  uint16_t color;
  if (value < 0) {
    strcpy(buf, "FPS: --");  // no 1 s window has completed yet
    color = COLOR_GRAY;
  } else {
    if (value > 999) value = 999;
    snprintf(buf, sizeof(buf), "FPS: %d", value);
    // perf.drawFPS's colour code.
    color = (value >= 55) ? COLOR_GREEN : (value >= 30) ? COLOR_YELLOW : COLOR_RED;
  }
  display_fill_rect(r->x, r->y, r->w, r->h, COLOR_BLACK);
  int tw = display_text_width(buf);
  int tx = fps_right() ? r->x + r->w - FPS_PAD - tw : r->x + FPS_PAD;
  display_draw_text(tx, r->y + FPS_PAD, buf, color, COLOR_BLACK);
}

static void draw_one(int which, const ov_rect_t *r, int fps) {
  if (which == OV_TOAST)
    toast_render();
  else
    draw_fps(r, fps);
}

// Put the overlay on the panel without leaving it in the draw buffer: save
// the app's pixels under it, draw, push the rectangle, put the pixels back.
// Without memory for the copy the overlay stays in the buffer (as a
// flush() leaves it).
static void compose_push(int which, const ov_rect_t *r, int fps) {
  display_wait_for_flush();  // flushRows' DMA may still be reading this buffer
  uint16_t *fb = display_get_back_buffer();
  size_t row = (size_t)r->w * sizeof(uint16_t);
  uint16_t *save = (uint16_t *)umm_malloc(row * (size_t)r->h);
  if (save)
    for (int i = 0; i < r->h; i++)
      memcpy(&save[i * r->w], &fb[(r->y + i) * FB_WIDTH + r->x], row);
  display_clear_clip_rect();
  draw_one(which, r, fps);
  display_push_rect(r->x, r->y, r->w, r->h);
  if (save) {
    for (int i = 0; i < r->h; i++)
      memcpy(&fb[(r->y + i) * FB_WIDTH + r->x], &save[i * r->w], row);
    umm_free(save);
  }
}

void os_overlay_draw(os_present_t kind, int y0, int y1) {
  bool partial = kind != OS_PRESENT_FLUSH;
  if (!partial) {
    y0 = 0;
    y1 = FB_HEIGHT - 1;
  }
  if (y0 < 0) y0 = 0;
  if (y1 >= FB_HEIGHT) y1 = FB_HEIGHT - 1;
  if (y0 > y1)
    return;  // the flush presents nothing either

  uint32_t now = to_ms_since_boot(get_absolute_time());
  if (kind == OS_PRESENT_ROWS)
    fps_counter_rows(&s_fps, y0, now);
  else
    fps_counter_present(&s_fps, now);

  uint32_t toast_id = toast_update();
  if (!toast_id && s_fps_mode == OS_FPS_OFF && !s_shown[OV_TOAST].id &&
      !s_shown[OV_FPS].id)
    return;

  if (display_get_scroll_offset() != 0) {
    // The panel shows frame-memory rows at scrolled positions, so nothing is
    // drawn, and everything is shown afresh once the offset is back to 0.
    // A partial present first pushes the draw buffer's rows back over what
    // the panel showed of an overlay (it would slide with the content); a
    // full one rewrites every row anyway. The push restores the app's own
    // pixels only where the overlay never went into the buffer (see below).
    if (partial)
      for (int i = 0; i < OV_COUNT; i++)
        if (s_shown[i].id)
          display_push_rect(s_shown[i].x, s_shown[i].y, s_shown[i].w, s_shown[i].h);
    memset(s_shown, 0, sizeof(s_shown));
    return;
  }

  // Full screen, built-in font: fixed geometry whatever the app set.
  int font = display_get_font();
  int cx, cy, cw, ch;
  display_get_clip_rect(&cx, &cy, &cw, &ch);
  display_set_font(0);

  ov_rect_t want[OV_COUNT];
  memset(want, 0, sizeof(want));
  if (toast_id) {
    int x, y, w, h;
    toast_rect(&x, &y, &w, &h);
    set_rect(&want[OV_TOAST], toast_id, x, y, w, h);
  }
  int fps = 0;
  if (s_fps_mode != OS_FPS_OFF) {
    fps = fps_counter_value(&s_fps, now);
    bool top = s_fps_mode == OS_FPS_TOP_RIGHT || s_fps_mode == OS_FPS_TOP_LEFT;
    // The id changes with the text, its colour and the corner.
    set_rect(&want[OV_FPS], (uint32_t)(fps + 2) | ((uint32_t)s_fps_mode << 16),
             fps_right() ? FPS_BOX_X_R : FPS_BOX_X_L, top ? FPS_BOX_Y_T : FPS_BOX_Y_B,
             FPS_BOX_W, FPS_BOX_H);
  }

  for (int i = 0; i < OV_COUNT; i++) {
    ov_rect_t *shown = &s_shown[i];
    const ov_rect_t *w = &want[i];
    bool moved = !w->id || !same_rect(shown, w);
    // Gone or moved: push the draw buffer's pixels there. compose_push never
    // leaves the overlay in the buffer, so that is what the app drew, unless
    // an earlier present sent those rows and so drew it into the buffer: an
    // app that has not redrawn them since keeps the old overlay, as it
    // would after flush().
    if (partial && shown->id && moved && !in_rows(shown, y0, y1))
      display_push_rect(shown->x, shown->y, shown->w, shown->h);
    if (w->id) {
      if (partial && (moved || w->id != shown->id) && !in_rows(w, y0, y1))
        compose_push(i, w, fps);
      // Into the rows this present sends (all of them for flush()).
      if (w->y <= y1 && w->y + w->h - 1 >= y0) {
        display_set_clip_rect(0, y0, FB_WIDTH, y1 - y0 + 1);
        draw_one(i, w, fps);
      }
    }
    *shown = *w;
  }

  display_set_clip_rect(cx, cy, cw, ch);
  display_set_font(font);
}
