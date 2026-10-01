#include "launcher.h"
#include "launcher_types.h"
#include "crashlog.h"
#include "app_manifest.h"
#include "app_runner.h"
#include "sim_hooks.h"
#include "lua_runner.h"
#include "native_loader.h"
#include "ota_update.h"
#include "perf.h"
#include "app_stack.h"
#include "zip_archive.h"
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/image_api.h"
#include "../drivers/keyboard.h"
#include "../drivers/mp3_player.h"
#include "../drivers/pio_psram.h"
#include "../drivers/sdcard.h"
#include "../drivers/wifi.h"
#include "../fonts/font_registry.h"

#include "clock.h"
#include "config.h"
#include "lua_psram_alloc.h"
#include "os_overlay.h"
#include "screenshot.h"
#include "system_menu.h"
#include "ui.h"
#include "umm_malloc.h"

#include <ctype.h>
#include <stdatomic.h>

#include "../dev_commands.h"
#include "../usb/usb_msc.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ── App discovery
// ─────────────────────────────────────────────────────────────

// App table lives in PSRAM; the only SRAM cost is s_cat_indices (uint8_t,
// so the cap can go to 255 before the index type needs widening).
#define MAX_APPS 64

static app_entry_t *s_apps = NULL;
static int s_app_count = 0;

// app.json is read by app_manifest.c (bounded, escape-aware, host-tested).

static void on_app_dir(const sdcard_entry_t *entry, void *user) {
  (void)user;
  if (!entry->is_dir)
    return;
  if (entry->name[0] == '.')
    return;
  if (s_app_count >= MAX_APPS) {
    printf("[LAUNCHER] WARNING: app cap (%d) reached, ignoring '%s'\n",
           MAX_APPS, entry->name);
    return;
  }

  // Detect available runtimes
  char lua_path[160], elf_path[160];
  snprintf(lua_path, sizeof(lua_path), "/apps/%s/main.lua", entry->name);
  snprintf(elf_path, sizeof(elf_path), "/apps/%s/main.elf", entry->name);
  bool has_lua = sdcard_fexists(lua_path);
  bool has_elf = sdcard_fexists(elf_path);

  if (!has_lua && !has_elf)
    return;

  // Native wins when both exist (rare, but log it)
  if (has_lua && has_elf)
    printf("[LAUNCHER] '%s': both main.lua and main.elf found — using native\n",
           entry->name);

  app_entry_t *app = &s_apps[s_app_count];
  snprintf(app->path, sizeof(app->path), "/apps/%s", entry->name);
  app->type                = has_elf ? APP_TYPE_NATIVE : APP_TYPE_LUA;

  // Try to load app.json for display name / description / id / requirements
  char json_path[160];
  snprintf(json_path, sizeof(json_path), "/apps/%s/app.json", entry->name);
  int json_len = 0;
  char *json = sdcard_read_file(json_path, &json_len);
  if (json) {
    app_manifest_parse(json, json_len > 0 ? (size_t)json_len : 0, entry->name,
                       app);
    umm_free(json);
  } else {
    printf("[LAUNCHER] WARNING: failed to read '%s', using dir name\n", json_path);
    app_manifest_defaults(entry->name, app);
  }

  // Try to load app icon (PNG first, then BMP)
  app->icon = NULL;
  char icon_path[160];
  snprintf(icon_path, sizeof(icon_path), "/apps/%s/icon.png", entry->name);
  app->icon = image_load(icon_path);
  if (!app->icon) {
    snprintf(icon_path, sizeof(icon_path), "/apps/%s/icon.bmp", entry->name);
    app->icon = image_load(icon_path);
  }

  s_app_count++;
}

static int compare_app_name(const void *a, const void *b) {
  return strcasecmp(((const app_entry_t *)a)->name,
                    ((const app_entry_t *)b)->name);
}

// Ids are folded, so two apps whose ids differ only in case (or were
// copied) share /data/<id> and the per-app config store.  Say so at scan
// time; the launcher does not guess which one is the impostor.
// dev `reboot-ota`, run on the OS stack (see the launcher loop).
typedef struct {
  const char *err;
} launcher_ota_ctx_t;

static void launcher_reboot_ota(void *arg) {
  launcher_ota_ctx_t *ctx = (launcher_ota_ctx_t *)arg;
  ota_trigger_update(OTA_BIN_PATH, &ctx->err);  // returns only on failure
}

static void warn_shared_ids(void) {
  for (int i = 0; i < s_app_count; i++) {
    for (int j = i + 1; j < s_app_count; j++) {
      if (strcmp(s_apps[i].id, s_apps[j].id) != 0)
        continue;
      printf("[LAUNCHER] WARNING: %s and %s share id %s (one /data dir)\n",
             s_apps[i].path, s_apps[j].path, s_apps[i].id);
      sim_log_os("[LAUNCHER] WARNING: %s and %s share id %s (one /data dir)",
                 s_apps[i].path, s_apps[j].path, s_apps[i].id);
    }
  }
}

static void scan_apps(void) {
  // Free previously loaded icons before rescan
  for (int i = 0; i < s_app_count; i++) {
    if (s_apps[i].icon) {
      image_free((pc_image_t *)s_apps[i].icon);
      s_apps[i].icon = NULL;
    }
  }

  s_app_count = 0;
  memset(s_apps, 0, sizeof(app_entry_t) * MAX_APPS);
  printf("[LAUNCHER] Scanning /apps directory...\n");
  fflush(stdout);
  sdcard_list_dir("/apps", on_app_dir, NULL);
  if (s_app_count > 1)
    qsort(s_apps, s_app_count, sizeof(app_entry_t), compare_app_name);
  warn_shared_ids();
  printf("[LAUNCHER] Found %d apps\n", s_app_count);
  fflush(stdout);
}

// ── Category system ──────────────────────────────────────────────────────────

