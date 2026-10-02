#include "display.h"
#include "../hardware.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/interp.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "hardware/xip_cache.h"
#include "lcd_spi.pio.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../os/image_decoders.h"
#include "../fonts/font_registry.h"
// The image blitters build at -O2 (the firmware is -Os): these loops are where
// sprite/tilemap frame time goes, and -O2 unswitches and unrolls the
// specialised loops in display_clip.h. Only these: display.c as a whole at
// -O2 grows by ~8 KB of flash, all of it competing with the Lua VM for the
// 16 KB XIP cache.
#define DISP_HOT __attribute__((optimize("O2")))
#include "display_clip.h"
#include "display_raster.h"

// ── Framebuffer ──────────────────────────────────────────────────────────────
// Placed in internal SRAM smoothly now that the Lua heap has been relocated
// to PSRAM, freeing up 256KB of internal memory!
static uint16_t s_framebuffers[2][FB_WIDTH * FB_HEIGHT];

// Pointer to the current back buffer
static uint16_t *s_framebuffer = s_framebuffers[0];
static int s_back_buffer_idx = 0;
// Buffer most recently sent to the panel.  display_flush() presents the old
// back buffer (after swap); display_flush_rows() presents the current draw
// buffer without swapping.  Screenshot/readback must follow this, not the
// front-buffer index, or flushRows-only apps read back a stale frame.
static const uint16_t *s_last_presented = s_framebuffers[0];

// Shadow of the write-only VSCRSADD scroll register (see
// display_get_scroll_offset).  0 = identity (no hardware scroll).
// The write counter lets apps detect that someone else (system menu,
// launcher) wrote the register even when the value matches what they last
// set — e.g. a reset to 0 while the app's own offset was 0.
static int s_scroll_offset = 0;
static uint32_t s_scroll_offset_writes = 0;
static bool s_dma_active = false;

// DMA channel for LCD transfers
static int s_dma_chan = -1;
static uint s_pio_sm = 0;

// Transparent color key (0 = disabled)
static uint16_t s_transparent_color = 0;

// Clip rect (inclusive bounds). All pixel-writing primitives respect it
// except display_clear(), display_flush*(), the post-processing effects
// (whole-framebuffer by design), and the tgx rotated-blit path.
static int s_clip_x0 = 0, s_clip_y0 = 0;
static int s_clip_x1 = FB_WIDTH - 1, s_clip_y1 = FB_HEIGHT - 1;

// The clip rect in the form the shared rasterisers (display_clip.h) take.
static inline disp_clip_t cur_clip(void) {
  return (disp_clip_t){s_clip_x0, s_clip_y0, s_clip_x1, s_clip_y1};
}

// Host RGB565 -> the framebuffer's byte-swapped (panel) order.
static inline uint16_t fb_color(uint16_t c) { return disp_px(c, true); }

// ── Active font ─────────────────────────────────────────────────────────────
// All glyph data and the renderer live in src/fonts/. This driver only
// tracks which registry slot is active — the id, never a pointer: an app can
// unload the slot it has selected (font_registry_unload is in the native API),
// so the pointer is re-resolved on every use and a freed slot self-heals the
// selection back to the built-in 6x8.
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

// ── ST7365P Command set
// ───────────────────────────────────────────────────────

#define ST7365P_NOP 0x00
#define ST7365P_SWRST 0x01
#define ST7365P_SLPOUT 0x11
#define ST7365P_NORON 0x13
#define ST7365P_INVOFF 0x20
#define ST7365P_INVON 0x21 // Inversion on (may be needed depending on panel)
#define ST7365P_DISPON 0x29
#define ST7365P_CASET 0x2A
#define ST7365P_RASET 0x2B
#define ST7365P_RAMWR 0x2C
#define ST7365P_MADCTL 0x36
#define ST7365P_COLMOD 0x3A

// MADCTL bits
#define MADCTL_MX 0x40  // Mirror X
#define MADCTL_MY 0x80  // Mirror Y
#define MADCTL_MV 0x20  // Row/column exchange (landscape)
#define MADCTL_BGR 0x08 // BGR order (vs RGB)

// ── Low-level SPI helpers
// ─────────────────────────────────────────────────────

static inline void lcd_cs_low(void) { gpio_put(LCD_PIN_CS, 0); }
static inline void lcd_cs_high(void) { gpio_put(LCD_PIN_CS, 1); }
static inline void lcd_dc_cmd(void) { gpio_put(LCD_PIN_DC, 0); }
static inline void lcd_dc_data(void) { gpio_put(LCD_PIN_DC, 1); }

static inline void pio_spi_write8(uint8_t data) {
  pio_sm_put_blocking(LCD_PIO, s_pio_sm, (uint32_t)data << 24);
}

// Timeout for PIO SPI idle waits (100ms).  If the LCD ribbon cable is
// disconnected or the PIO state machine glitches, this prevents an infinite
// hang that would otherwise brick the device until power-cycle.
#define LCD_SPI_TIMEOUT_US 100000u

static inline bool lcd_spi_wait_idle(void) {
  absolute_time_t deadline = make_timeout_time_us(LCD_SPI_TIMEOUT_US);
  while (!pio_sm_is_tx_fifo_empty(LCD_PIO, s_pio_sm)) {
    if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
      printf("[LCD] TIMEOUT: TX FIFO not empty\n");
      return false;
    }
    tight_loop_contents();
  }
  uint32_t stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + s_pio_sm);
  LCD_PIO->fdebug = stall_mask;
  while (!(LCD_PIO->fdebug & stall_mask)) {
    if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
      printf("[LCD] TIMEOUT: PIO not stalled\n");
      return false;
    }
    tight_loop_contents();
  }

  // The FIFO is physically empty and has stalled the state machine, but the
  // hardware Output Shift Register (OSR) is still holding the final bit chunk
  // and actively clocking it out! We must wait a few more cycles to guarantee
  // the trailing bits exit the screen controller wire before deasserting CS.
  busy_wait_us(1);
  return true;
}

static void lcd_write_cmd(uint8_t cmd) {
  lcd_cs_low();
  lcd_dc_cmd();
  pio_spi_write8(cmd);
  lcd_spi_wait_idle();
  lcd_cs_high();
}

static void lcd_write_data(const uint8_t *data, size_t len) {
  lcd_cs_low();
  lcd_dc_data();
  for (size_t i = 0; i < len; i++)
    pio_spi_write8(data[i]);
  lcd_spi_wait_idle();
  lcd_cs_high();
}

static void lcd_write_byte(uint8_t b) { lcd_write_data(&b, 1); }

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  lcd_cs_low();

  lcd_dc_cmd();
  pio_spi_write8(ST7365P_CASET);
  lcd_spi_wait_idle();

  lcd_dc_data();
  pio_spi_write8(x0 >> 8);
  pio_spi_write8(x0 & 0xFF);
  pio_spi_write8(x1 >> 8);
  pio_spi_write8(x1 & 0xFF);
  lcd_spi_wait_idle();

  lcd_dc_cmd();
  pio_spi_write8(ST7365P_RASET);
  lcd_spi_wait_idle();

  lcd_dc_data();
  pio_spi_write8(y0 >> 8);
  pio_spi_write8(y0 & 0xFF);
  pio_spi_write8(y1 >> 8);
  pio_spi_write8(y1 & 0xFF);
  lcd_spi_wait_idle();

  lcd_dc_cmd();
  pio_spi_write8(ST7365P_RAMWR);
  lcd_spi_wait_idle();

  lcd_cs_high();
}

// ── Init sequence
// ───────────────────────────────────────────────────────────── Ported directly
// from the working constellation-pico Rust project (st7789.rs). No
// ST7796S-style 0xF0 manufacturer unlock — the panel responds to the standard
// ST7789 init sequence. Backlight is controlled by the STM32 keyboard MCU.

void display_apply_clock(void) {
  // Safety: if a DMA transfer is active, wait for it to finish before
  // changing the clock divider to avoid corrupting the current frame.
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }

  uint32_t clk_hz = clock_get_hz(clk_sys);
  // Always use integer divider to avoid PIO jitter from fractional dividers.
  // Round up so SPI clock never exceeds LCD_SPI_BAUD.
  uint32_t target = LCD_SPI_BAUD * 2;
  uint32_t int_div = (clk_hz + target - 1) / target;
  if (int_div < 1) int_div = 1;
  float clkdiv = (float)int_div;

  pio_sm_set_clkdiv(LCD_PIO, s_pio_sm, clkdiv);
}

