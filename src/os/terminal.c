#include "terminal.h"
#include "umm_malloc.h"
#include <string.h>
#include <stdio.h>

// Shared scratch buffer for logical line text reconstruction.
// Allocated in PSRAM via umm_malloc on first use. Single-threaded on Core 0.
// Also used by terminal_render.c via extern.
#define WRAP_BUF_SIZE 1400
char* g_terminal_wrap_buf = NULL;

static void ensure_wrap_buf(void) {
    if (!g_terminal_wrap_buf) {
        g_terminal_wrap_buf = (char*)umm_malloc(WRAP_BUF_SIZE);
    }
}

terminal_t* terminal_new(int cols, int rows, int scrollback_lines) {
    ensure_wrap_buf();
    if (cols <= 0) cols = TERM_DEFAULT_COLS;
    if (rows <= 0) rows = TERM_DEFAULT_ROWS;
    if (scrollback_lines <= 0) scrollback_lines = TERM_DEFAULT_SCROLLBACK;
    if (scrollback_lines > TERM_MAX_SCROLLBACK) scrollback_lines = TERM_MAX_SCROLLBACK;

    terminal_t* term = (terminal_t*)umm_malloc(sizeof(terminal_t));
    if (!term) return NULL;

    term->cols = cols;
    term->rows = rows;
    term->scrollback_lines = scrollback_lines;
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->fg_color = 0xFFFF;
    term->bg_color = 0x0000;
    term->current_attr = 0;
    term->font = TERM_FONT_SCIENTIFICA;
    term->scroll_top = 0;
    term->scroll_bottom = rows - 1;
    term->full_dirty = true;
    term->scrollback_pos = 0;
    term->scrollback_count = 0;
    term->scrollback_offset = 0;

    // Line numbers (default disabled)
    term->line_numbers_enabled = false;
    term->line_number_start = 1;
    term->line_number_cols = 5;
    term->line_number_fg = 0xFFFF;  // White
    term->line_number_bg = 0x0000;  // Black

    // Scrollbar (default disabled)
    term->scrollbar_enabled = false;
    term->scrollbar_bg = 0x528A;     // Dark gray (RGB565: 0x52, 0x52, 0x52)
    term->scrollbar_thumb = 0xAD75;  // Light gray (RGB565: 0xAD, 0xAD, 0xAD)
    term->scrollbar_width = 4;
    term->total_lines = 0;
    term->scroll_position = 0;
    term->scrollbar_visible = false;
    term->scrollbar_last_scroll_time = 0;

    // Word wrap (default disabled, visual mode)
    term->word_wrap_enabled = false;
    term->word_wrap_column = 0;  // 0 = auto (use content cols)
    term->show_wrap_indicator = true;  // Show "…" at wrap points
    term->wrap_viewport_start = 0;
    term->wrap_viewport_scroll = 0;

    // Render bounds (default: below header, above footer)
    term->render_y_start = 28;
    term->render_y_end = 302;
    term->render_x_start = 4;
    term->last_render_fb = NULL;

    size_t cell_count = (size_t)cols * rows;
    size_t sb_count = (size_t)scrollback_lines * cols;
    term->cells = (uint16_t*)umm_malloc(cell_count * sizeof(uint16_t));
    term->prev_cells = (uint16_t*)umm_malloc(cell_count * sizeof(uint16_t));
    term->fg_colors = (uint16_t*)umm_malloc(cell_count * sizeof(uint16_t));
    term->bg_colors = (uint16_t*)umm_malloc(cell_count * sizeof(uint16_t));
    term->scrollback = (uint16_t*)umm_malloc(sb_count * sizeof(uint16_t));
    term->scrollback_fg = (uint16_t*)umm_malloc(sb_count * sizeof(uint16_t));
    term->scrollback_bg = (uint16_t*)umm_malloc(sb_count * sizeof(uint16_t));
    term->row_dirty = (uint8_t*)umm_malloc(rows * sizeof(uint8_t));
    term->row_continuation = (uint8_t*)umm_malloc(rows * sizeof(uint8_t));
    term->scrollback_continuation = (uint8_t*)umm_malloc(scrollback_lines * sizeof(uint8_t));

    if (!term->cells || !term->prev_cells || !term->fg_colors || !term->bg_colors ||
        !term->scrollback || !term->scrollback_fg || !term->scrollback_bg || !term->row_dirty ||
        !term->row_continuation || !term->scrollback_continuation) {
        terminal_free(term);
        return NULL;
    }

    memset(term->cells, 0, cell_count * sizeof(uint16_t));
    memset(term->prev_cells, 0xFF, cell_count * sizeof(uint16_t));
    for (size_t i = 0; i < cell_count; i++) {
        term->fg_colors[i] = 0xFFFF;
        term->bg_colors[i] = 0x0000;
    }
    memset(term->scrollback, 0, sb_count * sizeof(uint16_t));
    for (size_t i = 0; i < sb_count; i++) {
        term->scrollback_fg[i] = 0xFFFF;
        term->scrollback_bg[i] = 0x0000;
    }
    memset(term->row_dirty, 1, rows);
    memset(term->row_continuation, 0, rows);
    memset(term->scrollback_continuation, 0, scrollback_lines);

    return term;
}

