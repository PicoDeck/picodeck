#include "terminal_render.h"
#include "terminal.h"
#include "text_wrap.h"
#include "../drivers/display.h"
#include "../fonts/font_scientifica.h"
#include "umm_malloc.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>

static bool s_cursor_visible = true;
static bool s_cursor_blink = true;
static uint32_t s_last_blink_time = 0;
static bool s_blink_state = false;

static const uint8_t* get_glyph(terminal_t* term, char c) {
    if (term->font == TERM_FONT_SCIENTIFICA_BOLD) {
        return font_scientifica_bold_glyph(c);
    }
    return font_scientifica_glyph(c);
}

static inline int get_font_width(terminal_t* term) {
    (void)term;
    return FONT_SCI_WIDTH;
}

static inline int get_font_height(terminal_t* term) {
    (void)term;
    return FONT_SCI_HEIGHT;
}

#define FONT_W get_font_width(term)
#define FONT_H get_font_height(term)

// Shared scratch buffer defined in terminal.c — allocated in PSRAM, single-threaded on Core 0.
extern char* g_terminal_wrap_buf;

static inline uint16_t byte_swap(uint16_t val) {
    return (val >> 8) | ((val & 0xFF) << 8);
}

// Whether a text row starting at pixel y lies inside the render bounds: every
// draw path drops a row that does not fully fit.
static inline bool row_fits(const terminal_t* term, int y) {
    return y + FONT_SCI_HEIGHT <= term->render_y_end;
}

void terminal_render_init(void) {
    s_last_blink_time = 0;
    s_blink_state = false;
}

// Helper: Get content column count (accounting for line numbers and scrollbar)
static int get_content_cols(terminal_t* term) {
    if (!term) return 0;
    int cols = term->cols;
    if (term->line_numbers_enabled) cols -= term->line_number_cols;
    if (term->scrollbar_enabled) cols -= 1;
    return cols > 0 ? cols : 1;
}

// Find the first buffer row of the logical line containing the given buffer row.
// A logical line is a group of consecutive buffer rows where all but the first
// have row_continuation[row] == 1.
static int find_logical_line_start(terminal_t* term, int buffer_row) {
    while (buffer_row > 0 && term->row_continuation[buffer_row]) {
        buffer_row--;
    }
    return buffer_row;
}

// Count the number of buffer rows in a logical line starting at start_row.
static int count_logical_line_rows(terminal_t* term, int start_row) {
    int count = 1;
    while (start_row + count < term->rows && term->row_continuation[start_row + count]) {
        count++;
    }
    return count;
}