void display_init(void) {
  // Initialize PIO for SPI master
  uint offset = pio_add_program(LCD_PIO, &lcd_spi_program);
  pio_sm_config cfg_pio = lcd_spi_program_get_default_config(offset);
  sm_config_set_out_pins(&cfg_pio, LCD_PIN_MOSI, 1);
  sm_config_set_sideset_pins(&cfg_pio, LCD_PIN_SCK);
  // Disable auto-pull. Manual PULL fetches new word. Shift from MSB.
  sm_config_set_out_shift(&cfg_pio, false, false, 32);
  sm_config_set_fifo_join(&cfg_pio, PIO_FIFO_JOIN_TX);
  uint32_t init_target = LCD_SPI_BAUD * 2;
  uint32_t init_div = (clock_get_hz(clk_sys) + init_target - 1) / init_target;
  if (init_div < 1) init_div = 1;
  sm_config_set_clkdiv(&cfg_pio, (float)init_div);

  s_pio_sm = pio_claim_unused_sm(LCD_PIO, true);
  pio_sm_init(LCD_PIO, s_pio_sm, offset, &cfg_pio);
  pio_sm_set_pins_with_mask(LCD_PIO, s_pio_sm, (1u << LCD_PIN_SCK),
                            (1u << LCD_PIN_SCK) | (1u << LCD_PIN_MOSI));
  pio_sm_set_pindirs_with_mask(LCD_PIO, s_pio_sm,
                               (1u << LCD_PIN_SCK) | (1u << LCD_PIN_MOSI),
                               (1u << LCD_PIN_SCK) | (1u << LCD_PIN_MOSI));
  pio_gpio_init(LCD_PIO, LCD_PIN_MOSI);
  pio_gpio_init(LCD_PIO, LCD_PIN_SCK);

  pio_sm_set_enabled(LCD_PIO, s_pio_sm, true);
  printf("[LCD] PIO SPI initialized (req baud: %lu)\n",
         (unsigned long)LCD_SPI_BAUD);

  // Control pins as GPIO outputs
  gpio_init(LCD_PIN_CS);
  gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
  lcd_cs_high();
  gpio_init(LCD_PIN_DC);
  gpio_set_dir(LCD_PIN_DC, GPIO_OUT);
  lcd_dc_cmd();
  gpio_init(LCD_PIN_RST);
  gpio_set_dir(LCD_PIN_RST, GPIO_OUT);

  // Hardware reset — pulse RST low for 10 ms, then high, wait 120 ms
  gpio_put(LCD_PIN_RST, 0);
  sleep_ms(10);
  gpio_put(LCD_PIN_RST, 1);
  sleep_ms(120);

  printf("[LCD] init start\n");

  // Software reset — wait 10 ms before further commands
  lcd_write_cmd(ST7365P_SWRST);
  sleep_ms(10);

  // Colour mode: 16-bit RGB565 (COLMOD)
  lcd_write_cmd(ST7365P_COLMOD);
  lcd_write_byte(0x55);

  // Memory access control: 0x48 (MADCTL_MX | MADCTL_BGR, matches Picoware)
  lcd_write_cmd(ST7365P_MADCTL);
  lcd_write_byte(0x48);

  // Inversion on (INVON)
  lcd_write_cmd(ST7365P_INVON);

  // Entry mode set (EMS, 0xB7)
  // Maps 16-bit to 18-bit color conversion correctly for ST7789
  lcd_write_cmd(0xB7);
  lcd_write_byte(0xC6);

  // Sleep out — display enters normal operation, wait 10 ms
  lcd_write_cmd(ST7365P_SLPOUT);
  sleep_ms(10);

  // Display on (DISPON)
  lcd_write_cmd(ST7365P_DISPON);

  printf("[LCD] init done, flushing black fill...\n");

  // DMA channel for framebuffer flushes.
  // Must use DMA_SIZE_8 with the PIO SPI to send bytes sequentially.
  // By writing to `&LCD_PIO->txf[s_pio_sm] + 3`, we place each byte in the
  // MSB of the TX FIFO, where our PIO program expects it.
  s_dma_chan = dma_claim_unused_channel(true);
  dma_channel_config cfg = dma_channel_get_default_config(s_dma_chan);
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
  channel_config_set_dreq(&cfg, pio_get_dreq(LCD_PIO, s_pio_sm, true));
  channel_config_set_read_increment(&cfg, true);
  channel_config_set_write_increment(&cfg, false);
  dma_channel_configure(s_dma_chan, &cfg,
                        ((volatile uint8_t *)&LCD_PIO->txf[s_pio_sm]) + 3,
                        s_framebuffer,
                        FB_SIZE, // FB_WIDTH * FB_HEIGHT * 2 bytes
                        false);

  // Fill black to confirm flush path works
  display_clear(COLOR_BLACK);
  display_flush();
  printf("[LCD] flush done\n");
}

void display_deinit(void) {
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }
  if (s_dma_chan >= 0)
    dma_channel_unclaim(s_dma_chan);
  pio_sm_set_enabled(LCD_PIO, s_pio_sm, false);
}

// ── Drawing functions
// ─────────────────────────────────────────────────────────

void display_clear(uint16_t color) {
  // Big-endian swap: ST7365P wants bytes big-endian over SPI
  uint16_t be = (color >> 8) | (color << 8);

  // Fast path: use memset for black (0x0000) or white (0xFFFF)
  if (be == 0x0000 || be == 0xFFFF) {
    memset(s_framebuffer, be & 0xFF, FB_SIZE);
    return;
  }

  // Optimized: fill in 32-pixel chunks for better memory bandwidth
  uint32_t color32 = ((uint32_t)be << 16) | be; // Two pixels
  uint32_t *fb32 = (uint32_t *)s_framebuffer;
  size_t count32 = (FB_WIDTH * FB_HEIGHT) / 2; // Number of 32-bit words

  for (size_t i = 0; i < count32; i++) {
    fb32[i] = color32;
  }
}

void display_set_pixel(int x, int y, uint16_t color) {
  if (x < s_clip_x0 || x > s_clip_x1 || y < s_clip_y0 || y > s_clip_y1)
    return;
  uint16_t be = (color >> 8) | (color << 8);
  s_framebuffer[y * FB_WIDTH + x] = be;
}

uint16_t display_get_pixel(int x, int y) {
  if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT)
    return 0;
  uint16_t be = s_framebuffer[y * FB_WIDTH + x];
  return (uint16_t)((be >> 8) | (be << 8));
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
  // Clipped once in int64 (x + w cannot overflow); full-width bands use
  // 32-bit stores.
  disp_clip_t c = cur_clip();
  disp_fill(s_framebuffer, FB_WIDTH, &c, x, y, w, h, fb_color(color));
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color) {
  display_fill_rect(x, y, w, 1, color);
  display_fill_rect(x, y + h - 1, w, 1, color);
  display_fill_rect(x, y, 1, h, color);
  display_fill_rect(x + w - 1, y, 1, h, color);
}

// Bresenham, started at the first step inside the clip and stopped after the
// last (display_clip.h): drawLine(0,0,1e9,1e9) costs 320 steps, not 1e9.
void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
  disp_clip_t c = cur_clip();
  disp_line(s_framebuffer, FB_WIDTH, &c, x0, y0, x1, y1, fb_color(color));
}