void terminal_free(terminal_t* term) {
    if (!term) return;
    if (term->cells) umm_free(term->cells);
    if (term->prev_cells) umm_free(term->prev_cells);
    if (term->fg_colors) umm_free(term->fg_colors);
    if (term->bg_colors) umm_free(term->bg_colors);
    if (term->scrollback) umm_free(term->scrollback);
    if (term->scrollback_fg) umm_free(term->scrollback_fg);
    if (term->scrollback_bg) umm_free(term->scrollback_bg);
    if (term->row_dirty) umm_free(term->row_dirty);
    if (term->row_continuation) umm_free(term->row_continuation);
    if (term->scrollback_continuation) umm_free(term->scrollback_continuation);
    umm_free(term);
}

void terminal_clear(terminal_t* term) {
    if (!term) return;
    int cell_count = term->cols * term->rows;
    for (int i = 0; i < cell_count; i++) {
        term->cells[i] = TERM_BLANK_CELL;
        term->fg_colors[i] = term->fg_color;
        term->bg_colors[i] = term->bg_color;
    }
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->scroll_top = 0;
    term->scroll_bottom = term->rows - 1;
    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
    memset(term->row_continuation, 0, term->rows);
    term->scrollback_offset = 0;
}

void terminal_clearRow(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return;
    int base = row * term->cols;
    for (int x = 0; x < term->cols; x++) {
        term->cells[base + x] = TERM_BLANK_CELL;
        term->fg_colors[base + x] = term->fg_color;
        term->bg_colors[base + x] = term->bg_color;
    }
    term->row_dirty[row] = 1;
    term->row_continuation[row] = 0;
}

static void terminal_addScrollback(terminal_t* term) {
    if (!term || term->scrollback_lines == 0) return;
    int line_size = term->cols;
    int offset = term->scrollback_pos * line_size;
    memcpy(&term->scrollback[offset], &term->cells[0], line_size * sizeof(uint16_t));
    memcpy(&term->scrollback_fg[offset], &term->fg_colors[0], line_size * sizeof(uint16_t));
    memcpy(&term->scrollback_bg[offset], &term->bg_colors[0], line_size * sizeof(uint16_t));
    term->scrollback_continuation[term->scrollback_pos] = term->row_continuation[0];
    term->scrollback_pos = (term->scrollback_pos + 1) % term->scrollback_lines;
    if (term->scrollback_count < term->scrollback_lines) {
        term->scrollback_count++;
    }
}

void terminal_putChar(terminal_t* term, char c) {
    if (!term) return;

    if (c == '\n') {
        term->cursor_x = 0;
        if (term->cursor_y >= term->scroll_bottom) {
            term->cursor_y = term->scroll_bottom;
            terminal_scrollUp(term);
        } else {
            term->cursor_y++;
        }
        // Explicit newline: next row is NOT a continuation
        if (term->cursor_y < term->rows) {
            term->row_continuation[term->cursor_y] = 0;
        }
        return;
    }

    if (c == '\r') {
        term->cursor_x = 0;
        return;
    }

    if (c == '\t') {
        int tab_stop = 8;
        term->cursor_x = ((term->cursor_x / tab_stop) + 1) * tab_stop;
        if (term->cursor_x >= term->cols) {
            term->cursor_x = term->cols - 1;
        }
        return;
    }

    if (c == '\b') {
        if (term->cursor_x > 0) {
            term->cursor_x--;
        }
        return;
    }

    if (term->cursor_x >= term->cols) {
        term->cursor_x = 0;
        if (term->cursor_y >= term->scroll_bottom) {
            term->cursor_y = term->scroll_bottom;
            terminal_scrollUp(term);
        } else {
            term->cursor_y++;
        }
        // Auto-wrap: next row IS a continuation of the previous logical line
        if (term->cursor_y < term->rows) {
            term->row_continuation[term->cursor_y] = 1;
        }
    }

    int idx = term->cursor_y * term->cols + term->cursor_x;
    uint16_t cell = (uint8_t)c | ((uint16_t)term->current_attr << 8);
    term->cells[idx] = cell;
    term->fg_colors[idx] = term->fg_color;
    term->bg_colors[idx] = term->bg_color;
    term->row_dirty[term->cursor_y] = 1;
    term->cursor_x++;
}

