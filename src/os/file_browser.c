#include "file_browser.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../dev_commands.h"
#include "os.h"

#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "umm_malloc.h"

// ── Visual constants (matches system_menu.c) ─────────────────────────────────

#define FB_PANEL_W 300
#define FB_TITLE_H 16
#define FB_ITEM_H 13
#define FB_FOOTER_H 12
#define FB_VISIBLE 12 // maximum visible rows at once

#define FB_C_PANEL_BG RGB565(20, 28, 50)
#define FB_C_TITLE_BG RGB565(10, 14, 30)
#define FB_C_SEL_BG RGB565(40, 80, 160)
#define FB_C_BORDER RGB565(80, 100, 150)
#define FB_C_DIR RGB565(100, 180, 255) // directory names in light blue

// ── Entry list (in PSRAM to save ~8KB SRAM for JPEG decode perf)
// ────────────────────────────────────────────────────────────────

#define MAX_ENTRIES 128

typedef struct {
  char name[256];
  bool is_dir;
} fb_entry_t;

static fb_entry_t *s_entries = NULL;
static int s_entry_count = 0;

static void collect_cb(const sdcard_entry_t *e, void *user) {
  (void)user;
  if (s_entry_count >= MAX_ENTRIES)
    return;
  if (e->name[0] == '.')
    return; // skip hidden / system entries
  strncpy(s_entries[s_entry_count].name, e->name,
          sizeof(s_entries[0].name) - 1);
  s_entries[s_entry_count].name[sizeof(s_entries[0].name) - 1] = '\0';
  s_entries[s_entry_count].is_dir = e->is_dir;
  s_entry_count++;
}

// Sort: directories first, then files; alphabetical within each group.
static int entry_cmp(const void *a, const void *b) {
  const fb_entry_t *ea = (const fb_entry_t *)a;
  const fb_entry_t *eb = (const fb_entry_t *)b;
  if (ea->is_dir != eb->is_dir)
    return ea->is_dir ? -1 : 1;
  return strcmp(ea->name, eb->name);
}

static void load_dir(const char *path) {
  if (!s_entries) {
    s_entries = (fb_entry_t *)umm_malloc(sizeof(fb_entry_t) * MAX_ENTRIES);
    if (!s_entries) return;
  }
  s_entry_count = 0;
  sdcard_list_dir(path, collect_cb, NULL);
  if (s_entry_count > 1)
    qsort(s_entries, s_entry_count, sizeof(fb_entry_t), entry_cmp);
}

// ── Drawing
// ───────────────────────────────────────────────────────────────────

