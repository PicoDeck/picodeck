// driver_stubs.c - Stubs for PicoDeck driver functions
#define _XOPEN_SOURCE 500  // for nftw()
#include <errno.h>
#include <ftw.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <dirent.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "../hal/hal_display.h"
#include "../sim_socket.h"
#include "umm_malloc.h"
// Real driver header — pulled in so the stub definitions below are checked
// against the hardware signatures at compile time (any drift is an error).
// "pico/mutex.h" inside it resolves to the simulator stub in stubs/pico/.
#include "sdcard.h"

// External base path from hal_sdcard.c
extern char g_base_path[512];
#include "../hal/hal_sdcard.h"

// Core 1 control variables
volatile bool g_core1_pause = false;
volatile bool g_core1_paused = false;

// Display stubs - delegate to HAL
static uint16_t g_front_buffer[320 * 320];
static uint16_t g_back_buffer[320 * 320];
static int g_current_buffer = 0;

// Buffer most recently presented to the HAL framebuffer.  display_flush()
// presents the old back buffer (then swaps); display_flush_rows() presents the
// current draw buffer without swapping.  display_get_screen_buffer() must
// follow this, not the swap index, or flushRows-only apps read back a stale
// frame (mirrors s_last_presented in src/drivers/display.c).
static uint16_t* s_last_presented = g_front_buffer;

// Clip rect state (mirrors src/drivers/display.c)
static int s_clip_x0 = 0, s_clip_y0 = 0;
static int s_clip_x1 = 319, s_clip_y1 = 319;

// The clip-once rasterisers are shared with the firmware driver; this
// framebuffer is host order, so they run with swap = false.
#include "../../src/drivers/display_clip.h"
#include "../../src/drivers/display_raster.h"
static inline disp_clip_t cur_clip(void) {
    return (disp_clip_t){s_clip_x0, s_clip_y0, s_clip_x1, s_clip_y1};
}

// ── Hardware vertical scroll emulation (ST7365P VSCRDEF/VSCRSADD) ──────────
// The sim keeps a GRAM analog: every flush writes rows into s_gram, and
// present_gram() maps GRAM rows to screen rows through the scroll registers
// before handing the image to the HAL — mirroring how the panel scans its
// frame memory.  Only the visible 320 lines are emulated (the real chip has
// 480); scroll areas reaching past line 319 are clamped, so configure a ring
// that stays within the visible window, e.g. setScrollArea(0, 320, 160).
static uint16_t s_gram[320 * 320];
static int s_scroll_tfa = 0;      // top fixed area (rows)
static int s_scroll_vsa = 480;    // scroll area height (chip reset default)
static int s_scroll_offset = 0;   // VSCRSADD: GRAM line at top of scroll area
static uint32_t s_scroll_offset_writes = 0;  // foreign-write detection (see hw)

static bool scroll_identity(void) {
    return s_scroll_offset == s_scroll_tfa;  // offset==tfa maps every row to itself
}

// GRAM source row for visible row L under the current scroll registers.
static int scroll_src_row(int L) {
    int tfa = s_scroll_tfa;
    int vsa = s_scroll_vsa;
    if (tfa < 0) tfa = 0;
    if (tfa > 319) tfa = 319;
    if (vsa > 320 - tfa) vsa = 320 - tfa;  // clamp ring to emulated GRAM
    if (vsa <= 0) return L;
    if (L < tfa || L >= tfa + vsa) return L;  // fixed areas
    int rel = (s_scroll_offset - tfa) + (L - tfa);
    rel %= vsa;
    if (rel < 0) rel += vsa;
    return tfa + rel;
}

// Re-present the whole GRAM through the scroll mapping.
static void present_gram(void) {
    uint16_t* hal_fb = hal_display_get_framebuffer();
    if (!hal_fb) return;
    if (scroll_identity()) {
        memcpy(hal_fb, s_gram, 320 * 320 * sizeof(uint16_t));
    } else {
        for (int L = 0; L < 320; L++) {
            memcpy(&hal_fb[L * 320], &s_gram[scroll_src_row(L) * 320],
                   320 * sizeof(uint16_t));
        }
    }
    hal_display_present();
}

void display_darken(void) {
    // Copy front buffer to back buffer with darkening
    uint16_t* front = g_current_buffer == 0 ? g_front_buffer : g_back_buffer;
    uint16_t* back = g_current_buffer == 0 ? g_back_buffer : g_front_buffer;
    for (int i = 0; i < 320 * 320; i++) {
        uint16_t c = front[i];
        // Darken by half
        back[i] = ((c >> 1) & 0x7BEF);
    }
}

uint16_t* display_get_back_buffer(void) { 
    return g_current_buffer == 0 ? g_back_buffer : g_front_buffer;
}

// Mirrors display_save_buffers / display_restore_buffers in
// src/drivers/display.c (semantics, not steps): the saved front comes back as
// the front buffer and is what the panel (the GRAM analog) shows; the saved
// back comes back as the back buffer.
static uint16_t* sim_front_buffer(void) {
    return g_current_buffer == 0 ? g_front_buffer : g_back_buffer;
}

void display_save_buffers(uint16_t* dst) {
    const size_t n = 320 * 320;
    memcpy(dst, sim_front_buffer(), n * sizeof(uint16_t));
    memcpy(dst + n, display_get_back_buffer(), n * sizeof(uint16_t));
}

void display_restore_buffers(const uint16_t* src) {
    const size_t n = 320 * 320;
    uint16_t* front = sim_front_buffer();
    memcpy(front, src, n * sizeof(uint16_t));
    memcpy(display_get_back_buffer(), src + n, n * sizeof(uint16_t));
    memcpy(s_gram, front, n * sizeof(uint16_t));
    present_gram();
    s_last_presented = front;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    disp_clip_t c = cur_clip();
    disp_fill(display_get_back_buffer(), 320, &c, x, y, w, h, color);
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color) {
    display_fill_rect(x, y, w, 1, color);           // top edge
    display_fill_rect(x, y + h - 1, w, 1, color);   // bottom edge
    display_fill_rect(x, y, 1, h, color);           // left edge
    display_fill_rect(x + w - 1, y, 1, h, color);   // right edge
}

#include "../../src/fonts/font_registry.h"

// ── Active font ─────────────────────────────────────────────────────────────
// All glyph data and the renderer live in src/fonts/ — this stub only tracks
// which registry slot is active, by id and never by pointer, so an app that
// unloads the selected slot cannot leave a dangle behind (mirrors
// src/drivers/display.c).
static int s_active_font_id = 0;

static const pc_font_t *active_font(void) {
  const pc_font_t *f = font_registry_get(s_active_font_id);
  if (!f) { s_active_font_id = 0; f = font_registry_get(0); }
  return f;
}

void display_set_font(int font_id) {
  if (font_registry_get(font_id)) s_active_font_id = font_id;
}
int display_get_font(void) { return s_active_font_id; }
int display_get_font_width(void) { return active_font()->max_width; }
int display_get_font_height(void) { return active_font()->height; }
const pc_font_t *display_get_active_font(void) { return active_font(); }

