#include "ui.h"
#include "../dev_commands.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/pad_source.h"
#include "../drivers/wifi.h"
#include "../splash_logo.h"
#include "clock.h"
#include "config.h"
#include "os.h"    // BTN_* constants
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#define C_HEADER_BG RGB565(20, 20, 60)
#define C_TEXT COLOR_WHITE
#define C_TEXT_DIM COLOR_GRAY
#define C_BORDER RGB565(60, 60, 100)

// ── Header ──────────────────────────────────────────────────────────────────
// A UI_HEADER_H bar shaded from top to bottom, then a 1px C_BORDER line.
// Title on the left; clock, gamepad, WiFi, charging bolt and battery on the
// right.
// The icons carry the state, so the header stays grey and navy: colour only
// flags a problem (low battery, WiFi failure).

#define HDR_TOP_R 34
#define HDR_TOP_G 36
#define HDR_TOP_B 84
#define HDR_BOT_R 16
#define HDR_BOT_G 18
#define HDR_BOT_B 48
#define C_HDR_TEXT RGB565(230, 232, 240)
#define C_HDR_CLOCK RGB565(150, 155, 175)
#define C_ICON_DIM RGB565(100, 105, 135)
#define C_ALERT RGB565(230, 110, 100)

#define HDR_MARGIN 8      // left of the title, right of the battery
#define HDR_ICON_GAP 8    // between clock, WiFi and battery
#define BAT_LOW_PCT 15

// Glyphs are rows of '#' (drawn) and '.' (left as background).
static const char *const k_battery[] = {
    "###############..",
    "#.............#..",
    "#.............#..",
    "#.............###",
    "#.............###",
    "#.............###",
    "#.............#..",
    "#.............#..",
    "###############..",
};
#define BAT_W 17
#define BAT_H 9
#define BAT_INNER_X 2     // fill bar / digits area: 11x5 at (2,2)
#define BAT_INNER_Y 2
#define BAT_INNER_W 11

static const char *const k_bolt[] = {
    "...#.", "..##.", ".##..", "####.", ".####", "..##.", ".##..", "##...", "#....",
};
#define BOLT_W 5
#define BOLT_H 9
#define BOLT_GAP 2        // bolt to battery

static const char *const k_wifi[] = {
    "..#######..",
    ".#.......#.",
    "#..#####..#",
    "..#.....#..",
    ".#..###..#.",
    "...#...#...",
    ".....#.....",
    "....###....",
};
#define WIFI_W 11
#define WIFI_H 8

// A gamepad (D-pad and four face buttons cut out), shown while a pad source
// is connected (pad_source.h).
static const char *const k_pad[] = {
    ".###########.",
    "#############",
    "##.######.###",
    "#...####.#.##",
    "##.######.###",
    "#############",
    "####.....####",
    ".##.......##.",
};
#define PAD_W 13
#define PAD_H 8