void terminal_putString(terminal_t* term, const char* s) {
    if (!term || !s) return;
    while (*s) {
        terminal_putChar(term, *s++);
    }
}

void terminal_setCursor(terminal_t* term, int x, int y) {
    if (!term) return;
    if (x < 0) x = 0;
    if (x >= term->cols) x = term->cols - 1;
    if (y < 0) y = 0;
    if (y >= term->rows) y = term->rows - 1;
    term->cursor_x = x;
    term->cursor_y = y;
}

void terminal_getCursor(terminal_t* term, int* out_x, int* out_y) {
    if (!term) return;
    if (out_x) *out_x = term->cursor_x;
    if (out_y) *out_y = term->cursor_y;
}

void terminal_setColors(terminal_t* term, uint16_t fg, uint16_t bg) {
    if (!term) return;
    term->fg_color = fg;
    term->bg_color = bg;
}

void terminal_getColors(terminal_t* term, uint16_t* out_fg, uint16_t* out_bg) {
    if (!term) return;
    if (out_fg) *out_fg = term->fg_color;
    if (out_bg) *out_bg = term->bg_color;
}

void terminal_setAttribute(terminal_t* term, uint8_t attr) {
    if (!term) return;
    term->current_attr |= attr;
}

void terminal_clearAttribute(terminal_t* term, uint8_t attr) {
    if (!term) return;
    term->current_attr &= ~attr;
}

void terminal_scroll(terminal_t* term, int lines) {
    if (!term || lines == 0) return;
    if (lines > 0) {
        for (int i = 0; i < lines; i++) {
            terminal_scrollUp(term);
        }
    } else {
        for (int i = 0; i < -lines; i++) {
            terminal_scrollDown(term);
        }
    }
}

void terminal_scrollUp(terminal_t* term) {
    if (!term) return;

    int top = term->scroll_top;
    int bot = term->scroll_bottom;
    bool full_screen = (top == 0 && bot == term->rows - 1);

    if (full_screen) {
        terminal_addScrollback(term);
    }

    int line_size = term->cols * sizeof(uint16_t);
    int top_offset = top * term->cols;
    int region_lines = bot - top; // lines to move (bot - top)
    memmove(&term->cells[top_offset], &term->cells[top_offset + term->cols], region_lines * line_size);
    memmove(&term->fg_colors[top_offset], &term->fg_colors[top_offset + term->cols], region_lines * line_size);
    memmove(&term->bg_colors[top_offset], &term->bg_colors[top_offset + term->cols], region_lines * line_size);
    memmove(&term->row_continuation[top], &term->row_continuation[top + 1], region_lines * sizeof(uint8_t));

    int last_row = bot * term->cols;
    for (int x = 0; x < term->cols; x++) {
        term->cells[last_row + x] = TERM_BLANK_CELL;
        term->fg_colors[last_row + x] = term->fg_color;
        term->bg_colors[last_row + x] = term->bg_color;
    }
    term->row_continuation[bot] = 0;

    for (int r = top; r <= bot; r++) {
        term->row_dirty[r] = 1;
    }
    if (full_screen) {
        term->full_dirty = true;
    }
}

