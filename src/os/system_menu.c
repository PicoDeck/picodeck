#include "system_menu.h"
#include "crashlog.h"
#include "lua_bridge.h"
#include "lua_psram_alloc.h"
#include "../dev_commands.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../drivers/wifi.h"
#include "../usb/usb_msc.h"
#include "app_identity.h"
#include "app_stack.h"
#include "config.h"
#include "gamepad.h"
#include "gamepad_edit.h"
#include "idle_dim.h"
#include "launcher.h"
#include "os.h"
#include "os_overlay.h"
#include "screenshot.h"
#include "sim_hooks.h"
#include "text_input.h"
#include "tz_picker.h"
#include "ui.h"

#include "lauxlib.h"
#include "lua.h"

#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include "umm_malloc.h"

#include <stdio.h>
#include <string.h>

// Saved darkened background for restoring after sub-dialogs.
// Allocated in PSRAM (umm_malloc) at menu entry, freed on exit.
static uint16_t *s_saved_bg = NULL;

// ── App-registered items
// ──────────────────────────────────────────────────────

typedef struct {
  char label[32];
  void (*callback)(void *user);
  void *user;
} app_item_t;

static app_item_t s_app_items[SYSMENU_MAX_APP_ITEMS];
static int s_app_item_count = 0;
static uint8_t s_brightness = 128;
static bool s_dev_mode = false;
static bool s_wifi_auto_disconnect = true;

// ── Visual constants
// ──────────────────────────────────────────────────────────

#define PANEL_W 200
#define TITLE_H 16  // title bar height (px)
#define ITEM_H 13   // per-item row height (px): 8px font + 5px padding
#define FOOTER_H 12 // footer hint bar height (px)

#define C_PANEL_BG RGB565(20, 28, 50)
#define C_TITLE_BG RGB565(10, 14, 30)
#define C_SEL_BG RGB565(40, 80, 160)
#define C_BORDER RGB565(80, 100, 150)

// ── Flat item list types
// ──────────────────────────────────────────────────────

typedef enum {
  ITEM_APP_CB = 0,
  ITEM_BRIGHTNESS,
  ITEM_BATTERY,
  ITEM_WIFI,
  ITEM_TIMEZONE,
  ITEM_RAM_INFO,
  ITEM_REMOUNT_SD,
  ITEM_USB_MSC,
  ITEM_SCREENSHOT,
  ITEM_REBOOT,
  ITEM_EXIT,
  ITEM_SETTINGS,
  ITEM_REBOOT_FLASH,
  ITEM_WIFI_TOGGLE,
  ITEM_WIFI_SETTINGS,
  ITEM_WIFI_STATUS,
  ITEM_DEV_MODE,
  ITEM_WIFI_AUTO_DISCONNECT,
  ITEM_BATTERY_PCT,
  ITEM_SHOW_FPS,
  ITEM_CONTROLS,
} item_type_t;

typedef struct {
  item_type_t type;
  int app_idx; // valid only when type == ITEM_APP_CB
} flat_item_t;

typedef enum {
  PAGE_MAIN,
  PAGE_SETTINGS,
} menu_page_t;

