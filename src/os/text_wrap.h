#pragma once

// Shared word wrap utility for PicoDeck
// Used by terminal_render.c and lua_bridge_ui.c

// Find best break position in text, searching backward from max_cols
// for spaces/punctuation (space, tab, dash, comma, period, semicolons,
// colons, punctuation marks). Force-breaks at max_cols if none found.
//
// text:     pointer to current position in string
// text_len: remaining characters from this position
// max_cols: maximum columns per visual row
//
// Returns: number of chars to consume for this visual row.
//          0 if text_len <= max_cols (no break needed).
int text_wrap_find_break(const char *text, int text_len, int max_cols);

// Calculate wrap segments for a plain string.
// Each segment is the character offset where a new visual row starts.
//
// text:         input string (length given by text_len, not null-terminated)
// text_len:     length of text
// max_cols:     columns per visual row
// segments:     output array — segments[i] = char offset of visual row i
// max_segments: size of segments array
//
// Returns: number of visual rows (always >= 1)
int text_wrap_segments(const char *text, int text_len, int max_cols,
                       int *segments, int max_segments);

// One step of wrapping a whole text, newlines included (picocalc.ui.wrapText):
// the terminal's text_wrap_segments sees one logical line and has no newlines
// to honour. A row ends at a newline, which belongs to it, or where
// text_wrap_find_break says, so a text with no newline wraps exactly as
// text_wrap_segments wraps it.
//
// pos: where the current row starts (0 for the first)
//
// Returns: where the next row starts, or -1 if this row is the last. A text
//          ending in a newline has one more, empty, row (at text_len).
//          max_cols <= 0 makes the whole text one row.
int text_wrap_next_row(const char *text, int text_len, int pos, int max_cols);