// The simulator framebuffer is host byte order, so colours go in unswapped
// (the hardware one is byte-swapped). The clip rect is this stub's own.
static int sim_draw_text(int x, int y, const char *text, uint16_t fg,
                         uint16_t bg, bool transparent) {
  return font_render(active_font(), display_get_back_buffer(), 320,
                     s_clip_x0, s_clip_y0, s_clip_x1, s_clip_y1,
                     x, y, text, fg, bg, transparent);
}
int display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
  return sim_draw_text(x, y, text, fg, bg, false);
}
int display_draw_text_transparent(int x, int y, const char *text, uint16_t fg) {
  return sim_draw_text(x, y, text, fg, 0, true);
}
int display_text_width(const char *text) {
  return font_text_width(active_font(), text);
}

void display_flush(void) {
    // Copy back buffer into the GRAM analog and present through the scroll
    // mapping (identity when no hardware scroll is engaged).
    uint16_t* back = display_get_back_buffer();
    memcpy(s_gram, back, 320 * 320 * sizeof(uint16_t));
    present_gram();
    s_last_presented = back;
    // Swap buffer index
    g_current_buffer = 1 - g_current_buffer;
}

// Mirror of display_flush_rows in src/drivers/display.c: present rows y0..y1
// (inclusive) of the CURRENT draw buffer.  Does NOT swap buffers — the rest of
// the HAL framebuffer keeps whatever was presented previously, exactly like
// the LCD panel keeps its RAM outside the partial window.
void display_flush_rows(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 >= 320) y1 = 319;
    if (y0 > y1) return;

    uint16_t* back = display_get_back_buffer();
    memcpy(&s_gram[y0 * 320], &back[y0 * 320],
           (size_t)(y1 - y0 + 1) * 320 * sizeof(uint16_t));
    present_gram();
    s_last_presented = back;
}

// Mirror of display_flush_region (flush_region_impl with sync_back=true):
// SWAPS buffers like display_flush, presents only rows y0..y1 of the old draw
// buffer, then copies that band into the new back buffer so both buffers stay
// in sync.  Note the hardware version does not update s_last_presented — that
// quirk is mirrored deliberately.
void display_flush_region(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 >= 320) y1 = 319;
    if (y0 > y1) return;

    uint16_t* old_back = display_get_back_buffer();
    memcpy(&s_gram[y0 * 320], &old_back[y0 * 320],
           (size_t)(y1 - y0 + 1) * 320 * sizeof(uint16_t));
    present_gram();

    // Swap, then sync the flushed band front→back (see hardware comment about
    // stale two-frame-old content causing flicker).
    g_current_buffer = 1 - g_current_buffer;
    uint16_t* new_back = display_get_back_buffer();
    memcpy(&new_back[y0 * 320], &old_back[y0 * 320],
           (size_t)(y1 - y0 + 1) * 320 * sizeof(uint16_t));
}

// Mirror of display_push_rect: copy the rectangle from the back buffer into
// the GRAM analog and re-present (no swap, s_last_presented unchanged).
void display_push_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 320) h = 320 - y;
    if (w <= 0 || h <= 0) return;

    uint16_t* back = display_get_back_buffer();
    for (int r = y; r < y + h; r++)
        memcpy(&s_gram[r * 320 + x], &back[r * 320 + x],
               (size_t)w * sizeof(uint16_t));
    present_gram();
}

// No flush DMA here: every flush has finished when it returns.
void display_wait_for_flush(void) {}

void display_apply_clock(void) {}

void display_clear(uint16_t color) {
    // Deliberately IGNORES the clip rect — whole-framebuffer reset, matching
    // the hardware driver.  Use display_fill_rect for a clipped fill.
    uint16_t* fb = display_get_back_buffer();
    for (int i = 0; i < 320 * 320; i++) fb[i] = color;
}

void display_set_brightness(uint8_t brightness) { (void)brightness; }

void display_set_pixel(int x, int y, uint16_t color) {
    if (x >= s_clip_x0 && x <= s_clip_x1 && y >= s_clip_y0 && y <= s_clip_y1) {
        display_get_back_buffer()[y * 320 + x] = color;
    }
}

// Mirror of the firmware's display_get_pixel (host order here, no unswap).
uint16_t display_get_pixel(int x, int y) {
    if (x < 0 || x >= 320 || y < 0 || y >= 320) return 0;
    return display_get_back_buffer()[y * 320 + x];
}

void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    // Bresenham clipped to the visible steps (shared with the firmware).
    disp_clip_t c = cur_clip();
    disp_line(display_get_back_buffer(), 320, &c, x0, y0, x1, y1, color);
}

void display_draw_circle(int x, int y, int r, uint16_t color) {
    // Same rejection and large-radius path as the firmware driver.
    disp_clip_t c = cur_clip();
    if (disp_circle_rejected(&c, x, y, r)) return;
    if (r > DISP_CIRCLE_MIDPOINT_MAX) {
        disp_circle_big(display_get_back_buffer(), 320, &c, x, y, r, color);
        return;
    }
    int f = 1 - r;
    int ddF_x = 0;
    int ddF_y = -2 * r;
    int xi = 0;
    int yi = r;
    display_set_pixel(x, y + r, color);
    display_set_pixel(x, y - r, color);
    display_set_pixel(x + r, y, color);
    display_set_pixel(x - r, y, color);
    while (xi < yi) {
        if (f >= 0) {
            yi--;
            ddF_y += 2;
            f += ddF_y;
        }
        xi++;
        ddF_x += 2;
        f += ddF_x + 1;
        display_set_pixel(x + xi, y + yi, color);
        display_set_pixel(x - xi, y + yi, color);
        display_set_pixel(x + xi, y - yi, color);
        display_set_pixel(x - xi, y - yi, color);
        display_set_pixel(x + yi, y + xi, color);
        display_set_pixel(x - yi, y + xi, color);
        display_set_pixel(x + yi, y - xi, color);
        display_set_pixel(x - yi, y - xi, color);
    }
}

void display_fill_circle(int x, int y, int r, uint16_t color) {
    // One span per visible row, half-width floor(sqrt(r^2 - dy^2)) — the
    // pixels of the old per-row (int)sqrt loop, without its 2r iterations.
    disp_clip_t c = cur_clip();
    if (disp_circle_rejected(&c, x, y, r)) return;
    disp_fill_circle_rows(display_get_back_buffer(), 320, &c, x, y, r, color);
}

void display_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
    disp_clip_t c = cur_clip();
    disp_fill_triangle(display_get_back_buffer(), 320, &c, x0, y0, x1, y1, x2,
                       y2, color);
}

void display_get_raster_target(display_raster_target_t *t) {
    t->fb = display_get_back_buffer();
    t->stride = 320;
    t->clip_x0 = s_clip_x0;
    t->clip_y0 = s_clip_y0;
    t->clip_x1 = s_clip_x1;
    t->clip_y1 = s_clip_y1;
    t->swap = false;
}

