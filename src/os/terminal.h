#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TERM_DEFAULT_COLS 53
#define TERM_DEFAULT_ROWS 26
#define TERM_DEFAULT_SCROLLBACK 1000
#define TERM_MAX_SCROLLBACK 5000
#define TERM_BLANK_CELL 0x0020

#define TERM_ATTR_BOLD      (1 << 0)
#define TERM_ATTR_ITALIC    (1 << 1)
#define TERM_ATTR_UNDERLINE (1 << 2)
#define TERM_ATTR_BLINK     (1 << 3)
#define TERM_ATTR_INVERSE   (1 << 4)
#define TERM_ATTR_STRIKE    (1 << 5)
#define TERM_ATTR_DIM       (1 << 6)
#define TERM_ATTR_HIDDEN    (1 << 7)

enum terminal_font {
    TERM_FONT_SCIENTIFICA = 0,
    TERM_FONT_SCIENTIFICA_BOLD = 1
};

struct terminal {
    int cols;
    int rows;
    int cursor_x;
    int cursor_y;
    uint16_t fg_color;
    uint16_t bg_color;
    uint8_t current_attr;
    enum terminal_font font;
    int scroll_top;     // top of scroll region (0-based, default 0)
    int scroll_bottom;  // bottom of scroll region (0-based, default rows-1)

    uint16_t* cells;
    uint16_t* prev_cells;
    uint16_t* fg_colors;    // per-cell foreground RGB565
    uint16_t* bg_colors;    // per-cell background RGB565

    int scrollback_lines;
    int scrollback_pos;
    int scrollback_count;
    int scrollback_offset;  // 0 = live view, >0 = lines scrolled up
    uint16_t* scrollback;
    uint16_t* scrollback_fg;   // per-cell foreground colors in scrollback
    uint16_t* scrollback_bg;   // per-cell background colors in scrollback

    bool full_dirty;
    uint8_t* row_dirty;

    // Line numbers
    bool line_numbers_enabled;
    int line_number_start;      // Starting line number (default 1)
    int line_number_cols;       // Width in columns (default 5)
    uint16_t line_number_fg;    // Color for line numbers
    uint16_t line_number_bg;    // Background color for gutter

    // Scrollbar
    bool scrollbar_enabled;
    uint16_t scrollbar_bg;      // Track color
    uint16_t scrollbar_thumb;   // Thumb color
    int scrollbar_width;        // Width in pixels (default 4)
    int total_lines;            // Total document lines for thumb calculation
    int scroll_position;        // Current scroll position (0 = top)
    bool scrollbar_visible;     // Show scrollbar only when actively scrolling
    uint32_t scrollbar_last_scroll_time;  // Last scroll activity timestamp

    // Word wrap (visual - content not modified)
    bool word_wrap_enabled;      // Word wrap on/off
    int word_wrap_column;        // 0 = auto (use content cols), >0 = specific column
    bool show_wrap_indicator;    // Show "…" at wrap points (default true)
    int wrap_viewport_start;     // First logical line in current viewport
    int wrap_viewport_scroll;    // Current scroll position in visual rows

    // Line continuation flags (for word wrap logical line reconstruction)
    // 1 = this row continues the previous logical line (auto-wrapped at buffer edge)
    // 0 = this row starts a new logical line (from \n or start of buffer)
    uint8_t* row_continuation;
    uint8_t* scrollback_continuation;  // continuation flags for scrollback buffer

    // Render bounds (pixel coordinates)
    int render_y_start;   // Top pixel Y (default 28, below header)
    int render_y_end;     // Bottom pixel Y exclusive (default 302, above footer)
    int render_x_start;   // Left pixel X (default 4, padding)

    // The back buffer the last render drew into: flush() swaps without copying,
    // so a dirty-row redraw is only partial while this is still the back buffer.
    const uint16_t* last_render_fb;
};

typedef struct terminal terminal_t;

terminal_t* terminal_new(int cols, int rows, int scrollback_lines);

void terminal_free(terminal_t* term);

void terminal_clear(terminal_t* term);