// Midpoint circle algorithm (outline only). Circles whose bounding box misses
// the clip are rejected; radii past DISP_CIRCLE_MIDPOINT_MAX (where the O(r)
// loop would spin for up to minutes) are drawn per visible row/column.
void display_draw_circle(int cx, int cy, int r, uint16_t color) {
  disp_clip_t c = cur_clip();
  if (disp_circle_rejected(&c, cx, cy, r)) return;
  if (r > DISP_CIRCLE_MIDPOINT_MAX) {
    disp_circle_big(s_framebuffer, FB_WIDTH, &c, cx, cy, r, fb_color(color));
    return;
  }
  int x = r, y = 0, d = 1 - r;
  while (x >= y) {
    display_set_pixel(cx + x, cy + y, color);
    display_set_pixel(cx - x, cy + y, color);
    display_set_pixel(cx + x, cy - y, color);
    display_set_pixel(cx - x, cy - y, color);
    display_set_pixel(cx + y, cy + x, color);
    display_set_pixel(cx - y, cy + x, color);
    display_set_pixel(cx + y, cy - x, color);
    display_set_pixel(cx - y, cy - x, color);
    y++;
    if (d <= 0) {
      d += 2 * y + 1;
    } else {
      x--;
      d += 2 * (y - x) + 1;
    }
  }
}

// Filled circle using horizontal spans (rejection and the large-radius path
// as for the outline above).
void display_fill_circle(int cx, int cy, int r, uint16_t color) {
  disp_clip_t c = cur_clip();
  if (disp_circle_rejected(&c, cx, cy, r)) return;
  if (r > DISP_CIRCLE_MIDPOINT_MAX) {
    disp_fill_circle_rows(s_framebuffer, FB_WIDTH, &c, cx, cy, r,
                          fb_color(color));
    return;
  }
  int x = r, y = 0, d = 1 - r;
  while (x >= y) {
    display_fill_rect(cx - x, cy + y, 2 * x + 1, 1, color);
    display_fill_rect(cx - x, cy - y, 2 * x + 1, 1, color);
    display_fill_rect(cx - y, cy + x, 2 * y + 1, 1, color);
    display_fill_rect(cx - y, cy - x, 2 * y + 1, 1, color);
    y++;
    if (d <= 0) {
      d += 2 * y + 1;
    } else {
      x--;
      d += 2 * (y - x) + 1;
    }
  }
}

// Fast vertical line fill — stride-based pointer walk, no Bresenham overhead.
void display_fill_vline(int x, int y0, int y1, uint16_t color) {
  if (x < s_clip_x0 || x > s_clip_x1)
    return;
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  if (y0 < s_clip_y0)
    y0 = s_clip_y0;
  if (y1 > s_clip_y1)
    y1 = s_clip_y1;
  if (y0 > y1)
    return;
  uint16_t be = (color >> 8) | (color << 8);
  uint16_t *p = &s_framebuffer[y0 * FB_WIDTH + x];
  for (int y = y0; y <= y1; y++, p += FB_WIDTH)
    *p = be;
}

// Draw a textured vertical column using 16.16 fixed-point texture sampling.
void display_draw_textured_column(int x, int y0, int y1,
                                  const uint16_t *tex, int tex_w, int tex_h,
                                  int tex_x, int tex_y0, int tex_y1) {
  if (x < s_clip_x0 || x > s_clip_x1 || y0 > y1 || !tex)
    return;
  if (tex_x < 0 || tex_x >= tex_w)
    return;

  // Total screen height and texture height for this column
  int screen_h = y1 - y0 + 1;
  int tex_span = tex_y1 - tex_y0 + 1;
  if (screen_h <= 0 || tex_span <= 0)
    return;

  // Fixed-point 16.16 step: how much texture to advance per screen pixel
  uint32_t step = ((uint32_t)tex_span << 16) / (uint32_t)screen_h;

  // Clip top
  uint32_t tex_pos = (uint32_t)tex_y0 << 16; // starting texture position
  if (y0 < s_clip_y0) {
    tex_pos += step * (uint32_t)(s_clip_y0 - y0);
    y0 = s_clip_y0;
  }
  if (y1 > s_clip_y1)
    y1 = s_clip_y1;
  if (y0 > y1)
    return;

  uint16_t *dst = &s_framebuffer[y0 * FB_WIDTH + x];
  for (int y = y0; y <= y1; y++, dst += FB_WIDTH) {
    int ty = (int)(tex_pos >> 16);
    if (ty < 0)
      ty = 0;
    if (ty >= tex_h)
      ty = tex_h - 1;
    uint16_t c = tex[ty * tex_w + tex_x];
    *dst = (c >> 8) | (c << 8); // byte-swap to big-endian
    tex_pos += step;
  }
}

// Gradient vertical line — interpolates RGB565 channels using fixed-point.
void display_fill_vline_gradient(int x, int y0, int y1, uint16_t color_top,
                                 uint16_t color_bottom) {
  if (x < s_clip_x0 || x > s_clip_x1)
    return;
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
    uint16_t tc = color_top;
    color_top = color_bottom;
    color_bottom = tc;
  }

  // Clip
  int orig_y0 = y0;
  if (y0 < s_clip_y0)
    y0 = s_clip_y0;
  if (y1 > s_clip_y1)
    y1 = s_clip_y1;
  if (y0 > y1)
    return;

  int span = (y1 == orig_y0) ? 1 : (y1 - orig_y0); // avoid divide by zero

  // Extract RGB565 channels
  int r0 = (color_top >> 11) & 0x1F, g0 = (color_top >> 5) & 0x3F,
      b0 = color_top & 0x1F;
  int r1 = (color_bottom >> 11) & 0x1F, g1 = (color_bottom >> 5) & 0x3F,
      b1 = color_bottom & 0x1F;

  uint16_t *p = &s_framebuffer[y0 * FB_WIDTH + x];
  for (int y = y0; y <= y1; y++, p += FB_WIDTH) {
    int t = y - orig_y0;
    int r = r0 + (r1 - r0) * t / span;
    int g = g0 + (g1 - g0) * t / span;
    int b = b0 + (b1 - b0) * t / span;
    uint16_t c = ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F);
    *p = (c >> 8) | (c << 8);
  }
}

// Scanline triangle: rows clipped to the clip rect before the loop, each row
// a span fill (display_clip.h keeps the original edge formulas).
void display_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                           uint16_t color) {
  disp_clip_t c = cur_clip();
  disp_fill_triangle(s_framebuffer, FB_WIDTH, &c, x0, y0, x1, y1, x2, y2,
                     fb_color(color));
}

void display_get_raster_target(display_raster_target_t *t) {
  t->fb = s_framebuffer;
  t->stride = FB_WIDTH;
  t->clip_x0 = s_clip_x0;
  t->clip_y0 = s_clip_y0;
  t->clip_x1 = s_clip_x1;
  t->clip_y1 = s_clip_y1;
  t->swap = true;
}

// Text goes through the shared renderer in src/fonts/font.c. The hardware
// framebuffer is byte-swapped, so colors are swapped here; the clip rect is
// the driver's own.
static int draw_text_fb(int x, int y, const char *text, uint16_t fg,
                        uint16_t bg, bool transparent) {
  uint16_t fg_be = (fg >> 8) | (fg << 8);
  uint16_t bg_be = (bg >> 8) | (bg << 8);
  return font_render(active_font(), s_framebuffer, FB_WIDTH,
                     s_clip_x0, s_clip_y0, s_clip_x1, s_clip_y1,
                     x, y, text, fg_be, bg_be, transparent);
}

int display_draw_text(int x, int y, const char *text, uint16_t fg,
                      uint16_t bg) {
  return draw_text_fb(x, y, text, fg, bg, false);
}

int display_draw_text_transparent(int x, int y, const char *text, uint16_t fg) {
  return draw_text_fb(x, y, text, fg, 0, true);
}

// Horizontal counterpart to display_fill_vline. Sugar over display_fill_rect,
// which is already the row-optimized path — this exists for call-site symmetry
// with fill_vline, not for speed.
void display_fill_hline(int y, int x0, int x1, uint16_t color) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  display_fill_rect(x0, y, x1 - x0 + 1, 1, color);
}

// Offscreen buffers are host byte order (no swap) and clip to their bounds.
int display_draw_text_to_buffer(uint16_t *buf, int buf_w, int buf_h,
                                int x, int y, const char *text,
                                uint16_t fg, uint16_t bg) {
  return font_render(active_font(), buf, buf_w, 0, 0, buf_w - 1, buf_h - 1,
                     x, y, text, fg, bg, false);
}

int display_text_width(const char *text) {
  return font_text_width(active_font(), text);
}