void display_draw_textured_column(int x, int y0, int y1,
                                  const uint16_t *tex, int tex_w, int tex_h,
                                  int tex_x, int tex_y0, int tex_y1) {
    if (!tex || y0 > y1) return;
    if (tex_x < 0 || tex_x >= tex_w) return;
    if (x < s_clip_x0 || x > s_clip_x1) return;

    int screen_h = y1 - y0 + 1;
    int tex_span = tex_y1 - tex_y0 + 1;
    if (screen_h <= 0 || tex_span <= 0) return;

    uint32_t step = ((uint32_t)tex_span << 16) / (uint32_t)screen_h;
    uint32_t tex_pos = (uint32_t)tex_y0 << 16;
    if (y0 < s_clip_y0) {
        tex_pos += step * (uint32_t)(s_clip_y0 - y0);
        y0 = s_clip_y0;
    }
    if (y1 > s_clip_y1) y1 = s_clip_y1;
    if (y0 > y1) return;

    uint16_t *fb = display_get_back_buffer();
    for (int y = y0; y <= y1; y++) {
        int ty = (int)(tex_pos >> 16);
        if (ty < 0) ty = 0;
        if (ty >= tex_h) ty = tex_h - 1;
        fb[y * 320 + x] = tex[ty * tex_w + tex_x];
        tex_pos += step;
    }
}
void display_fill_hline(int y, int x0, int x1, uint16_t color) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    display_fill_rect(x0, y, x1 - x0 + 1, 1, color);
}

void display_fill_vline(int x, int y0, int y1, uint16_t color) {
    display_draw_line(x, y0, x, y1, color);
}
void display_fill_vline_gradient(int x, int y0, int y1, uint16_t color_top, uint16_t color_bottom) {
    (void)color_bottom;
    display_draw_line(x, y0, x, y1, color_top);
}
// ── Clip rect API (state declared at top of file) ────────────────────────────

void display_set_clip_rect(int x, int y, int w, int h) {
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > 319) x1 = 319;
    if (y1 > 319) y1 = 319;
    if (x1 < x || y1 < y) { x = 0; y = 0; x1 = -1; y1 = -1; }
    s_clip_x0 = x; s_clip_y0 = y;
    s_clip_x1 = x1; s_clip_y1 = y1;
}

void display_get_clip_rect(int *x, int *y, int *w, int *h) {
    if (x) *x = s_clip_x0;
    if (y) *y = s_clip_y0;
    if (w) *w = s_clip_x1 - s_clip_x0 + 1;
    if (h) *h = s_clip_y1 - s_clip_y0 + 1;
}

void display_clear_clip_rect(void) {
    s_clip_x0 = 0; s_clip_y0 = 0;
    s_clip_x1 = 319; s_clip_y1 = 319;
}

// ── Mode 7 perspective ground plane (host-endian framebuffer, no byte swap) ──
void display_draw_plane(const uint16_t *tex, int tex_w, int tex_h,
                        float cam_x, float cam_y, float cam_z,
                        float angle, int horizon_y, float scale) {
    if (!tex || tex_w <= 0 || tex_h <= 0 || cam_z <= 0.0f) return;
    if (scale <= 0.0f) scale = 1.0f;

    const bool pow2 = ((tex_w & (tex_w - 1)) == 0) && ((tex_h & (tex_h - 1)) == 0);
    const uint32_t mask_w = (uint32_t)tex_w - 1;
    const uint32_t mask_h = (uint32_t)tex_h - 1;

    const float sin_a = sinf(angle);
    const float cos_a = cosf(angle);
    const float fwd_x = -sin_a, fwd_y = cos_a;
    const float right_x = cos_a, right_y = sin_a;

    int y0 = horizon_y + 1;
    if (y0 < s_clip_y0) y0 = s_clip_y0;
    if (y0 < 0) y0 = 0;
    int y1 = s_clip_y1 > 319 ? 319 : s_clip_y1;
    if (y0 > y1) return;

    const int cx0 = s_clip_x0 < 0 ? 0 : s_clip_x0;
    const int cx1 = s_clip_x1 > 319 ? 319 : s_clip_x1;
    uint16_t *fb = display_get_back_buffer();

    for (int y = y0; y <= y1; y++) {
        const int p = y - horizon_y;
        const float z = cam_z * scale / (float)p;
        const float center_x = cam_x + fwd_x * z;
        const float center_y = cam_y + fwd_y * z;
        const float step_x = right_x * z / scale;
        const float step_y = right_y * z / scale;

        int32_t fx = (int32_t)((center_x + (cx0 - 160) * step_x) * 65536.0f);
        int32_t fy = (int32_t)((center_y + (cx0 - 160) * step_y) * 65536.0f);
        const int32_t dx = (int32_t)(step_x * 65536.0f);
        const int32_t dy = (int32_t)(step_y * 65536.0f);

        uint16_t *row = &fb[y * 320 + cx0];
        if (pow2) {
            for (int x = cx0; x <= cx1; x++) {
                *row++ = tex[((fy >> 16) & mask_h) * tex_w + ((fx >> 16) & mask_w)];
                fx += dx;
                fy += dy;
            }
        } else {
            for (int x = cx0; x <= cx1; x++) {
                int tx = fx >> 16, ty = fy >> 16;
                if (tx < 0) tx = 0; else if (tx >= tex_w) tx = tex_w - 1;
                if (ty < 0) ty = 0; else if (ty >= tex_h) ty = tex_h - 1;
                *row++ = tex[ty * tex_w + tx];
                fx += dx;
                fy += dy;
            }
        }
    }
}

void display_set_scroll_area(int top_fixed, int scroll_height, int bottom_fixed) {
    (void)bottom_fixed;  // derived: the emulation only needs tfa + vsa
    s_scroll_tfa = top_fixed;
    s_scroll_vsa = scroll_height;
    present_gram();
}
void display_set_scroll_offset(int offset) {
    // On hardware this is the whole scroll: the register write instantly
    // remaps GRAM lines to screen lines with no pixel transfer.  Re-present.
    s_scroll_offset = offset;
    s_scroll_offset_writes++;
    present_gram();
}
int display_get_scroll_offset(void) { return s_scroll_offset; }
uint32_t display_get_scroll_offset_writes(void) { return s_scroll_offset_writes; }
void display_set_transparent_color(uint16_t color) { (void)color; }
uint16_t display_get_transparent_color(void) { return 0; }
uint16_t* display_get_framebuffer(void) { return display_get_back_buffer(); }
uint16_t* display_get_screen_buffer(void) {
    // Return the buffer most recently presented — after display_flush() that
    // is the front buffer, but after display_flush_rows() (no swap) it is the
    // current draw buffer.  Matches s_last_presented on hardware.
    //
    // With hardware scroll engaged the panel no longer shows any framebuffer
    // directly; compose the scrolled view from the GRAM analog so
    // screenshots show what the panel shows.  (Hardware screenshots cannot
    // do this — the register remap happens in the LCD — so on-device
    // captures of a scrolling app read back the ring-layout draw buffer.)
    if (scroll_identity()) return s_last_presented;
    static uint16_t s_screen_out[320 * 320];
    for (int L = 0; L < 320; L++) {
        memcpy(&s_screen_out[L * 320], &s_gram[scroll_src_row(L) * 320],
               320 * sizeof(uint16_t));
    }
    return s_screen_out;
}