// 3x5 digits for the battery percentage (battery_pct setting).
static const char *const k_digits[10][5] = {
    {"###", "#.#", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", ".#.", "###"},
    {"###", "..#", "###", "#..", "###"}, {"###", "..#", ".##", "..#", "###"},
    {"#.#", "#.#", "###", "..#", "..#"}, {"###", "#..", "###", "..#", "###"},
    {"###", "#..", "###", "#.#", "###"}, {"###", "..#", "..#", ".#.", ".#."},
    {"###", "#.#", "###", "#.#", "###"}, {"###", "#.#", "###", "..#", "###"},
};

static void draw_glyph(int x, int y, const char *const *rows, int h,
                       uint16_t color) {
  for (int r = 0; r < h; r++)
    for (int c = 0; rows[r][c]; c++)
      if (rows[r][c] == '#')
        display_set_pixel(x + c, y + r, color);
}

bool ui_battery_pct_enabled(void) {
  const char *v = config_get("battery_pct");
  return v && strcmp(v, "1") == 0;
}

static void draw_battery(int x, int y, int pct, bool digits) {
  if (pct > 100)
    pct = 100;
  uint16_t c = (pct <= BAT_LOW_PCT) ? C_ALERT : C_HDR_TEXT;
  draw_glyph(x, y, k_battery, BAT_H, c);
  int ix = x + BAT_INNER_X, iy = y + BAT_INNER_Y;
  if (digits) {
    char buf[4];
    int n = snprintf(buf, sizeof(buf), "%d", pct);
    int dx = ix + (BAT_INNER_W - (n * 4 - 1)) / 2;
    for (int i = 0; i < n; i++)
      draw_glyph(dx + i * 4, iy, k_digits[buf[i] - '0'], 5, c);
  } else {
    int fill = (pct * BAT_INNER_W + 99) / 100;  // any charge shows 1px
    display_fill_rect(ix, iy, fill, 5, c);
  }
}

// Online: bright. Connecting or no internet yet: dim. Failed: dim with a
// slash. Disconnected (the idle state after the boot time sync) or no WiFi
// hardware: nothing, and the function returns false.
static bool draw_wifi(int x, int y, int status) {
  switch (status) {
  case WIFI_STATUS_ONLINE:
    draw_glyph(x, y, k_wifi, WIFI_H, C_HDR_TEXT);
    return true;
  case WIFI_STATUS_CONNECTING:
  case WIFI_STATUS_CONNECTED:
    draw_glyph(x, y, k_wifi, WIFI_H, C_ICON_DIM);
    return true;
  case WIFI_STATUS_FAILED:
    draw_glyph(x, y, k_wifi, WIFI_H, C_ICON_DIM);
    for (int i = 0; i < WIFI_H; i++)   // bottom-left to top-right
      display_set_pixel(x + 2 + i, y + WIFI_H - 1 - i, C_ALERT);
    return true;
  default:
    return false;
  }
}

static int s_last_bat = -999;
static bool s_last_charging = false;
static bool s_last_bat_pct = false;
static int s_last_wifi_status = -1;
static bool s_last_pad = false;
static char s_last_clock[16] = "";

static int header_wifi_status(void) {
  return wifi_is_available() ? (int)wifi_get_status() : -1;
}

bool ui_needs_header_redraw(void) {
  if (kbd_get_battery_percent() != s_last_bat)
    return true;
  if (kbd_is_charging() != s_last_charging)
    return true;
  if (ui_battery_pct_enabled() != s_last_bat_pct)
    return true;
  if (header_wifi_status() != s_last_wifi_status)
    return true;
  if ((pad_sources_connected() != 0) != s_last_pad)
    return true;

  if (clock_is_set()) {
    char clk_buf[16];
    clock_format(clk_buf, sizeof(clk_buf));
    if (strcmp(clk_buf, s_last_clock) != 0)
      return true;
  }
  return false;
}

int ui_draw_header(const char *title) {
  const int span = UI_HEADER_H - 1;
  for (int y = 0; y < UI_HEADER_H; y++) {
    int r = HDR_TOP_R + (HDR_BOT_R - HDR_TOP_R) * y / span;
    int g = HDR_TOP_G + (HDR_BOT_G - HDR_TOP_G) * y / span;
    int b = HDR_TOP_B + (HDR_BOT_B - HDR_TOP_B) * y / span;
    display_fill_rect(0, y, FB_WIDTH, 1, RGB565(r, g, b));
  }
  display_fill_rect(0, UI_HEADER_H, FB_WIDTH, 1, C_BORDER);

  // Text cells carry a blank descender row, so centre on height - 1.
  int text_y = (UI_HEADER_H - (display_get_font_height() - 1)) / 2;
  display_draw_text_transparent(HDR_MARGIN, text_y, title ? title : "",
                                C_HDR_TEXT);

  // Right-side status, laid out right to left.
  int x = FB_WIDTH - HDR_MARGIN;

  int bat = kbd_get_battery_percent();
  bool charging = kbd_is_charging();
  bool pct_mode = ui_battery_pct_enabled();
  s_last_bat = bat;
  s_last_charging = charging;
  s_last_bat_pct = pct_mode;
  if (bat >= 0) {
    x -= BAT_W;
    draw_battery(x, (UI_HEADER_H - BAT_H) / 2, bat, pct_mode);
    if (charging) {
      x -= BOLT_GAP + BOLT_W;
      draw_glyph(x, (UI_HEADER_H - BOLT_H) / 2, k_bolt, BOLT_H, C_HDR_TEXT);
    }
    x -= HDR_ICON_GAP;
  }

  s_last_wifi_status = header_wifi_status();
  if (draw_wifi(x - WIFI_W, (UI_HEADER_H - WIFI_H) / 2, s_last_wifi_status))
    x -= WIFI_W + HDR_ICON_GAP;

  s_last_pad = pad_sources_connected() != 0;
  if (s_last_pad) {
    x -= PAD_W;
    draw_glyph(x, (UI_HEADER_H - PAD_H) / 2, k_pad, PAD_H, C_HDR_TEXT);
    x -= HDR_ICON_GAP;
  }

  if (clock_is_set()) {
    char clk_buf[16];
    clock_format(clk_buf, sizeof(clk_buf));
    strncpy(s_last_clock, clk_buf, sizeof(s_last_clock));
    x -= display_text_width(clk_buf);
    display_draw_text_transparent(x, text_y, clk_buf, C_HDR_CLOCK);
  }

  return UI_HEADER_H + 1;
}

void ui_draw_footer(const char *left_text, const char *right_text) {
  display_fill_rect(0, FB_HEIGHT - 18, FB_WIDTH, 18, C_HEADER_BG);
  display_fill_rect(0, FB_HEIGHT - 18, FB_WIDTH, 1, C_BORDER);

  if (left_text && left_text[0]) {
    display_draw_text(8, FB_HEIGHT - 13, left_text, C_TEXT_DIM, C_HEADER_BG);
  }

  if (right_text && right_text[0]) {
    int w = display_text_width(right_text);
    display_draw_text(FB_WIDTH - 8 - w, FB_HEIGHT - 13, right_text, C_TEXT_DIM,
                      C_HEADER_BG);
  }
}

int ui_draw_tabs(const char **tabs, int count, int active_index, int y) {
  if (count <= 0 || !tabs)
    return 0;

  // Clamp active_index to valid range
  if (active_index < 0)
    active_index = 0;
  if (active_index >= count)
    active_index = count - 1;

  const int TAB_HEIGHT = 20;
  const int TAB_PADDING = 8;
  const int TAB_SPACING = 4;

  // Draw background and border
  display_fill_rect(0, y, FB_WIDTH, TAB_HEIGHT, C_HEADER_BG);
  display_fill_rect(0, y + TAB_HEIGHT, FB_WIDTH, 1, C_BORDER);

  // Calculate tab widths (distribute evenly)
  int available_width = FB_WIDTH - (TAB_PADDING * 2) - (TAB_SPACING * (count - 1));
  int tab_width = available_width / count;

  // Draw each tab
  int x = TAB_PADDING;
  for (int i = 0; i < count; i++) {
    if (!tabs[i])
      continue;

    bool is_active = (i == active_index);
    uint16_t text_color = is_active ? C_TEXT : C_TEXT_DIM;
    
    // Draw active tab background highlight
    if (is_active) {
      display_fill_rect(x - 2, y + 2, tab_width + 4, TAB_HEIGHT - 4, 
                        RGB565(40, 40, 80));
    }

    // Center text within tab
    int text_w = display_text_width(tabs[i]);
    int text_x = x + (tab_width - text_w) / 2;
    int text_y = y + (TAB_HEIGHT - 8) / 2;
    
    display_draw_text(text_x, text_y, tabs[i], text_color, C_HEADER_BG);

    x += tab_width + TAB_SPACING;
  }

  return TAB_HEIGHT + 1; // Return total height including border
}

void ui_draw_splash(const char *status, const char *subtext) {
  display_clear(COLOR_BLACK);

#if LOGO_W > 0 && LOGO_H > 0
  // ── Logo ──────────────────────────────────────────────────────────────────
  int lx = (FB_WIDTH - LOGO_W) / 2;
  int ly = (FB_HEIGHT - LOGO_H) / 2 - 16;
  display_draw_image(lx, ly, LOGO_W, LOGO_H, logo_data);

  // ── Status text below logo ─────────────────────────────────────────────
  if (status && status[0]) {
    int sx = (FB_WIDTH - display_text_width(status)) / 2;
    display_draw_text(sx, ly + LOGO_H + 12, status, COLOR_GRAY, COLOR_BLACK);
  }
  if (subtext && subtext[0]) {
    int s2x = (FB_WIDTH - display_text_width(subtext)) / 2;
    display_draw_text(s2x, ly + LOGO_H + 24, subtext, COLOR_GRAY, COLOR_BLACK);
  }
#else
  // ── No logo: centred title + status ───────────────────────────────────────
  const char *title = "PicoDeck";
  int tx = (FB_WIDTH - display_text_width(title)) / 2;
  display_draw_text(tx, FB_HEIGHT / 2 - 14, title, COLOR_WHITE, COLOR_BLACK);

  if (status && status[0]) {
    int sx = (FB_WIDTH - display_text_width(status)) / 2;
    display_draw_text(sx, FB_HEIGHT / 2 + 2, status, COLOR_GRAY, COLOR_BLACK);
  }
  if (subtext && subtext[0]) {
    int s2x = (FB_WIDTH - display_text_width(subtext)) / 2;
    display_draw_text(s2x, FB_HEIGHT / 2 + 14, subtext, COLOR_GRAY,
                      COLOR_BLACK);
  }
#endif

  display_flush();
}

// ── Shared dialog style ───────────────────────────────────────────────────────

#define DLG_BG      RGB565(20, 28, 50)
#define DLG_BORDER  RGB565(80, 100, 150)
#define DLG_FIELD   RGB565(10, 14, 30)
#define DLG_DIM     RGB565(100, 100, 100)
#define DLG_W       280
#define DLG_X       ((FB_WIDTH  - DLG_W) / 2)

// Draw a wrapped message into a dialog box at (x, y_start).
// Returns the y coordinate after the last line drawn.
static int dlg_draw_message(int x, int y, int w, const char *msg, uint16_t bg) {
  if (!msg || !msg[0]) return y;
  const int COLS = w / 6; // chars that fit (6px font)
  while (*msg) {
    int avail = strlen(msg);
    int take  = avail > COLS ? COLS : avail;
    // Try to break at a word boundary
    if (avail > COLS) {
      for (int k = take - 1; k > 0; k--) {
        if (msg[k] == ' ' || msg[k] == '/') { take = k; break; }
      }
    }
    char line[64];
    int n = take < (int)sizeof(line) - 1 ? take : (int)sizeof(line) - 1;
    memcpy(line, msg, n);
    line[n] = '\0';
    display_draw_text(x, y, line, COLOR_WHITE, bg);
    y += 12;
    msg += take;
    if (*msg == ' ') msg++; // skip leading space on next line
  }
  return y;
}

// ── ui_text_input ─────────────────────────────────────────────────────────────

bool ui_text_input(const char *prompt, const char *default_val,
                   char *out_buf, int out_len) {
  // Layout
  const int DH = 90;
  const int DY = (FB_HEIGHT - DH) / 2;
  const int FX = DLG_X + 8;
  const int FY = DY + 30;
  const int FW = DLG_W - 16;
  const int FH = 18;

  // Draw dialog (once)
  display_darken();
  display_fill_rect(DLG_X, DY, DLG_W, DH, DLG_BG);
  display_draw_rect(DLG_X, DY, DLG_W, DH, DLG_BORDER);
  display_draw_text(DLG_X + 10, DY + 10,
                    prompt ? prompt : "Input:", COLOR_WHITE, DLG_BG);
  display_fill_rect(FX, FY, FW, FH, DLG_FIELD);
  display_draw_rect(FX, FY, FW, FH, DLG_BORDER);
  const char *hint = "Enter=OK  Esc=Cancel";
  int hw = display_text_width(hint);
  display_draw_text(DLG_X + (DLG_W - hw) / 2, DY + DH - 14,
                    hint, DLG_DIM, DLG_BG);
  display_flush();

  // Second pass: draw dialog chrome on the other buffer too (double-buffering)
  display_fill_rect(DLG_X, DY, DLG_W, DH, DLG_BG);
  display_draw_rect(DLG_X, DY, DLG_W, DH, DLG_BORDER);
  display_draw_text(DLG_X + 10, DY + 10,
                    prompt ? prompt : "Input:", COLOR_WHITE, DLG_BG);
  display_fill_rect(FX, FY, FW, FH, DLG_FIELD);
  display_draw_rect(FX, FY, FW, FH, DLG_BORDER);
  display_draw_text(DLG_X + (DLG_W - hw) / 2, DY + DH - 14,
                    hint, DLG_DIM, DLG_BG);
  display_flush();

  // Start from a clean keyboard: edges gathered by sys.sleep's background
  // polls, or injections queued before this modal opened, were not typed
  // at this dialog and must not accept/cancel it.
  kbd_clear_state();

  // Input buffer
  const int MAX_CHARS = 127;
  char buf[128] = "";
  int len = 0;
  if (default_val) {
    strncpy(buf, default_val, MAX_CHARS);
    buf[MAX_CHARS] = '\0';
    len = strlen(buf);
  }

  bool accepted = false;
  bool dirty    = true;
  while (true) {
    kbd_poll();
    dev_commands_poll();
    dev_commands_process();
    // A dev "exit" must be able to unwind this modal: cancel so the Lua hook
    // can process the exit once control returns to the app.
    if (dev_commands_wants_exit()) break;
    // The menu key cancels, like Esc, and takes the latch.
    if (kbd_consume_menu_press()) break;
    uint32_t btns = kbd_get_buttons_pressed();
    char c        = kbd_get_char();

    if (btns & BTN_ESC) break;
    if (c == '\n') { accepted = true; break; }  // Enter = 0x0A
    if (c == '\b') {
      if (len > 0) { len--; dirty = true; }
    } else if (c >= 32 && c < 127 && len < MAX_CHARS) {
      buf[len++] = c;
      dirty = true;
    }
    buf[len] = '\0';

    if (dirty) {
      // Redraw only the input field interior
      display_fill_rect(FX + 1, FY + 1, FW - 2, FH - 2, DLG_FIELD);
      // Scroll: show only the last N chars that fit
      const int VCOLS = (FW - 8) / 6;
      const char *view = (len > VCOLS) ? (buf + len - VCOLS) : buf;
      char dbuf[66];
      snprintf(dbuf, sizeof(dbuf), "%.*s_", VCOLS, view);
      display_draw_text(FX + 4, FY + 5, dbuf, COLOR_WHITE, DLG_FIELD);
      display_flush();
      dirty = false;
    }
    watchdog_update();
    sleep_ms(20);
  }

  if (accepted && out_buf && out_len > 0) {
    strncpy(out_buf, buf, out_len - 1);
    out_buf[out_len - 1] = '\0';
  }
  return accepted;
}

#define UI_CONFIRM_GRACE_MS 400u

// ── ui_confirm ────────────────────────────────────────────────────────────────

bool ui_confirm(const char *message) {
  // Only input typed at THIS dialog may answer it.  Keys queued while the
  // app was not polling (still in the STM32 FIFO, in the keyboard bus
  // engine's ring, or decoded by sys.sleep's background polls) or injected
  // earlier are discarded, input during a short grace period is ignored, and
  // Enter must be a fresh press edge after it — so an app cannot pre-load a
  // "yes" (sys.applyUpdate relies on this).
  kbd_discard_pending();
  // Measure how many lines the message needs (up to 2)
  const int COLS = (DLG_W - 20) / 6;
  int msg_len = message ? strlen(message) : 0;
  int lines   = msg_len == 0 ? 1 : (msg_len + COLS - 1) / COLS;
  if (lines > 2) lines = 2;

  const int DH = 44 + lines * 12 + 18; // border + text lines + hint
  const int DY = (FB_HEIGHT - DH) / 2;

  // OS modals draw full-screen even if the app set a clip rect.
  int saved_cx, saved_cy, saved_cw, saved_ch;
  display_get_clip_rect(&saved_cx, &saved_cy, &saved_cw, &saved_ch);
  display_clear_clip_rect();

  display_darken();
  display_fill_rect(DLG_X, DY, DLG_W, DH, DLG_BG);
  display_draw_rect(DLG_X, DY, DLG_W, DH, DLG_BORDER);

  dlg_draw_message(DLG_X + 10, DY + 14, DLG_W - 20,
                   message ? message : "Are you sure?", DLG_BG);

  const char *hint = "Enter=Yes  Esc=No";
  int hw = display_text_width(hint);
  display_draw_text(DLG_X + (DLG_W - hw) / 2, DY + DH - 14,
                    hint, DLG_DIM, DLG_BG);
  display_flush();

  const uint32_t shown_ms = to_ms_since_boot(get_absolute_time());
  bool armed = false;  // grace over and Enter seen released
  while (true) {
    kbd_poll();
    dev_commands_poll();
    dev_commands_process();
    // Let a dev "exit" unwind this modal (cancel; the Lua hook handles exit).
    if (dev_commands_wants_exit()) { display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return false; }
    // The menu key answers No, like Esc, even in the grace period, and takes
    // the latch: left set it would open the system menu after the dialog.
    if (kbd_consume_menu_press()) { display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return false; }
    uint32_t btns = kbd_get_buttons_pressed();
    char c        = kbd_get_char();
    if (!armed) {
      // Consume (and ignore) everything until the grace period is over and
      // Enter is up, so a held or queued Enter cannot count.
      uint32_t now = to_ms_since_boot(get_absolute_time());
      armed = (now - shown_ms >= UI_CONFIRM_GRACE_MS) &&
              !(kbd_get_buttons() & BTN_ENTER);
      watchdog_update();
      sleep_ms(20);
      continue;
    }
    if (btns & BTN_ESC || c == 'n' || c == 'N') { display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return false; }
    if ((btns & BTN_ENTER) || c == 'y' || c == 'Y') { display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return true; }
    watchdog_update();
    sleep_ms(20);
  }
}

// ── ui_draw_spinner ───────────────────────────────────────────────────────────
// Draw a single frame of a rotating line spinner at (cx, cy) with radius r.
// Call once per frame with an incrementing frame counter for animation.
void ui_draw_spinner(int cx, int cy, int r, int frame) {
  static const int dx8[] = { 0,  1,  1,  1,  0, -1, -1, -1};
  static const int dy8[] = {-1, -1,  0,  1,  1,  1,  0, -1};
  int idx = (frame / 2) & 7; // rotate through 8 positions
  int x1 = cx + dx8[idx] * r;
  int y1 = cy + dy8[idx] * r;
  int x2 = cx - dx8[idx] * r;
  int y2 = cy - dy8[idx] * r;
  // Draw main line bright, secondary lines dimmer for motion effect
  display_draw_line(x1, y1, x2, y2, COLOR_WHITE);
  int prev = (idx + 7) & 7;
  display_draw_line(cx + dx8[prev] * r, cy + dy8[prev] * r,
                    cx - dx8[prev] * r, cy - dy8[prev] * r, COLOR_GRAY);
}
