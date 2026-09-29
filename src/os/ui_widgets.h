#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../drivers/display.h"

// ── Widget color constants (matching existing PicoDeck UI theme) ────────────────

#define UW_BG         RGB565(20,  28,  50)   // Panel background
#define UW_BG_INPUT   RGB565( 5,  10,  20)   // Input field background
#define UW_BORDER     RGB565(80, 100, 150)   // Unfocused border
#define UW_FOCUS      RGB565(100, 140, 220)  // Focused border
#define UW_ACCENT     RGB565(40,  80, 160)   // Selection highlight
#define UW_TITLE_BG   RGB565(10,  14,  30)   // Title bar background
#define UW_TOAST_BG   RGB565(40,  40,  40)   // Toast background
#define UW_BTN_BG     RGB565(30,  40,  70)   // Button background
#define UW_BTN_FOCUS  RGB565(45,  60, 100)   // Button focused background

// ── Stateless widget rendering primitives ────────────────────────────────────
// These functions draw widgets to the framebuffer with no internal state.
// All state (text, cursor, focus) is managed by the caller (Lua side).

// Panel with optional title bar
void ui_widget_panel(int x, int y, int w, int h,
                     const char *title);

// Single-line text field with cursor and horizontal scroll
void ui_widget_textfield(int x, int y, int w,
                         const char *text, int text_len,
                         int cursor_pos, int scroll_offset,
                         bool focused, bool show_cursor);

// Multiline text area with pre-computed wrap segments
void ui_widget_textarea(int x, int y, int w, int h,
                        const char *text, int text_len,
                        const int *wrap_segments, int num_segments,
                        int scroll_y, int visible_rows,
                        int cursor_row, int cursor_col,
                        bool focused, bool show_cursor);

// Single list item row
void ui_widget_list_item(int x, int y, int w,
                         const char *text,
                         bool selected, bool focused);

// Progress bar (progress: 0.0 to 1.0)
void ui_widget_progress(int x, int y, int w, int h,
                        float progress,
                        uint16_t fill_color, uint16_t border);

// Checkbox indicator (12x12 box with optional checkmark)
void ui_widget_checkbox(int x, int y, bool checked, bool focused);

// Radio button indicator (12x12 circle with optional dot)
void ui_widget_radio(int x, int y, bool selected, bool focused);

// Horizontal divider line
void ui_widget_divider(int x, int y, int w, uint16_t color);

// Toast notification bar (bg_color=0 uses default UW_TOAST_BG)
void ui_widget_toast(int y, const char *text, uint16_t bg_color);
// The x, width and height ui_widget_toast(y, text, ...) covers, in the
// current font (x is negative for text wider than the screen).
void ui_widget_toast_rect(const char *text, int *x, int *w, int *h);

// Button with centered label
void ui_widget_button(int x, int y, int w,
                      const char *label, bool focused, bool pressed);