void terminal_scrollDown(terminal_t* term) {
    if (!term) return;

    int top = term->scroll_top;
    int bot = term->scroll_bottom;
    int line_size = term->cols * sizeof(uint16_t);
    int top_offset = top * term->cols;
    int region_lines = bot - top; // lines to move
    memmove(&term->cells[top_offset + term->cols], &term->cells[top_offset], region_lines * line_size);
    memmove(&term->fg_colors[top_offset + term->cols], &term->fg_colors[top_offset], region_lines * line_size);
    memmove(&term->bg_colors[top_offset + term->cols], &term->bg_colors[top_offset], region_lines * line_size);
    memmove(&term->row_continuation[top + 1], &term->row_continuation[top], region_lines * sizeof(uint8_t));

    int top_row = top * term->cols;
    for (int x = 0; x < term->cols; x++) {
        term->cells[top_row + x] = TERM_BLANK_CELL;
        term->fg_colors[top_row + x] = term->fg_color;
        term->bg_colors[top_row + x] = term->bg_color;
    }
    term->row_continuation[top] = 0;

    for (int r = top; r <= bot; r++) {
        term->row_dirty[r] = 1;
    }
    if (top == 0 && bot == term->rows - 1) {
        term->full_dirty = true;
    }
}

static void terminal_erase_cell(terminal_t* term, int idx) {
    term->cells[idx] = TERM_BLANK_CELL;
    term->fg_colors[idx] = term->fg_color;
    term->bg_colors[idx] = term->bg_color;
}

void terminal_eraseDisplay(terminal_t* term, int mode) {
    if (!term) return;

    if (mode == 0) {
        for (int y = term->cursor_y; y < term->rows; y++) {
            int start_x = (y == term->cursor_y) ? term->cursor_x : 0;
            for (int x = start_x; x < term->cols; x++) {
                terminal_erase_cell(term, y * term->cols + x);
            }
            // Clear continuation for fully-erased rows below cursor
            if (y > term->cursor_y) {
                term->row_continuation[y] = 0;
            }
        }
    } else if (mode == 1) {
        for (int y = 0; y <= term->cursor_y; y++) {
            int end_x = (y == term->cursor_y) ? term->cursor_x + 1 : term->cols;
            for (int x = 0; x < end_x; x++) {
                terminal_erase_cell(term, y * term->cols + x);
            }
            // Clear continuation for fully-erased rows above cursor
            if (y < term->cursor_y) {
                term->row_continuation[y] = 0;
            }
        }
    } else if (mode == 2) {
        int cell_count = term->cols * term->rows;
        for (int i = 0; i < cell_count; i++) {
            terminal_erase_cell(term, i);
        }
        memset(term->row_continuation, 0, term->rows);
    }

    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
}

void terminal_eraseLine(terminal_t* term, int mode) {
    if (!term || term->cursor_y < 0 || term->cursor_y >= term->rows) return;

    int row = term->cursor_y;
    int base = row * term->cols;

    if (mode == 0) {
        for (int x = term->cursor_x; x < term->cols; x++) {
            terminal_erase_cell(term, base + x);
        }
    } else if (mode == 1) {
        for (int x = 0; x <= term->cursor_x; x++) {
            terminal_erase_cell(term, base + x);
        }
    } else if (mode == 2) {
        for (int x = 0; x < term->cols; x++) {
            terminal_erase_cell(term, base + x);
        }
    }

    term->row_dirty[row] = 1;
}

int terminal_getCols(terminal_t* term) {
    return term ? term->cols : 0;
}

int terminal_getRows(terminal_t* term) {
    return term ? term->rows : 0;
}

uint16_t terminal_getCell(terminal_t* term, int x, int y) {
    if (!term || x < 0 || x >= term->cols || y < 0 || y >= term->rows) return 0;
    return term->cells[y * term->cols + x];
}

void terminal_setCell(terminal_t* term, int x, int y, uint16_t cell) {
    if (!term || x < 0 || x >= term->cols || y < 0 || y >= term->rows) return;
    term->cells[y * term->cols + x] = cell;
    term->row_dirty[y] = 1;
}

uint16_t terminal_getScrollback(terminal_t* term, int line) {
    if (!term || line < 0 || line >= term->scrollback_count) return 0;
    int idx = (term->scrollback_pos - term->scrollback_count + line + term->scrollback_lines) % term->scrollback_lines;
    return term->scrollback[idx * term->cols];
}

int terminal_getScrollbackCount(terminal_t* term) {
    if (!term) return 0;
    return term->scrollback_count;
}

void terminal_getScrollbackLine(terminal_t* term, int line, uint16_t* out_cells) {
    if (!term || line < 0 || line >= term->scrollback_count || !out_cells) return;
    int idx = (term->scrollback_pos - term->scrollback_count + line + term->scrollback_lines) % term->scrollback_lines;
    memcpy(out_cells, &term->scrollback[idx * term->cols], term->cols * sizeof(uint16_t));
}