static void draw_browser(const char *path, int sel, int scroll) {
  // Always show at least one row even when the directory is empty.
  int visible = s_entry_count < FB_VISIBLE ? s_entry_count : FB_VISIBLE;
  if (visible < 1)
    visible = 1;

  int panel_h = 1 + FB_TITLE_H + 1 + visible * FB_ITEM_H + 1 + FB_FOOTER_H + 1;
  int px = (FB_WIDTH - FB_PANEL_W) / 2;
  int py = (FB_HEIGHT - panel_h) / 2;

  // Outer border
  display_draw_rect(px, py, FB_PANEL_W, panel_h, FB_C_BORDER);

  // Title bar — show the current path, truncated to fit
  display_fill_rect(px + 1, py + 1, FB_PANEL_W - 2, FB_TITLE_H, FB_C_TITLE_BG);
  char title[48];
  int max_chars = (FB_PANEL_W - 8) / 6; // 6 px per char
  int plen = (int)strlen(path);
  if (plen > max_chars) {
    // Show ".../<tail>" so the deepest component is always visible
    const char *tail = path + plen - (max_chars - 3);
    snprintf(title, sizeof(title), "...%s", tail);
  } else {
    strncpy(title, path, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
  }
  display_draw_text(px + 4, py + 4, title, COLOR_WHITE, FB_C_TITLE_BG);

  // Divider below title
  display_fill_rect(px + 1, py + 1 + FB_TITLE_H, FB_PANEL_W - 2, 1,
                    FB_C_BORDER);

  // Item rows
  int items_y = py + 1 + FB_TITLE_H + 1;
  for (int i = 0; i < visible; i++) {
    int idx = i + scroll;
    int iy = items_y + i * FB_ITEM_H;

    if (s_entry_count == 0) {
      // Empty directory placeholder
      display_fill_rect(px + 1, iy, FB_PANEL_W - 2, FB_ITEM_H, FB_C_PANEL_BG);
      display_draw_text(px + 4, iy + 2, "(empty)", COLOR_GRAY, FB_C_PANEL_BG);
      break;
    }

    if (idx >= s_entry_count)
      break;

    bool selected = (idx == sel);
    uint16_t bg = selected ? FB_C_SEL_BG : FB_C_PANEL_BG;
    display_fill_rect(px + 1, iy, FB_PANEL_W - 2, FB_ITEM_H, bg);

    // Truncate name to fit the panel width; append "/" for directories
    char label[46];
    int max_name = (FB_PANEL_W - 22) / 6; // reserve space for ">" prefix
    if (max_name > (int)(sizeof(label) - 2))
      max_name = (int)sizeof(label) - 2;
    strncpy(label, s_entries[idx].name, max_name);
    label[max_name] = '\0';
    if (s_entries[idx].is_dir) {
      int llen = (int)strlen(label);
      if (llen < (int)sizeof(label) - 1) {
        label[llen] = '/';
        label[llen + 1] = '\0';
      }
    }

    uint16_t fg = s_entries[idx].is_dir ? FB_C_DIR : COLOR_WHITE;
    display_draw_text(px + 4, iy + 2, selected ? ">" : " ", COLOR_WHITE, bg);
    display_draw_text(px + 10, iy + 2, label, fg, bg);
  }

  // Divider above footer
  int footer_div_y = items_y + visible * FB_ITEM_H;
  display_fill_rect(px + 1, footer_div_y, FB_PANEL_W - 2, 1, FB_C_BORDER);

  // Footer hint
  int footer_y = footer_div_y + 1;
  display_fill_rect(px + 1, footer_y, FB_PANEL_W - 2, FB_FOOTER_H,
                    FB_C_TITLE_BG);
  display_draw_text(px + 4, footer_y + 2, "Enter:open  Esc:up/cancel",
                    COLOR_GRAY, FB_C_TITLE_BG);
}

// ── Public API
// ────────────────────────────────────────────────────────────────

bool file_browser_show(const char *start_path, const char *root_path,
                       char *out_path, int out_len) {
  char cur_path[192];
  strncpy(cur_path, start_path, sizeof(cur_path) - 1);
  cur_path[sizeof(cur_path) - 1] = '\0';

  // root_path is the highest directory the user may navigate to.
  // Fall back to start_path if caller passes NULL.
  if (!root_path)
    root_path = start_path;

  load_dir(cur_path);

  // Darken the current framebuffer once to create the overlay backdrop
  display_darken();

  // The browser draws full-screen even if the app set a clip rect.
  int saved_cx, saved_cy, saved_cw, saved_ch;
  display_get_clip_rect(&saved_cx, &saved_cy, &saved_cw, &saved_ch);
  display_clear_clip_rect();

  // Start from a clean keyboard: edges gathered by sys.sleep's background
  // polls, or injections queued before this modal opened, were not typed
  // at this browser and must not navigate or select on its behalf.
  kbd_clear_state();
  kbd_consume_menu_press();  // a stale press was not made here

  int sel = 0;
  int scroll = 0;
  bool need_redraw = true;

  while (true) {
    int visible = s_entry_count < FB_VISIBLE ? s_entry_count : FB_VISIBLE;
    if (visible < 1)
      visible = 1;

    if (need_redraw) {
      draw_browser(cur_path, sel, scroll);
      display_flush();
      need_redraw = false;
    }

    kbd_poll();
    // Pump dev commands (keypress/screenshot/exit) so they don't stall
    // for the entire time this modal is open.
    dev_commands_poll();
    dev_commands_process();
    // Let a dev "exit" unwind this modal (cancel; the Lua hook handles exit).
    if (dev_commands_wants_exit()) {
      kbd_clear_state();
      display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return false;
    }
    // The menu key cancels the browser, like Esc (and takes the latch, so it
    // cannot open the system menu once the browser has returned).
    if (kbd_consume_menu_press()) {
      kbd_clear_state();
      display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return false;
    }
    uint32_t pressed = kbd_get_buttons_pressed();

    if (pressed & BTN_UP) {
      if (sel > 0) {
        sel--;
        if (sel < scroll)
          scroll = sel;
        need_redraw = true;
      }
    }

    if (pressed & BTN_DOWN) {
      if (s_entry_count > 0 && sel < s_entry_count - 1) {
        sel++;
        if (sel >= scroll + visible)
          scroll = sel - visible + 1;
        need_redraw = true;
      }
    }

    if (pressed & BTN_ENTER) {
      if (s_entry_count > 0 && sel < s_entry_count) {
        if (s_entries[sel].is_dir) {
          // Navigate into the selected directory
          char new_path[256];
          snprintf(new_path, sizeof(new_path), "%s/%s", cur_path,
                   s_entries[sel].name);
          strncpy(cur_path, new_path, sizeof(cur_path) - 1);
          cur_path[sizeof(cur_path) - 1] = '\0';
          load_dir(cur_path);
          sel = 0;
          scroll = 0;
          need_redraw = true;
        } else {
          // File selected — return the full path
          snprintf(out_path, out_len, "%s/%s", cur_path, s_entries[sel].name);
          // Clear the button edge-latch before returning (matches
          // system_menu.c) so the caller's own button checks don't see a
          // stale Enter/Esc edge from this modal's input handling.
          kbd_clear_state();
          display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return true;
        }
      }
    }

    if (pressed & BTN_ESC) {
      // Go up one directory; cancel if already at (or above) root_path
      if (strcmp(cur_path, root_path) == 0) {
        // Clear the button edge-latch before returning (matches
        // system_menu.c) so the caller's own button checks don't see a
        // stale Esc edge from this modal's input handling.
        kbd_clear_state();
        display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return false; // already at the sandbox root — cancel
      }
      char *last_slash = strrchr(cur_path, '/');
      if (last_slash && last_slash != cur_path) {
        *last_slash = '\0';
        // Don't let navigation go above root_path
        if (strlen(cur_path) < strlen(root_path))
          strncpy(cur_path, root_path, sizeof(cur_path) - 1);
        load_dir(cur_path);
        sel = 0;
        scroll = 0;
        need_redraw = true;
      } else {
        // Clear the button edge-latch before returning (matches
        // system_menu.c) so the caller's own button checks don't see a
        // stale Esc edge from this modal's input handling.
        kbd_clear_state();
        display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch); return false;
      }
    }

    watchdog_update();
    sleep_ms(16);
  }
}