void terminal_clearRow(terminal_t* term, int row);

void terminal_putChar(terminal_t* term, char c);

void terminal_putString(terminal_t* term, const char* s);

void terminal_setCursor(terminal_t* term, int x, int y);

void terminal_getCursor(terminal_t* term, int* out_x, int* out_y);

void terminal_setColors(terminal_t* term, uint16_t fg, uint16_t bg);

void terminal_getColors(terminal_t* term, uint16_t* out_fg, uint16_t* out_bg);

void terminal_setAttribute(terminal_t* term, uint8_t attr);

void terminal_clearAttribute(terminal_t* term, uint8_t attr);

void terminal_scroll(terminal_t* term, int lines);

void terminal_scrollUp(terminal_t* term);

void terminal_scrollDown(terminal_t* term);

void terminal_eraseDisplay(terminal_t* term, int mode);

void terminal_eraseLine(terminal_t* term, int mode);

int terminal_getCols(terminal_t* term);

int terminal_getRows(terminal_t* term);

uint16_t terminal_getCell(terminal_t* term, int x, int y);

void terminal_setCell(terminal_t* term, int x, int y, uint16_t cell);

uint16_t terminal_getScrollback(terminal_t* term, int line);

int terminal_getScrollbackCount(terminal_t* term);

void terminal_getScrollbackLine(terminal_t* term, int line, uint16_t* out_cells);

void terminal_getScrollbackLineColors(terminal_t* term, int line, uint16_t* out_fg, uint16_t* out_bg);

void terminal_setScrollbackOffset(terminal_t* term, int offset);

int terminal_getScrollbackOffset(terminal_t* term);

void terminal_markAllDirty(terminal_t* term);

void terminal_markRowDirty(terminal_t* term, int row);

bool terminal_isRowDirty(terminal_t* term, int row);

void terminal_clearRowDirty(terminal_t* term, int row);

void terminal_getDirtyRange(terminal_t* term, int* out_first, int* out_last);

bool terminal_isFullDirty(terminal_t* term);

void terminal_clearFullDirty(terminal_t* term);

void terminal_setFont(terminal_t* term, enum terminal_font font);

enum terminal_font terminal_getFont(terminal_t* term);

void terminal_insertChars(terminal_t* term, int count);

void terminal_deleteChars(terminal_t* term, int count);

void terminal_setScrollRegion(terminal_t* term, int top, int bottom);

// Line numbers
void terminal_setLineNumbers(terminal_t* term, bool enabled);
void terminal_setLineNumberStart(terminal_t* term, int start);
void terminal_setLineNumberCols(terminal_t* term, int cols);
void terminal_setLineNumberColors(terminal_t* term, uint16_t fg, uint16_t bg);
int terminal_getContentCols(terminal_t* term);  // Returns cols - line_number_cols

// Scrollbar
void terminal_setScrollbar(terminal_t* term, bool enabled);
void terminal_setScrollbarColors(terminal_t* term, uint16_t bg, uint16_t thumb);
void terminal_setScrollbarWidth(terminal_t* term, int width);
void terminal_setScrollInfo(terminal_t* term, int total_lines, int scroll_position);

// Render bounds
void terminal_setRenderBounds(terminal_t* term, int y_start, int y_end);

// Word wrap (visual)
void terminal_setWordWrap(terminal_t* term, bool enabled);
void terminal_setWordWrapColumn(terminal_t* term, int column);
void terminal_setWrapIndicator(terminal_t* term, bool enabled);
bool terminal_getWordWrap(terminal_t* term);
int terminal_getVisualRowCount(terminal_t* term);  // Total visual rows for scrollbar
// Convert between logical and visual coordinates
void terminal_logicalToVisual(terminal_t* term, int log_x, int log_y, int* vis_x, int* vis_y);
void terminal_visualToLogical(terminal_t* term, int vis_x, int vis_y, int* log_x, int* log_y);
// Calculate wrap for a specific line (streaming)
int terminal_calculateLineWraps(terminal_t* term, int logical_line, int* segments, int max_segments);