void terminal_getScrollbackLineColors(terminal_t* term, int line, uint16_t* out_fg, uint16_t* out_bg) {
    if (!term || line < 0 || line >= term->scrollback_count) return;
    int idx = (term->scrollback_pos - term->scrollback_count + line + term->scrollback_lines) % term->scrollback_lines;
    int offset = idx * term->cols;
    if (out_fg) memcpy(out_fg, &term->scrollback_fg[offset], term->cols * sizeof(uint16_t));
    if (out_bg) memcpy(out_bg, &term->scrollback_bg[offset], term->cols * sizeof(uint16_t));
}

void terminal_setScrollbackOffset(terminal_t* term, int offset) {
    if (!term) return;
    term->scrollback_offset = offset;
    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
}

int terminal_getScrollbackOffset(terminal_t* term) {
    return term ? term->scrollback_offset : -1;
}

void terminal_markAllDirty(terminal_t* term) {
    if (!term) return;
    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
}

void terminal_markRowDirty(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return;
    term->row_dirty[row] = 1;
}

bool terminal_isRowDirty(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return false;
    return term->row_dirty[row] != 0;
}

void terminal_clearRowDirty(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return;
    term->row_dirty[row] = 0;
}

void terminal_getDirtyRange(terminal_t* term, int* out_first, int* out_last) {
    if (!term) {
        if (out_first) *out_first = -1;
        if (out_last) *out_last = -1;
        return;
    }

    int first = -1;
    int last = -1;

    for (int i = 0; i < term->rows; i++) {
        if (term->row_dirty[i]) {
            if (first < 0) first = i;
            last = i;
        }
    }

    if (out_first) *out_first = first;
    if (out_last) *out_last = last;
}

bool terminal_isFullDirty(terminal_t* term) {
    return term ? term->full_dirty : false;
}

void terminal_clearFullDirty(terminal_t* term) {
    if (!term) return;
    term->full_dirty = false;
}

void terminal_setFont(terminal_t* term, enum terminal_font font) {
    if (!term) return;
    if (term->font != font) {
        term->font = font;
        term->full_dirty = true;
    }
}

enum terminal_font terminal_getFont(terminal_t* term) {
    return term ? term->font : TERM_FONT_SCIENTIFICA;
}

void terminal_setScrollRegion(terminal_t* term, int top, int bottom) {
    if (!term) return;
    if (top < 0) top = 0;
    if (bottom >= term->rows) bottom = term->rows - 1;
    if (top >= bottom) return;  // invalid range
    term->scroll_top = top;
    term->scroll_bottom = bottom;
}

void terminal_insertChars(terminal_t* term, int count) {
    if (!term || count <= 0) return;
    int y = term->cursor_y;
    int x = term->cursor_x;
    int base = y * term->cols;
    int remaining = term->cols - x;
    if (count > remaining) count = remaining;
    // Shift cells right from cursor position
    memmove(&term->cells[base + x + count], &term->cells[base + x], (remaining - count) * sizeof(uint16_t));
    memmove(&term->fg_colors[base + x + count], &term->fg_colors[base + x], (remaining - count) * sizeof(uint16_t));
    memmove(&term->bg_colors[base + x + count], &term->bg_colors[base + x], (remaining - count) * sizeof(uint16_t));
    // Fill inserted positions with blanks
    for (int i = 0; i < count; i++) {
        term->cells[base + x + i] = TERM_BLANK_CELL;
        term->fg_colors[base + x + i] = term->fg_color;
        term->bg_colors[base + x + i] = term->bg_color;
    }
    term->row_dirty[y] = 1;
}

void terminal_deleteChars(terminal_t* term, int count) {
    if (!term || count <= 0) return;
    int y = term->cursor_y;
    int x = term->cursor_x;
    int base = y * term->cols;
    int remaining = term->cols - x;
    if (count > remaining) count = remaining;
    // Shift cells left
    memmove(&term->cells[base + x], &term->cells[base + x + count], (remaining - count) * sizeof(uint16_t));
    memmove(&term->fg_colors[base + x], &term->fg_colors[base + x + count], (remaining - count) * sizeof(uint16_t));
    memmove(&term->bg_colors[base + x], &term->bg_colors[base + x + count], (remaining - count) * sizeof(uint16_t));
    // Fill end with blanks
    for (int i = remaining - count; i < remaining; i++) {
        term->cells[base + x + i] = TERM_BLANK_CELL;
        term->fg_colors[base + x + i] = term->fg_color;
        term->bg_colors[base + x + i] = term->bg_color;
    }
    term->row_dirty[y] = 1;
}

