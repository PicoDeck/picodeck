// sim_native_api.c: native-app state the Unicorn trampolines and the web
// build's compiled-in apps share (see sim_native_api.h), and the host API
// table the compiled-in apps call.

#include "sim_native_api.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "os.h"
#include "os_overlay.h"
#include "native_loader.h"   // native_file_adopt / native_file_drop
#include "sdcard.h"
#include "font_registry.h"
#include "hal/hal_timing.h"

// The simulator's drivers, declared as unicorn_trampolines.c declares them
// (their headers pull in firmware-only code).
extern void kbd_poll(void);
extern bool kbd_consume_menu_press(void);
extern bool system_menu_show_for_native(void);
extern void dev_commands_poll(void);
extern bool dev_commands_process(void);
extern bool dev_commands_wants_exit(void);
extern void dev_commands_clear_exit(void);

// Set by the system menu's Exit App or the dev `exit` command; read, and
// cleared, by the app's shouldExit(), as firmware's sys_shouldExit()
// (src/main.c) does.
static volatile bool s_exit;

void sim_native_poll(void) {
    kbd_poll();
    if (kbd_consume_menu_press() && system_menu_show_for_native())
        s_exit = true;
    // Run a queued control-channel command (`dev_command` RPC) on this
    // thread, as the firmware's sys_poll() runs the serial console's. The
    // simulator's dev handlers have no reboot flags, so an `exit` is all that
    // can come out of one.
    dev_commands_poll();
    dev_commands_process();
    if (dev_commands_wants_exit()) {
        s_exit = true;
        dev_commands_clear_exit();
    }
}

bool sim_native_should_exit(void) {
    bool v = s_exit;
    s_exit = false;
    return v;
}

// ── The host API table for compiled-in native apps ─────────────────────────
// Firmware gives native apps g_api as src/main.c wires it. The desktop
// simulator's Unicorn trampolines never read g_api (they call the drivers
// themselves), but an app compiled for the host (the web build's main.wasm)
// calls g_api's function pointers directly. These tables mirror src/main.c's
// entry for entry, except:
//   * getBackBuffer() hands out a panel-order (byte-swapped) shadow buffer,
//     because the simulator's framebuffer is host order. A present copies it
//     in, swapped, unless a drawing call wrote the back buffer since the last
//     present: the Unicorn trampolines' EMU_FB_BASE rule, so an app that
//     mixes raw pixels and drawing calls in one frame sees the drawing win;
//   * fs->open adopts the handle, so the launcher's teardown closes it;
//   * entries the simulator has no implementation for stay NULL:
//     sys->reboot and fs->browse.
// No function-pointer casts: WebAssembly traps an indirect call whose type
// differs at all, and a cast would hide the difference from the compiler.