// Opaque blit of a whole w x h image (clipped once, paired byte swap).
void display_draw_image(int x, int y, int w, int h, const uint16_t *data) {
  disp_clip_t c = cur_clip();
  disp_blit(s_framebuffer, FB_WIDTH, &c, x, y, data, w, h, 0, 0, w, h, false,
            false, 0, true);
}

// Backs every image, sprite, tilemap and animation draw. The destination rect
// is clipped once, then one of four specialised loops runs (opaque/keyed x
// flipped/not; display_clip.h). transparent_color 0 means "use the global
// key"; a key of 0 draws opaque (black cannot be a colour key).
DISP_HOT void display_draw_image_partial(int x, int y, int img_w, int img_h,
                                const uint16_t *data, int sx, int sy, int sw,
                                int sh, bool flip_x, bool flip_y,
                                uint16_t transparent_color) {
  if (transparent_color == 0)
    transparent_color = s_transparent_color;
  disp_clip_t c = cur_clip();
  disp_blit(s_framebuffer, FB_WIDTH, &c, x, y, data, img_w, img_h, sx, sy, sw,
            sh, flip_x, flip_y, transparent_color, true);
}

void display_draw_image_scaled(int x, int y, int img_w, int img_h,
                               const uint16_t *data, float scale, float angle,
                               uint16_t transparent_color) {
  // If transparent_color is 0, use the global setting
  if (transparent_color == 0)
    transparent_color = s_transparent_color;

  // tgx_draw_image_scaled renders directly into the framebuffer using TGX's
  // native RGB565 format (little-endian).  Our framebuffer stores pixels
  // byte-swapped (big-endian) for the 8-bit DMA path, so we need to:
  //   1. Byte-swap the affected region to native LE so TGX math is correct.
  //   2. Let TGX render.
  //   3. Byte-swap the affected region back to BE for the DMA flush.

  // Calculate bounding box of transformed image to only swap affected region.
  // (x, y) is the top-left corner; compute centre for TGX and rotation.
  float half_w = img_w * scale * 0.5f;
  float half_h = img_h * scale * 0.5f;
  float cx = x + half_w;
  float cy = y + half_h;

  float cosa = cosf(angle);
  float sina = sinf(angle);

  // Transform all 4 corners and track min/max
  float corners[4][2] = {
    {-half_w, -half_h}, {half_w, -half_h}, {half_w, half_h}, {-half_w, half_h}
  };
  float min_x = cx, max_x = cx, min_y = cy, max_y = cy;
  for (int i = 0; i < 4; i++) {
    float tx = corners[i][0] * cosa - corners[i][1] * sina + cx;
    float ty = corners[i][0] * sina + corners[i][1] * cosa + cy;
    if (tx < min_x) min_x = tx;
    if (tx > max_x) max_x = tx;
    if (ty < min_y) min_y = ty;
    if (ty > max_y) max_y = ty;
  }

  // Clamp to framebuffer bounds. TGX rasterises with its own rounding and can
  // touch one row/column past the exact float box, so pad it by 1 px a side:
  // a pixel it writes outside the swapped region keeps the wrong byte order.
  int bx = (int)floorf(min_x) - 1;
  int by = (int)floorf(min_y) - 1;
  int bw = (int)ceilf(max_x - min_x) + 3;
  int bh = (int)ceilf(max_y - min_y) + 3;
  if (bx < 0) bx = 0;
  if (by < 0) by = 0;
  if (bx + bw > FB_WIDTH) bw = FB_WIDTH - bx;
  if (by + bh > FB_HEIGHT) bh = FB_HEIGHT - by;
  if (bw <= 0 || bh <= 0) return;

  // Byte-swap only the affected region
  uint16_t *fb = s_framebuffer;
  for (int row = by; row < by + bh; row++) {
    for (int col = bx; col < bx + bw; col++) {
      fb[row * FB_WIDTH + col] = (fb[row * FB_WIDTH + col] >> 8) | (fb[row * FB_WIDTH + col] << 8);
    }
  }

  // TGX takes the rotation in degrees; the bounding box above and the Lua API
  // use radians.
  const float angle_deg = angle * (180.0f / (float)M_PI);

  // Use masked version if transparency is enabled, otherwise use regular
  // version.  The clip is passed through (converted from the driver's
  // inclusive bounds to the decoder's half-open rect) so TGX renders into a
  // sub-view of the framebuffer and cannot write outside it.
  if (transparent_color != 0) {
    tgx_draw_image_scaled_masked(fb, FB_WIDTH, FB_HEIGHT,
                                 s_clip_x0, s_clip_y0,
                                 s_clip_x1 + 1, s_clip_y1 + 1,
                                 data, img_w, img_h, cx, cy,
                                 scale, angle_deg, transparent_color);
  } else {
    tgx_draw_image_scaled(fb, FB_WIDTH, FB_HEIGHT,
                          s_clip_x0, s_clip_y0,
                          s_clip_x1 + 1, s_clip_y1 + 1,
                          data, img_w, img_h, cx, cy,
                          scale, angle_deg);
  }

  // Byte-swap back only the affected region
  for (int row = by; row < by + bh; row++) {
    for (int col = bx; col < bx + bw; col++) {
      fb[row * FB_WIDTH + col] = (fb[row * FB_WIDTH + col] >> 8) | (fb[row * FB_WIDTH + col] << 8);
    }
  }
}

// Native drawImageNN: integer nearest-neighbour upscale, clipped once
// (display_clip.h). Negative offsets that are not a multiple of scale start
// mid-block; they used to be rounded to a block boundary and wrote rows above
// the framebuffer.
DISP_HOT void display_draw_image_nn(int x, int y, const uint16_t *data,
                           int src_w, int src_h, int scale) {
  disp_clip_t c = cur_clip();
  disp_blit_nn(s_framebuffer, FB_WIDTH, &c, x, y, data, src_w, src_h, scale,
               true);
}

void display_blit_be(int x, int y, const uint16_t *data, int w, int h) {
  if (!data || w <= 0 || h <= 0)
    return;

  // Early reject if entirely outside the clip rect
  if (x > s_clip_x1 || y > s_clip_y1 || x + w - 1 < s_clip_x0 || y + h - 1 < s_clip_y0)
    return;

  // Clip source region
  int src_x0 = 0, src_y0 = 0;
  int src_x1 = w, src_y1 = h;
  int dst_x = x, dst_y = y;

  if (dst_x < s_clip_x0) { src_x0 = s_clip_x0 - dst_x; dst_x = s_clip_x0; }
  if (dst_y < s_clip_y0) { src_y0 = s_clip_y0 - dst_y; dst_y = s_clip_y0; }
  if (dst_x + (src_x1 - src_x0) - 1 > s_clip_x1) src_x1 = src_x0 + (s_clip_x1 - dst_x + 1);
  if (dst_y + (src_y1 - src_y0) - 1 > s_clip_y1) src_y1 = src_y0 + (s_clip_y1 - dst_y + 1);

  int copy_w = src_x1 - src_x0;
  int copy_h = src_y1 - src_y0;
  if (copy_w <= 0 || copy_h <= 0)
    return;

  uint16_t *fb = s_framebuffer;
  for (int row = 0; row < copy_h; row++) {
    memcpy(&fb[(dst_y + row) * FB_WIDTH + dst_x],
           &data[(src_y0 + row) * w + src_x0],
           copy_w * sizeof(uint16_t));
  }
}

// Nearest-neighbour scale to dst_w x dst_h, clipped once. Source pixel
// (dx*src_w/dst_w, dy*src_h/dst_h) exactly — the old 16.16 ratio truncated
// 65536/3 and so sampled the wrong column at every multiple of 3 for scale 3.
DISP_HOT void display_draw_image_scaled_nn(int x, int y, const uint16_t *data,
                                   int src_w, int src_h, int dst_w, int dst_h,
                                   uint16_t transparent_color) {
  if (transparent_color == 0)
    transparent_color = s_transparent_color;
  disp_clip_t c = cur_clip();
  disp_blit_scaled(s_framebuffer, FB_WIDTH, &c, x, y, data, src_w, src_h,
                   dst_w, dst_h, transparent_color, true);
}

