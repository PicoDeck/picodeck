#include "text_wrap.h"

int text_wrap_find_break(const char *text, int text_len, int max_cols) {
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

    return max_cols;  // Force break
}

int text_wrap_segments(const char *text, int text_len, int max_cols,
                       int *segments, int max_segments) {
    if (text_len == 0 || max_cols <= 0) {
        if (segments && max_segments > 0) segments[0] = 0;
        return 1;
    }

    int visual_rows = 0;
    int pos = 0;

    while (pos < text_len && visual_rows < max_segments) {
        if (segments) segments[visual_rows] = pos;
        visual_rows++;

        int remaining = text_len - pos;
        if (remaining <= max_cols) {
            break;
        }

        int wrap_pos = text_wrap_find_break(text + pos, remaining, max_cols);
        if (wrap_pos == 0) wrap_pos = max_cols;
        pos += wrap_pos;
    }

    return visual_rows > 0 ? visual_rows : 1;
}

int text_wrap_next_row(const char *text, int text_len, int pos, int max_cols) {
    if (max_cols <= 0) return -1;

    int remaining = text_len - pos;
    // A newline right after max_cols characters still ends this row, so look
    // one further. Stopping there keeps a long line's rows cheap.
    int look = remaining < max_cols + 1 ? remaining : max_cols + 1;
    for (int i = 0; i < look; i++) {
        if (text[pos + i] == '\n') return pos + i + 1;
    }

    if (remaining <= max_cols) return -1;
    return pos + text_wrap_find_break(text + pos, remaining, max_cols);
}