extern void display_clear(uint16_t color);
extern void display_set_pixel(int x, int y, uint16_t color);
extern void display_fill_rect(int x, int y, int w, int h, uint16_t color);
extern void display_draw_rect(int x, int y, int w, int h, uint16_t color);
extern void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
extern void display_draw_circle(int cx, int cy, int r, uint16_t color);
extern void display_fill_circle(int cx, int cy, int r, uint16_t color);
extern int  display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
extern void display_flush(void);
extern void display_flush_rows(int y0, int y1);
extern void display_flush_region(int y0, int y1);
extern void display_set_brightness(uint8_t brightness);
extern uint16_t *display_get_back_buffer(void);
extern void display_draw_image_nn(int x, int y, const uint16_t *data, int src_w, int src_h, int scale);
extern void display_effect_invert(void);
extern void display_effect_darken(uint8_t factor);
extern void display_effect_brighten(uint8_t factor);
extern void display_effect_tint(uint8_t r, uint8_t g, uint8_t b, uint8_t strength);
extern void display_effect_grayscale(void);
extern void display_effect_blend(const uint16_t *src, int w, int h, uint8_t alpha);
extern void display_effect_palette(const uint16_t *lut, int lut_size);
extern void display_effect_dither(uint8_t levels);
extern void display_effect_scanline(uint8_t intensity);
extern void display_effect_posterize(uint8_t levels);
extern void display_fill_vline(int x, int y0, int y1, uint16_t color);
extern void display_draw_textured_column(int x, int y0, int y1, const uint16_t *tex, int tex_w, int tex_h, int tex_x, int tex_y0, int tex_y1);
extern void display_fill_vline_gradient(int x, int y0, int y1, uint16_t color_top, uint16_t color_bottom);
extern void display_set_clip_rect(int x, int y, int w, int h);
extern void display_get_clip_rect(int *x, int *y, int *w, int *h);
extern void display_clear_clip_rect(void);
extern void display_fill_hline(int y, int x0, int x1, uint16_t color);
extern void display_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
extern void display_set_scroll_area(int top_fixed, int scroll_height, int bottom_fixed);
extern void display_set_scroll_offset(int offset);
extern void display_draw_plane(const uint16_t *tex, int tex_w, int tex_h, float cam_x, float cam_y, float cam_z, float angle, int horizon_y, float scale);
extern void display_set_font(int font_id);
extern int  display_get_font(void);
extern int  display_get_font_width(void);
extern int  display_get_font_height(void);
extern int  display_text_width(const char *text);
extern int  display_draw_text_transparent(int x, int y, const char *text, uint16_t fg);
extern uint32_t kbd_get_buttons(void);
extern uint32_t kbd_get_buttons_pressed(void);
extern uint32_t kbd_get_buttons_released(void);
extern char kbd_get_char(void);
extern int kbd_get_battery_percent(void);
extern void system_menu_add_item(const char *label, void (*cb)(void *user), void *user);
extern void system_menu_clear_items(void);
extern void sim_log_append_src(const char *src, const char *text);
extern void perf_begin_frame(void);
extern void perf_end_frame(void);
extern int  perf_get_fps(void);
extern uint32_t perf_get_frame_time(void);
extern void perf_set_target_fps(uint32_t fps);
extern void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
extern void audio_stop_tone(void);
extern void audio_set_volume(uint8_t volume);
extern void audio_start_stream(uint32_t sample_rate);
extern void audio_stop_stream(void);
extern void audio_push_samples(const int16_t *samples, int count);

#define FB_W 320
#define FB_H 320

// ── Display ────────────────────────────────────────────────────────────────

static uint16_t s_panel_fb[FB_W * FB_H];  // what getBackBuffer() hands out
static bool s_drawn;  // a drawing call wrote the back buffer since the last present

// Rows y0..y1 (inclusive, in range) of the shadow into the back buffer,
// byte-swapped to host order, unless a drawing call wins this present.
static void present_panel_rows(int y0, int y1) {
    if (s_drawn) return;
    uint16_t *back = display_get_back_buffer();
    for (int i = y0 * FB_W; i < (y1 + 1) * FB_W; i++) {
        uint16_t p = s_panel_fb[i];
        back[i] = (uint16_t)((p >> 8) | (p << 8));
    }
}

static int clamp_row(int y) { return y < 0 ? 0 : y >= FB_H ? FB_H - 1 : y; }

static void napi_flush(void) {
    present_panel_rows(0, FB_H - 1);
    s_drawn = false;
    os_overlay_draw(OS_PRESENT_FLUSH, 0, FB_H - 1);
    display_flush();
}
static void napi_flush_rows(int y0, int y1) {
    int a = clamp_row(y0), b = clamp_row(y1);
    if (a <= b) present_panel_rows(a, b);
    s_drawn = false;
    os_overlay_draw(OS_PRESENT_ROWS, y0, y1);
    display_flush_rows(y0, y1);
}
static void napi_flush_region(int y0, int y1) {
    int a = clamp_row(y0), b = clamp_row(y1);
    if (a <= b) present_panel_rows(a, b);
    s_drawn = false;
    os_overlay_draw(OS_PRESENT_REGION, y0, y1);
    display_flush_region(y0, y1);
}
static uint16_t *napi_get_back_buffer(void) { return s_panel_fb; }
static int napi_get_width(void) { return FB_W; }
static int napi_get_height(void) { return FB_H; }

