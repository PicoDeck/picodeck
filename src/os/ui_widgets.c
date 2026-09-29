#include "ui_widgets.h"
#include "../drivers/display.h"
#include <string.h>

// Font metrics (6x8 default font)
#define FW 6
#define FH 8
#define ROW_H 13   // FH + 5px padding

// ── Panel ────────────────────────────────────────────────────────────────────

void ui_widget_panel(int x, int y, int w, int h, const char *title) {
    // Border
    display_draw_rect(x, y, w, h, UW_BORDER);

    if (title && title[0]) {
        // Title bar
        int title_h = 16;
        display_fill_rect(x + 1, y + 1, w - 2, title_h, UW_TITLE_BG);
        int tw = display_text_width(title);
        display_draw_text(x + (w - tw) / 2, y + 5, title, COLOR_WHITE, UW_TITLE_BG);
        // Divider below title
        display_fill_rect(x + 1, y + 1 + title_h, w - 2, 1, UW_BORDER);
        // Body
        display_fill_rect(x + 1, y + 2 + title_h, w - 2, h - 3 - title_h, UW_BG);
    } else {
        // Body only
        display_fill_rect(x + 1, y + 1, w - 2, h - 2, UW_BG);
    }
}

// ── Text Field ───────────────────────────────────────────────────────────────

void ui_widget_textfield(int x, int y, int w,
                         const char *text, int text_len,
                         int cursor_pos, int scroll_offset,
                         bool focused, bool show_cursor) {
    uint16_t border = focused ? UW_FOCUS : UW_BORDER;
    int h = ROW_H;

    // Background and border
    display_draw_rect(x, y, w, h, border);
    display_fill_rect(x + 1, y + 1, w - 2, h - 2, UW_BG_INPUT);

    // Text area with 4px left padding
    int text_x = x + 4;
    int text_y = y + 3;  // vertically center 8px font in 13px row
    int max_chars = (w - 8) / FW;

    // Visible slice of text
    if (text && text_len > 0) {
        int vis_start = scroll_offset;
        if (vis_start > text_len) vis_start = text_len;
        int vis_len = text_len - vis_start;
        if (vis_len > max_chars) vis_len = max_chars;

        // Draw visible text character by character (text may not be null-terminated at vis_len)
        char vis_buf[54];  // max 53 cols + null
        if (vis_len > (int)sizeof(vis_buf) - 1) vis_len = (int)sizeof(vis_buf) - 1;
        memcpy(vis_buf, text + vis_start, vis_len);
        vis_buf[vis_len] = '\0';
        display_draw_text(text_x, text_y, vis_buf, COLOR_WHITE, UW_BG_INPUT);
    }

    // Cursor
    if (show_cursor && focused) {
        int cursor_vis_pos = cursor_pos - scroll_offset;
        if (cursor_vis_pos >= 0 && cursor_vis_pos <= max_chars) {
            int cx = text_x + cursor_vis_pos * FW;
            display_fill_rect(cx, text_y, 2, FH, COLOR_WHITE);
        }
    }
}

// ── Text Area ────────────────────────────────────────────────────────────────

void ui_widget_textarea(int x, int y, int w, int h,
                        const char *text, int text_len,
                        const int *wrap_segments, int num_segments,
                        int scroll_y, int visible_rows,
                        int cursor_row, int cursor_col,
                        bool focused, bool show_cursor) {
    uint16_t border = focused ? UW_FOCUS : UW_BORDER;

    // Border and background
    display_draw_rect(x, y, w, h, border);
    display_fill_rect(x + 1, y + 1, w - 2, h - 2, UW_BG_INPUT);

    int text_x = x + 4;
    int text_y = y + 3;
    int max_chars = (w - 8) / FW;

    // Draw visible rows
    for (int vi = 0; vi < visible_rows; vi++) {
        int seg_idx = scroll_y + vi;
        if (seg_idx >= num_segments) break;

        int seg_start = wrap_segments[seg_idx];
        int seg_end;
        if (seg_idx + 1 < num_segments) {
            seg_end = wrap_segments[seg_idx + 1];
        } else {
            seg_end = text_len;
        }

        int seg_len = seg_end - seg_start;
        if (seg_len > max_chars) seg_len = max_chars;
        if (seg_len < 0) seg_len = 0;

        int row_y = text_y + vi * FH;
        if (row_y + FH > y + h - 1) break;  // clip to widget bounds

        if (seg_len > 0 && text) {
            char row_buf[54];
            if (seg_len > (int)sizeof(row_buf) - 1) seg_len = (int)sizeof(row_buf) - 1;
            // Copy segment, replacing newlines/control chars with spaces
            int out = 0;
            for (int j = 0; j < seg_len; j++) {
                char ch = text[seg_start + j];
                row_buf[out++] = (ch >= 0x20 && ch < 0x7F) ? ch : ' ';
            }
            row_buf[out] = '\0';
            display_draw_text(text_x, row_y, row_buf, COLOR_WHITE, UW_BG_INPUT);
        }

        // Cursor on this row
        if (show_cursor && focused && seg_idx == cursor_row) {
            int cx = text_x + cursor_col * FW;
            if (cx < x + w - 2) {
                display_fill_rect(cx, row_y, 2, FH, COLOR_WHITE);
            }
        }
    }

    // Scrollbar (if content exceeds visible area)
    if (num_segments > visible_rows) {
        int sb_x = x + w - 4;
        int sb_y = y + 2;
        int sb_h = h - 4;
        display_fill_rect(sb_x, sb_y, 2, sb_h, UW_BORDER);

        int thumb_h = sb_h * visible_rows / num_segments;
        if (thumb_h < 4) thumb_h = 4;
        int thumb_y = sb_y + (sb_h - thumb_h) * scroll_y / (num_segments - visible_rows);
        display_fill_rect(sb_x, thumb_y, 2, thumb_h, COLOR_WHITE);
    }
}