// Same signature as the firmware (x, y, w, h, data): this stub used to take
// (x, y, data, w, h), so src/os/ui.c's call passed w as the pixel pointer.
void display_draw_image(int x, int y, int w, int h, const uint16_t* data) {
    disp_clip_t c = cur_clip();
    disp_blit(display_get_back_buffer(), 320, &c, x, y, data, w, h, 0, 0, w, h,
              false, false, 0, false);
}

// Twin of the firmware's native drawImageNN (the Unicorn trampoline calls it):
// clipped to the clip rect, host-order pixels.
void display_draw_image_nn(int x, int y, const uint16_t *data,
                           int src_w, int src_h, int scale) {
    disp_clip_t c = cur_clip();
    disp_blit_nn(display_get_back_buffer(), 320, &c, x, y, data, src_w, src_h,
                 scale, false);
}

void display_draw_image_partial(int x, int y, int img_w, int img_h,
                                const uint16_t *data, int sx, int sy, int sw,
                                int sh, bool flip_x, bool flip_y,
                                uint16_t transparent_color) {
    // Clip once, specialised loops (shared with the firmware, no swap).
    disp_clip_t c = cur_clip();
    disp_blit(display_get_back_buffer(), 320, &c, x, y, data, img_w, img_h, sx,
              sy, sw, sh, flip_x, flip_y, transparent_color, false);
}

void display_draw_image_scaled_nn(int x, int y, const uint16_t *data,
                                  int src_w, int src_h, int dst_w, int dst_h,
                                  uint16_t transparent_color) {
    disp_clip_t c = cur_clip();
    disp_blit_scaled(display_get_back_buffer(), 320, &c, x, y, data, src_w,
                     src_h, dst_w, dst_h, transparent_color, false);
}

void display_draw_image_stretched(int x, int y, int dst_w, int dst_h,
                                  const uint16_t *data, int img_w, int img_h,
                                  int sx, int sy, int sw, int sh,
                                  uint16_t transparent_color) {
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sw > img_w - sx) sw = img_w - sx;
    if (sh > img_h - sy) sh = img_h - sy;
    if (sw <= 0 || sh <= 0) return;
    disp_clip_t c = cur_clip();
    disp_blit_scaled_rect(display_get_back_buffer(), 320, &c, x, y, data, img_w,
                          sx, sy, sw, sh, dst_w, dst_h, transparent_color, false);
}

void display_draw_image_scaled(int x, int y, int img_w, int img_h,
                               const uint16_t *data, float scale, float angle,
                               uint16_t transparent_color) {
    if (!data || img_w <= 0 || img_h <= 0) return;
    int dst_w = (int)(img_w * scale);
    int dst_h = (int)(img_h * scale);
    display_draw_image_scaled_nn(x, y, data, img_w, img_h, dst_w, dst_h, transparent_color);
}

// Keyboard stubs are now in keyboard_stub.c

// WiFi stubs — removed, now provided by sim_wifi.c

// USB MSC stub
void usb_msc_enter_mode(void) {
    printf("[USB] MSC mode not supported in simulator\n");
}

// SD card stubs
bool sdcard_remount(void) { return true; }
void sdcard_apply_clock(void) {}
bool sdcard_ensure_ready(void) { return true; }
void sd_set_slow_mode(bool slow) { (void)slow; }

bool sdcard_fexists(const char* path) {
    char full_path[1024];
    if (!hal_sdcard_resolve(path, full_path, sizeof(full_path))) return false;

    struct stat st;
    return stat(full_path, &st) == 0;
}

char* sdcard_read_file(const char* path, int* out_len) {
    char full_path[1024];
    if (!hal_sdcard_resolve(path, full_path, sizeof(full_path))) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    
    FILE* f = fopen(full_path, "rb");
    if (!f) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // umm_malloc, as on firmware: every caller frees with umm_free, and a
    // plain malloc here slipped past the counting allocator (the free then
    // subtracted bytes that were never added, skewing the heap metrics).
    char* buf = size >= 0 ? (char*)umm_malloc((size_t)size + 1) : NULL;
    if (!buf) {
        fclose(f);
        if (out_len) *out_len = 0;
        return NULL;
    }
    
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    
    if (out_len) *out_len = (int)got;
    return buf;
}
int sdcard_list_dir(const char* path,
                    void (*callback)(const sdcard_entry_t* entry, void* user),
                    void* user) {
    // Real implementation using host filesystem ("/" maps to the SD root).
    char full_path[1024];
    if (!hal_sdcard_resolve(path, full_path, sizeof(full_path))) return -1;
    
    printf("[SDCARD] Listing directory: %s (full: %s)\n", path, full_path);
    fflush(stdout);
    
    DIR* dir = opendir(full_path);
    if (!dir) {
        printf("[SDCARD] Failed to open directory: %s\n", full_path);
        fflush(stdout);
        return -1;
    }
    
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip dotfiles (FAT32 doesn't have them; filters .DS_Store, .git, etc.)
        if (entry->d_name[0] == '.') {
            continue;
        }
        
        // Build sdcard_entry_t
        sdcard_entry_t sdc_entry;
        strncpy(sdc_entry.name, entry->d_name, sizeof(sdc_entry.name) - 1);
        sdc_entry.name[sizeof(sdc_entry.name) - 1] = '\0';
        
        char entry_path[1024];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, entry->d_name);
        
        struct stat st;
        if (stat(entry_path, &st) == 0) {
            sdc_entry.is_dir = S_ISDIR(st.st_mode);
            sdc_entry.size = (uint32_t)st.st_size;
            // Set default date/time (Jan 1, 2020)
            sdc_entry.fdate = (2020 - 1980) << 9 | 1 << 5 | 1;
            sdc_entry.ftime = 0;
        } else {
            sdc_entry.is_dir = false;
            sdc_entry.size = 0;
            sdc_entry.fdate = 0;
            sdc_entry.ftime = 0;
        }
        
        printf("[SDCARD] Found: %s (is_dir=%d)\n", sdc_entry.name, sdc_entry.is_dir);
        fflush(stdout);
        
        // Call the callback
        if (callback) {
            callback(&sdc_entry, user);
        }
        count++;
    }
    
    closedir(dir);
    printf("[SDCARD] Listed %d entries\n", count);
    fflush(stdout);
    return count;
}