DISP_HOT void display_draw_image_stretched(int x, int y, int dst_w, int dst_h,
                                           const uint16_t *data, int img_w,
                                           int img_h, int sx, int sy, int sw,
                                           int sh, uint16_t transparent_color) {
  if (sx < 0) { sw += sx; sx = 0; }
  if (sy < 0) { sh += sy; sy = 0; }
  if (sw > img_w - sx) sw = img_w - sx;
  if (sh > img_h - sy) sh = img_h - sy;
  if (sw <= 0 || sh <= 0) return;
  uint16_t key = transparent_color ? transparent_color : s_transparent_color;
  disp_clip_t c = cur_clip();
  disp_blit_scaled_rect(s_framebuffer, FB_WIDTH, &c, x, y, data, img_w, sx, sy,
                        sw, sh, dst_w, dst_h, key, true);
}

void display_set_transparent_color(uint16_t color) {
  s_transparent_color = color;
}

uint16_t display_get_transparent_color(void) {
  return s_transparent_color;
}

// ── Clip rect ────────────────────────────────────────────────────────────────

void display_set_clip_rect(int x, int y, int w, int h) {
  int x1 = x + w - 1;
  int y1 = y + h - 1;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > FB_WIDTH - 1) x1 = FB_WIDTH - 1;
  if (y1 > FB_HEIGHT - 1) y1 = FB_HEIGHT - 1;
  if (x1 < x || y1 < y) {
    // Empty rect: clamp to a 1x1 region outside all writes is impossible,
    // so collapse to a zero-size marker at (0,0)-(-1,-1)
    x = 0; y = 0; x1 = -1; y1 = -1;
  }
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
  s_clip_x1 = FB_WIDTH - 1; s_clip_y1 = FB_HEIGHT - 1;
}

// ── Mode 7 (perspective ground plane) ───────────────────────────────────────
// Renders `tex` as a ground plane seen from a camera at (cam_x, cam_y),
// `cam_z` units above the plane, facing `angle` radians (0 = toward +Y in
// texture space, rotating clockwise). Rows below `horizon_y` are filled;
// `scale` tunes the field of view (larger = further view / smaller texels).
// Power-of-two texture dimensions wrap seamlessly; other sizes clamp at edges.
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
  // Forward (0 rad → +Y in texture space) and right vectors
  const float fwd_x = -sin_a, fwd_y = cos_a;
  const float right_x = cos_a, right_y = sin_a;

  int y0 = horizon_y + 1;
  if (y0 < s_clip_y0) y0 = s_clip_y0;
  if (y0 < 0) y0 = 0;
  int y1 = s_clip_y1;
  if (y1 > FB_HEIGHT - 1) y1 = FB_HEIGHT - 1;
  if (y0 > y1) return;

  const int cx0 = s_clip_x0 < 0 ? 0 : s_clip_x0;
  const int cx1 = s_clip_x1 > FB_WIDTH - 1 ? FB_WIDTH - 1 : s_clip_x1;
  const int half_w = FB_WIDTH / 2;

  for (int y = y0; y <= y1; y++) {
    const int p = y - horizon_y;  // > 0
    // Depth of this scanline along the ground
    const float z = cam_z * scale / (float)p;

    // World position at the centre of the row
    const float center_x = cam_x + fwd_x * z;
    const float center_y = cam_y + fwd_y * z;

    // World-units per screen pixel along the row
    const float step_x = right_x * z / scale;
    const float step_y = right_y * z / scale;

    // 16.16 fixed point for the inner loop; start at the clip left edge
    int32_t fx = (int32_t)((center_x + (cx0 - half_w) * step_x) * 65536.0f);
    int32_t fy = (int32_t)((center_y + (cx0 - half_w) * step_y) * 65536.0f);
    const int32_t dx = (int32_t)(step_x * 65536.0f);
    const int32_t dy = (int32_t)(step_y * 65536.0f);

    uint16_t *row = &s_framebuffer[y * FB_WIDTH + cx0];
    if (pow2) {
      for (int x = cx0; x <= cx1; x++) {
        const uint16_t c = tex[((fy >> 16) & mask_h) * tex_w + ((fx >> 16) & mask_w)];
        *row++ = (c >> 8) | (c << 8);
        fx += dx;
        fy += dy;
      }
    } else {
      for (int x = cx0; x <= cx1; x++) {
        int tx = fx >> 16, ty = fy >> 16;
        if (tx < 0) tx = 0; else if (tx >= tex_w) tx = tex_w - 1;
        if (ty < 0) ty = 0; else if (ty >= tex_h) ty = tex_h - 1;
        const uint16_t c = tex[ty * tex_w + tx];
        *row++ = (c >> 8) | (c << 8);
        fx += dx;
        fy += dy;
      }
    }
  }
}

// When true, display_flush() blocks until DMA completes before returning.
// Previously set for native PSRAM apps under the assumption that DMA and CPU
// instruction fetches share the same bus.  In reality, DMA reads from SRAM
// (AHB) while CPU fetches app code from PSRAM (QMI/XIP) — no contention.
// Double buffering already prevents CPU/DMA conflicts on the framebuffer.
// Left as a safety valve but no longer set by the native loader.
bool g_display_flush_blocking = false;

void display_flush(void) {
  if (s_dma_active) {
    // Wait for the previous frame's DMA to finish before starting the next.
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }

  // Swap buffers
  int front_buffer_idx = s_back_buffer_idx;
  s_back_buffer_idx = 1 - s_back_buffer_idx;
  s_framebuffer = s_framebuffers[s_back_buffer_idx];

  lcd_set_window(0, 0, FB_WIDTH - 1, FB_HEIGHT - 1);

  lcd_cs_low();
  lcd_dc_data();

  dma_channel_set_read_addr(s_dma_chan, s_framebuffers[front_buffer_idx],
                            false);
  dma_channel_set_trans_count(s_dma_chan, FB_SIZE, true); // start transfer
  s_dma_active = true;
  s_last_presented = s_framebuffers[front_buffer_idx];

  if (g_display_flush_blocking) {
    // Native PSRAM apps: must not return while DMA holds the SPI bus,
    // or the CPU will hard-fault trying to fetch instructions from PSRAM.
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }
  // Non-blocking path: DMA runs concurrently while CPU executes Lua/OS code.
}

void display_wait_for_flush(void) {
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }
}

static void flush_region_impl(int y0, int y1, bool sync_back) {
  // Clamp to valid range
  if (y0 < 0) y0 = 0;
  if (y1 >= FB_HEIGHT) y1 = FB_HEIGHT - 1;
  if (y0 > y1) return;

  // Wait for previous DMA
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }

  // Swap buffers (same as display_flush)
  int front_buffer_idx = s_back_buffer_idx;
  s_back_buffer_idx = 1 - s_back_buffer_idx;
  s_framebuffer = s_framebuffers[s_back_buffer_idx];

  // Set partial window
  lcd_set_window(0, y0, FB_WIDTH - 1, y1);

  lcd_cs_low();
  lcd_dc_data();

  int row_count = y1 - y0 + 1;
  dma_channel_set_read_addr(s_dma_chan,
                            &s_framebuffers[front_buffer_idx][y0 * FB_WIDTH],
                            false);
  dma_channel_set_trans_count(s_dma_chan,
                              row_count * FB_WIDTH * sizeof(uint16_t), true);
  s_dma_active = true;

  if (!sync_back) return;

  // Copy the flushed region to the new back buffer so both buffers stay in
  // sync.  Without this, the back buffer has stale content from 2 frames ago,
  // causing visible flickering when overlays (header, FPS) are drawn after
  // the flush, or when the next iteration does a full display_flush().
  // DMA reads the front buffer via AHB; this memcpy also reads it — no
  // conflict (both are reads from SRAM).
  memcpy(&s_framebuffers[s_back_buffer_idx][y0 * FB_WIDTH],
         &s_framebuffers[front_buffer_idx][y0 * FB_WIDTH],
         row_count * FB_WIDTH * sizeof(uint16_t));
}

void display_flush_region(int y0, int y1) {
  flush_region_impl(y0, y1, true);
}