// Line numbers implementation
void terminal_setLineNumbers(terminal_t* term, bool enabled) {
    if (!term) return;
    if (term->line_numbers_enabled != enabled) {
        term->line_numbers_enabled = enabled;
        term->full_dirty = true;
    }
}

void terminal_setLineNumberStart(terminal_t* term, int start) {
    if (!term) return;
    if (start < 1) start = 1;
    if (term->line_number_start != start) {
        term->line_number_start = start;
        if (term->line_numbers_enabled) {
            term->full_dirty = true;
        }
    }
}

void terminal_setLineNumberCols(terminal_t* term, int cols) {
    if (!term) return;
    if (cols < 2) cols = 2;
    if (cols > 10) cols = 10;
    if (term->line_number_cols != cols) {
        term->line_number_cols = cols;
        if (term->line_numbers_enabled) {
            term->full_dirty = true;
        }
    }
}

void terminal_setLineNumberColors(terminal_t* term, uint16_t fg, uint16_t bg) {
    if (!term) return;
    if (term->line_number_fg != fg || term->line_number_bg != bg) {
        term->line_number_fg = fg;
        term->line_number_bg = bg;
        if (term->line_numbers_enabled) {
            term->full_dirty = true;
        }
    }
}

int terminal_getContentCols(terminal_t* term) {
    if (!term) return 0;
    int content_cols = term->cols;
    if (term->line_numbers_enabled) {
        content_cols -= term->line_number_cols;
    }
    if (term->scrollbar_enabled) {
        content_cols -= 1; // Scrollbar uses 1 column
    }
    return content_cols > 0 ? content_cols : 1;
}

// Scrollbar implementation
void terminal_setScrollbar(terminal_t* term, bool enabled) {
    if (!term) return;
    if (term->scrollbar_enabled != enabled) {
        term->scrollbar_enabled = enabled;
        term->full_dirty = true;
    }
}

void terminal_setScrollbarColors(terminal_t* term, uint16_t bg, uint16_t thumb) {
    if (!term) return;
    term->scrollbar_bg = bg;
    term->scrollbar_thumb = thumb;
}

void terminal_setScrollbarWidth(terminal_t* term, int width) {
    if (!term) return;
    if (width < 2) width = 2;
    if (width > 8) width = 8;
    term->scrollbar_width = width;
}

void terminal_setScrollInfo(terminal_t* term, int total_lines, int scroll_position) {
    if (!term) return;
    term->total_lines = total_lines > 0 ? total_lines : 0;
    term->scroll_position = scroll_position > 0 ? scroll_position : 0;
    if (term->scrollbar_enabled) {
        term->scrollbar_visible = true;
        term->scrollbar_last_scroll_time = 0; // Will be set in render
    }
}

// Render bounds
void terminal_setRenderBounds(terminal_t* term, int y_start, int y_end) {
    if (!term) return;
    if (y_start < 0) y_start = 0;
    if (y_end > 320) y_end = 320;
    if (y_end <= y_start) return;
    term->render_y_start = y_start;
    term->render_y_end = y_end;
    term->full_dirty = true;
}

// Word wrap implementation (visual - content not modified)
void terminal_setWordWrap(terminal_t* term, bool enabled) {
    if (!term) return;
    if (term->word_wrap_enabled != enabled) {
        term->word_wrap_enabled = enabled;
        term->full_dirty = true;
    }
}

void terminal_setWordWrapColumn(terminal_t* term, int column) {
    if (!term) return;
    if (column < 0) column = 0;
    if (column > term->cols) column = term->cols;
    if (term->word_wrap_column != column) {
        term->word_wrap_column = column;
        if (term->word_wrap_enabled) {
            term->full_dirty = true;
        }
    }
}

void terminal_setWrapIndicator(terminal_t* term, bool enabled) {
    if (!term) return;
    term->show_wrap_indicator = enabled;
}

bool terminal_getWordWrap(terminal_t* term) {
    return term ? term->word_wrap_enabled : false;
}