// SD card file handle stubs — use HAL functions.
// Signatures/semantics must match src/drivers/sdcard.h exactly (the header is
// #included at the top of this file, so any drift is a compile error):
//   sdcard_fseek        → bool, true on success
//   sdcard_ftell        → uint32_t position (0 for NULL handle)
//   sdcard_fsize        → int, -1 on error
//   sdcard_fsize_handle → int, -1 on error
sdfile_t sdcard_fopen(const char* path, const char* mode) { return hal_sdcard_open(path, mode); }
void sdcard_fclose(sdfile_t f) { hal_sdcard_close(f); }
int sdcard_fread(sdfile_t f, void* buf, int len) { return (int)hal_sdcard_read(f, buf, (size_t)len); }
// No cross-core SD mutex in the simulator: the try-read never finds it busy.
int sdcard_try_fread_at(sdfile_t f, uint32_t offset, void* buf, int len) {
    if (!f) return -1;
    if (hal_sdcard_seek(f, (long)offset) != 0) return -1;
    return len > 0 ? sdcard_fread(f, buf, len) : 0;
}
bool sdcard_fseek(sdfile_t f, uint32_t offset) {
    if (!f) return false;
    return hal_sdcard_seek(f, (long)offset) == 0;
}
uint32_t sdcard_ftell(sdfile_t f) {
    if (!f) return 0;
    long pos = hal_sdcard_tell(f);
    return pos > 0 ? (uint32_t)pos : 0;
}
int sdcard_fsize_handle(sdfile_t f) {
    if (!f) return -1;
    /* hal_sdcard_seek only supports SEEK_SET, so use fseek/ftell directly */
    FILE *fp = hal_sdcard_stream(f);
    long pos = ftell(fp);
    if (fseek(fp, 0, SEEK_END) != 0) return -1;
    long size = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    return size >= 0 ? (int)size : -1;
}
int sdcard_fwrite(sdfile_t f, const void* buf, int len) { return (int)hal_sdcard_write(f, buf, (size_t)len); }
int sdcard_fsize(const char* path) { return hal_sdcard_size(path); }
bool sdcard_mkdir(const char* path) {
    // Match hardware semantics: f_mkdir is single-level (no parent creation)
    // and FR_EXIST counts as success.
    if (hal_sdcard_mkdir(path) == 0) return true;
    return errno == EEXIST;
}
bool sdcard_is_mounted(void) { return true; }

bool sdcard_delete(const char* path) {
    char full[1024];
    if (!hal_sdcard_resolve(path, full, sizeof(full))) return false;
    return remove(full) == 0;
}
// Recursive delete helper using nftw
static int nftw_remove_cb(const char *fpath, const struct stat *sb,
                          int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(fpath);
}
bool sdcard_delete_recursive(const char* path) {
    char full[1024];
    if (!hal_sdcard_resolve(path, full, sizeof(full))) return false;
    return nftw(full, nftw_remove_cb, 64, FTW_DEPTH | FTW_PHYS) == 0;
}
bool sdcard_rename(const char* oldpath, const char* newpath) {
    char full_old[1024], full_new[1024];
    if (!hal_sdcard_resolve(oldpath, full_old, sizeof(full_old)) ||
        !hal_sdcard_resolve(newpath, full_new, sizeof(full_new)))
        return false;
    return rename(full_old, full_new) == 0;
}
bool sdcard_copy(const char* src, const char* dst,
                 void (*progress_cb)(uint32_t done, uint32_t total, void* user),
                 void* user) {
    // Mirrors src/drivers/sdcard.c: chunked copy, progress callback per
    // chunk, dst removed on failure.
    char full_src[1024], full_dst[1024];
    if (!hal_sdcard_resolve(src, full_src, sizeof(full_src)) ||
        !hal_sdcard_resolve(dst, full_dst, sizeof(full_dst)))
        return false;
    struct stat st;
    if (stat(full_src, &st) != 0 || S_ISDIR(st.st_mode)) return false;
    FILE* in = fopen(full_src, "rb");
    if (!in) return false;
    FILE* out = fopen(full_dst, "wb");
    if (!out) { fclose(in); return false; }
    uint32_t total = (uint32_t)st.st_size, done = 0;
    char buf[4096];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
        done += (uint32_t)n;
        if (progress_cb) progress_cb(done, total, user);
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(full_dst);
    return ok;
}
bool sdcard_stat(const char* path, sdcard_stat_t* out) {
    char full_path[1024];
    if (!hal_sdcard_resolve(path, full_path, sizeof(full_path))) return false;
    struct stat host_st;
    if (stat(full_path, &host_st) != 0) return false;
    out->size = (uint32_t)host_st.st_size;
    out->is_dir = S_ISDIR(host_st.st_mode);
    out->fdate = 0;
    out->ftime = 0;
    return true;
}
bool sdcard_disk_info(uint32_t* out_free_kb, uint32_t* out_total_kb) {
    extern char g_base_path[512];
    struct statvfs st;
    if (statvfs(g_base_path, &st) != 0) {
        if (out_free_kb) *out_free_kb = 0;
        if (out_total_kb) *out_total_kb = 0;
        return false;
    }
    // Model an SD card, not the host disk: fs.diskInfo pushes these as 32-bit
    // Lua integers, and a multi-terabyte host filesystem (btrfs, XFS) would
    // wrap negative. 32 GB is a typical card size. The device itself cannot
    // overflow: FF_LBA64 is 0 (at most 2^32 sectors) and the SD spec stops at
    // 2 TB, about 1.95e9 KB, under INT32_MAX.
    const uint64_t cap_kb = 32ull * 1024 * 1024;
    uint64_t total_kb = ((uint64_t)st.f_blocks * st.f_frsize) / 1024;
    uint64_t free_kb  = ((uint64_t)st.f_bavail * st.f_frsize) / 1024;
    if (total_kb > cap_kb) total_kb = cap_kb;
    if (free_kb > total_kb) free_kb = total_kb;
    if (out_total_kb) *out_total_kb = (uint32_t)total_kb;
    if (out_free_kb)  *out_free_kb  = (uint32_t)free_kb;
    return true;
}

// ── Display effects ──────────────────────────────────────────────────────────
//
// Ports of the hardware implementations in src/drivers/display.c:1500-1867.
//
// TWO deliberate differences from the hardware versions:
//
//   1. Byte order. The hardware framebuffer holds byte-SWAPPED RGB565 (the
//      ST7365P is big-endian), so display.c wraps every access in
//      FB_UNSWAP/FB_RESWAP. The simulator framebuffer holds HOST-order RGB565
//      (see display_fill_rect above, which writes `color` straight through), so
//      those macros collapse to identity here and are omitted. The one place
//      this is visible is the scanline halving mask, which is byte-order
//      dependent — see display_effect_scanline.
//
//   2. No RP2350 interpolator. The hardware versions drive interp0 in BLEND
//      mode; blend8() below reproduces its exact arithmetic so the two
//      implementations agree pixel-for-pixel.
//
// Effects operate on the back buffer, matching every other draw call here.

#define SIM_FB_W 320
#define SIM_FB_H 320

// RP2350 interpolator BLEND mode:  (base0*(255-accum1) + base1*accum1) >> 8
// Reproduced exactly, including the slight undershoot at t=255, so simulator
// output matches hardware rather than merely looking similar.
static inline uint8_t blend8(uint8_t a, uint8_t b, uint8_t t) {
    return (uint8_t)((a * (255 - t) + b * t) >> 8);
}

void display_effect_invert(void) {
    uint16_t *fb = display_get_back_buffer();
    size_t n = SIM_FB_W * SIM_FB_H;
    // Bit-level complement — endian-agnostic, same as hardware.
    uint32_t *fb32 = (uint32_t *)fb;
    for (size_t i = 0; i < n / 2; i++) fb32[i] ^= 0xFFFFFFFFu;
}

