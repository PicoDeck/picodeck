#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// A text terminal widget: a grid of character cells with scrollback. Sizes are
// in cells unless they say pixels, and colours are RGB565. It draws in its own
// font, not the one display->setFont selects.
// cols and rows only size the grid. What shows is set by the render bounds, in
// pixels: by default y 28 to 302 at x 4 (below a header, above a footer), so a
// full-screen terminal calls setRenderBounds(term, 0, 320), and rows past the
// bottom bound are not drawn. render() and renderDirty() draw into the back
// buffer and do not flush: call display->flush() afterwards.

typedef struct terminal terminal_t;

typedef struct {
    terminal_t* (*create)(int cols, int rows, int scrollback_lines);
    void (*free)(terminal_t* term);
    void (*clear)(terminal_t* term);
    void (*write)(terminal_t* term, const char* str);
    void (*putChar)(terminal_t* term, char c);
    void (*setCursor)(terminal_t* term, int x, int y);
    void (*getCursor)(terminal_t* term, int* out_x, int* out_y);
    void (*setColors)(terminal_t* term, uint16_t fg, uint16_t bg);
    void (*getColors)(terminal_t* term, uint16_t* out_fg, uint16_t* out_bg);
    void (*scroll)(terminal_t* term, int lines);
    void (*render)(terminal_t* term);
    void (*renderDirty)(terminal_t* term);
    int (*getCols)(terminal_t* term);
    int (*getRows)(terminal_t* term);
    // NOTE: the next two take no terminal_t*; they set one global cursor state.
    void (*setCursorVisible)(bool visible);
    void (*setCursorBlink)(bool blink);
    void (*markAllDirty)(terminal_t* term);
    bool (*isFullDirty)(terminal_t* term);
    void (*getDirtyRange)(terminal_t* term, int* out_first, int* out_last);
    int (*getScrollbackCount)(terminal_t* term);
    void (*setScrollbackOffset)(terminal_t* term, int offset);
    int (*getScrollbackOffset)(terminal_t* term);
    void (*getScrollbackLine)(terminal_t* term, int line, uint16_t* out_cells);
    void (*getScrollbackLineColors)(terminal_t* term, int line, uint16_t* out_fg, uint16_t* out_bg);
    // Line numbers
    void (*setLineNumbers)(terminal_t* term, bool enabled);
    void (*setLineNumberStart)(terminal_t* term, int start);
    void (*setLineNumberCols)(terminal_t* term, int cols);
    void (*setLineNumberColors)(terminal_t* term, uint16_t fg, uint16_t bg);
    int (*getContentCols)(terminal_t* term);
    // Scrollbar
    void (*setScrollbar)(terminal_t* term, bool enabled);
    void (*setScrollbarColors)(terminal_t* term, uint16_t bg, uint16_t thumb);
    void (*setScrollbarWidth)(terminal_t* term, int width);
    void (*setScrollInfo)(terminal_t* term, int total_lines, int scroll_position);
    // Render bounds
    void (*setRenderBounds)(terminal_t* term, int y_start, int y_end);
    // Word wrap (visual - content not modified)
    void (*setWordWrap)(terminal_t* term, bool enabled);
    void (*setWordWrapColumn)(terminal_t* term, int column);
    void (*setWrapIndicator)(terminal_t* term, bool enabled);
    bool (*getWordWrap)(terminal_t* term);
    int (*getVisualRowCount)(terminal_t* term);
    void (*logicalToVisual)(terminal_t* term, int log_x, int log_y, int* vis_x, int* vis_y);
    void (*visualToLogical)(terminal_t* term, int vis_x, int vis_y, int* log_x, int* log_y);
    int (*calculateLineWraps)(terminal_t* term, int logical_line, int* segments, int max_segments);
} picocalc_terminal_t;

#ifdef __cplusplus
}  // extern "C"
#endif