// ── Background save/restore helpers
// ──────────────────────────────────────────────────────────
// Save the current back buffer (darkened app background) to PSRAM.
// Must be called once after display_darken() at menu entry.
static void bg_save(void) {
  if (!s_saved_bg)
    s_saved_bg = (uint16_t *)umm_malloc(FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
  if (s_saved_bg)
    memcpy(s_saved_bg, display_get_back_buffer(),
           FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
}

// Restore the saved darkened background into both framebuffers.
// Call before redrawing the menu panel after a sub-dialog has returned.
static void bg_restore(void) {
  if (!s_saved_bg)
    return;
  uint16_t *back = display_get_back_buffer();
  memcpy(back, s_saved_bg, FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
}

static void bg_free(void) {
  if (s_saved_bg) {
    umm_free(s_saved_bg);
    s_saved_bg = NULL;
  }
}

// ── Item list builder
// ──────────────────────────────────────────────────────────

static int build_items(flat_item_t *items, menu_page_t page, bool has_exit,
                       bool is_launcher) {
  int count = 0;

  if (page == PAGE_MAIN) {
    for (int i = 0; i < s_app_item_count; i++) {
      items[count].type = ITEM_APP_CB;
      items[count].app_idx = i;
      count++;
    }
    items[count++] = (flat_item_t){ITEM_BATTERY, 0};
    if (s_dev_mode)
      items[count++] = (flat_item_t){ITEM_RAM_INFO, 0};
    items[count++] = (flat_item_t){ITEM_SETTINGS, 0};
    if (is_launcher)
      items[count++] = (flat_item_t){ITEM_USB_MSC, 0};
    items[count++] = (flat_item_t){ITEM_SCREENSHOT, 0};
    items[count++] = (flat_item_t){ITEM_REBOOT, 0};
    if (s_dev_mode)
      items[count++] = (flat_item_t){ITEM_REBOOT_FLASH, 0};
    if (has_exit)
      items[count++] = (flat_item_t){ITEM_EXIT, 0};
  } else { // PAGE_SETTINGS
    items[count++] = (flat_item_t){ITEM_BRIGHTNESS, 0};
    items[count++] = (flat_item_t){ITEM_BATTERY_PCT, 0};
    items[count++] = (flat_item_t){ITEM_SHOW_FPS, 0};
    items[count++] = (flat_item_t){ITEM_CONTROLS, 0};
    items[count++] = (flat_item_t){ITEM_TIMEZONE, 0};
    items[count++] = (flat_item_t){ITEM_WIFI_TOGGLE, 0};
    items[count++] = (flat_item_t){ITEM_WIFI_SETTINGS, 0};
    if (s_dev_mode)
      items[count++] = (flat_item_t){ITEM_REMOUNT_SD, 0};
    items[count++] = (flat_item_t){ITEM_WIFI_AUTO_DISCONNECT, 0};
    items[count++] = (flat_item_t){ITEM_DEV_MODE, 0};
  }
  return count;
}

// ── WiFi helpers
// ──────────────────────────────────────────────────────────────

static void run_wifi_config(void) {
  char ssid[CONFIG_VAL_MAX];
  char pass[CONFIG_VAL_MAX];

  const char *saved_ssid = config_get("wifi_ssid");
  if (!saved_ssid)
    saved_ssid = "";

  if (!text_input_show("WiFi Settings", "Network (SSID):", saved_ssid, ssid,
                       sizeof(ssid)))
    return;

  if (!text_input_show("WiFi Settings", "Password:", "", pass, sizeof(pass)))
    return;

  config_set("wifi_ssid", ssid);
  config_set("wifi_pass", pass);
  config_save();
  wifi_connect(ssid, pass);
}

// ── Panel drawing
// ─────────────────────────────────────────────────────────────

static void draw_panel(const flat_item_t *items, int count, int sel, int px,
                       int py, int ph, int bat, menu_page_t page) {
  // Outer border
  display_draw_rect(px, py, PANEL_W, ph, C_BORDER);

  // Title bar
  display_fill_rect(px + 1, py + 1, PANEL_W - 2, TITLE_H, C_TITLE_BG);
  const char *title = (page == PAGE_MAIN) ? "System Menu" : "Settings";
  int tw = display_text_width(title);
  display_draw_text(px + (PANEL_W - tw) / 2, py + 5, title, COLOR_WHITE,
                    C_TITLE_BG);

  // Divider after title
  display_fill_rect(px + 1, py + 1 + TITLE_H, PANEL_W - 2, 1, C_BORDER);

  // Items
  int items_y = py + 1 + TITLE_H + 1;
  for (int i = 0; i < count; i++) {
    int iy = items_y + i * ITEM_H;
    bool selected = (i == sel);
    uint16_t bg = selected ? C_SEL_BG : C_PANEL_BG;
    display_fill_rect(px + 1, iy, PANEL_W - 2, ITEM_H, bg);

    char label[48];
    uint16_t fg = COLOR_WHITE;

    switch (items[i].type) {
    case ITEM_APP_CB:
      snprintf(label, sizeof(label), "%s", s_app_items[items[i].app_idx].label);
      break;
    case ITEM_BRIGHTNESS:
      snprintf(label, sizeof(label), "Brightness: %d <>", s_brightness);
      break;
    case ITEM_BATTERY:
      if (bat >= 0)
        snprintf(label, sizeof(label), "Battery: %d%%", bat);
      else
        snprintf(label, sizeof(label), "Battery: N/A");
      fg = (bat > 20) ? COLOR_GREEN : COLOR_RED;
      break;
    case ITEM_WIFI:
      // Legacy — kept for compatibility but not used in paged menu
      snprintf(label, sizeof(label), "WiFi");
      break;
    case ITEM_WIFI_TOGGLE:
      if (!wifi_is_available()) {
        snprintf(label, sizeof(label), "WiFi: N/A");
        fg = COLOR_GRAY;
      } else {
        switch (wifi_get_status()) {
        case WIFI_STATUS_ONLINE: {
          const char *ip = wifi_get_ip();
          snprintf(label, sizeof(label), "WiFi: On (%s)", ip ? ip : "?");
          fg = COLOR_GREEN;
          break;
        }
        case WIFI_STATUS_CONNECTED: {
          const char *ip = wifi_get_ip();
          snprintf(label, sizeof(label), "WiFi: On (%s) no inet",
                   ip ? ip : "?");
          fg = COLOR_YELLOW;
          break;
        }
        case WIFI_STATUS_CONNECTING:
          snprintf(label, sizeof(label), "WiFi: Connecting...");
          fg = COLOR_YELLOW;
          break;
        case WIFI_STATUS_FAILED:
          snprintf(label, sizeof(label), "WiFi: Failed");
          fg = COLOR_RED;
          break;
        default:
          snprintf(label, sizeof(label), "WiFi: Off");
          fg = COLOR_GRAY;
          break;
        }
      }
      break;
    case ITEM_WIFI_SETTINGS:
      snprintf(label, sizeof(label), "WiFi Settings");
      break;
    case ITEM_WIFI_STATUS:
      if (!wifi_is_available()) {
        snprintf(label, sizeof(label), "WiFi: N/A");
        fg = COLOR_GRAY;
      } else {
        switch (wifi_get_status()) {
        case WIFI_STATUS_ONLINE:
          snprintf(label, sizeof(label), "WiFi: Ok");
          fg = COLOR_GREEN;
          break;
        case WIFI_STATUS_CONNECTED:
          snprintf(label, sizeof(label), "WiFi: No internet");
          fg = COLOR_YELLOW;
          break;
        case WIFI_STATUS_CONNECTING:
          snprintf(label, sizeof(label), "WiFi: Connecting...");
          fg = COLOR_YELLOW;
          break;
        case WIFI_STATUS_FAILED:
          snprintf(label, sizeof(label), "WiFi: Failed");
          fg = COLOR_RED;
          break;
        default:
          snprintf(label, sizeof(label), "WiFi: Off");
          fg = COLOR_GRAY;
          break;
        }
      }
      break;
    case ITEM_TIMEZONE: {
      const char *tz = config_get("tz_offset");
      if (tz && tz[0])
        snprintf(label, sizeof(label), "Timezone: %s min", tz);
      else
        snprintf(label, sizeof(label), "Timezone: UTC");
      break;
    }
    case ITEM_RAM_INFO: {
      size_t free_kb  = lua_psram_alloc_free_size() / 1024;
      size_t total_kb = lua_psram_alloc_total_size() / 1024;
      size_t used_kb  = total_kb - free_kb;
      snprintf(label, sizeof(label), "PSRAM: %uk/%uk used", (unsigned)used_kb, (unsigned)total_kb);
      fg = (free_kb > 512) ? COLOR_GREEN : (free_kb > 128) ? COLOR_YELLOW : COLOR_RED;
      break;
    }
    case ITEM_SETTINGS:
      snprintf(label, sizeof(label), "Settings");
      break;
    case ITEM_REMOUNT_SD:
      snprintf(label, sizeof(label), "Remount SD Card");
      break;
    case ITEM_USB_MSC:
      snprintf(label, sizeof(label), "USB Disk Mode");
      break;
    case ITEM_SCREENSHOT:
      snprintf(label, sizeof(label), "Screenshot");
      break;
    case ITEM_REBOOT:
      snprintf(label, sizeof(label), "Reboot");
      fg = selected ? COLOR_WHITE : COLOR_RED;
      break;
    case ITEM_REBOOT_FLASH:
      snprintf(label, sizeof(label), "Reboot to Flash");
      fg = selected ? COLOR_WHITE : COLOR_RED;
      break;
    case ITEM_EXIT:
      snprintf(label, sizeof(label), "Exit App");
      fg = selected ? COLOR_WHITE : COLOR_YELLOW;
      break;
    case ITEM_WIFI_AUTO_DISCONNECT:
      snprintf(label, sizeof(label), "Auto Disconnect: %s",
               s_wifi_auto_disconnect ? "On" : "Off");
      break;
    case ITEM_BATTERY_PCT:
      snprintf(label, sizeof(label), "Battery %%: %s",
               ui_battery_pct_enabled() ? "On" : "Off");
      break;
    case ITEM_SHOW_FPS:
      snprintf(label, sizeof(label), "Show FPS: %s",
               os_overlay_fps_label(os_overlay_fps_mode()));
      break;
    case ITEM_CONTROLS:
      snprintf(label, sizeof(label), "Controls");
      break;
    case ITEM_DEV_MODE:
      snprintf(label, sizeof(label), "Developer Mode: %s", s_dev_mode ? "On" : "Off");
      fg = s_dev_mode ? COLOR_GREEN : COLOR_GRAY;
      break;
    }

    // Selection indicator and item text
    display_draw_text(px + 4, iy + 2, selected ? ">" : " ", COLOR_WHITE, bg);
    display_draw_text(px + 10, iy + 2, label, fg, bg);
  }

  // Divider before footer
  int footer_div_y = items_y + count * ITEM_H;
  display_fill_rect(px + 1, footer_div_y, PANEL_W - 2, 1, C_BORDER);

  // Footer hint
  int footer_y = footer_div_y + 1;
  display_fill_rect(px + 1, footer_y, PANEL_W - 2, FOOTER_H, C_TITLE_BG);
  const char *footer = (page == PAGE_MAIN) ? "Enter:select  Esc:close"
                                           : "Enter:select  Esc:back";
  display_draw_text(px + 4, footer_y + 2, footer, COLOR_GRAY, C_TITLE_BG);

  // Show current clock speed in bottom-left corner
  char clk_buf[16];
  uint32_t clk_hz = clock_get_hz(clk_sys);
  snprintf(clk_buf, sizeof(clk_buf), "%lu MHz", (unsigned long)(clk_hz / 1000000));
  display_draw_text(4, FB_HEIGHT - 12, clk_buf, COLOR_GREEN, 0);
}

// ── Settings -> Controls
// ──────────────────────────────────────────────────────────────────
// Rebinds the gamepad: the global map at the launcher; inside a game, This
// game (its override, /data/<id>/gamepad.json; the default) or All games.
// gamepad_edit.c holds the state logic; this page loads the files, feeds it
// the keys, draws it and saves on the way out. The menu also runs at the
// launcher, on the 4 KB MSP that the IRQs share: the state (two maps, the
// cursor, the notice) is umm_malloc'd for the page's lifetime, the frames
// here stay small, and the SD work runs on an OS stack (app_stack_run_os).

#define CTL_PANEL_W 264
#define CTL_LABEL_X 10  // button names, from the panel's left edge
#define CTL_CELL_X0 70  // the Primary column
#define CTL_CELL_X1 166 // the Alt column
#define CTL_CELL_W 90
// The page needs about 3 KB of PSRAM heap (its state, the SD layer's
// FILINFO, a file's text; 2 KB more to save). An allocation that fails
// inside the file reads looks like a missing file, which would put the
// defaults on screen and then over the user's file: so check first.
#define CTL_HEAP_MIN (8u * 1024u)
#define C_DIM COLOR_GRAY // a binding inherited from All games

// Notices and saves go to the serial log (and the simulator's, for tests).
static void ctl_log(const char *text) {
  printf("[CONTROLS] %s\n", text);
  sim_log_os("[CONTROLS] %s", text);
}

// A short message in a red box over the menu's backdrop, for 1.5 s.
static void ctl_alert(const char *text) {
  ctl_log(text);
  int w = display_text_width(text) + 20;
  int x = (FB_WIDTH - w) / 2, y = FB_HEIGHT / 2 - 12;
  bg_restore();
  display_fill_rect(x, y, w, 24, COLOR_RED);
  display_draw_text(x + 10, y + 8, text, COLOR_WHITE, COLOR_RED);
  display_flush();
  sleep_ms(1500);
}

typedef struct {
  gamepad_edit_t *e;
  gamepad_file_t global, game; // what each file read gave
} ctl_load_t;

static void ctl_load_on_stack(void *arg) {
  ctl_load_t *job = (ctl_load_t *)arg;
  gamepad_edit_t *e = job->e;
  uint16_t listed = 0;
  job->global = gamepad_load_file(GAMEPAD_GLOBAL_PATH, &e->global, &listed);
  job->game = GAMEPAD_FILE_MISSING;
  const app_identity_t *me = app_identity_current();
  char path[GAMEPAD_PATH_MAX];
  if (e->has_game && me && gamepad_app_path(path, sizeof(path), me->id))
    job->game = gamepad_load_file(path, &e->game, &e->game_mask);
}

typedef struct {
  const gamepad_edit_t *e;
  bool ok;
} ctl_save_t;

// The global file lists the buttons that differ from the defaults (none: it
// goes); the override lists its buttons (none: it goes). gamepad_save keeps
// the previous file when it fails.
static void ctl_save_on_stack(void *arg) {
  ctl_save_t *job = (ctl_save_t *)arg;
  const gamepad_edit_t *e = job->e;
  bool ok = true;
  if (e->global_dirty) {
    uint16_t mask = gamepad_edit_global_mask(e);
    ok = mask ? gamepad_save(GAMEPAD_GLOBAL_PATH, &e->global, mask)
              : gamepad_remove(GAMEPAD_GLOBAL_PATH);
  }
  if (e->game_dirty) {
    const app_identity_t *me = app_identity_current();
    char path[GAMEPAD_PATH_MAX];
    if (!me || !gamepad_app_path(path, sizeof(path), me->id))
      ok = false;
    else if (!(e->game_mask ? gamepad_save(path, &e->game, e->game_mask)
                            : gamepad_remove(path)))
      ok = false;
  }
  job->ok = ok;
}

static void ctl_draw_cell(const gamepad_edit_t *e, int button, int slot,
                          int x, int y) {
  bool focused = e->row == button && e->slot == slot;
  uint16_t bg = focused ? C_SEL_BG : C_PANEL_BG;
  if (focused)
    display_fill_rect(x, y, CTL_CELL_W, ITEM_H, bg);
  if (focused && e->capturing) {
    display_draw_text(x + 4, y + 2, "press a key...", COLOR_YELLOW, bg);
    return;
  }
  bool inherited = false;
  uint8_t key = gamepad_edit_cell(e, button, slot, &inherited);
  const char *name = key ? gamepad_key_name(key) : "-";
  display_draw_text(x + 4, y + 2, name ? name : "?",
                    inherited ? C_DIM : COLOR_WHITE, bg);
}

// Title, the scope row (in a game), the column heads, a row per button, the
// notice line and a two-line footer: 239 px high in a game (226 at the
// launcher) at the 8 px font.
static void ctl_draw(const gamepad_edit_t *e, bool error) {
  int h = 1 + TITLE_H + 1 + (e->has_game ? ITEM_H : 0) +
          (2 + KBD_PAD_BUTTONS) * ITEM_H + 1 + 2 * FOOTER_H + 1;
  int px = (FB_WIDTH - CTL_PANEL_W) / 2;
  int py = (FB_HEIGHT - h) / 2;
  display_draw_rect(px, py, CTL_PANEL_W, h, C_BORDER);
  display_fill_rect(px + 1, py + 1, CTL_PANEL_W - 2, h - 2, C_PANEL_BG);
  display_fill_rect(px + 1, py + 1, CTL_PANEL_W - 2, TITLE_H, C_TITLE_BG);
  display_draw_text(px + (CTL_PANEL_W - display_text_width("Controls")) / 2,
                    py + 5, "Controls", COLOR_WHITE, C_TITLE_BG);
  display_fill_rect(px + 1, py + 1 + TITLE_H, CTL_PANEL_W - 2, 1, C_BORDER);
  int y = py + 2 + TITLE_H;

  if (e->has_game) {
    bool focused = e->row == GAMEPAD_EDIT_ROW_SCOPE;
    uint16_t bg = focused ? C_SEL_BG : C_PANEL_BG;
    display_fill_rect(px + 1, y, CTL_PANEL_W - 2, ITEM_H, bg);
    display_draw_text(px + 4, y + 2, focused ? ">" : " ", COLOR_WHITE, bg);
    display_draw_text(px + CTL_LABEL_X, y + 2, "Edit", COLOR_WHITE, bg);
    for (int i = 0; i < 2; i++) {
      bool on = e->this_game == (i == 0);
      int x = px + (i ? CTL_CELL_X1 : CTL_CELL_X0);
      if (on)
        display_fill_rect(x, y + 1, CTL_CELL_W, ITEM_H - 2, C_BORDER);
      display_draw_text(x + 4, y + 2, i ? "All games" : "This game",
                        on ? COLOR_WHITE : C_DIM, on ? C_BORDER : bg);
    }
    y += ITEM_H;
  }

  display_draw_text(px + CTL_LABEL_X, y + 2, "Button", C_DIM, C_PANEL_BG);
  display_draw_text(px + CTL_CELL_X0 + 4, y + 2, "Primary", C_DIM, C_PANEL_BG);
  display_draw_text(px + CTL_CELL_X1 + 4, y + 2, "Alt", C_DIM, C_PANEL_BG);
  y += ITEM_H;

  for (int b = 0; b < KBD_PAD_BUTTONS; b++, y += ITEM_H) {
    display_draw_text(px + 4, y + 2, e->row == b ? ">" : " ", COLOR_WHITE,
                      C_PANEL_BG);
    display_draw_text(px + CTL_LABEL_X, y + 2, gamepad_button_label(b),
                      COLOR_WHITE, C_PANEL_BG);
    ctl_draw_cell(e, b, 0, px + CTL_CELL_X0, y);
    ctl_draw_cell(e, b, 1, px + CTL_CELL_X1, y);
  }

  display_draw_text(px + 4, y + 2, e->notice, error ? COLOR_RED : COLOR_YELLOW,
                    C_PANEL_BG);
  y += ITEM_H;

  display_fill_rect(px + 1, y, CTL_PANEL_W - 2, 1, C_BORDER);
  display_fill_rect(px + 1, y + 1, CTL_PANEL_W - 2, 2 * FOOTER_H, C_TITLE_BG);
  const char *hint1 = "Enter:bind  C:clear  Esc:back";
  const char *hint2 = e->this_game ? "R twice:reset this game"
                                   : "R twice:reset to defaults";
  if (e->capturing) {
    hint1 = "Press the key to bind";
    hint2 = "Menu key:cancel";
  }
  display_draw_text(px + 4, y + 3, hint1, COLOR_GRAY, C_TITLE_BG);
  display_draw_text(px + 4, y + 3 + FOOTER_H, hint2, COLOR_GRAY, C_TITLE_BG);
}

// Runs until Esc (or a dev exit), then saves what changed and rebuilds the
// installed map (gamepad_apply). Keys come from the event queue, as the
// gamepad's do (keycodes: letters bind); navigation from the button masks.
// A bindings file that is there but could not be read keeps the page shut:
// the page would show the defaults and save them over it.
static void controls_run(gamepad_edit_t *e, bool in_app) {
  gamepad_edit_init(e, in_app && app_identity_current() != NULL);
  ctl_load_t load = {e, GAMEPAD_FILE_MISSING, GAMEPAD_FILE_MISSING};
  if (!app_stack_run_os(ctl_load_on_stack, &load)) {
    ctl_alert("Not enough memory for Controls");
    return;
  }
  if (load.global == GAMEPAD_FILE_UNREADABLE ||
      load.game == GAMEPAD_FILE_UNREADABLE) {
    ctl_alert("Could not read the bindings");
    return;
  }
  if (load.global == GAMEPAD_FILE_IGNORED ||
      load.game == GAMEPAD_FILE_IGNORED) {
    // Launches ignore it too: the defaults on screen are what is in use,
    // and a save replaces the broken file.
    snprintf(e->notice, sizeof(e->notice), "Ignored a corrupt bindings file");
    ctl_log(e->notice);
  }
  // The menu pages never read the event queue: drop what they left in it,
  // and a menu key pressed before now.
  kbd_flush_events();
  kbd_consume_menu_press();

  uint8_t seq = e->notice_seq;
  bool redraw = true;
  for (;;) {
    if (redraw) {
      bg_restore();
      ctl_draw(e, false);
      display_flush();
      redraw = false;
    }
    kbd_poll();
    dev_commands_poll();
    dev_commands_process();
    if (dev_commands_wants_exit())
      break;
    kbd_event_t ev;
    bool menu = kbd_consume_menu_press(); // never bound: it cancels capture
    if (e->capturing) {
      if (menu) {
        gamepad_edit_cancel(e);
        redraw = true;
      }
    } else {
      uint32_t pressed = kbd_get_buttons_pressed();
      if (pressed & BTN_ESC)
        break;
      int dy = ((pressed & BTN_DOWN) ? 1 : 0) - ((pressed & BTN_UP) ? 1 : 0);
      int dx = ((pressed & BTN_RIGHT) ? 1 : 0) - ((pressed & BTN_LEFT) ? 1 : 0);
      if (dy || dx) {
        gamepad_edit_move(e, dy, dx);
        redraw = true;
      }
      if (pressed & BTN_ENTER) {
        gamepad_edit_enter(e, kbd_is_key_down(KEY_ENTER) ? KEY_ENTER : 0);
        redraw = true;
        // The keys of this poll came before capture started.
        while (e->capturing && kbd_poll_event(&ev))
          ;
      }
    }
    while (kbd_poll_event(&ev))
      redraw |= gamepad_edit_key(e, ev.type, ev.key, ev.flags);
    if (e->notice_seq != seq) {
      seq = e->notice_seq;
      ctl_log(e->notice);
    }
    watchdog_update();
    sleep_ms(16);
  }

  if (e->global_dirty || e->game_dirty) {
    ctl_save_t job = {e, false};
    if (app_stack_run_os(ctl_save_on_stack, &job) && job.ok) {
      ctl_log("bindings saved");
    } else {
      snprintf(e->notice, sizeof(e->notice), "Could not save the bindings");
      ctl_log(e->notice);
      bg_restore();
      ctl_draw(e, true);
      display_flush();
      sleep_ms(1500);
    }
    gamepad_apply(); // the effective map, from the files as they now are
  }
}

static void controls_page(bool in_app) {
  int saved_font = display_get_font();
  display_set_font(0);
  gamepad_edit_t *e = NULL;
  if (lua_psram_alloc_largest_block() >= CTL_HEAP_MIN)
    e = (gamepad_edit_t *)umm_malloc(sizeof(*e));
  if (e) {
    controls_run(e, in_app);
    umm_free(e);
  } else {
    ctl_alert("Not enough memory for Controls");
  }
  display_set_font(saved_font);
}

// ── Shared menu loop
// ────────────────────────────────────────────────────────────────

// Persist brightness once per menu session (on close / before reboot) rather
// than on every adjustment keypress, to avoid burst SD writes.
static void save_brightness_if_changed(uint8_t entry_brightness) {
  if (s_brightness == entry_brightness)
    return;
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", s_brightness);
  config_set("brightness", buf);
  config_save();
}

// context: 0=launcher, 1=Lua app, 2=native app
// Returns true if Exit App was selected.
static bool menu_loop(lua_State *L, int context) {
  bool has_exit = (context != 0);   // both Lua and native apps have Exit App
  bool is_launcher = (context == 0);
  menu_page_t page = PAGE_MAIN;
  flat_item_t items[SYSMENU_MAX_APP_ITEMS + 12];
  int count = build_items(items, page, has_exit, is_launcher);

  int panel_h = 32 + count * ITEM_H;
  int panel_x = (FB_WIDTH - PANEL_W) / 2;
  int panel_y = (FB_HEIGHT - panel_h) / 2;

  int bat = kbd_get_battery_percent();

  display_darken();
  bg_save();

  // The menu must draw full-screen even if the app set a clip rect.
  int saved_clip_x, saved_clip_y, saved_clip_w, saved_clip_h;
  display_get_clip_rect(&saved_clip_x, &saved_clip_y, &saved_clip_w, &saved_clip_h);
  display_clear_clip_rect();

  // If the app engaged hardware scroll, its GRAM holds a rotated ring image
  // the menu's flushes would display scrambled.  Reset the scroll offset and
  // do NOT restore it on close: apps driving hardware scroll detect the
  // reset via display_get_scroll_offset() and repaint (panels.lua contract).
  // The darkened backdrop may show ring-rotated for such apps — cosmetic.
  display_set_scroll_offset(0);

  // Start from a clean keyboard: an edge from before the menu opened (a key
  // typed during a sys.sleep whose background polls also saw the Sym press,
  // or the app's own last poll) must not select or activate an item. This
  // also ends any background-poll run, so the first kbd_poll below starts a
  // normal poll. The app loses those edges; closing the menu clears the
  // state again anyway (as it always has).
  kbd_clear_state();

  int sel = 0;
  uint8_t entry_brightness = s_brightness;
  bool running = true;
  bool need_redraw = true;
  bool need_bg_restore = false;
  bool exit_requested = false;

  while (running) {
    if (need_redraw) {
      if (need_bg_restore) {
        bg_restore();
        need_bg_restore = false;
      }
      draw_panel(items, count, sel, panel_x, panel_y, panel_h, bat, page);
      display_flush();
      need_redraw = false;
    }

    kbd_poll();
    // Pump dev commands (keypress/screenshot/exit) so they don't stall
    // for the entire time this modal is open.
    dev_commands_poll();
    dev_commands_process();
    // Let a dev "exit" unwind this modal (close; the Lua hook handles exit).
    if (dev_commands_wants_exit()) {
      running = false;
      continue;
    }
    uint32_t pressed = kbd_get_buttons_pressed();

    if (pressed & BTN_UP) {
      sel = (sel > 0) ? sel - 1 : count - 1;
      need_redraw = true;
    }
    if (pressed & BTN_DOWN) {
      sel = (sel < count - 1) ? sel + 1 : 0;
      need_redraw = true;
    }

    // Left / Right: adjust brightness when on Brightness item
    if ((pressed & BTN_LEFT) && items[sel].type == ITEM_BRIGHTNESS) {
      s_brightness = (s_brightness >= 16) ? s_brightness - 16 : 0;
      kbd_set_backlight(s_brightness);
      idle_dim_set_brightness(s_brightness);
      need_redraw = true;
    }
    if ((pressed & BTN_RIGHT) && items[sel].type == ITEM_BRIGHTNESS) {
      s_brightness = (s_brightness <= 239) ? s_brightness + 16 : 255;
      kbd_set_backlight(s_brightness);
      idle_dim_set_brightness(s_brightness);
      need_redraw = true;
    }

    if (pressed & BTN_ENTER) {
      switch (items[sel].type) {
      case ITEM_APP_CB:
        s_app_items[items[sel].app_idx].callback(
            s_app_items[items[sel].app_idx].user);
        running = false;
        break;
      case ITEM_BRIGHTNESS:
        s_brightness = (s_brightness <= 239) ? s_brightness + 16 : 0;
        kbd_set_backlight(s_brightness);
        idle_dim_set_brightness(s_brightness);
        need_redraw = true;
        break;
      case ITEM_BATTERY:
      case ITEM_RAM_INFO:
      case ITEM_WIFI_STATUS:
        break; // read-only items
      case ITEM_WIFI:
        break; // legacy, unused
      case ITEM_SETTINGS:
        page = PAGE_SETTINGS;
        count = build_items(items, page, has_exit, is_launcher);
        panel_h = 32 + count * ITEM_H;
        panel_y = (FB_HEIGHT - panel_h) / 2;
        sel = 0;
        need_bg_restore = true;
        need_redraw = true;
        break;
      case ITEM_WIFI_TOGGLE:
        if (wifi_is_available()) {
          { wifi_status_t wst = wifi_get_status();
          if (wst == WIFI_STATUS_CONNECTED || wst == WIFI_STATUS_ONLINE) {
            wifi_disconnect();
          } else {
            const char *ssid = config_get("wifi_ssid");
            const char *pass = config_get("wifi_pass");
            if (ssid && ssid[0])
              wifi_connect(ssid, pass ? pass : "");
            else
              run_wifi_config(); // no saved credentials, prompt
          }}
          need_bg_restore = true;
          need_redraw = true;
        }
        break;
      case ITEM_WIFI_SETTINGS:
        if (wifi_is_available()) {
          run_wifi_config();
          need_bg_restore = true;
          need_redraw = true;
        }
        break;
      case ITEM_TIMEZONE:
        tz_picker_show();
        need_bg_restore = true;
        need_redraw = true;
        break;
      case ITEM_REMOUNT_SD: {
        display_fill_rect(panel_x + 10, panel_y + panel_h / 2 - 10,
                          PANEL_W - 20, 30, C_TITLE_BG);
        display_draw_text(panel_x + 15, panel_y + panel_h / 2 - 5,
                          "Remounting SD...", COLOR_WHITE, C_TITLE_BG);
        display_flush();

        if (sdcard_remount()) {
          display_fill_rect(panel_x + 10, panel_y + panel_h / 2 - 10,
                            PANEL_W - 20, 30, COLOR_GREEN);
          display_draw_text(panel_x + 15, panel_y + panel_h / 2 - 5,
                            "SD Remounted!", COLOR_BLACK, COLOR_GREEN);
          display_flush();
          sleep_ms(800);
          if (is_launcher)
            launcher_refresh_apps();
        } else {
          display_fill_rect(panel_x + 10, panel_y + panel_h / 2 - 10,
                            PANEL_W - 20, 30, COLOR_RED);
          display_draw_text(panel_x + 15, panel_y + panel_h / 2 - 5,
                            "Remount Failed!", COLOR_WHITE, COLOR_RED);
          display_flush();
          sleep_ms(1500);
        }
        need_bg_restore = true;
        need_redraw = true;
        break;
      }
      case ITEM_USB_MSC: {
        usb_msc_enter_mode();
        kbd_clear_state();
        kbd_recover_i2c_bus();
        if (is_launcher)
          launcher_refresh_apps();
        need_bg_restore = true;
        need_redraw = true;
        break;
      }
      case ITEM_SCREENSHOT:
        screenshot_schedule(250);
        kbd_clear_state();
        running = false;
        break;
      case ITEM_REBOOT:
        save_brightness_if_changed(entry_brightness);
        crashlog_clear_running(); // intentional — not an unclean exit
        kbd_prepare_reset();
        watchdog_enable(1, true);
        for (;;)
          tight_loop_contents();
        break; /* unreachable */
      case ITEM_REBOOT_FLASH:
        save_brightness_if_changed(entry_brightness);
        crashlog_clear_running();
        kbd_prepare_reset();
        reset_usb_boot(0, 0);
        break; /* unreachable */
      case ITEM_BATTERY_PCT:
        config_set("battery_pct", ui_battery_pct_enabled() ? "0" : "1");
        config_save();
        need_redraw = true;
        break;
      case ITEM_SHOW_FPS:
        // Off -> Top right -> Top left -> Bottom right -> Bottom left (Off
        // removes the key); the overlay picks it up when the menu closes.
        config_set("show_fps", os_overlay_fps_key((os_overlay_fps_mode() + 1) %
                                                  OS_FPS_MODES));
        config_save();
        need_redraw = true;
        break;
      case ITEM_CONTROLS:
        controls_page(!is_launcher);
        need_bg_restore = true;
        need_redraw = true;
        break;
      case ITEM_WIFI_AUTO_DISCONNECT:
        s_wifi_auto_disconnect = !s_wifi_auto_disconnect;
        config_set("wifi_auto_disconnect", s_wifi_auto_disconnect ? "1" : "0");
        config_save();
        need_redraw = true;
        break;
      case ITEM_DEV_MODE:
        s_dev_mode = !s_dev_mode;
        config_set("dev_mode", s_dev_mode ? "1" : "0");
        config_save();
        count = build_items(items, page, has_exit, is_launcher);
        panel_h = 32 + count * ITEM_H;
        panel_y = (FB_HEIGHT - panel_h) / 2;
        if (sel >= count) sel = count - 1;
        need_redraw = true;
        break;
      case ITEM_EXIT:
        system_menu_clear_items();
        exit_requested = true;
        running = false;
        break;
      }
    }

    if (pressed & BTN_ESC) {
      if (page == PAGE_SETTINGS) {
        page = PAGE_MAIN;
        count = build_items(items, page, has_exit, is_launcher);
        panel_h = 32 + count * ITEM_H;
        panel_y = (FB_HEIGHT - panel_h) / 2;
        sel = 0;
        need_bg_restore = true;
        need_redraw = true;
      } else {
        running = false;
      }
    }

    watchdog_update();
    sleep_ms(16);
  }
  bg_free();
  kbd_clear_state();
  save_brightness_if_changed(entry_brightness);
  display_set_clip_rect(saved_clip_x, saved_clip_y, saved_clip_w, saved_clip_h);
  os_overlay_reload();  // the Show FPS setting; the menu drew over the overlays
  return exit_requested;
}

// ── Public API
// ────────────────────────────────────────────────────────────────

void system_menu_init(void) {
  s_app_item_count = 0;
  s_brightness = config_parse_brightness(config_get("brightness"));
  const char *dm = config_get("dev_mode");
  s_dev_mode = (dm && strcmp(dm, "1") == 0);
  const char *wad = config_get("wifi_auto_disconnect");
  s_wifi_auto_disconnect = (!wad || strcmp(wad, "0") != 0);
}

void system_menu_add_item(const char *label, void (*callback)(void *user),
                          void *user) {
  if (s_app_item_count >= SYSMENU_MAX_APP_ITEMS)
    return;
  app_item_t *it = &s_app_items[s_app_item_count++];
  strncpy(it->label, label, sizeof(it->label) - 1);
  it->label[sizeof(it->label) - 1] = '\0';
  it->callback = callback;
  it->user = user;
}

void system_menu_clear_items(void) { s_app_item_count = 0; }

bool system_menu_get_wifi_auto_disconnect(void) { return s_wifi_auto_disconnect; }

void system_menu_show(lua_State *L) {
  // L==NULL → launcher (context 0), L!=NULL → Lua app (context 1)
  int context = (L != NULL) ? 1 : 0;
  bool exit = menu_loop(L, context);
  if (exit && L != NULL)
    lua_bridge_raise_exit(L); /* does longjmp */
}

bool system_menu_show_for_native(void) {
  return menu_loop(NULL, 2); // context 2 = native app
}