// Get the full text content of a logical line spanning multiple buffer rows.
// Returns the total character count. Writes into line[] (must be large enough).
// Strips trailing blanks from the LAST buffer row only (intermediate rows are full).
static int get_logical_line_text(terminal_t* term, int start_row, int num_rows, char* line, int max_len) {
    int total = 0;
    for (int r = 0; r < num_rows && total < max_len; r++) {
        int base = (start_row + r) * term->cols;
        int row_len = term->cols;
        // Only strip trailing blanks from the last row
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

// Calculate wrap segments for a logical line (may span multiple buffer rows).
// Assembles text from terminal cells, then delegates to shared text_wrap utility.
static int calculate_wrap_segments(terminal_t* term, int start_row, int num_rows,
                                   int* segments, int max_segments,
                                   int content_cols, int wrap_col,
                                   int* out_line_len) {
    (void)content_cols;
    if (start_row < 0 || start_row >= term->rows) {
        if (out_line_len) *out_line_len = 0;
        return 1;
    }

    // Get full logical line content into shared static buffer
    if (!g_terminal_wrap_buf) {
        if (out_line_len) *out_line_len = 0;
        return 1;
    }
    int line_len = get_logical_line_text(term, start_row, num_rows, g_terminal_wrap_buf, 1399);
    if (out_line_len) *out_line_len = line_len;

    // Delegate to shared word wrap utility
    return text_wrap_segments(g_terminal_wrap_buf, line_len, wrap_col,
                              segments, max_segments);
}

// Build a list of logical line start rows by scanning continuation flags.
// Returns the number of logical lines found.
// logical_starts[] receives the first buffer row of each logical line.
static int build_logical_lines(terminal_t* term, int* logical_starts, int max_lines) {
    int count = 0;
    for (int r = 0; r < term->rows && count < max_lines; r++) {
        if (!term->row_continuation[r]) {
            logical_starts[count++] = r;
        }
    }
    return count;
}

static void terminal_render_glyph(int x, int y, uint16_t fg, uint16_t bg,
                                   const uint8_t *glyph, int font_w, int font_h) {
    uint16_t* fb = display_get_back_buffer();
    uint16_t fg_be = byte_swap(fg);
    uint16_t bg_be = byte_swap(bg);

    for (int row = 0; row < font_h; row++) {
        uint8_t rowdata = glyph[row];
        int py = y + row;
        if (py < 0 || py >= 320) continue;
        for (int col = 0; col < font_w; col++) {
            int px = x + col;
            if (px >= 0 && px < 320) {
                fb[py * 320 + px] = (rowdata & (0x80 >> col)) ? fg_be : bg_be;
            }
        }
    }
}

// Helper function to render line numbers
static void terminal_renderLineNumbers(terminal_t* term) {
    if (!term || !term->line_numbers_enabled) return;

    int start_line = term->line_number_start;
    int content_cols = get_content_cols(term);
    int wrap_col = term->word_wrap_column > 0 ? term->word_wrap_column : content_cols;
    if (wrap_col > content_cols) wrap_col = content_cols;

    // Build logical line map
    int logical_starts[27]; // max 26 rows + sentinel
    int num_logical = build_logical_lines(term, logical_starts, 26);

    int next_line_num = start_line;  // no-wrap mode: next number to draw

    for (int row = 0; row < term->rows; row++) {
        int y = term->render_y_start + row * FONT_H;
        if (!row_fits(term, y)) break;
        int x = term->render_x_start;
        bool is_continuation = false;
        int logical_line_idx = 0;
        int line_num = 0;

        if (term->word_wrap_enabled) {
            // Find which logical line and segment this visual row corresponds to
            int target_visual = row;
            int current_visual = 0;

            for (int li = 0; li < num_logical; li++) {
                int lr = logical_starts[li];
                int lr_count = count_logical_line_rows(term, lr);
                int segments[64];
                int ns = calculate_wrap_segments(term, lr, lr_count, segments, 64, content_cols, wrap_col, NULL);
                if (current_visual + ns > target_visual) {
                    int seg_idx = target_visual - current_visual;
                    is_continuation = (seg_idx > 0);
                    logical_line_idx = li;
                    break;
                }
                current_visual += ns;
                logical_line_idx = li + 1;
            }
            line_num = start_line + logical_line_idx;
        } else {
            // Continuation rows (auto-wrapped long lines) belong to the previous
            // logical line: blank gutter, and the line number is not advanced.
            is_continuation = term->row_continuation[row] != 0;
            if (!is_continuation) line_num = next_line_num++;
        }

        if (!is_continuation) {
            char num_str[16];
            snprintf(num_str, sizeof(num_str), "%*d", term->line_number_cols - 1, line_num);
            num_str[term->line_number_cols - 1] = ' ';
            num_str[term->line_number_cols] = '\0';

            for (int i = 0; i < term->line_number_cols && num_str[i]; i++) {
                const uint8_t* glyph = get_glyph(term, num_str[i]);
                terminal_render_glyph(x, y, term->line_number_fg, term->line_number_bg,
                                      glyph, FONT_W, FONT_H);
                x += FONT_W;
            }
        } else {
            // Draw empty gutter for continuation rows
            for (int i = 0; i < term->line_number_cols; i++) {
                const uint8_t* space = get_glyph(term, ' ');
                terminal_render_glyph(x, y, term->line_number_fg, term->line_number_bg,
                                      space, FONT_W, FONT_H);
                x += FONT_W;
            }
        }
    }
}

// Helper function to render scrollbar
static void terminal_renderScrollbar(terminal_t* term) {
    if (!term || !term->scrollbar_enabled || term->total_lines <= term->rows) {
        return;
    }

    // Check if scrollbar should be visible (always show for now, or based on recent scroll activity)
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool should_show = term->scrollbar_visible;

    // Auto-hide after 2 seconds of no scrolling
    if (term->scrollbar_last_scroll_time > 0 &&
        (now - term->scrollbar_last_scroll_time) > 2000) {
        should_show = false;
    }

    if (!should_show) return;

    // Scrollbar is drawn in the last column (rightmost)
    int scrollbar_col = term->cols - 1;
    int x = term->render_x_start + scrollbar_col * FONT_W + (FONT_W - term->scrollbar_width) / 2;
    if (x < 0) x = 0;
    int y = term->render_y_start;
    int height = term->rows * FONT_H;

    // Draw track (background)
    for (int row = 0; row < term->rows; row++) {
        int row_y = term->render_y_start + row * FONT_H;
        // Fill the cell background with scrollbar_bg color
        for (int py = row_y; py < row_y + FONT_H && py < term->render_y_end; py++) {
            for (int px = x; px < x + term->scrollbar_width && px < 320; px++) {
                if (px >= 0 && px < 320 && py >= 0 && py < term->render_y_end) {
                    uint16_t* fb = display_get_back_buffer();
                    fb[py * 320 + px] = byte_swap(term->scrollbar_bg);
                }
            }
        }
    }

    // Calculate thumb position and size
    float visible_ratio = (float)term->rows / term->total_lines;
    int thumb_height_px = (int)(height * visible_ratio);
    if (thumb_height_px < FONT_H) thumb_height_px = FONT_H;  // Minimum 1 row

    float scroll_ratio = (term->total_lines > term->rows) ?
        (float)term->scroll_position / (term->total_lines - term->rows) : 0;
    int thumb_y = y + (int)((height - thumb_height_px) * scroll_ratio);

    // Draw thumb
    for (int py = thumb_y; py < thumb_y + thumb_height_px && py < y + height && py < term->render_y_end; py++) {
        for (int px = x; px < x + term->scrollbar_width && px < 320; px++) {
            if (px >= 0 && px < 320 && py >= 0 && py < term->render_y_end) {
                uint16_t* fb = display_get_back_buffer();
                fb[py * 320 + px] = byte_swap(term->scrollbar_thumb);
            }
        }
    }
}

void terminal_renderScrollback(terminal_t* term) {
    if (!term || term->scrollback_offset <= 0) return;

    int offset = term->scrollback_offset;
    int visible_rows = term->rows;
    int scrollback_count = term->scrollback_count;
    // Combined virtual document: scrollback lines then current screen lines
    // offset=0 is live (handled by caller), offset=N scrolls up N lines
    // Top visible virtual line:
    int top_virtual = scrollback_count + visible_rows - offset - visible_rows;
    // = scrollback_count - offset
    if (top_virtual < 0) top_virtual = 0;

    int cols = term->cols;
    uint16_t* line_cells = (uint16_t*)umm_malloc(cols * sizeof(uint16_t));
    uint16_t* line_fg = (uint16_t*)umm_malloc(cols * sizeof(uint16_t));
    uint16_t* line_bg = (uint16_t*)umm_malloc(cols * sizeof(uint16_t));
    if (!line_cells || !line_fg || !line_bg) {
        if (line_cells) umm_free(line_cells);
        if (line_fg) umm_free(line_fg);
        if (line_bg) umm_free(line_bg);
        return;
    }

    for (int row = 0; row < visible_rows; row++) {
        if (!row_fits(term, term->render_y_start + row * FONT_H)) break;
        int vline = top_virtual + row;

        if (vline < 0) {
            // Before scrollback — blank
            const uint8_t *space = get_glyph(term, ' ');
            int fw = get_font_width(term);
            int fh = get_font_height(term);
            for (int col = 0; col < cols; col++) {
                terminal_render_glyph(term->render_x_start + col * fw, term->render_y_start + row * fh, 0xFFFF, 0x0000, space, fw, fh);
            }
        } else if (vline < scrollback_count) {
            // Scrollback line — use stored colors
            terminal_getScrollbackLine(term, vline, line_cells);
            terminal_getScrollbackLineColors(term, vline, line_fg, line_bg);

            for (int col = 0; col < cols; col++) {
                uint16_t cell = line_cells[col];
                uint8_t ch = cell & 0xFF;
                uint8_t attr = (cell >> 8) & 0xFF;
                if (ch == 0) ch = ' ';

                uint16_t fg = line_fg[col];
                uint16_t bg = line_bg[col];
                if (attr & TERM_ATTR_INVERSE) {
                    uint16_t tmp = fg; fg = bg; bg = tmp;
                }

                const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
                terminal_render_glyph(term->render_x_start + col * FONT_W, term->render_y_start + row * FONT_H, fg, bg, glyph, FONT_W, FONT_H);
            }
        } else {
            // Current screen line
            int screen_row = vline - scrollback_count;
            if (screen_row < visible_rows) {
                int base = screen_row * cols;
                for (int col = 0; col < cols; col++) {
                    int idx = base + col;
                    uint16_t cell = term->cells[idx];
                    uint8_t ch = cell & 0xFF;
                    uint8_t attr = (cell >> 8) & 0xFF;

                    uint16_t fg = term->fg_colors[idx];
                    uint16_t bg = term->bg_colors[idx];
                    if (attr & TERM_ATTR_INVERSE) {
                        uint16_t tmp = fg; fg = bg; bg = tmp;
                    }

                    const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
                    terminal_render_glyph(term->render_x_start + col * FONT_W, term->render_y_start + row * FONT_H, fg, bg, glyph, FONT_W, FONT_H);
                }
            } else {
                const uint8_t *space = get_glyph(term, ' ');
                for (int col = 0; col < cols; col++) {
                    terminal_render_glyph(term->render_x_start + col * FONT_W, term->render_y_start + row * FONT_H, 0xFFFF, 0x0000, space, FONT_W, FONT_H);
                }
            }
        }
    }

    umm_free(line_cells);
    umm_free(line_fg);
    umm_free(line_bg);

    terminal_clearFullDirty(term);
    for (int i = 0; i < term->rows; i++) {
        terminal_clearRowDirty(term, i);
    }
}

// Helper: Given a character offset within a logical line (spanning multiple buffer rows),
// return the buffer cell index (row * cols + col).
static int logical_char_to_cell_idx(terminal_t* term, int start_row, int char_offset) {
    int buffer_row = start_row + char_offset / term->cols;
    int buffer_col = char_offset % term->cols;
    return buffer_row * term->cols + buffer_col;
}

void terminal_render(terminal_t* term) {
    if (!term) return;

    if (term->scrollback_offset > 0) {
        terminal_renderScrollback(term);
        return;
    }

    // Calculate content area (accounts for line numbers and scrollbar)
    int content_offset_cols = 0;
    if (term->line_numbers_enabled) {
        content_offset_cols = term->line_number_cols;
    }

    int content_cols = term->cols;
    if (term->line_numbers_enabled) {
        content_cols -= term->line_number_cols;
    }
    if (term->scrollbar_enabled) {
        content_cols -= 1;
    }
    if (content_cols < 1) content_cols = 1;

    int x_offset = term->render_x_start + content_offset_cols * FONT_W;

    int wrap_col = term->word_wrap_column > 0 ? term->word_wrap_column : content_cols;
    if (wrap_col > content_cols) wrap_col = content_cols;

    // Pre-build logical line map (used by word wrap mode)
    int logical_starts[27];
    int num_logical = 0;
    if (term->word_wrap_enabled) {
        num_logical = build_logical_lines(term, logical_starts, 26);
    }

    // Render each display row
    for (int row = 0; row < term->rows; row++) {
        int y = term->render_y_start + row * FONT_H;
        if (!row_fits(term, y)) break;

        if (term->word_wrap_enabled) {

            int target_visual = row;
            int current_visual = 0;
            int found_li = -1;
            int segment_idx = 0;
            int segments[64];
            int num_segments = 0;
            int ll_start_row = 0;
            int ll_num_rows = 0;
            int logical_line_len = 0;

            for (int li = 0; li < num_logical; li++) {
                ll_start_row = logical_starts[li];
                ll_num_rows = count_logical_line_rows(term, ll_start_row);
                num_segments = calculate_wrap_segments(term, ll_start_row, ll_num_rows, segments, 64, content_cols, wrap_col, &logical_line_len);
                if (current_visual + num_segments > target_visual) {
                    segment_idx = target_visual - current_visual;
                    found_li = li;
                    break;
                }
                current_visual += num_segments;
            }

            if (found_li < 0) {
                // Beyond content — render blank row
                for (int col = 0; col < content_cols; col++) {
                    int x = x_offset + col * FONT_W;
                    if (x >= 320) break;
                    const uint8_t *space = get_glyph(term, ' ');
                    terminal_render_glyph(x, y, term->fg_color, term->bg_color, space, FONT_W, FONT_H);
                }
                continue;
            }

            // Get segment boundaries
            int segment_start = segments[segment_idx];
            int segment_end = (segment_idx + 1 < num_segments) ? segments[segment_idx + 1] : logical_line_len;
            int chars_to_render = segment_end - segment_start;
            if (chars_to_render > content_cols) chars_to_render = content_cols;

            // Render the segment characters
            for (int i = 0; i < chars_to_render; i++) {
                int x = x_offset + i * FONT_W;
                if (x >= 320) break;

                int idx = logical_char_to_cell_idx(term, ll_start_row, segment_start + i);
                uint16_t cell = term->cells[idx];
                uint8_t ch = cell & 0xFF;
                uint8_t attr = (cell >> 8) & 0xFF;
                uint16_t fg = term->fg_colors[idx];
                uint16_t bg = term->bg_colors[idx];

                if (attr & TERM_ATTR_INVERSE) {
                    uint16_t tmp = fg; fg = bg; bg = tmp;
                }

                const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
                terminal_render_glyph(x, y, fg, bg, glyph, FONT_W, FONT_H);
            }

            // Clear remaining columns in this row
            for (int i = chars_to_render; i < content_cols; i++) {
                int x = x_offset + i * FONT_W;
                if (x >= 320) break;
                const uint8_t *space = get_glyph(term, ' ');
                terminal_render_glyph(x, y, term->fg_color, term->bg_color, space, FONT_W, FONT_H);
            }

            // Draw wrap indicator if this segment wraps
            if (term->show_wrap_indicator && segment_idx < num_segments - 1) {
                int indicator_x = x_offset + (content_cols - 1) * FONT_W;
                if (indicator_x < 320) {
                    const uint8_t *ellipsis = get_glyph(term, '.');
                    terminal_render_glyph(indicator_x, y, 0xAD75, term->bg_color, ellipsis, FONT_W, FONT_H);
                }
            }
        } else {
            // Normal rendering mode (no word wrap)
            int logical_row_idx = row;
            if (logical_row_idx >= term->rows) continue;

            int base = logical_row_idx * term->cols;
            for (int col = 0; col < content_cols; col++) {
                int x = x_offset + col * FONT_W;
                if (x >= 320) break;

                int idx = base + col;
                uint16_t cell = term->cells[idx];
                uint8_t ch = cell & 0xFF;
                uint8_t attr = (cell >> 8) & 0xFF;

                uint16_t fg = term->fg_colors[idx];
                uint16_t bg = term->bg_colors[idx];

                if (attr & TERM_ATTR_INVERSE) {
                    uint16_t tmp = fg; fg = bg; bg = tmp;
                }

                const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
                terminal_render_glyph(x, y, fg, bg, glyph, FONT_W, FONT_H);
            }
        }
    }

    // Render cursor (positioned relative to content area)
    if (s_cursor_visible && term->cursor_y >= 0 && term->cursor_y < term->rows) {
        int cursor_vis_x = term->cursor_x;
        int cursor_vis_y = term->cursor_y;

        if (term->word_wrap_enabled) {
            // Find which logical line the cursor's buffer row belongs to
            int cursor_ll_start = find_logical_line_start(term, term->cursor_y);
            // Cursor's character offset within the logical line
            int cursor_char_offset = (term->cursor_y - cursor_ll_start) * term->cols + term->cursor_x;

            int current_visual = 0;
            for (int li = 0; li < num_logical; li++) {
                int lr = logical_starts[li];
                int lr_count = count_logical_line_rows(term, lr);
                int segments[64];
                int ns = calculate_wrap_segments(term, lr, lr_count, segments, 64, content_cols, wrap_col, NULL);

                if (lr == cursor_ll_start) {
                    // Find which segment contains cursor_char_offset
                    int seg_idx = 0;
                    for (int s = 0; s < ns; s++) {
                        int seg_end = (s + 1 < ns) ? segments[s + 1] : 9999;
                        if (cursor_char_offset >= segments[s] && cursor_char_offset < seg_end) {
                            seg_idx = s;
                            cursor_vis_x = cursor_char_offset - segments[s];
                            break;
                        }
                    }
                    cursor_vis_y = current_visual + seg_idx;
                    break;
                }
                current_visual += ns;
            }
        } else {
            // cursor_x/y are buffer (window) coordinates; scroll_position is
            // document-level info for the scrollbar thumb only and must not
            // offset the cursor (editors pass window-relative cursor coords).
            cursor_vis_y = term->cursor_y;
        }

        if (cursor_vis_y >= 0 && cursor_vis_y < term->rows && cursor_vis_x >= 0 && cursor_vis_x < content_cols) {
            bool draw_cursor = true;
            if (s_cursor_blink) {
                uint32_t now = to_ms_since_boot(get_absolute_time());
                if (now - s_last_blink_time > 500) {
                    s_blink_state = !s_blink_state;
                    s_last_blink_time = now;
                }
                draw_cursor = !s_blink_state;
            }

            int cy = term->render_y_start + cursor_vis_y * FONT_H;
            if (draw_cursor && row_fits(term, cy)) {
                int cx = x_offset + cursor_vis_x * FONT_W;

                int cursor_idx = term->cursor_y * term->cols + term->cursor_x;
                uint16_t cell = term->cells[cursor_idx];
                uint8_t ch = cell & 0xFF;
                uint16_t cursor_fg = term->fg_colors[cursor_idx];
                uint16_t cursor_bg = term->bg_colors[cursor_idx];

                const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
                terminal_render_glyph(cx, cy, cursor_bg, cursor_fg, glyph, FONT_W, FONT_H);
            }
        }
    }

    // Render line numbers on top
    if (term->line_numbers_enabled) {
        terminal_renderLineNumbers(term);
    }

    // Render scrollbar on top
    if (term->scrollbar_enabled) {
        terminal_renderScrollbar(term);
    }

    terminal_clearFullDirty(term);
    for (int i = 0; i < term->rows; i++) {
        terminal_clearRowDirty(term, i);
    }
}

void terminal_renderDirty(terminal_t* term) {
    if (!term) return;

    if (term->scrollback_offset > 0) {
        terminal_renderScrollback(term);
        return;
    }

    int first_dirty, last_dirty;
    terminal_getDirtyRange(term, &first_dirty, &last_dirty);

    if (first_dirty < 0) return;

    if (terminal_isFullDirty(term) || term->word_wrap_enabled) {
        terminal_render(term);
        return;
    }

    // Calculate content area (accounts for line numbers and scrollbar)
    int content_offset_cols = 0;
    if (term->line_numbers_enabled) {
        content_offset_cols = term->line_number_cols;
    }

    int content_cols = term->cols;
    if (term->line_numbers_enabled) {
        content_cols -= term->line_number_cols;
    }
    if (term->scrollbar_enabled) {
        content_cols -= 1; // Scrollbar uses 1 column
    }
    if (content_cols < 1) content_cols = 1;

    int x_offset = term->render_x_start + content_offset_cols * FONT_W;

    for (int row = first_dirty; row <= last_dirty; row++) {
        if (row < 0 || row >= term->rows) continue;
        if (!terminal_isRowDirty(term, row)) continue;

        int y = term->render_y_start + row * FONT_H;
        if (!row_fits(term, y)) {
            terminal_clearRowDirty(term, row);
            continue;
        }

        for (int col = 0; col < content_cols; col++) {
            int x = x_offset + col * FONT_W;

            int idx = row * term->cols + col;
            uint16_t cell = term->cells[idx];
            uint8_t ch = cell & 0xFF;
            uint8_t attr = (cell >> 8) & 0xFF;

            uint16_t fg = term->fg_colors[idx];
            uint16_t bg = term->bg_colors[idx];

            if (attr & TERM_ATTR_INVERSE) {
                uint16_t tmp = fg;
                fg = bg;
                bg = tmp;
            }

            const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
            terminal_render_glyph(x, y, fg, bg, glyph, FONT_W, FONT_H);
        }

        terminal_clearRowDirty(term, row);
    }

    // Render cursor
    if (s_cursor_visible && term->cursor_x >= 0 && term->cursor_x < content_cols &&
        term->cursor_y >= 0 && term->cursor_y < term->rows) {

        bool draw_cursor = true;
        if (s_cursor_blink) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - s_last_blink_time > 500) {
                s_blink_state = !s_blink_state;
                s_last_blink_time = now;
            }
            draw_cursor = !s_blink_state;
        }

        int cy = term->render_y_start + term->cursor_y * FONT_H;
        if (draw_cursor && row_fits(term, cy)) {
            int cx = x_offset + term->cursor_x * FONT_W;

            int cursor_idx = term->cursor_y * term->cols + term->cursor_x;
            uint16_t cell = term->cells[cursor_idx];
            uint8_t ch = cell & 0xFF;
            uint16_t cursor_fg = term->fg_colors[cursor_idx];
            uint16_t cursor_bg = term->bg_colors[cursor_idx];

            const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
            terminal_render_glyph(cx, cy, cursor_bg, cursor_fg, glyph, FONT_W, FONT_H);
        }
    }

    // Render line numbers if dirty
    if (term->line_numbers_enabled) {
        terminal_renderLineNumbers(term);
    }

    // Render scrollbar if dirty
    if (term->scrollbar_enabled) {
        terminal_renderScrollbar(term);
    }
}

void terminal_renderRow(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return;

    int y = term->render_y_start + row * FONT_H;
    if (!row_fits(term, y)) {
        terminal_clearRowDirty(term, row);
        return;
    }

    for (int col = 0; col < term->cols; col++) {
        int x = term->render_x_start + col * FONT_W;

        int idx = row * term->cols + col;
        uint16_t cell = term->cells[idx];
        uint8_t ch = cell & 0xFF;
        uint8_t attr = (cell >> 8) & 0xFF;

        uint16_t fg = term->fg_colors[idx];
        uint16_t bg = term->bg_colors[idx];

        if (attr & TERM_ATTR_INVERSE) {
            uint16_t tmp = fg;
            fg = bg;
            bg = tmp;
        }

        const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
        terminal_render_glyph(x, y, fg, bg, glyph, FONT_W, FONT_H);
    }

    terminal_clearRowDirty(term, row);
}

void terminal_setCursorVisible(bool visible) {
    s_cursor_visible = visible;
}

bool terminal_getCursorVisible(void) {
    return s_cursor_visible;
}

void terminal_setCursorBlink(bool blink) {
    s_cursor_blink = blink;
}

bool terminal_getCursorBlink(void) {
    return s_cursor_blink;
}