typedef enum {
  CAT_GAMES = 0,
  CAT_TOOLS,
  CAT_SYSTEM,
  CAT_DEMOS,
  CAT_EMULATORS,
  CAT_NETWORK,
  CAT_COUNT,
  CAT_ALL  // virtual: unfiltered
} category_t;

static const char *s_cat_names[CAT_COUNT] = {
    "Games", "Tools", "System", "Demos", "Emulators", "Network"};

static const uint16_t s_cat_colors[CAT_COUNT] = {
    RGB565(255, 100, 50),   // Games: orange
    RGB565(100, 180, 255),  // Tools: blue
    RGB565(160, 160, 160),  // System: gray
    RGB565(255, 200, 50),   // Demos: yellow
    RGB565(150, 100, 255),  // Emulators: purple
    RGB565(50, 200, 150),   // Network: teal
};

// Per-category app indices into s_apps[]
_Static_assert(MAX_APPS <= 255, "s_cat_indices is uint8_t");
static uint8_t s_cat_indices[CAT_COUNT][MAX_APPS];
static int s_cat_counts[CAT_COUNT];

static category_t parse_category(const char *cat_str) {
  if (!cat_str || !cat_str[0]) return CAT_DEMOS;  // default
  if (strcasecmp(cat_str, "games") == 0) return CAT_GAMES;
  if (strcasecmp(cat_str, "tools") == 0) return CAT_TOOLS;
  if (strcasecmp(cat_str, "system") == 0) return CAT_SYSTEM;
  if (strcasecmp(cat_str, "demos") == 0) return CAT_DEMOS;
  if (strcasecmp(cat_str, "emulators") == 0) return CAT_EMULATORS;
  if (strcasecmp(cat_str, "network") == 0) return CAT_NETWORK;
  return CAT_DEMOS;
}

static void build_category_indices(void) {
  memset(s_cat_counts, 0, sizeof(s_cat_counts));
  for (int i = 0; i < s_app_count; i++) {
    category_t cat = parse_category(s_apps[i].category);
    if (cat < CAT_COUNT)
      s_cat_indices[cat][s_cat_counts[cat]++] = (uint8_t)i;
  }
}

// ── Launcher rendering ──────────────────────────────────────────────────────

#define ITEM_H 32          // row pitch; the selection bar is ITEM_H - 2
#define LIST_X 8
#define LIST_Y (TAB_BAR_Y + TAB_BAR_H + 1 + 2)  // tab bar, its border, 2px gap
#define LIST_VISIBLE 8
#define DESC_SCROLL_RESET_PAUSE 40
#define ICON_SIZE   24     // app icon size in list view (icon.png is scaled to it)
#define ICON_DY     ((ITEM_H - 2 - ICON_SIZE) / 2)  // icon centred in the bar
#define NAME_DY     6      // name + description lines, centred on the icon
#define DESC_DY     17

#define TAB_COUNT   (CAT_COUNT + 1)  // "All" + 6 categories

static const char *s_tab_names[TAB_COUNT] = {
    "All", "Games", "Tools", "System", "Demos", "Emulators", "Network"};

static const uint16_t s_tab_colors[TAB_COUNT] = {
    RGB565(140, 160, 255),  // All: light blue
    RGB565(255, 100, 50),   // Games: orange
    RGB565(100, 180, 255),  // Tools: blue
    RGB565(160, 160, 160),  // System: gray
    RGB565(255, 200, 50),   // Demos: yellow
    RGB565(150, 100, 255),  // Emulators: purple
    RGB565(50, 200, 150),   // Network: teal
};

static int s_active_tab = 0;  // 0 = All, 1..6 = categories

static int s_selected = 0;
static int s_scroll = 0;
static int s_desc_scroll = 0;
static int s_desc_scroll_timer = 0;

void launcher_refresh_apps(void) {
  scan_apps();
  build_category_indices();
  s_selected = 0;
  s_scroll = 0;
}

#define C_BG COLOR_BLACK
#define C_HEADER_BG RGB565(20, 20, 60)
#define C_SEL_BG RGB565(40, 80, 160)
#define C_TEXT COLOR_WHITE
#define C_TEXT_DIM COLOR_GRAY
#define C_BORDER RGB565(60, 60, 100)

// ── Tab bar (horizontal category tabs) ──────────────────────────────────────

#define TAB_BAR_Y    (UI_HEADER_H + 1)  // below the header's border
#define TAB_BAR_H    18
#define TAB_DOT_W    14    // width of an unselected tab (colored dot)
#define TAB_DOT_R    3     // dot radius
#define TAB_GAP      2
#define TAB_OUTER_PAD 6
#define TAB_LABEL_PAD 6    // horizontal padding around selected label text
#define TAB_SEL_BG   RGB565(40, 60, 120)

static void draw_tab_bar(void) {
  display_fill_rect(0, TAB_BAR_Y, FB_WIDTH, TAB_BAR_H, C_HEADER_BG);
  display_fill_rect(0, TAB_BAR_Y + TAB_BAR_H, FB_WIDTH, 1, C_BORDER);

  // Calculate selected tab width
  const char *sel_label = s_tab_names[s_active_tab];
  int sel_label_w = display_text_width(sel_label);
  int sel_w = sel_label_w + TAB_LABEL_PAD * 2;

  // Calculate total width to center the tab bar
  int total_w = sel_w + (TAB_COUNT - 1) * TAB_DOT_W + (TAB_COUNT - 1) * TAB_GAP;
  int x = (FB_WIDTH - total_w) / 2;

  for (int i = 0; i < TAB_COUNT; i++) {
    if (i == s_active_tab) {
      // Selected: highlight bg + text label + bottom accent line
      uint16_t accent = s_tab_colors[i];
      display_fill_rect(x, TAB_BAR_Y + 1, sel_w, TAB_BAR_H - 2, TAB_SEL_BG);
      display_fill_rect(x, TAB_BAR_Y + TAB_BAR_H - 3, sel_w, 3, accent);
      int tx = x + (sel_w - sel_label_w) / 2;
      display_draw_text(tx, TAB_BAR_Y + (TAB_BAR_H - 8) / 2, sel_label,
                        C_TEXT, TAB_SEL_BG);
      x += sel_w + TAB_GAP;
    } else {
      // Unselected: colored dot
      int cx = x + TAB_DOT_W / 2;
      int cy = TAB_BAR_Y + TAB_BAR_H / 2;
      display_fill_circle(cx, cy, TAB_DOT_R, s_tab_colors[i]);
      x += TAB_DOT_W + TAB_GAP;
    }
  }
}