// Same as display_flush_region but skips the front→back sync memcpy.
// Only correct when the caller repaints the ENTIRE region into the back
// buffer before the next flush (the video player decodes every pixel of
// the region each frame, so the ~200KB/frame copy is pure waste there).
void display_flush_region_nocopy(int y0, int y1) {
  flush_region_impl(y0, y1, false);
}

// One-shot front→back sync of a row band.  Callers that used
// display_flush_region_nocopy() during an animation must call this when the
// animation stops (e.g. video pause), otherwise the two buffers hold
// different frames and subsequent full display_flush() calls flicker.
void display_sync_back_region(int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 >= FB_HEIGHT) y1 = FB_HEIGHT - 1;
  if (y0 > y1) return;

  display_wait_for_flush();  // front buffer must be stable before copying

  int front_buffer_idx = 1 - s_back_buffer_idx;
  int row_count = y1 - y0 + 1;
  memcpy(&s_framebuffers[s_back_buffer_idx][y0 * FB_WIDTH],
         &s_framebuffers[front_buffer_idx][y0 * FB_WIDTH],
         row_count * FB_WIDTH * sizeof(uint16_t));
}


void display_flush_rows(int y0, int y1) {
  // Clamp to valid range
  if (y0 < 0) y0 = 0;
  if (y1 >= FB_HEIGHT) y1 = FB_HEIGHT - 1;
  if (y0 > y1) return;

  // Wait for any previous DMA transfer to complete
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }

  // Target only the row band on the LCD
  lcd_set_window(0, y0, FB_WIDTH - 1, y1);

  lcd_cs_low();
  lcd_dc_data();

  int row_count = y1 - y0 + 1;
  dma_channel_set_read_addr(s_dma_chan,
                            &s_framebuffer[y0 * FB_WIDTH], false);
  dma_channel_set_trans_count(s_dma_chan,
                              row_count * FB_WIDTH * sizeof(uint16_t), true);
  s_dma_active = true;
  s_last_presented = s_framebuffer;
  // Non-blocking: no buffer swap — caller uses display_flush() for that.
}

void display_push_rect(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > FB_WIDTH) w = FB_WIDTH - x;
  if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
  if (w <= 0 || h <= 0) return;

  display_wait_for_flush();
  lcd_set_window(x, y, x + w - 1, y + h - 1);
  lcd_cs_low();
  lcd_dc_data();
  // Bytes as stored: the framebuffer is already in panel byte order.
  for (int r = y; r < y + h; r++) {
    const uint8_t *p = (const uint8_t *)&s_framebuffer[r * FB_WIDTH + x];
    for (int i = 0; i < w * 2; i++)
      pio_spi_write8(p[i]);
  }
  lcd_spi_wait_idle();
  lcd_cs_high();
}

void display_set_brightness(uint8_t brightness) {
  // Backlight is controlled by the STM32 keyboard MCU (kbd_set_backlight).
  // This function is a no-op on PicoCalc v2.0.
  (void)brightness;
}

void display_darken(void) {
  // If a DMA transfer is in progress, wait for it to finish and end the SPI
  // transaction before we read the front buffer.
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }

  // With double buffering, we want to create a stable, darkened snapshot of the
  // currently visible screen (the front buffer) and place it into BOTH buffers.
  // This ensures that when an overlay app (like the system menu) redraws and
  // flushes, it doesn't alternate between a darkened and a bright background.
  uint16_t *front = s_framebuffers[1 - s_back_buffer_idx];
  uint16_t *back = s_framebuffers[s_back_buffer_idx];
  size_t n = (FB_WIDTH * FB_HEIGHT);

  for (size_t i = 0; i < n; i++) {
    uint16_t p = front[i];
    uint16_t native = (p >> 8) | (p << 8); // unswap
    native = (native >> 1) &
             0xFBEF; // darken (clear bits that shift across channels)
    uint16_t darkened = (native >> 8) | (native << 8); // reswap
    front[i] = darkened;
    back[i] = darkened;
  }
}

void display_save_buffers(uint16_t *dst) {
  const size_t n = FB_WIDTH * FB_HEIGHT;
  display_wait_for_flush();
  memcpy(dst, s_framebuffers[1 - s_back_buffer_idx], n * sizeof(uint16_t));
  memcpy(dst + n, s_framebuffers[s_back_buffer_idx], n * sizeof(uint16_t));
}

void display_restore_buffers(const uint16_t *src) {
  const size_t n = FB_WIDTH * FB_HEIGHT;
  display_wait_for_flush();
  // The saved front goes into the back buffer, and the flush swaps and
  // presents it: it is the front buffer again, and on the panel.
  memcpy(s_framebuffers[s_back_buffer_idx], src, n * sizeof(uint16_t));
  display_flush();
  // The saved back goes into the new back buffer. The DMA just started
  // reads only the front, so this copy can run alongside it.
  memcpy(s_framebuffers[s_back_buffer_idx], src + n, n * sizeof(uint16_t));
}

// =============================================================================
// Framebuffer Effects
// =============================================================================

// NOTE: Effects operate on s_framebuffer (the back buffer). DMA reads from the
// front buffer (the other one). No DMA wait is needed — the two buffers don't
// overlap. display_flush() swaps buffers atomically after DMA completes.

// Unswap/reswap macros for BE framebuffer ↔ native RGB565
#define FB_UNSWAP(p) (((p) >> 8) | ((p) << 8))
#define FB_RESWAP(p) FB_UNSWAP(p)

void display_effect_invert(void) {

  uint16_t *fb = s_framebuffer;
  size_t n = FB_WIDTH * FB_HEIGHT;
  // XOR works directly on byte-swapped pixels (bit-level complement)
  uint32_t *fb32 = (uint32_t *)fb;
  for (size_t i = 0; i < n / 2; i++) {
    fb32[i] ^= 0xFFFFFFFF;
  }
}

void display_effect_darken(uint8_t factor) {

  uint16_t *fb = s_framebuffer;
  size_t n = FB_WIDTH * FB_HEIGHT;

  if (factor == 0) {
    // Full black
    memset(fb, 0, n * sizeof(uint16_t));
    return;
  }
  if (factor >= 255) return; // No change

  // Use interpolator BLEND mode for per-channel darkening.
  // blend = pixel * factor / 256 (lerp toward black)
  interp_config cfg0 = interp_default_config();
  interp_config_set_blend(&cfg0, true);
  interp_set_config(interp0, 0, &cfg0);

  interp_config cfg1 = interp_default_config();
  interp_set_config(interp0, 1, &cfg1);

  // Blend factor in accum[1]: 0=100% base0, 255=100% base1
  // We want: result = pixel * factor / 256
  // Set base[1] = 0 (black), base[0] = channel, accum[1] = 255 - factor
  // Then result = pixel * (255 - (255-factor)) / 256... no, BLEND mode:
  // result = (base0 * (255 - accum1) + base1 * accum1) >> 8
  // We want: base0=channel, base1=0, accum1=(255-factor)
  // result = channel * (255 - (255-factor)) / 256 = channel * factor / 256
  interp0->accum[1] = 255 - factor;

  for (size_t i = 0; i < n; i++) {
    uint16_t native = FB_UNSWAP(fb[i]);
    uint8_t r = (native >> 11) & 0x1F;
    uint8_t g = (native >> 5) & 0x3F;
    uint8_t b = native & 0x1F;

    interp0->base[0] = r;
    interp0->base[1] = 0;
    r = interp0->peek[1];

    interp0->base[0] = g;
    g = interp0->peek[1];

    interp0->base[0] = b;
    b = interp0->peek[1];

    fb[i] = FB_RESWAP((r << 11) | (g << 5) | b);
  }
}