// ── List Item ────────────────────────────────────────────────────────────────

void ui_widget_list_item(int x, int y, int w,
                         const char *text,
                         bool selected, bool focused) {
    uint16_t bg = selected ? UW_ACCENT : UW_BG;
    uint16_t fg = COLOR_WHITE;

    display_fill_rect(x, y, w, ROW_H, bg);

    if (focused && selected) {
        display_draw_rect(x, y, w, ROW_H, UW_FOCUS);
    }

    if (text && text[0]) {
        int max_chars = (w - 8) / FW;
        char buf[54];
        int len = (int)strlen(text);
        if (len > max_chars) len = max_chars;
        if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        memcpy(buf, text, len);
        buf[len] = '\0';
        display_draw_text(x + 4, y + 3, buf, fg, bg);
    }
}

// ── Progress Bar ─────────────────────────────────────────────────────────────

void ui_widget_progress(int x, int y, int w, int h,
                        float progress,
                        uint16_t fill_color, uint16_t border) {
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    // Border
    display_draw_rect(x, y, w, h, border);

    // Background
    display_fill_rect(x + 1, y + 1, w - 2, h - 2, UW_BG_INPUT);

    // Fill
    int fill_w = (int)((w - 2) * progress);
    if (fill_w > 0) {
        display_fill_rect(x + 1, y + 1, fill_w, h - 2, fill_color);
    }
}

// ── Checkbox ─────────────────────────────────────────────────────────────────

void ui_widget_checkbox(int x, int y, bool checked, bool focused) {
    int sz = 10;
    uint16_t border = focused ? UW_FOCUS : UW_BORDER;

    // Box
    display_draw_rect(x, y, sz, sz, border);
    display_fill_rect(x + 1, y + 1, sz - 2, sz - 2, UW_BG_INPUT);

    // Checkmark (two lines forming a check)
    if (checked) {
        uint16_t ck = COLOR_GREEN;
        // Short stroke: (x+2,y+5) to (x+4,y+7)
        display_draw_line(x + 2, y + 5, x + 4, y + 7, ck);
        // Long stroke: (x+4,y+7) to (x+8,y+2)
        display_draw_line(x + 4, y + 7, x + 8, y + 2, ck);
    }
}

// ── Radio Button ─────────────────────────────────────────────────────────────

void ui_widget_radio(int x, int y, bool selected, bool focused) {
    int cx = x + 5;
    int cy = y + 5;
    uint16_t border = focused ? UW_FOCUS : UW_BORDER;

    // Outer circle
    display_draw_circle(cx, cy, 5, border);

    // Inner fill (clear)
    display_fill_circle(cx, cy, 4, UW_BG_INPUT);

    // Selected dot
    if (selected) {
        display_fill_circle(cx, cy, 3, COLOR_GREEN);
    }
}

// ── Divider ──────────────────────────────────────────────────────────────────

void ui_widget_divider(int x, int y, int w, uint16_t color) {
    display_fill_rect(x, y, w, 1, color);
}

// ── Toast ────────────────────────────────────────────────────────────────────

#define TOAST_PAD 8

void ui_widget_toast_rect(const char *text, int *x, int *w, int *h) {
    *w = display_text_width(text) + TOAST_PAD * 2;
    *h = FH + 6;
    *x = (FB_WIDTH - *w) / 2;
}

void ui_widget_toast(int y, const char *text, uint16_t bg_color) {
    if (bg_color == 0) bg_color = UW_TOAST_BG;
    int x, w, h;
    ui_widget_toast_rect(text, &x, &w, &h);

    display_fill_rect(x, y, w, h, bg_color);
    display_draw_rect(x, y, w, h, UW_BORDER);
    display_draw_text(x + TOAST_PAD, y + 3, text, COLOR_WHITE, bg_color);
}

// ── Button ───────────────────────────────────────────────────────────────────

void ui_widget_button(int x, int y, int w,
                      const char *label, bool focused, bool pressed) {
    int h = ROW_H + 2;  // 15px
    uint16_t bg = pressed ? UW_ACCENT : (focused ? UW_BTN_FOCUS : UW_BTN_BG);
    uint16_t border = focused ? UW_FOCUS : UW_BORDER;

    display_draw_rect(x, y, w, h, border);
    display_fill_rect(x + 1, y + 1, w - 2, h - 2, bg);

    if (label && label[0]) {
        int tw = display_text_width(label);
        int tx = x + (w - tw) / 2;
        int ty = y + 4;  // vertically center
        display_draw_text(tx, ty, label, COLOR_WHITE, bg);
    }
}