// Get content column count (accounting for line numbers and scrollbar)
static int get_content_cols(terminal_t* term) {
    if (!term) return 0;
    int cols = term->cols;
    if (term->line_numbers_enabled) cols -= term->line_number_cols;
    if (term->scrollbar_enabled) cols -= 1;
    return cols > 0 ? cols : 1;
}

// Find best wrap position looking backwards for space/punctuation
// Returns: position to wrap after (1-based), 0 if no good break found
static int find_wrap_position(const char* text, int text_len, int max_cols) {
    if (text_len <= max_cols) return 0;

    int search_start = max_cols;
    int search_end = max_cols - 20;
    if (search_end < 1) search_end = 1;

    for (int i = search_start; i >= search_end; i--) {
        char c = text[i - 1];
        if (c == ' ' || c == '\t' || c == '-' || c == ',' || c == '.' ||
            c == ';' || c == ':' || c == '!' || c == '?' || c == ')' ||
            c == ']' || c == '}') {
            return i;
        }
    }

    return max_cols;
}

// Find first buffer row of the logical line containing buffer_row
static int find_logical_line_start(terminal_t* term, int buffer_row) {
    while (buffer_row > 0 && term->row_continuation[buffer_row]) {
        buffer_row--;
    }
    return buffer_row;
}

// Count buffer rows in a logical line starting at start_row
static int count_logical_line_rows(terminal_t* term, int start_row) {
    int count = 1;
    while (start_row + count < term->rows && term->row_continuation[start_row + count]) {
        count++;
    }
    return count;
}

// Get full text of a logical line spanning multiple buffer rows
static int get_logical_line_text(terminal_t* term, int start_row, int num_rows, char* line, int max_len) {
    int total = 0;
    for (int r = 0; r < num_rows && total < max_len; r++) {
        int base = (start_row + r) * term->cols;
        int row_len = term->cols;
        if (r == num_rows - 1) {
            while (row_len > 0 && term->cells[base + row_len - 1] == TERM_BLANK_CELL) {
                row_len--;
            }
        }
        for (int i = 0; i < row_len && total < max_len; i++) {
            line[total++] = (char)(term->cells[base + i] & 0xFF);
        }
    }
    return total;
}

// Build list of logical line start rows
static int build_logical_lines(terminal_t* term, int* logical_starts, int max_lines) {
    int count = 0;
    for (int r = 0; r < term->rows && count < max_lines; r++) {
        if (!term->row_continuation[r]) {
            logical_starts[count++] = r;
        }
    }
    return count;
}

// Calculate wrap segments for a logical line (continuation-aware)
static int calc_wraps_for_logical_line(terminal_t* term, int start_row, int num_rows,
                                       int* segments, int max_segments,
                                       int content_cols, int wrap_col) {
    ensure_wrap_buf();
    if (!g_terminal_wrap_buf) return 1;
    int line_len = get_logical_line_text(term, start_row, num_rows, g_terminal_wrap_buf, WRAP_BUF_SIZE - 1);

    if (line_len == 0) {
        if (segments && max_segments > 0) segments[0] = 0;
        return 1;
    }

    int visual_rows = 0;
    int pos = 0;

    while (pos < line_len && visual_rows < max_segments) {
        if (segments) segments[visual_rows] = pos;
        visual_rows++;

        int remaining = line_len - pos;
        if (remaining <= wrap_col) break;

        int wp = find_wrap_position(g_terminal_wrap_buf + pos, remaining, wrap_col);
        if (wp == 0) wp = wrap_col;
        pos += wp;
    }

    return visual_rows > 0 ? visual_rows : 1;
}

// Public API: Calculate wraps for a buffer row (finds its logical line first)
int terminal_calculateLineWraps(terminal_t* term, int logical_line, int* segments, int max_segments) {
    if (!term) return 1;
    if (!term->word_wrap_enabled) return 1;
    if (logical_line < 0 || logical_line >= term->rows) return 1;

    // If this buffer row is a continuation, it's part of a previous logical line
    if (term->row_continuation[logical_line]) return 0;

    int content_cols = get_content_cols(term);
    int wrap_col = term->word_wrap_column > 0 ? term->word_wrap_column : content_cols;
    if (wrap_col > content_cols) wrap_col = content_cols;

    int num_rows = count_logical_line_rows(term, logical_line);
    return calc_wraps_for_logical_line(term, logical_line, num_rows,
                                       segments, max_segments, content_cols, wrap_col);
}