void display_effect_brighten(uint8_t factor) {

  uint16_t *fb = s_framebuffer;
  size_t n = FB_WIDTH * FB_HEIGHT;

  if (factor == 0) return; // No change

  // Blend toward white: result = pixel + (max - pixel) * factor / 256
  // Using interpolator: base0=pixel, base1=max, accum1=factor
  interp_config cfg0 = interp_default_config();
  interp_config_set_blend(&cfg0, true);
  interp_set_config(interp0, 0, &cfg0);

  interp_config cfg1 = interp_default_config();
  interp_set_config(interp0, 1, &cfg1);

  interp0->accum[1] = factor;

  for (size_t i = 0; i < n; i++) {
    uint16_t native = FB_UNSWAP(fb[i]);
    uint8_t r = (native >> 11) & 0x1F;
    uint8_t g = (native >> 5) & 0x3F;
    uint8_t b = native & 0x1F;

    interp0->base[0] = r;
    interp0->base[1] = 31; // max 5-bit
    r = interp0->peek[1];

    interp0->base[0] = g;
    interp0->base[1] = 63; // max 6-bit
    g = interp0->peek[1];

    interp0->base[0] = b;
    interp0->base[1] = 31; // max 5-bit
    b = interp0->peek[1];

    fb[i] = FB_RESWAP((r << 11) | (g << 5) | b);
  }
}

void display_effect_tint(uint8_t r_tint, uint8_t g_tint, uint8_t b_tint,
                         uint8_t strength) {

  if (strength == 0) return;

  uint16_t *fb = s_framebuffer;
  size_t n = FB_WIDTH * FB_HEIGHT;

  // Convert 8-bit tint to RGB565 channel widths
  uint8_t tr = r_tint >> 3;  // 5-bit
  uint8_t tg = g_tint >> 2;  // 6-bit
  uint8_t tb = b_tint >> 3;  // 5-bit

  // Interpolator BLEND: result = base0*(255-accum1)/256 + base1*accum1/256
  // base0 = pixel channel, base1 = tint channel, accum1 = strength
  interp_config cfg0 = interp_default_config();
  interp_config_set_blend(&cfg0, true);
  interp_set_config(interp0, 0, &cfg0);

  interp_config cfg1 = interp_default_config();
  interp_set_config(interp0, 1, &cfg1);

  interp0->accum[1] = strength;

  for (size_t i = 0; i < n; i++) {
    uint16_t native = FB_UNSWAP(fb[i]);
    uint8_t r = (native >> 11) & 0x1F;
    uint8_t g = (native >> 5) & 0x3F;
    uint8_t b = native & 0x1F;

    interp0->base[0] = r;
    interp0->base[1] = tr;
    r = interp0->peek[1];

    interp0->base[0] = g;
    interp0->base[1] = tg;
    g = interp0->peek[1];

    interp0->base[0] = b;
    interp0->base[1] = tb;
    b = interp0->peek[1];

    fb[i] = FB_RESWAP((r << 11) | (g << 5) | b);
  }
}

void display_effect_grayscale(void) {

  uint16_t *fb = s_framebuffer;
  size_t n = FB_WIDTH * FB_HEIGHT;

  for (size_t i = 0; i < n; i++) {
    uint16_t native = FB_UNSWAP(fb[i]);
    uint8_t r = (native >> 11) & 0x1F;
    uint8_t g = (native >> 5) & 0x3F;
    uint8_t b = native & 0x1F;

    // ITU-R BT.601 luma: Y = 0.299*R + 0.587*G + 0.114*B
    // Scale to 5-bit: use 6-bit green scaled to 5-bit range
    // Approximate with integer: Y5 = (r*77 + (g>>1)*150 + b*29) >> 8
    uint8_t g5 = g >> 1; // 6-bit green → 5-bit for uniform weighting
    uint8_t luma = (r * 77 + g5 * 150 + b * 29) >> 8;
    if (luma > 31) luma = 31;
    uint8_t luma6 = luma << 1; // back to 6-bit for green channel

    fb[i] = FB_RESWAP((luma << 11) | (luma6 << 5) | luma);
  }
}

void display_effect_blend(const uint16_t *src, int w, int h, uint8_t alpha) {

  if (alpha == 0) return; // Fully transparent, no change

  uint16_t *fb = s_framebuffer;

  interp_config cfg0 = interp_default_config();
  interp_config_set_blend(&cfg0, true);
  interp_set_config(interp0, 0, &cfg0);

  interp_config cfg1 = interp_default_config();
  interp_set_config(interp0, 1, &cfg1);

  interp0->accum[1] = alpha;

  // Blend src image onto framebuffer, clipped to screen bounds
  int max_h = (h < FB_HEIGHT) ? h : FB_HEIGHT;
  int max_w = (w < FB_WIDTH) ? w : FB_WIDTH;

  for (int y = 0; y < max_h; y++) {
    for (int x = 0; x < max_w; x++) {
      int idx = y * FB_WIDTH + x;
      uint16_t fb_native = FB_UNSWAP(fb[idx]);
      // src is in host byte order (same as RGB565 macro)
      uint16_t src_native = src[y * w + x];

      uint8_t fr = (fb_native >> 11) & 0x1F;
      uint8_t fg = (fb_native >> 5) & 0x3F;
      uint8_t fbb = fb_native & 0x1F;

      uint8_t sr = (src_native >> 11) & 0x1F;
      uint8_t sg = (src_native >> 5) & 0x3F;
      uint8_t sb = src_native & 0x1F;

      // result = fb*(255-alpha)/256 + src*alpha/256
      interp0->base[0] = fr;
      interp0->base[1] = sr;
      uint8_t r = interp0->peek[1];

      interp0->base[0] = fg;
      interp0->base[1] = sg;
      uint8_t g = interp0->peek[1];

      interp0->base[0] = fbb;
      interp0->base[1] = sb;
      uint8_t b = interp0->peek[1];

      fb[idx] = FB_RESWAP((r << 11) | (g << 5) | b);
    }
  }
}

void display_effect_palette(const uint16_t *lut, int lut_size) {

  if (!lut || lut_size <= 0) return;

  uint16_t *fb = s_framebuffer;
  size_t n = FB_WIDTH * FB_HEIGHT;

  for (size_t i = 0; i < n; i++) {
    uint16_t native = FB_UNSWAP(fb[i]);
    // Map RGB565 to an index: use upper bits of each channel
    // Simple approach: (r3 << 5) | (g3 << 2) | b2 → 8-bit index (256 entries)
    uint8_t r = (native >> 13) & 0x07; // top 3 bits of red
    uint8_t g = (native >> 8) & 0x07;  // top 3 bits of green
    uint8_t b = (native >> 3) & 0x03;  // top 2 bits of blue
    int idx = (r << 5) | (g << 2) | b;
    if (idx >= lut_size) idx = lut_size - 1;
    // LUT entries are in host byte order
    fb[i] = FB_RESWAP(lut[idx]);
  }
}

void display_effect_dither(uint8_t levels) {

  if (levels < 2) levels = 2;
  if (levels > 32) levels = 32;

  uint16_t *fb = s_framebuffer;

  // Bayer 4×4 ordered dither matrix (0-15, normalized to 0-255 range)
  static const uint8_t bayer4[4][4] = {
    {  0, 128,  32, 160},
    {192,  64, 224,  96},
    { 48, 176,  16, 144},
    {240, 112, 208,  80}
  };

  for (int y = 0; y < FB_HEIGHT; y++) {
    for (int x = 0; x < FB_WIDTH; x++) {
      uint16_t native = FB_UNSWAP(fb[y * FB_WIDTH + x]);
      uint8_t r = (native >> 11) & 0x1F;
      uint8_t g = (native >> 5) & 0x3F;
      uint8_t b = native & 0x1F;

      uint8_t threshold = bayer4[y & 3][x & 3];

      // Quantize each channel: scale to 0-255, add threshold, quantize, scale back
      // Red (5-bit → 8-bit → quantize → 5-bit)
      int r8 = (r << 3) | (r >> 2);
      r8 = r8 + (threshold / levels) - 128 / levels;
      if (r8 < 0) r8 = 0;
      if (r8 > 255) r8 = 255;
      r = (uint8_t)(r8 / (256 / levels)) * (255 / (levels - 1));
      r >>= 3;
      if (r > 31) r = 31;

      // Green (6-bit → 8-bit → quantize → 6-bit)
      int g8 = (g << 2) | (g >> 4);
      g8 = g8 + (threshold / levels) - 128 / levels;
      if (g8 < 0) g8 = 0;
      if (g8 > 255) g8 = 255;
      g = (uint8_t)(g8 / (256 / levels)) * (255 / (levels - 1));
      g >>= 2;
      if (g > 63) g = 63;

      // Blue (5-bit)
      int b8 = (b << 3) | (b >> 2);
      b8 = b8 + (threshold / levels) - 128 / levels;
      if (b8 < 0) b8 = 0;
      if (b8 > 255) b8 = 255;
      b = (uint8_t)(b8 / (256 / levels)) * (255 / (levels - 1));
      b >>= 3;
      if (b > 31) b = 31;

      fb[y * FB_WIDTH + x] = FB_RESWAP((r << 11) | (g << 5) | b);
    }
  }
}