void display_effect_darken(uint8_t factor) {
    uint16_t *fb = display_get_back_buffer();
    size_t n = SIM_FB_W * SIM_FB_H;

    if (factor == 0) { memset(fb, 0, n * sizeof(uint16_t)); return; }
    if (factor >= 255) return;

    for (size_t i = 0; i < n; i++) {
        uint16_t p = fb[i];
        uint8_t r = blend8((p >> 11) & 0x1F, 0, 255 - factor);
        uint8_t g = blend8((p >> 5)  & 0x3F, 0, 255 - factor);
        uint8_t b = blend8( p        & 0x1F, 0, 255 - factor);
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

void display_effect_brighten(uint8_t factor) {
    if (factor == 0) return;

    uint16_t *fb = display_get_back_buffer();
    size_t n = SIM_FB_W * SIM_FB_H;

    for (size_t i = 0; i < n; i++) {
        uint16_t p = fb[i];
        uint8_t r = blend8((p >> 11) & 0x1F, 31, factor);  // max 5-bit
        uint8_t g = blend8((p >> 5)  & 0x3F, 63, factor);  // max 6-bit
        uint8_t b = blend8( p        & 0x1F, 31, factor);  // max 5-bit
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

void display_effect_tint(uint8_t r_tint, uint8_t g_tint, uint8_t b_tint,
                         uint8_t strength) {
    if (strength == 0) return;

    uint16_t *fb = display_get_back_buffer();
    size_t n = SIM_FB_W * SIM_FB_H;

    uint8_t tr = r_tint >> 3;  // 5-bit
    uint8_t tg = g_tint >> 2;  // 6-bit
    uint8_t tb = b_tint >> 3;  // 5-bit

    for (size_t i = 0; i < n; i++) {
        uint16_t p = fb[i];
        uint8_t r = blend8((p >> 11) & 0x1F, tr, strength);
        uint8_t g = blend8((p >> 5)  & 0x3F, tg, strength);
        uint8_t b = blend8( p        & 0x1F, tb, strength);
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

void display_effect_grayscale(void) {
    uint16_t *fb = display_get_back_buffer();
    size_t n = SIM_FB_W * SIM_FB_H;

    for (size_t i = 0; i < n; i++) {
        uint16_t p = fb[i];
        uint8_t r = (p >> 11) & 0x1F;
        uint8_t g = (p >> 5)  & 0x3F;
        uint8_t b =  p        & 0x1F;

        // ITU-R BT.601 luma, 6-bit green scaled to 5-bit for uniform weighting
        uint8_t g5 = g >> 1;
        uint8_t luma = (uint8_t)((r * 77 + g5 * 150 + b * 29) >> 8);
        if (luma > 31) luma = 31;
        uint8_t luma6 = (uint8_t)(luma << 1);

        fb[i] = (uint16_t)((luma << 11) | (luma6 << 5) | luma);
    }
}

void display_effect_blend(const uint16_t *src, int w, int h, uint8_t alpha) {
    if (!src || alpha == 0) return;

    uint16_t *fb = display_get_back_buffer();

    int max_h = (h < SIM_FB_H) ? h : SIM_FB_H;
    int max_w = (w < SIM_FB_W) ? w : SIM_FB_W;

    for (int y = 0; y < max_h; y++) {
        for (int x = 0; x < max_w; x++) {
            int idx = y * SIM_FB_W + x;
            uint16_t f = fb[idx];
            uint16_t s = src[y * w + x];   // src is host order on both targets

            uint8_t r = blend8((f >> 11) & 0x1F, (s >> 11) & 0x1F, alpha);
            uint8_t g = blend8((f >> 5)  & 0x3F, (s >> 5)  & 0x3F, alpha);
            uint8_t b = blend8( f        & 0x1F,  s        & 0x1F, alpha);

            fb[idx] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

void display_effect_palette(const uint16_t *lut, int lut_size) {
    if (!lut || lut_size <= 0) return;

    uint16_t *fb = display_get_back_buffer();
    size_t n = SIM_FB_W * SIM_FB_H;

    for (size_t i = 0; i < n; i++) {
        uint16_t p = fb[i];
        // Index from the top bits of each channel: (r3 << 5) | (g3 << 2) | b2
        uint8_t r = (p >> 13) & 0x07;
        uint8_t g = (p >> 8)  & 0x07;
        uint8_t b = (p >> 3)  & 0x03;
        int idx = (r << 5) | (g << 2) | b;
        if (idx >= lut_size) idx = lut_size - 1;
        fb[i] = lut[idx];          // LUT entries are host order
    }
}

void display_effect_dither(uint8_t levels) {
    if (levels < 2) levels = 2;
    if (levels > 32) levels = 32;

    uint16_t *fb = display_get_back_buffer();

    // Bayer 4x4 ordered dither matrix, normalised to 0-255
    static const uint8_t bayer4[4][4] = {
        {   0, 128,  32, 160 },
        { 192,  64, 224,  96 },
        {  48, 176,  16, 144 },
        { 240, 112, 208,  80 }
    };

    for (int y = 0; y < SIM_FB_H; y++) {
        for (int x = 0; x < SIM_FB_W; x++) {
            uint16_t p = fb[y * SIM_FB_W + x];
            uint8_t r = (p >> 11) & 0x1F;
            uint8_t g = (p >> 5)  & 0x3F;
            uint8_t b =  p        & 0x1F;

            uint8_t threshold = bayer4[y & 3][x & 3];
            int bias = (threshold / levels) - 128 / levels;

            int r8 = ((r << 3) | (r >> 2)) + bias;
            if (r8 < 0) r8 = 0; if (r8 > 255) r8 = 255;
            r = (uint8_t)((uint8_t)(r8 / (256 / levels)) * (255 / (levels - 1))) >> 3;
            if (r > 31) r = 31;

            int g8 = ((g << 2) | (g >> 4)) + bias;
            if (g8 < 0) g8 = 0; if (g8 > 255) g8 = 255;
            g = (uint8_t)((uint8_t)(g8 / (256 / levels)) * (255 / (levels - 1))) >> 2;
            if (g > 63) g = 63;

            int b8 = ((b << 3) | (b >> 2)) + bias;
            if (b8 < 0) b8 = 0; if (b8 > 255) b8 = 255;
            b = (uint8_t)((uint8_t)(b8 / (256 / levels)) * (255 / (levels - 1))) >> 3;
            if (b > 31) b = 31;

            fb[y * SIM_FB_W + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

void display_effect_scanline(uint8_t intensity) {
    if (intensity == 0) return;

    uint16_t *fb = display_get_back_buffer();

    // Darken every odd row. intensity selects how many times to halve:
    //   1-127 = one shift (50%), 128-254 = two shifts (25%), 255 = black.
    int shifts = (intensity < 128) ? 1 : (intensity < 255) ? 2 : 0;

    if (intensity == 255) {
        for (int y = 1; y < SIM_FB_H; y += 2)
            memset(&fb[y * SIM_FB_W], 0, SIM_FB_W * sizeof(uint16_t));
        return;
    }

    // Halving mask stops bits bleeding across channel boundaries.
    // Host-order RGB565 (RRRRRGGG GGGBBBBB) -> 0x7BEF, so 0x7BEF7BEF for a
    // 32-bit pair. Hardware uses 0xEF7BEF7B because its buffer is byte-swapped;
    // this is THE line that differs, and getting it wrong corrupts colour
    // rather than failing loudly.
    const uint32_t hmask = 0x7BEF7BEFu;

    for (int y = 1; y < SIM_FB_H; y += 2) {
        uint32_t *row32 = (uint32_t *)&fb[y * SIM_FB_W];
        int n32 = SIM_FB_W / 2;
        for (int i = 0; i < n32; i++) {
            uint32_t v = row32[i];
            for (int s = 0; s < shifts; s++) v = (v >> 1) & hmask;
            row32[i] = v;
        }
    }
}

void display_effect_posterize(uint8_t levels) {
    if (levels < 2) levels = 2;
    if (levels > 32) levels = 32;

    uint16_t *fb = display_get_back_buffer();
    size_t n = SIM_FB_W * SIM_FB_H;

    uint8_t lut5[32], lut6[64];
    for (int i = 0; i < 32; i++) {
        int q = (i * (levels - 1) + 15) / 31;
        lut5[i] = (uint8_t)((q * 31 + (levels - 1) / 2) / (levels - 1));
    }
    for (int i = 0; i < 64; i++) {
        int q = (i * (levels - 1) + 31) / 63;
        lut6[i] = (uint8_t)((q * 63 + (levels - 1) / 2) / (levels - 1));
    }

    for (size_t i = 0; i < n; i++) {
        uint16_t p = fb[i];
        uint8_t r = lut5[(p >> 11) & 0x1F];
        uint8_t g = lut6[(p >> 5)  & 0x3F];
        uint8_t b = lut5[ p        & 0x1F];
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

// Audio/sound/fileplayer/mp3 are the firmware's own code (src/drivers/);
// simulator/sim_audio.c is only the output.

// Native audio callback
_Atomic(void (*)(void)) g_native_audio_callback = NULL;

// audio_ring_free / audio_stream_debug live in src/drivers/audio_mix.c.

// umm_malloc: a counting allocator over the host malloc (see stubs/
// umm_malloc.h). Live bytes are the requested sizes of every block handed
// out (kept in a header, below), so the metric moves exactly with the
// allocations; Core 1 (network thread) allocates too, hence the atomics. The simulated heap refuses
// requests that would exceed 8 MB, like the device heap would.
//
// --real-umm (sim_umm_use_real) switches every umm_* call to the firmware's
// own umm_malloc (stubs/sim_real_umm.c) on a device-sized arena instead, so
// the 200-byte block cost, fragmentation and the largest free block behave
// as on hardware. Chosen once at startup, before the first allocation.
static _Atomic size_t s_umm_live;
static _Atomic size_t s_umm_peak;
static bool s_umm_real;

void   sim_real_umm_init_heap(void *ptr, size_t size);
void  *sim_real_umm_malloc(size_t size);
void  *sim_real_umm_calloc(size_t num, size_t size);
void  *sim_real_umm_realloc(void *ptr, size_t size);
void   sim_real_umm_free(void *ptr);
size_t sim_real_umm_free_heap_size(void);
size_t sim_real_umm_max_free_block_size(void);
int    sim_real_umm_fragmentation_metric(void);

bool sim_umm_use_real(void) {
    if (s_umm_real) return true;
    // +8 so the heap can start 4 bytes past an 8-byte boundary: umm blocks
    // are 200 bytes and data sits 4 bytes into a block, so every umm pointer
    // is then 8-byte aligned (the 64-bit host's Lua objects want that; the
    // device's 0x11200000 heap gives 4-byte alignment, enough for 32-bit).
    uint8_t *arena = malloc(SIM_REAL_UMM_HEAP_SIZE + 8);
    if (!arena) return false;
    uint8_t *base = arena + ((4 - ((uintptr_t)arena & 7)) & 7);
    sim_real_umm_init_heap(base, SIM_REAL_UMM_HEAP_SIZE);
    s_umm_real = true;
    return true;
}

bool sim_umm_is_real(void) { return s_umm_real; }

size_t sim_umm_heap_size(void) {
    return s_umm_real ? SIM_REAL_UMM_HEAP_SIZE : SIM_UMM_HEAP_SIZE;
}

static void umm_count_add(size_t n) {
    size_t live = atomic_fetch_add(&s_umm_live, n) + n;
    size_t peak = atomic_load(&s_umm_peak);
    while (live > peak && !atomic_compare_exchange_weak(&s_umm_peak, &peak, live)) {
    }
}

static void umm_count_sub(size_t n) {
    // Never wrap: a block that did not come from umm_* (a malloc/umm_free mix,
    // itself a bug on hardware) must not make the heap look 16 EB free.
    size_t live = atomic_load(&s_umm_live);
    while (!atomic_compare_exchange_weak(&s_umm_live, &live, live > n ? live - n : 0)) {
    }
}

static bool umm_would_overflow(size_t size) {
    return size > SIM_UMM_HEAP_SIZE || atomic_load(&s_umm_live) > SIM_UMM_HEAP_SIZE - size;
}

size_t sim_umm_live_bytes(void) {
    if (s_umm_real) return SIM_REAL_UMM_HEAP_SIZE - sim_real_umm_free_heap_size();
    return atomic_load(&s_umm_live);
}
size_t sim_umm_peak_bytes(void) {
    // The real umm keeps no peak; report the counting one (0 in real mode).
    return atomic_load(&s_umm_peak);
}

size_t umm_free_heap_size(void) {
    if (s_umm_real) return sim_real_umm_free_heap_size();
    size_t live = atomic_load(&s_umm_live);
    return live < SIM_UMM_HEAP_SIZE ? SIM_UMM_HEAP_SIZE - live : 0;
}
size_t umm_max_free_block_size(void) {
    if (s_umm_real) return sim_real_umm_max_free_block_size();
    return umm_free_heap_size();
}
int umm_fragmentation_metric(void) {
    if (s_umm_real) return sim_real_umm_fragmentation_metric();
    return 0;
}

// Every counting block carries its requested size in a 16-byte header (16
// keeps malloc's alignment), so the count is exactly what the OS asked for.
// malloc_usable_size() would count glibc's chunk instead, and glibc hands out
// a reused chunk whole when the remainder is under its 32-byte minimum: the
// same allocations then counted differently with the host heap's layout
// (glibc version, thread arenas, timing). ASan builds poison the header.
#define UMM_HDR 16
_Static_assert(sizeof(size_t) <= UMM_HDR, "size_t fits the umm header");
#if defined(__SANITIZE_ADDRESS__)
#define UMM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UMM_ASAN 1
#endif
#endif
#ifdef UMM_ASAN
#include <sanitizer/asan_interface.h>
#define UMM_HDR_POISON(b)   ASAN_POISON_MEMORY_REGION((b), UMM_HDR)
#define UMM_HDR_UNPOISON(b) ASAN_UNPOISON_MEMORY_REGION((b), UMM_HDR)
#else
#define UMM_HDR_POISON(b)   ((void)(b))
#define UMM_HDR_UNPOISON(b) ((void)(b))
#endif

static void *umm_hdr_wrap(uint8_t *base, size_t size) {
    memcpy(base, &size, sizeof(size));
    UMM_HDR_POISON(base);
    return base + UMM_HDR;
}
// The block's malloc base, unpoisoned; *size gets its requested size.
static uint8_t *umm_hdr_open(void *ptr, size_t *size) {
    uint8_t *base = (uint8_t *)ptr - UMM_HDR;
    UMM_HDR_UNPOISON(base);
    memcpy(size, base, sizeof(*size));
    return base;
}

void* umm_malloc(size_t size) {
    if (s_umm_real) return sim_real_umm_malloc(size);
    if (umm_would_overflow(size)) return NULL;
    uint8_t *base = malloc(UMM_HDR + size);
    if (!base) return NULL;
    umm_count_add(size);
    return umm_hdr_wrap(base, size);
}
void umm_free(void* ptr) {
    if (s_umm_real) {
        sim_real_umm_free(ptr);
        return;
    }
    if (!ptr) return;
    size_t size;
    uint8_t *base = umm_hdr_open(ptr, &size);
    umm_count_sub(size);
    free(base);
}
void* umm_realloc(void* ptr, size_t size) {
    if (s_umm_real) return sim_real_umm_realloc(ptr, size);
    if (!ptr) return umm_malloc(size);
    if (size == 0) {
        umm_free(ptr);
        return NULL;
    }
    size_t old;
    uint8_t *base = umm_hdr_open(ptr, &old);
    if (size > old && umm_would_overflow(size - old)) {
        UMM_HDR_POISON(base);
        return NULL;
    }
    uint8_t *grown = realloc(base, UMM_HDR + size);
    if (!grown) {  // ptr is untouched and still counted
        UMM_HDR_POISON(base);
        return NULL;
    }
    umm_count_sub(old);
    umm_count_add(size);
    return umm_hdr_wrap(grown, size);
}
void* umm_calloc(size_t num, size_t size) {
    if (s_umm_real) return sim_real_umm_calloc(num, size);
    if (size && num > SIM_UMM_HEAP_SIZE / size) return NULL;
    void *p = umm_malloc(num * size);
    if (p) memset(p, 0, num * size);
    return p;
}

// Lua bridge stubs — network/tcp now provided by real lua_bridge_network.c/tcp.c
// Must match lua_bridge_internal.h: WASM traps on signature mismatches.
struct lua_State;
void lua_bridge_crypto_init(struct lua_State *L) { (void)L; }

// OTA: flashing is firmware-only, but the validation sys.applyUpdate runs
// before its confirm is the firmware's own code (ota_verify.c: size, vector
// table, strict .sha256, ECDSA signature against the TEST update key), so
// E2E catches regressions.  Signatures match ota_update.h.
#include "../../src/os/ota_update.h"
#include "../../src/os/ota_verify.h"
bool ota_prepare_update(const char *bin_path, const char **out_err) {
    return ota_prepare_check(bin_path, OTA_HASH_PATH, OTA_SIG_PATH, out_err);
}
bool ota_trigger_update(const char *bin_path, const char **out_err) {
    (void)bin_path;
    *out_err = "OTA flashing is not supported in the simulator";
    return false;
}

// Video player stubs
typedef struct { int dummy; } VideoPlayer;
void* video_player_create(void) { return NULL; }
void video_player_destroy(void* player) { (void)player; }
int video_player_load(void* player, const char* path) { (void)player; (void)path; return -1; }
void video_player_play(void* player) { (void)player; }
void video_player_pause(void* player) { (void)player; }
void video_player_resume(void* player) { (void)player; }
void video_player_stop(void* player) { (void)player; }
void video_player_seek(void* player, uint32_t frame) { (void)player; (void)frame; }
bool video_player_update(void* player) { (void)player; return false; }
float video_player_get_fps(void* player) { (void)player; return 0; }
int video_player_get_dropped_frames(void* player) { (void)player; return 0; }
void video_player_reset_stats(void* player) { (void)player; }
bool video_player_has_audio(void* player) { (void)player; return false; }
void video_player_set_audio_volume(void* player, uint8_t volume) { (void)player; (void)volume; }
uint8_t video_player_get_audio_volume(void* player) { (void)player; return 100; }
void video_player_set_audio_muted(void* player, bool muted) { (void)player; (void)muted; }
bool video_player_get_audio_muted(void* player) { (void)player; return false; }
uint32_t video_player_get_frame_count(void* player) { (void)player; return 0; }
uint32_t video_player_get_duration_ms(void* player) { (void)player; return 0; }
uint32_t video_player_get_position_ms(void* player) { (void)player; return 0; }
void video_player_seek_ms(void* player, uint32_t ms) { (void)player; (void)ms; }
void video_player_seek_relative_ms(void* player, int32_t delta_ms) { (void)player; (void)delta_ms; }
bool video_player_has_ended(void* player) { (void)player; return false; }
void video_player_set_osd(void* player, bool enabled) { (void)player; (void)enabled; }
void video_player_show_osd(void* player) { (void)player; }
void video_player_set_osd_timeout(void* player, uint32_t ms) { (void)player; (void)ms; }

// ── g_api global ────────────────────────────────────────────────────────────
#include "os.h"
PicoCalcAPI g_api = {0};

// image_* is the real src/drivers/image_api.c (linked, not stubbed).

// --- Basic runner stub ---
#include "../../src/os/app_runner.h"
#include "../../src/os/launcher_types.h"
static bool basic_stub_can_handle(const app_entry_t *app) { (void)app; return false; }
static bool basic_stub_run(const app_entry_t *app) { (void)app; return false; }
const AppRunner g_basic_runner = {"basic", basic_stub_can_handle, basic_stub_run};

// --- PIO PSRAM stubs ---
bool pio_psram_init(void) { return false; }
bool pio_psram_available(void) { return false; }
uint32_t pio_psram_size(void) { return 0; }
void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len) { (void)addr; (void)dst; (void)len; }
void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len) { (void)addr; (void)src; (void)len; }
void pio_psram_set_sysclk(uint32_t sys_khz) { (void)sys_khz; }
const char *pio_psram_mode_str(void) { return "none"; }
void pio_psram_debug_test(bool full) { (void)full; }

// --- Image preload stubs ---
#include "../../src/drivers/image_api.h"
void image_preload_init(void) {}
bool image_preload_start(const char *path) { (void)path; return false; }
pc_image_t *image_preload_poll(bool *ready) { if (ready) *ready = false; return NULL; }
void image_preload_cancel(void) {}
void image_preload_update(void) {}

// --- display_draw_text_to_buffer (host-order offscreen buffer) ---
int display_draw_text_to_buffer(uint16_t *buf, int buf_w, int buf_h,
                                int x, int y, const char *text,
                                uint16_t fg, uint16_t bg) {
  return font_render(active_font(), buf, buf_w, 0, 0, buf_w - 1, buf_h - 1,
                     x, y, text, fg, bg, false);
}

// sound_player_set_finish_callback / sound_player_set_loop_callback now come
// from src/drivers/sound.c, compiled into the simulator (Task 5): defining
// them here too would be a duplicate symbol.