// Get total visual rows for entire content (for scrollbar)
int terminal_getVisualRowCount(terminal_t* term) {
    if (!term) return 0;
    if (!term->word_wrap_enabled) return term->rows;

    int logical_starts[27];
    int num_logical = build_logical_lines(term, logical_starts, 26);
    int content_cols = get_content_cols(term);
    int wrap_col = term->word_wrap_column > 0 ? term->word_wrap_column : content_cols;
    if (wrap_col > content_cols) wrap_col = content_cols;

    int total = 0;
    for (int li = 0; li < num_logical; li++) {
        int sr = logical_starts[li];
        int nr = count_logical_line_rows(term, sr);
        total += calc_wraps_for_logical_line(term, sr, nr, NULL, 64, content_cols, wrap_col);
    }
    return total;
}

// Convert logical (buffer) coordinates to visual coordinates
void terminal_logicalToVisual(terminal_t* term, int log_x, int log_y, int* vis_x, int* vis_y) {
    if (!term) {
        if (vis_x) *vis_x = log_x;
        if (vis_y) *vis_y = log_y;
        return;
    }

    if (!term->word_wrap_enabled) {
        if (vis_x) *vis_x = log_x;
        if (vis_y) *vis_y = log_y;
        return;
    }

    int logical_starts[27];
    int num_logical = build_logical_lines(term, logical_starts, 26);
    int content_cols = get_content_cols(term);
    int wrap_col = term->word_wrap_column > 0 ? term->word_wrap_column : content_cols;
    if (wrap_col > content_cols) wrap_col = content_cols;

    // Find which logical line contains log_y
    int ll_start = find_logical_line_start(term, log_y);
    int char_offset = (log_y - ll_start) * term->cols + log_x;

    int visual_y = 0;
    for (int li = 0; li < num_logical; li++) {
        int sr = logical_starts[li];
        int nr = count_logical_line_rows(term, sr);
        int segments[64];
        int ns = calc_wraps_for_logical_line(term, sr, nr, segments, 64, content_cols, wrap_col);

        if (sr == ll_start) {
            // Find which segment contains char_offset
            for (int s = 0; s < ns; s++) {
                int seg_end = (s + 1 < ns) ? segments[s + 1] : 9999;
                if (char_offset >= segments[s] && char_offset < seg_end) {
                    if (vis_x) *vis_x = char_offset - segments[s];
                    if (vis_y) *vis_y = visual_y + s;
                    return;
                }
            }
            // Fallback: past end
            if (vis_x) *vis_x = 0;
            if (vis_y) *vis_y = visual_y + ns - 1;
            return;
        }
        visual_y += ns;
    }

    if (vis_x) *vis_x = log_x;
    if (vis_y) *vis_y = log_y;
}

// Convert visual coordinates to logical (buffer) coordinates
void terminal_visualToLogical(terminal_t* term, int vis_x, int vis_y, int* log_x, int* log_y) {
    if (!term) {
        if (log_x) *log_x = vis_x;
        if (log_y) *log_y = vis_y;
        return;
    }

    if (!term->word_wrap_enabled) {
        if (log_x) *log_x = vis_x;
        if (log_y) *log_y = vis_y;
        return;
    }

    int logical_starts[27];
    int num_logical = build_logical_lines(term, logical_starts, 26);
    int content_cols = get_content_cols(term);
    int wrap_col = term->word_wrap_column > 0 ? term->word_wrap_column : content_cols;
    if (wrap_col > content_cols) wrap_col = content_cols;

    int current_visual = 0;
    for (int li = 0; li < num_logical; li++) {
        int sr = logical_starts[li];
        int nr = count_logical_line_rows(term, sr);
        int segments[64];
        int ns = calc_wraps_for_logical_line(term, sr, nr, segments, 64, content_cols, wrap_col);

        if (current_visual + ns > vis_y) {
            int seg_idx = vis_y - current_visual;
            int char_offset = segments[seg_idx] + vis_x;
            // Convert char_offset back to buffer row/col
            int buf_row = sr + char_offset / term->cols;
            int buf_col = char_offset % term->cols;
            if (buf_row >= term->rows) buf_row = term->rows - 1;
            if (log_x) *log_x = buf_col;
            if (log_y) *log_y = buf_row;
            return;
        }
        current_visual += ns;
    }

    if (log_x) *log_x = vis_x;
    if (log_y) *log_y = vis_y;
}