// ── Fallback app icon: a cartridge in the category colour ──────────────────
// Rows of the 24x24 icon: '.' the dark icon background (shared with the
// shipped full-colour icons), 'c' the category colour, 'h' / 'l' / 'd' its
// highlight, shade and dark tints, 'w' the label. The app's first letter
// goes on the label at 2x, in the dark tint.

#define C_ICON_BG    RGB565(12, 16, 48)
#define C_CART_LABEL RGB565(239, 232, 212)
#define CART_LETTER_X 7   // 2x glyph cell on the label
#define CART_LETTER_Y 6

_Static_assert(ICON_SIZE == 24, "k_cart is drawn for 24px icons");
static const char *const k_cart[24] = {
    "........................",
    "...hhhhhhhhhhhhhhh......",
    "...hcccccccccccccccc....",
    "...hcclllllllllllccc....",
    "...hccccccccccccccccc...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...hcwwwwwwwwwwwwwwwl...",
    "...llllllllllllllllll...",
    "........ddddddddd.......",
};

// Scale each RGB565 channel by num/den, clamped.
static uint16_t tint565(uint16_t c, int num, int den) {
  int r = ((c >> 11) & 0x1F) * num / den;
  int g = ((c >> 5) & 0x3F) * num / den;
  int b = (c & 0x1F) * num / den;
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_fallback_icon(int x, int y, const char *name,
                               const char *category) {
  category_t cat = parse_category(category);
  uint16_t body = (cat < CAT_COUNT) ? s_cat_colors[cat] : COLOR_GRAY;
  uint16_t dark = tint565(body, 9, 20);
  for (int r = 0; r < 24; r++) {
    for (int c = 0; c < 24; c++) {
      uint16_t col;
      switch (k_cart[r][c]) {
      case 'c': col = body; break;
      case 'h': col = tint565(body, 6, 5); break;
      case 'l': col = tint565(body, 7, 10); break;
      case 'd': col = dark; break;
      case 'w': col = C_CART_LABEL; break;
      default:  col = C_ICON_BG; break;
      }
      display_set_pixel(x + c, y + r, col);
    }
  }

  // The first letter from the built-in 6x8 font, each pixel as a 2x2 block.
  const pc_font_t *f = font_registry_get(0);
  unsigned char ch = (name && name[0]) ? (unsigned char)toupper((unsigned char)name[0]) : '?';
  if (!f || ch < f->first || ch > f->last)
    return;
  const uint8_t *glyph = f->bitmaps + (ch - f->first) * f->height * f->stride;
  for (int r = 0; r < f->height; r++)
    for (int c = 0; c < f->max_width; c++)
      if (glyph[r * f->stride] & (0x80 >> c))
        display_fill_rect(x + CART_LETTER_X + 2 * c, y + CART_LETTER_Y + 2 * r,
                          2, 2, dark);
}

// ── Draw app icon (image or fallback) ───────────────────────────────────────

static void draw_app_icon(int x, int y, int size, const app_entry_t *app) {
  if (app->icon) {
    image_draw_scaled((pc_image_t *)app->icon, x, y, size, size);
  } else {
    draw_fallback_icon(x, y, app->name, app->category);
  }
}

// ── App list helpers ────────────────────────────────────────────────────────

static int list_count(void) {
  if (s_active_tab == 0) return s_app_count;  // "All"
  int cat = s_active_tab - 1;
  if (cat < CAT_COUNT) return s_cat_counts[cat];
  return 0;
}

static int list_app_idx(int list_idx) {
  if (s_active_tab == 0) return list_idx;  // "All"
  int cat = s_active_tab - 1;
  if (cat < CAT_COUNT && list_idx < s_cat_counts[cat])
    return s_cat_indices[cat][list_idx];
  return 0;
}

static void draw_app_list(void) {
  display_clear(C_BG);
  ui_draw_header("PicoDeck");
  draw_tab_bar();

  int count = list_count();

  if (count == 0) {
    display_draw_text(8, LIST_Y + 20, "No apps in this category.",
                      C_TEXT_DIM, C_BG);
    ui_draw_footer("L/R:Category  F10:Menu", NULL);
    display_flush();
    return;
  }

  int text_x = LIST_X + ICON_SIZE + 4;  // shifted right for icon

  for (int i = 0; i < LIST_VISIBLE && (i + s_scroll) < count; i++) {
    int list_idx = i + s_scroll;
    int app_idx = list_app_idx(list_idx);
    int y = LIST_Y + i * ITEM_H;
    bool sel = (list_idx == s_selected);

    uint16_t bg = sel ? C_SEL_BG : C_BG;
    display_fill_rect(LIST_X - 4, y, FB_WIDTH - LIST_X * 2 + 8, ITEM_H - 2, bg);

    // App icon
    draw_app_icon(LIST_X, y + ICON_DY, ICON_SIZE, &s_apps[app_idx]);

    // App name
    display_draw_text(text_x, y + NAME_DY, s_apps[app_idx].name, C_TEXT, bg);

    // Version (right-aligned)
    if (s_apps[app_idx].version[0]) {
      int vw = display_text_width(s_apps[app_idx].version);
      display_draw_text(FB_WIDTH - 8 - vw, y + NAME_DY, s_apps[app_idx].version,
                        C_TEXT_DIM, bg);
    }

    // Description
    if (s_apps[app_idx].description[0]) {
      int max_w = FB_WIDTH - text_x - 8;
      int tw = display_text_width(s_apps[app_idx].description);
      if (tw > max_w && sel) {
        const char *p = s_apps[app_idx].description + s_desc_scroll;
        int avail = strlen(p);
        char buf[64];
        int out_len = (max_w / 6 + 1);
        if (out_len > avail) out_len = avail;
        if (out_len > 63) out_len = 63;
        strncpy(buf, p, out_len);
        buf[out_len] = '\0';
        display_draw_text(text_x, y + DESC_DY, buf, C_TEXT_DIM, bg);
      } else {
        display_draw_text(text_x, y + DESC_DY, s_apps[app_idx].description,
                          C_TEXT_DIM, bg);
      }
    }
  }

  // Scrollbar
  if (count > LIST_VISIBLE) {
    int bar_h = (LIST_VISIBLE * ITEM_H) * LIST_VISIBLE / count;
    if (bar_h < 4) bar_h = 4;
    int bar_y = LIST_Y + (LIST_VISIBLE * ITEM_H) * s_scroll / count;
    display_fill_rect(FB_WIDTH - 6, LIST_Y, 4, LIST_VISIBLE * ITEM_H, C_BORDER);
    display_fill_rect(FB_WIDTH - 6, bar_y, 4, bar_h, C_TEXT);
  }

  ui_draw_footer("L/R:Category  Enter:Launch  F10:Menu", NULL);
  display_flush();
}

// ── Unified draw dispatch ───────────────────────────────────────────────────

static void draw_current_view(void) {
  draw_app_list();
}

// ── Runner dispatch table ─────────────────────────────────────────────────────

static const AppRunner *s_runners[] = {
    &g_lua_runner,
    &g_native_runner,
    NULL,
};

// ── App launcher
// ──────────────────────────────────────────────────────────────

extern _Atomic bool g_core1_pause;
extern _Atomic bool g_core1_paused;

// Retime the QMI CS1 (PSRAM) interface whenever clk_sys changes.  A bare
// sysclk change silently scales the PSRAM SCK with it and reads start
// glitching (verified live: Doom died in 3-22 s at 300 MHz on the old serial
// mode).  In quad mode (drivers/qmi_psram.c) the full timing (CLKDIV,
// MAX_SELECT, MIN_DESELECT — all in sysclk units) is recomputed; if the boot
// quad init fell back to serial mode, rescale CLKDIV to keep SCK ≤50 MHz as
// before.
//
// Two entry points, because a clock change is not atomic: PSRAM keeps
// running at the OLD clkdiv from set_sys_clock_khz until something retimes
// it, so a bare "recompute for sys_khz" call at either end of the change can
// momentarily leave SCK above what the boot-calibrated RXDELAY was proven at
// (Task 4b: this HardFaulted the Lua VM's PSRAM stack about 1 in 8 boosts).
//   - psram_qmi_prescale(): call BEFORE set_sys_clock_khz, with the current
//     and the target clk_sys. In quad mode this is timing safe at both
//     (qmi_psram_prescale_timing/qmi_psram_timing); in serial mode there is
//     no fine-grained timing to pre-scale, so it rescales CLKDIV for the
//     faster (worse) of the two, as before.
//   - psram_qmi_retime(): call for the clk_sys actually in force — after a
//     successful set_sys_clock_khz, or with the old clk_sys on the abort
//     path — to retime exactly for steady-state running there.
#define PSRAM_QMI_SERIAL_MAX_SCK_KHZ 50000u
#if defined(PICO_RP2350) && !defined(PICODECK_SIMULATOR)
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "drivers/qmi_psram.h"
static void psram_qmi_serial_clkdiv(uint32_t sys_khz) {
  uint32_t clkdiv = (sys_khz + PSRAM_QMI_SERIAL_MAX_SCK_KHZ - 1)
                  / PSRAM_QMI_SERIAL_MAX_SCK_KHZ;
  if (clkdiv < 1) clkdiv = 1;
  if (clkdiv > QMI_M1_TIMING_CLKDIV_BITS >> QMI_M1_TIMING_CLKDIV_LSB)
    clkdiv = QMI_M1_TIMING_CLKDIV_BITS >> QMI_M1_TIMING_CLKDIV_LSB;
  uint32_t save = save_and_disable_interrupts();
  uint32_t timing = qmi_hw->m[1].timing;
  timing = (timing & ~QMI_M1_TIMING_CLKDIV_BITS)
         | (clkdiv << QMI_M1_TIMING_CLKDIV_LSB);
  qmi_hw->m[1].timing = timing;
  __asm volatile ("dsb sy" ::: "memory");
  restore_interrupts(save);
  printf("[LAUNCHER] QMI PSRAM (serial) clkdiv=%lu (%lu kHz SCK at %lu kHz sysclk)\n",
         (unsigned long)clkdiv, (unsigned long)(sys_khz / clkdiv),
         (unsigned long)sys_khz);
}
static void psram_qmi_prescale(uint32_t current_khz, uint32_t khz) {
  if (qmi_psram_is_quad()) {
    qmi_psram_prescale_timing(current_khz * 1000, khz * 1000);
    return;
  }
  psram_qmi_serial_clkdiv(khz > current_khz ? khz : current_khz);
}
static void psram_qmi_retime(uint32_t sys_khz) {
  if (qmi_psram_is_quad()) {
    qmi_psram_update_timing();
    return;
  }
  psram_qmi_serial_clkdiv(sys_khz);
}
#else
static void psram_qmi_prescale(uint32_t current_khz, uint32_t khz) {
  (void)current_khz; (void)khz;
}
static void psram_qmi_retime(uint32_t sys_khz) { (void)sys_khz; }
#endif

void launcher_apply_clock(uint32_t khz) {
  if (khz == 0) khz = 200000; // Default OS clock
  uint32_t current_khz = clock_get_hz(clk_sys) / 1000;
  if (khz == current_khz) return;

  printf("[LAUNCHER] Changing clock: %lu -> %lu MHz\n", 
         (unsigned long)(current_khz / 1000), (unsigned long)(khz / 1000));

  // 1. Pause Core 1 background tasks (WiFi/Audio) to avoid bus corruption.
  // Paused Core 1 relays the watchdog only while Core 0's heartbeat is
  // fresh, so kick it here: the whole change takes milliseconds, well
  // inside the 10 s timeout, and a hang in it still resets.
  watchdog_update();
  g_core1_pause = true;
  for (int i = 0; i < 200 && !g_core1_paused; i++)
    sleep_ms(1);
  if (!g_core1_paused) {
    // Changing VREG/PLL/QMI timing under a running Core 1 risks corrupting
    // its PSRAM/SD/WiFi traffic mid-transfer. Nothing has been touched yet
    // (no VREG, no QMI/PIO PSRAM timing, no PLL), so give up cleanly: the
    // app runs at the current clock.
    printf("[LAUNCHER] Core 1 pause timeout (200ms): clock change to %lu MHz "
           "aborted, staying at %lu MHz\n",
           (unsigned long)(khz / 1000), (unsigned long)(current_khz / 1000));
    g_core1_pause = false;
    return;
  }

  // 1b. Stop the keyboard bus engine at a transaction boundary: from the
  // clock switch until kbd_apply_clock() (step 7, which restarts it) the I2C
  // divider is wrong for clk_sys.
  kbd_pause_bus();

  // 2. Up-clocking: Raise voltage BEFORE increasing frequency
  if (khz > current_khz) {
    enum vreg_voltage v = VREG_VOLTAGE_DEFAULT;  // 1.10V, good to ~200 MHz
    if (khz >= 400000)
      v = VREG_VOLTAGE_1_25;
    else if (khz >= 300000)
      v = VREG_VOLTAGE_1_20;
    else if (khz >= 250000)
      v = VREG_VOLTAGE_1_15;

    if (v != VREG_VOLTAGE_DEFAULT) {
      vreg_set_voltage(v);
      sleep_ms(2);
    }
  }

  // 3. Ensure display DMA is finished before changing clock source
  display_apply_clock(); // This now waits for DMA internally

  // 3b. Pre-scale the QMI PSRAM timing so it stays safe at BOTH the current
  // and the target clk_sys for the whole transition. The old code did no
  // pre-scale at all in quad mode: it applied the current clock's timing,
  // so PSRAM ran the old divider on the new clk_sys until the retime (only
  // serial mode took the faster clock's clkdiv). qmi_psram_prescale_timing()
  // computes all three fields (CLKDIV, MAX_SELECT, MIN_DESELECT) for both
  // clocks together (Task 4b).
  psram_qmi_prescale(current_khz, khz);
  pio_psram_set_sysclk(khz > current_khz ? khz : current_khz);

  // 4. Apply the new system clock
  bool ok = set_sys_clock_khz(khz, false);

  if (!ok) {
    printf("[LAUNCHER] Clock change to %lu MHz failed (PLL cannot produce this frequency)\n",
           (unsigned long)(khz / 1000));
    psram_qmi_retime(current_khz);
    pio_psram_set_sysclk(current_khz);
    kbd_resume_bus();  // The clock did not change, so the old divider is still right.
    g_core1_pause = false;
    return;
  }

  // 4b. Retune the QMI PSRAM divider exactly for the new sysclk.
  psram_qmi_retime(khz);
  pio_psram_set_sysclk(khz);

  // 5. Re-configure peripheral clock so SPI/I2C/UART/PWM stay stable.
  clock_configure(
      clk_peri,
      0,
      CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
      khz * 1000,
      khz * 1000);

  // 6. Update display PIO divider for new clk_sys frequency
  display_apply_clock();

  // 6b. The audio output's PWM divider comes from clk_sys: re-derive it
  // (it ran at the old rate for the few ms since step 4).
  audio_apply_clock();

  // 7. Update keyboard I2C divider for new clk_sys frequency, and re-start
  // the bus engine paused in step 1b.
  kbd_apply_clock();

  // 7b. Re-set SD SPI baud rate (derived from clk_peri)
  sdcard_apply_clock();

  // 8. Re-init UART baud rate (depends on clk_peri)
#if LIB_PICO_STDIO_UART
  uart_init(uart0, 115200);
#endif

  // 9. Down-clocking: Lower voltage AFTER decreasing frequency
  if (khz < current_khz) {
    enum vreg_voltage v = VREG_VOLTAGE_DEFAULT;
    if (khz >= 400000)
      v = VREG_VOLTAGE_1_25;
    else if (khz >= 300000)
      v = VREG_VOLTAGE_1_20;
    else if (khz >= 250000)
      v = VREG_VOLTAGE_1_15;

    vreg_set_voltage(v);
    sleep_ms(2);
  }

  // 10. Resume Core 1
  g_core1_pause = false;
}

static volatile const char *s_running_app_name = NULL;
static volatile uint32_t s_app_launch_time_ms = 0;

// Refuse a launch: error.log entry, test-channel outcome, and a screen with
// the reason held for 3 s (skipped in --test-mode).
static void launcher_refuse(const app_entry_t *app, const char *title,
                            const char *reason, const char *detail,
                            const char *heap, const char *hint) {
  // The app never started: a dev `exit` during the hold must not name it.
  s_running_app_name = NULL;
  s_app_launch_time_ms = 0;
  crashlog_write("APP FAILED", app->name, reason, detail);
  sim_log_err("[LAUNCHER] %s: %s: %s", app->name, reason, detail);
#ifdef PICODECK_SIMULATOR
  {
    char outcome[160];
    snprintf(outcome, sizeof(outcome), "%s: %s", reason, detail);
    sim_app_outcome_set(SIM_APP_RESULT_LOAD_FAILED, outcome);
  }
#endif
  display_clear(COLOR_BLACK);
  display_draw_text(8, 8, title, COLOR_RED, COLOR_BLACK);
  display_draw_text(8, 20, app->name, COLOR_WHITE, COLOR_BLACK);
  display_draw_text(8, 36, reason, COLOR_WHITE, COLOR_BLACK);
  display_draw_text(8, 48, detail, COLOR_WHITE, COLOR_BLACK);
  display_draw_text(8, 64, heap, COLOR_GRAY, COLOR_BLACK);
  display_draw_text(8, 88, hint, COLOR_GRAY, COLOR_BLACK);
  display_flush();
  // Hold the reason on screen; keep serving dev commands meanwhile (still
  // launcher context, no app running) so a screenshot can capture it.
  // A dev `exit` or `launch` ends the hold early; the launcher loop then
  // clears the exit and runs any pending launch.
  for (int i = 0; i < 30 && !sim_test_mode(); i++) {
    watchdog_update();
    dev_commands_poll();
    dev_commands_process();
    if (dev_commands_wants_exit())
      break;
    sleep_ms(100);
  }
}

static bool run_app(int idx) {
  if (idx < 0 || idx >= s_app_count)
    return false;

  app_entry_t *app = &s_apps[idx];
  s_running_app_name = app->name;
  s_app_launch_time_ms = to_ms_since_boot(get_absolute_time());
  sim_app_outcome_begin(app->name, app->id);

  // Free any PSRAM used by the MP3 player (from a previous Lua app)
  // so we have maximum memory for the next app.
  mp3_player_deinit();

  const char *type_str = app->type == APP_TYPE_NATIVE ? "native" : "lua";
  char heap[96];
  crashlog_describe_heap(heap, sizeof(heap));
  printf("[LAUNCHER] Starting app %d '%s' (type=%s), PSRAM %s\n",
         idx, app->name, type_str, heap);

  // An id is used as a path component (/data/<id>/...): refuse one that could
  // escape it ("../evil", "a/b") instead of sandboxing the app into someone
  // else's directory.
  if (!app_manifest_id_valid(app->id)) {
    launcher_refuse(app, "Cannot launch app:", "invalid app id in app.json",
                    app->id, heap, "Use only A-Z a-z 0-9 . _ - in \"id\".");
    return false;
  }

  // Refuse up front when the app declares a contiguous-PSRAM requirement the
  // heap cannot meet.  Total free bytes are not the test: a native image or
  // a big asset arena needs ONE block, and a fragmented heap fails that with
  // plenty free.  Failing here gives a readable reason instead of the app
  // dying part-way through its own init.
  if (app->min_psram_kb > 0) {
    size_t largest_kb = lua_psram_alloc_largest_block() / 1024u;
    if (largest_kb < app->min_psram_kb) {
      char detail[96];
      snprintf(detail, sizeof(detail),
               "needs %luK contiguous, largest block %luK",
               (unsigned long)app->min_psram_kb, (unsigned long)largest_kb);
      launcher_refuse(app, "Not enough memory to launch:",
                      "not enough PSRAM to launch", detail, heap,
                      "Reboot to defragment the heap.");
      return false;
    }
  }

  // ── Shared pre-launch setup ───────────────────────────────────────────────

  // Disconnect WiFi before clock change if app doesn't need it.
  // The CYW43 PIO SPI clock divider is set at init time (200 MHz) and is NOT
  // updated by launcher_apply_clock(), so running WiFi at a different sys clock
  // causes SPI timing failures ("hdr mismatch" errors) that stall Core 1.
  if (app->system_clock_khz > 0 && !app->has_http && wifi_is_available()) {
    wifi_status_t wst = wifi_get_status();
    if (wst == WIFI_STATUS_CONNECTED || wst == WIFI_STATUS_CONNECTING ||
        wst == WIFI_STATUS_ONLINE) {
      wifi_disconnect();
    }
    // Wait for Core 1 to finish the hardware disconnect (bounded) before the
    // clock change: changing sysclk while the driver still talks to the chip
    // triggers an "hdr mismatch" error storm on Core 1.
    for (int i = 0; i < WIFI_HW_DISCONNECT_WAIT_MS / 5 && !wifi_hw_disconnected();
         i++)
      sleep_ms(5);  // bounded (WIFI_HW_DISCONNECT_WAIT_MS)
    if (!wifi_hw_disconnected())
      printf("[LAUNCHER] WiFi not idle after %d ms; changing clock anyway\n",
             WIFI_HW_DISCONNECT_WAIT_MS);
  }

  if (app->system_clock_khz > 0) {
    launcher_apply_clock(app->system_clock_khz);
  }

  wifi_set_http_required(app->has_http);

  if (app->has_http && wifi_is_available()) {
    wifi_status_t wst2 = wifi_get_status();
    if (wst2 != WIFI_STATUS_CONNECTED && wst2 != WIFI_STATUS_ONLINE) {
      const char *ssid = config_get("wifi_ssid");
      const char *pass = config_get("wifi_pass");
      if (ssid && ssid[0])
        wifi_connect(ssid, pass ? pass : "");
    }
  }

  // ── Dispatch to runner ────────────────────────────────────────────────────
  // Apps inherit global display state: always hand them a full-screen clip
  // rect and an identity scroll offset so a previous app's setClipRect or
  // hardware-scroll registers can't leak into the next one.
  display_clear_clip_rect();
  display_set_scroll_offset(0);
  display_set_font(0);            // nor the previous app's font selection
  font_registry_unload_all();     // nor its loaded fonts
  perf_init();                    // nor its setTargetFPS pacing and frame history
  os_overlay_app_start();         // a fresh FPS count, the current Show FPS

  // Dirty-exit marker: if this file still exists at the next boot, the app
  // never returned to the launcher (hang → watchdog, hardfault, panic, or
  // power loss) and the boot code reports it with the app's name.
  crashlog_mark_running(app->id, app->name, type_str);

  bool ok = false;
  for (int i = 0; s_runners[i]; i++) {
    if (s_runners[i]->can_handle(app)) {
      ok = s_runners[i]->run(app);
      break;
    }
  }

  // The runner returned, so whatever happened has been reported already.
  crashlog_clear_running();
  sim_app_outcome_end(ok);

  // ── Shared post-exit cleanup ──────────────────────────────────────────────
  display_clear_clip_rect();      // don't let an app's clip rect leak back to the launcher
  display_set_scroll_offset(0);   // nor its hardware-scroll offset
  display_set_font(0);            // nor the previous app's font selection
  font_registry_unload_all();     // nor its loaded fonts
  perf_init();                    // nor its perf pacing target

  if (app->system_clock_khz > 0) {
    launcher_apply_clock(200000); // Reset to system default
  }

  system_menu_clear_items();

  // Native apps have no GC — reclaim any archive handles they leaked so the
  // fixed pool is whole for the next app.
  zip_archive_close_all();

  s_running_app_name = NULL;
  s_app_launch_time_ms = 0;

  crashlog_describe_heap(heap, sizeof(heap));
  printf("[LAUNCHER] App '%s' exited (ok=%d), PSRAM %s\n", app->name, ok, heap);

  return ok;
}

// ── Public interface
// ──────────────────────────────────────────────────────────

// ── Input: unified navigation ───────────────────────────────────────────────

static void handle_input(uint32_t pressed, bool *dirty) {
  int count = list_count();

  // LEFT/RIGHT: switch tabs
  if (pressed & BTN_LEFT) {
    if (s_active_tab > 0) s_active_tab--;
    else s_active_tab = TAB_COUNT - 1;
    s_selected = 0;
    s_scroll = 0;
    s_desc_scroll = 0;
    s_desc_scroll_timer = 0;
    *dirty = true;
  }
  if (pressed & BTN_RIGHT) {
    if (s_active_tab < TAB_COUNT - 1) s_active_tab++;
    else s_active_tab = 0;
    s_selected = 0;
    s_scroll = 0;
    s_desc_scroll = 0;
    s_desc_scroll_timer = 0;
    *dirty = true;
  }

  // UP/DOWN: navigate app list
  if (pressed & BTN_UP) {
    if (count > 0) {
      if (s_selected > 0) s_selected--;
      else s_selected = count - 1;
      if (s_selected < s_scroll) s_scroll = s_selected;
      if (s_selected >= s_scroll + LIST_VISIBLE)
        s_scroll = s_selected - LIST_VISIBLE + 1;
      s_desc_scroll = 0;
      s_desc_scroll_timer = 0;
      *dirty = true;
    }
  }
  if (pressed & BTN_DOWN) {
    if (count > 0) {
      if (s_selected < count - 1) s_selected++;
      else s_selected = 0;
      if (s_selected >= s_scroll + LIST_VISIBLE)
        s_scroll = s_selected - LIST_VISIBLE + 1;
      if (s_selected < s_scroll) s_scroll = s_selected;
      s_desc_scroll = 0;
      s_desc_scroll_timer = 0;
      *dirty = true;
    }
  }

  // ENTER: launch app
  if (pressed & BTN_ENTER) {
    if (count > 0 && s_selected < count) {
      int app_idx = list_app_idx(s_selected);
      int saved_tab = s_active_tab;

      run_app(app_idx);
      kbd_clear_state();
      scan_apps();
      build_category_indices();

      s_active_tab = saved_tab;
      s_desc_scroll = 0;
      s_desc_scroll_timer = 0;

      // Clamp selection if app count changed
      int new_count = list_count();
      if (s_selected >= new_count) s_selected = new_count > 0 ? new_count - 1 : 0;
      if (s_scroll > s_selected) s_scroll = s_selected;

      *dirty = true;
    }
  }
}

// ── Description scroll update (for list views) ─────────────────────────────

static void update_desc_scroll(bool *dirty) {
  int count = list_count();
  s_desc_scroll_timer++;
  if (s_desc_scroll_timer >= 10) {
    s_desc_scroll_timer = 0;
    if (s_selected < count) {
      int app_idx = list_app_idx(s_selected);
      if (s_apps[app_idx].description[0]) {
        int text_x = LIST_X + ICON_SIZE + 4;
        int max_w = FB_WIDTH - text_x - 8;
        int tw = display_text_width(s_apps[app_idx].description);
        int desc_len = strlen(s_apps[app_idx].description);
        int max_scroll = desc_len - (max_w / 6);
        if (tw > max_w) {
          if (s_desc_scroll < max_scroll) {
            s_desc_scroll++;
            *dirty = true;
          } else {
            s_desc_scroll = 0;
            s_desc_scroll_timer = -DESC_SCROLL_RESET_PAUSE;
            *dirty = true;
          }
        }
      }
    }
  }
}

// ── Main entry point ────────────────────────────────────────────────────────

void launcher_run(void) {
  if (!s_apps) {
    s_apps = umm_malloc(sizeof(app_entry_t) * MAX_APPS);
    if (!s_apps) {
      printf("[LAUNCHER] FATAL: failed to alloc s_apps in PSRAM\n");
      return;
    }
    memset(s_apps, 0, sizeof(app_entry_t) * MAX_APPS);
  }
  scan_apps();
  build_category_indices();

  // Check for simulator auto-launch
#ifdef PICODECK_SIMULATOR
  extern const char* simulator_get_auto_launch_app(void);
  const char* auto_launch = simulator_get_auto_launch_app();
  if (auto_launch) {
    printf("[LAUNCHER] Auto-launching: %s\n", auto_launch);
    fflush(stdout);
    if (launcher_launch_by_name(auto_launch)) {
      printf("[LAUNCHER] Auto-launched app exited, returning to launcher\n");
      fflush(stdout);
    } else {
      printf("[LAUNCHER] Failed to auto-launch: %s\n", auto_launch);
      fflush(stdout);
    }
  }
#else
  (void)0;
#endif

  draw_current_view();

  while (true) {
    kbd_poll();
    watchdog_update();

    dev_commands_poll();
    dev_commands_process();

#ifdef PICODECK_SIMULATOR
    extern bool sim_handler_check_launch(void);
    bool sim_launched = sim_handler_check_launch();
#else
    bool sim_launched = false;
#endif

    bool dirty = sim_launched;
    if (dev_commands_get_pending_launch()) {
      // Take the request before running it: the name points into the dev
      // command buffer, and a `launch` that arrives while this one runs (or
      // during a refusal's hold) must stay pending, not be cleared after.
      char name[64];
      snprintf(name, sizeof(name), "%s", dev_commands_get_pending_launch());
      dev_commands_clear_pending_launch();
      dev_commands_clear_exit();
      if (launcher_launch_by_name(name)) {
        kbd_clear_state();
        scan_apps();
        build_category_indices();
        dirty = true;
      }
    }

#ifdef PICODECK_SIMULATOR
    {
      // Simulator shutdown (window close, SIGINT, the shutdown RPC) is the
      // only way out of the launcher. It used to ride the dev exit flag, so
      // an `exit` at the launcher also quit the simulator.
      extern volatile int g_running;
      if (!g_running) {
        printf("[LAUNCHER] Simulator shutting down\n");
        fflush(stdout);
        break;
      }
    }
#endif
    // A dev `exit` with no app running (already answered "no app running"
    // by the command), or one left over after a modal or an app already
    // closed, has nothing to exit: drop it. launcher_run must never return
    // on hardware — main() would fall into newlib's _exit, whose breakpoint
    // HardFaults and reboots the device.
    if (dev_commands_wants_exit())
      dev_commands_clear_exit();

    if (dev_commands_wants_list()) {
      launcher_list_apps();
      dev_commands_clear_list();
    }
    if (dev_commands_wants_reboot()) {
      printf("[DEV] Rebooting...\n");
      stdio_flush();
      sleep_ms(100);
      kbd_prepare_reset();
      watchdog_reboot(0, 0, 0);
    }
    if (dev_commands_wants_reboot_ota()) {
      // The host staged /system/update.bin + .sha256 + .sig (ota_flash.py):
      // validate (hash + ECDSA verify), set the one-shot OTA token and
      // reboot. Does not return on success. Runs on the 32 KB OS stack:
      // the signature check is too deep for the launcher's 4 KB MSP.
      dev_commands_clear_reboot_ota();
      launcher_ota_ctx_t ctx = {0};
      if (!app_stack_run_os(launcher_reboot_ota, &ctx))
        ctx.err = "out of memory for the OS stack";
      printf("[DEV] reboot-ota failed: %s\n",
             ctx.err ? ctx.err : "unknown error");
    }
    if (dev_commands_wants_reboot_flash()) {
      printf("[DEV] Rebooting to BOOTSEL mode...\n");
      stdio_flush();
      sleep_ms(100);
      kbd_prepare_reset();
      reset_usb_boot(0, 0);
    }
    if (dev_commands_wants_usb()) {
      dev_commands_clear_usb();
      usb_msc_enter_mode();
      kbd_clear_state();
      dirty = true;
    }

    if (kbd_consume_menu_press()) {
      system_menu_show(NULL);
      dirty = true;
    }
    if (kbd_consume_screenshot_press())
      screenshot_save();
    if (screenshot_check_scheduled())
      screenshot_save();

    // A pad picks and launches apps too (D-pad, A = Enter).
    uint32_t pressed = kbd_get_buttons_pressed() | kbd_get_pad_nav_pressed();

    if (pressed)
      handle_input(pressed, &dirty);

    if (ui_needs_header_redraw())
      dirty = true;

    // Description scrolling (list views only)
    update_desc_scroll(&dirty);

    if (dirty)
      draw_current_view();

    sleep_ms(16); // ~60 Hz polling
  }
}

// ── Dev command support ─────────────────────────────────────────────────────

void launcher_list_apps(void) {
  printf("[DEV] Available apps:\n");
  for (int i = 0; i < s_app_count; i++) {
    printf("  %s  (%s)\n", s_apps[i].name, s_apps[i].id);
  }
  printf("[DEV] Total: %d apps\n", s_app_count);
}

bool launcher_launch_by_id(const char *id) {
  if (!id || !id[0]) {
    return false;
  }

  // Find the app by ID
  for (int i = 0; i < s_app_count; i++) {
    if (strcmp(s_apps[i].id, id) == 0) {
      printf("[DEV] Launching app by ID: %s (%s)\n", id, s_apps[i].name);
      stdio_flush();
      run_app(i);
      return true;
    }
  }

  return false;
}

bool launcher_launch_by_name(const char *name) {
  if (!name || !name[0]) {
    printf("[DEV] Error: no app name provided\n");
    return false;
  }

  // First try to match by ID
  if (launcher_launch_by_id(name)) {
    return true;
  }

  // Fall back to display name match (case-insensitive)
  for (int i = 0; i < s_app_count; i++) {
    if (strcasecmp(s_apps[i].name, name) == 0) {
      printf("[DEV] Launching app: %s\n", name);
      stdio_flush();
      run_app(i);
      return true;
    }
  }

  // Fall back to directory name match (e.g. "doom" matches "/apps/doom")
  for (int i = 0; i < s_app_count; i++) {
    const char *slash = strrchr(s_apps[i].path, '/');
    const char *dirname = slash ? slash + 1 : s_apps[i].path;
    if (strcasecmp(dirname, name) == 0) {
      printf("[DEV] Launching app by dir: %s (%s)\n", name, s_apps[i].name);
      stdio_flush();
      run_app(i);
      return true;
    }
  }

  printf("[DEV] Error: app '%s' not found\n", name);
  return false;
}

const char* launcher_get_running_app_name(void) {
  return (const char*)s_running_app_name;
}

uint32_t launcher_get_app_uptime_ms(void) {
  uint32_t t = s_app_launch_time_ms;
  if (t == 0) return 0;
  return to_ms_since_boot(get_absolute_time()) - t;
}