// Drawing calls: they write the host back buffer, so the next present shows it.
static void napi_clear(uint16_t c) { display_clear(c); s_drawn = true; }
static void napi_set_pixel(int x, int y, uint16_t c) { display_set_pixel(x, y, c); s_drawn = true; }
static void napi_fill_rect(int x, int y, int w, int h, uint16_t c) { display_fill_rect(x, y, w, h, c); s_drawn = true; }
static void napi_draw_rect(int x, int y, int w, int h, uint16_t c) { display_draw_rect(x, y, w, h, c); s_drawn = true; }
static void napi_draw_line(int x0, int y0, int x1, int y1, uint16_t c) { display_draw_line(x0, y0, x1, y1, c); s_drawn = true; }
static void napi_draw_circle(int cx, int cy, int r, uint16_t c) { display_draw_circle(cx, cy, r, c); s_drawn = true; }
static void napi_fill_circle(int cx, int cy, int r, uint16_t c) { display_fill_circle(cx, cy, r, c); s_drawn = true; }
static int napi_draw_text(int x, int y, const char *t, uint16_t fg, uint16_t bg) { s_drawn = true; return display_draw_text(x, y, t, fg, bg); }
static void napi_draw_image_nn(int x, int y, const uint16_t *d, int w, int h, int s) { display_draw_image_nn(x, y, d, w, h, s); s_drawn = true; }
static void napi_effect_invert(void) { display_effect_invert(); s_drawn = true; }
static void napi_effect_darken(uint8_t f) { display_effect_darken(f); s_drawn = true; }
static void napi_effect_brighten(uint8_t f) { display_effect_brighten(f); s_drawn = true; }
static void napi_effect_tint(uint8_t r, uint8_t g, uint8_t b, uint8_t s) { display_effect_tint(r, g, b, s); s_drawn = true; }
static void napi_effect_grayscale(void) { display_effect_grayscale(); s_drawn = true; }
static void napi_effect_blend(const uint16_t *src, int w, int h, uint8_t a) { display_effect_blend(src, w, h, a); s_drawn = true; }
static void napi_effect_palette(const uint16_t *lut, int n) { display_effect_palette(lut, n); s_drawn = true; }
static void napi_effect_dither(uint8_t l) { display_effect_dither(l); s_drawn = true; }
static void napi_effect_scanline(uint8_t i) { display_effect_scanline(i); s_drawn = true; }
static void napi_effect_posterize(uint8_t l) { display_effect_posterize(l); s_drawn = true; }
static void napi_fill_vline(int x, int y0, int y1, uint16_t c) { display_fill_vline(x, y0, y1, c); s_drawn = true; }
static void napi_draw_textured_column(int x, int y0, int y1, const uint16_t *tex, int tw, int th, int tx, int ty0, int ty1) { display_draw_textured_column(x, y0, y1, tex, tw, th, tx, ty0, ty1); s_drawn = true; }
static void napi_fill_vline_gradient(int x, int y0, int y1, uint16_t top, uint16_t bottom) { display_fill_vline_gradient(x, y0, y1, top, bottom); s_drawn = true; }
static void napi_fill_hline(int y, int x0, int x1, uint16_t c) { display_fill_hline(y, x0, x1, c); s_drawn = true; }
static void napi_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) { display_fill_triangle(x0, y0, x1, y1, x2, y2, c); s_drawn = true; }
static void napi_draw_plane(const uint16_t *tex, int tw, int th, float cx, float cy, float cz, float angle, int horizon_y, float scale) { display_draw_plane(tex, tw, th, cx, cy, cz, angle, horizon_y, scale); s_drawn = true; }
static int napi_draw_text_transparent(int x, int y, const char *t, uint16_t fg) { s_drawn = true; return display_draw_text_transparent(x, y, t, fg); }