void display_effect_scanline(uint8_t intensity) {

  if (intensity == 0) return;

  uint16_t *fb = s_framebuffer;

  // Fast bit-shift scanline: darken every odd row.
  // Works directly on byte-swapped pixels — the shift+mask approach is
  // endian-agnostic for halving luminance. Process 2 pixels at a time
  // using 32-bit ops for ~2x throughput.
  //
  // intensity controls how many times we halve:
  //   1-127   = 1 shift (50% brightness on scanlines)
  //   128-254 = 2 shifts (25% brightness on scanlines)
  //   255     = black scanlines
  int shifts = (intensity < 128) ? 1 : (intensity < 255) ? 2 : 0;

  if (intensity == 255) {
    // Full black scanlines — memset alternate rows
    for (int y = 1; y < FB_HEIGHT; y += 2) {
      memset(&fb[y * FB_WIDTH], 0, FB_WIDTH * sizeof(uint16_t));
    }
    return;
  }

  // RGB565 halving mask: prevents bits from bleeding across channel boundaries.
  // Native RGB565: RRRRRGGG GGGBBBBB → halve mask = 0x7BEF
  // Byte-swapped:  GGGBBBBB RRRRRGGG → halve mask = 0xEF7B
  // For 32-bit (two swapped pixels): 0xEF7BEF7B
  const uint32_t hmask = 0xEF7BEF7Bu;

  for (int y = 1; y < FB_HEIGHT; y += 2) {
    uint32_t *row32 = (uint32_t *)&fb[y * FB_WIDTH];
    int n32 = FB_WIDTH / 2;
    for (int i = 0; i < n32; i++) {
      uint32_t v = row32[i];
      for (int s = 0; s < shifts; s++) {
        v = (v >> 1) & hmask;
      }
      row32[i] = v;
    }
  }
}

void display_effect_posterize(uint8_t levels) {

  if (levels < 2) levels = 2;
  if (levels > 32) levels = 32;

  uint16_t *fb = s_framebuffer;
  size_t n = FB_WIDTH * FB_HEIGHT;

  // Precompute 5-bit and 6-bit quantization LUTs
  uint8_t lut5[32], lut6[64];
  for (int i = 0; i < 32; i++) {
    int q = (i * (levels - 1) + 15) / 31;  // round
    lut5[i] = (q * 31 + (levels - 1) / 2) / (levels - 1);
  }
  for (int i = 0; i < 64; i++) {
    int q = (i * (levels - 1) + 31) / 63;
    lut6[i] = (q * 63 + (levels - 1) / 2) / (levels - 1);
  }

  for (size_t i = 0; i < n; i++) {
    uint16_t native = FB_UNSWAP(fb[i]);
    uint8_t r = lut5[(native >> 11) & 0x1F];
    uint8_t g = lut6[(native >> 5) & 0x3F];
    uint8_t b = lut5[native & 0x1F];
    fb[i] = FB_RESWAP((r << 11) | (g << 5) | b);
  }
}

const uint16_t *display_get_framebuffer(void) { return s_framebuffer; }

uint16_t *display_get_back_buffer(void) { return s_framebuffer; }

// ── Bulk back-buffer pixel blocks (see display.h) ────────────────────────────
// The framebuffer holds byte-swapped RGB565; both directions convert here so
// callers deal in host-order values.

bool display_set_pixels_block(int x, int y, int w, int h,
                              const void *rgb565_host, size_t len) {
  if (w <= 0 || h <= 0) return false;
  if (x < 0 || y < 0 || x + w > FB_WIDTH || y + h > FB_HEIGHT) return false;
  if (len != (size_t)w * (size_t)h * 2u) return false;

  const uint8_t *src = (const uint8_t *)rgb565_host;
  bool wrote_any = false;
  for (int row = 0; row < h; row++) {
    const uint8_t *s = src + (size_t)row * (size_t)w * 2u;
    int py = y + row;
    // Same clip test as display_set_pixel, hoisted out of the inner loop.
    if (py < s_clip_y0 || py > s_clip_y1) continue;
    for (int col = 0; col < w; col++) {
      int px = x + col;
      if (px < s_clip_x0 || px > s_clip_x1) continue;
      uint16_t v = (uint16_t)(s[0] | ((uint16_t)s[1] << 8));
      s_framebuffer[py * FB_WIDTH + px] = (uint16_t)((v >> 8) | (v << 8));
      wrote_any = true;
      s += 2;
    }
  }
  return wrote_any;
}

bool display_get_pixels_block(int x, int y, int w, int h,
                              void *rgb565_host, size_t len) {
  if (w <= 0 || h <= 0) return false;
  if (x < 0 || y < 0 || x + w > FB_WIDTH || y + h > FB_HEIGHT) return false;
  if (len != (size_t)w * (size_t)h * 2u) return false;

  uint8_t *dst = (uint8_t *)rgb565_host;
  for (int row = 0; row < h; row++) {
    uint8_t *d = dst + (size_t)row * (size_t)w * 2u;
    const uint16_t *src = &s_framebuffer[(y + row) * FB_WIDTH + x];
    for (int col = 0; col < w; col++) {
      uint16_t be = src[col];
      uint16_t v = (uint16_t)((be >> 8) | (be << 8));
      d[0] = (uint8_t)(v & 0xFF);
      d[1] = (uint8_t)(v >> 8);
      d += 2;
    }
  }
  return true;
}


const uint16_t *display_get_front_buffer(void) {
  return s_framebuffers[1 - s_back_buffer_idx];
}

const uint16_t *display_get_screen_buffer(void) {
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }
  return s_last_presented;
}

// Hardware vertical scroll using ST7365P VSCRDEF + VSCRSADD registers.
// top_fixed: fixed rows at top, scroll_height: scrollable area height,
// bottom_fixed: fixed rows at bottom. The ST7365P frame memory is 480 lines
// (the visible panel is lines 0..319), so the three values must sum to 480 —
// e.g. (0, 320, 160) makes the whole visible panel a mod-320 scroll ring.
// scroll_offset: absolute frame-memory line shown at the top of the scroll
// area; wraps within the scroll area.
//
// Both setters must wait out any in-flight flush DMA: lcd_write_cmd toggles
// CS and would interleave command bytes into an active pixel stream.
void display_set_scroll_area(int top_fixed, int scroll_height, int bottom_fixed) {
  display_wait_for_flush();
  lcd_write_cmd(0x33); // VSCRDEF
  uint8_t data[6] = {
    (uint8_t)(top_fixed >> 8), (uint8_t)(top_fixed & 0xFF),
    (uint8_t)(scroll_height >> 8), (uint8_t)(scroll_height & 0xFF),
    (uint8_t)(bottom_fixed >> 8), (uint8_t)(bottom_fixed & 0xFF),
  };
  lcd_write_data(data, 6);
}

void display_set_scroll_offset(int offset) {
  display_wait_for_flush();
  lcd_write_cmd(0x37); // VSCRSADD
  uint8_t data[2] = {(uint8_t)(offset >> 8), (uint8_t)(offset & 0xFF)};
  lcd_write_data(data, 2);
  s_scroll_offset = offset;
  s_scroll_offset_writes++;
}

// Last offset written via display_set_scroll_offset (the panel register is
// write-only).  The OS resets the offset to 0 whenever it takes over the
// screen (system menu, app switch), so apps driving hardware scroll must
// poll this each frame and resynchronise when it no longer matches what
// they last set.
int display_get_scroll_offset(void) {
  return s_scroll_offset;
}

uint32_t display_get_scroll_offset_writes(void) {
  return s_scroll_offset_writes;
}
