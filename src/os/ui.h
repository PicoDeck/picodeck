#pragma once

#include <stdbool.h>
#include <stdint.h>

// Header bar height; ui_draw_header also draws a 1px border below it.
#define UI_HEADER_H 20

// Draw standard OS header (titlebar) with battery/wifi/clock status indicators.
// Returns the rows it covers (UI_HEADER_H + 1): content starts at that y.
int ui_draw_header(const char *title);

// Check if the header needs redrawing (e.g. clock or wifi status changed)
bool ui_needs_header_redraw(void);

// The battery_pct system setting: the header shows the charge as digits
// inside the battery icon instead of a fill bar.
bool ui_battery_pct_enabled(void);

// Draw standard OS footer with optional left and right alignment texts
void ui_draw_footer(const char *left_text, const char *right_text);

// Draw tab container with customizable keyboard shortcuts
// tabs: array of tab labels (NULL-terminated)
// count: number of tabs
// active_index: 0-based index of active tab (highlighted)
// y: vertical position
int ui_draw_tabs(const char **tabs, int count, int active_index, int y);

// Draw fullscreen splash screen with an optional status and subtext message
void ui_draw_splash(const char *status, const char *subtext);

// Show a blocking text-input dialog overlaid on the current screen.
// prompt:       label shown above the input field (may be NULL)
// default_val:  pre-filled text (may be NULL for empty)
// out_buf:      caller-supplied buffer that receives the typed string
// out_len:      size of out_buf (including NUL terminator)
// Returns true if the user confirmed (Enter), false if cancelled (Esc).
bool ui_text_input(const char *prompt, const char *default_val,
                   char *out_buf, int out_len);

// Show a blocking yes/no confirmation dialog overlaid on the current screen.
// message: text to display (up to two lines; wrapped at ~43-char boundaries)
// Returns true if the user pressed Enter/Y, false if Esc/N.
bool ui_confirm(const char *message);

// Draw one frame of a rotating line spinner at center (cx, cy) with radius r.
// Call once per frame with an incrementing frame counter for animation.
void ui_draw_spinner(int cx, int cy, int r, int frame);