static const picocalc_display_t s_display = {
    .clear = napi_clear,
    .setPixel = napi_set_pixel,
    .fillRect = napi_fill_rect,
    .drawRect = napi_draw_rect,
    .drawLine = napi_draw_line,
    .drawCircle = napi_draw_circle,
    .fillCircle = napi_fill_circle,
    .drawText = napi_draw_text,
    .flush = napi_flush,
    .getWidth = napi_get_width,
    .getHeight = napi_get_height,
    .setBrightness = display_set_brightness,
    .drawImageNN = napi_draw_image_nn,
    .flushRows = napi_flush_rows,
    .flushRegion = napi_flush_region,
    .getBackBuffer = napi_get_back_buffer,
    .effectInvert = napi_effect_invert,
    .effectDarken = napi_effect_darken,
    .effectBrighten = napi_effect_brighten,
    .effectTint = napi_effect_tint,
    .effectGrayscale = napi_effect_grayscale,
    .effectBlend = napi_effect_blend,
    .effectPalette = napi_effect_palette,
    .effectDither = napi_effect_dither,
    .effectScanline = napi_effect_scanline,
    .effectPosterize = napi_effect_posterize,
    .fillVLine = napi_fill_vline,
    .drawTexturedColumn = napi_draw_textured_column,
    .fillVLineGradient = napi_fill_vline_gradient,
    .setClipRect = display_set_clip_rect,
    .getClipRect = display_get_clip_rect,
    .clearClipRect = display_clear_clip_rect,
    .fillHLine = napi_fill_hline,
    .fillTriangle = napi_fill_triangle,
    .setScrollArea = display_set_scroll_area,
    .setScrollOffset = display_set_scroll_offset,
    .drawPlane = napi_draw_plane,
    .setFont = display_set_font,
    .getFont = display_get_font,
    .getFontWidth = display_get_font_width,
    .getFontHeight = display_get_font_height,
    .textWidth = display_text_width,
    .loadFont = font_registry_load,
    .unloadFont = font_registry_unload,
    .drawTextTransparent = napi_draw_text_transparent,
};

// ── System ─────────────────────────────────────────────────────────────────

static bool napi_is_usb_powered(void) { return true; }  // as the trampolines: always USB here

// As the trampolines' sys->log: "[APP] ..." on stdout and the log ring.
static void napi_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("[APP] %s\n", line);
    sim_log_append_src("native", line);
}

static void napi_set_audio_callback(void (*cb)(void)) {
    atomic_store(&g_native_audio_callback, cb);
}

static const picocalc_sys_t s_sys = {
    .getTimeMs = hal_get_time_ms,
    .getTimeUs = hal_get_time_us,
    .reboot = NULL,
    .getBatteryPercent = kbd_get_battery_percent,
    .isUSBPowered = napi_is_usb_powered,
    .addMenuItem = system_menu_add_item,
    .clearMenuItems = system_menu_clear_items,
    .log = napi_log,
    .poll = sim_native_poll,
    .shouldExit = sim_native_should_exit,
    .setAudioCallback = napi_set_audio_callback,
};

static const picocalc_input_t s_input = {
    .getButtons = kbd_get_buttons,
    .getButtonsPressed = kbd_get_buttons_pressed,
    .getButtonsReleased = kbd_get_buttons_released,
    .getChar = kbd_get_char,
};

static void napi_draw_fps(int x, int y) {  // as src/main.c's perf_draw_fps_wrapper
    int fps = perf_get_fps();
    char buf[16];
    snprintf(buf, sizeof(buf), "FPS: %d", fps);
    uint16_t color = (fps >= 55) ? 0x07E0 : (fps >= 30) ? 0xFFE0 : 0xF800;  // green, yellow, red
    napi_draw_text(x, y, buf, color, 0x0000);
}

static const picocalc_perf_t s_perf = {
    .beginFrame = perf_begin_frame,
    .endFrame = perf_end_frame,
    .getFPS = perf_get_fps,
    .getFrameTime = perf_get_frame_time,
    .drawFPS = napi_draw_fps,
    .setTargetFPS = perf_set_target_fps,
};

// ── Files ──────────────────────────────────────────────────────────────────

static pcfile_t napi_fs_open(const char *path, const char *mode) {
    return native_file_adopt(sdcard_fopen(path, mode));
}
static int napi_fs_read(pcfile_t f, void *buf, int len) { return sdcard_fread(f, buf, len); }
static int napi_fs_write(pcfile_t f, const void *buf, int len) { return sdcard_fwrite(f, buf, len); }
static void napi_fs_close(pcfile_t f) { native_file_drop(f); }
static bool napi_fs_exists(const char *path) { return sdcard_fexists(path); }
static int napi_fs_size(const char *path) { return sdcard_fsize(path); }
static int napi_fs_fsize(pcfile_t f) { return sdcard_fsize_handle(f); }
static bool napi_fs_seek(pcfile_t f, uint32_t offset) { return sdcard_fseek(f, offset); }
static uint32_t napi_fs_tell(pcfile_t f) { return sdcard_ftell(f); }
static bool napi_fs_mkdir(const char *path) { return sdcard_mkdir(path); }
static bool napi_fs_delete(const char *path) { return sdcard_delete(path); }
static bool napi_fs_rename(const char *src, const char *dst) { return sdcard_rename(src, dst); }
static bool napi_fs_is_dir(const char *path) {
    sdcard_stat_t st;
    return sdcard_stat(path, &st) && st.is_dir;
}

// As src/main.c's fs_list_dir: the app's callback takes name, is_dir, size.
static struct {
    void (*fn)(const char *, bool, uint32_t, void *);
    void *user;
} s_list_cb;

static void napi_list_dir_entry(const sdcard_entry_t *e, void *user) {
    (void)user;
    s_list_cb.fn(e->name, e->is_dir, e->size, s_list_cb.user);
}
static int napi_fs_list_dir(const char *path,
                            void (*cb)(const char *name, bool is_dir, uint32_t size, void *user),
                            void *user) {
    s_list_cb.fn = cb;
    s_list_cb.user = user;
    return sdcard_list_dir(path, napi_list_dir_entry, NULL);
}

static const picocalc_fs_t s_fs = {
    .open = napi_fs_open,
    .read = napi_fs_read,
    .write = napi_fs_write,
    .close = napi_fs_close,
    .exists = napi_fs_exists,
    .size = napi_fs_size,
    .fsize = napi_fs_fsize,
    .seek = napi_fs_seek,
    .tell = napi_fs_tell,
    .listDir = napi_fs_list_dir,
    .mkdir = napi_fs_mkdir,
    .deleteFile = napi_fs_delete,
    .renameFile = napi_fs_rename,
    .isDir = napi_fs_is_dir,
    .browse = NULL,
};

static const picocalc_audio_t s_audio = {
    .playTone = audio_play_tone,
    .stopTone = audio_stop_tone,
    .setVolume = audio_set_volume,
    .startStream = audio_start_stream,
    .stopStream = audio_stop_stream,
    .pushSamples = audio_push_samples,
};

void sim_native_api_wire(PicoCalcAPI *api) {
    api->display = &s_display;
    api->sys = &s_sys;
    api->input = &s_input;
    api->perf = &s_perf;
    api->fs = &s_fs;
    api->audio = &s_audio;
}

// ── App lifetime ───────────────────────────────────────────────────────────

void sim_native_app_begin(void) {
    s_exit = false;
    s_drawn = false;
    memset(s_panel_fb, 0, sizeof(s_panel_fb));
}

void sim_native_app_end(void) { s_exit = false; }
