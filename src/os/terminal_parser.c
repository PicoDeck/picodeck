#include "terminal_parser.h"
#include "terminal.h"
#include "terminal_render.h"
#include "color256.h"
#include <string.h>
#include <stdio.h>

#define RGB565(r, g, b) (((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))))

static uint16_t s_default_fg = RGB565(255, 255, 255);
static uint16_t s_default_bg = RGB565(0, 0, 0);

static uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return RGB565(r, g, b);
}

void terminal_parser_init(terminal_parser_t* parser, terminal_t* term) {
    memset(parser, 0, sizeof(terminal_parser_t));
    parser->terminal = term;
    parser->state = TERM_STATE_NORMAL;
    terminal_setColors(term, s_default_fg, s_default_bg);
}

void terminal_parser_reset_attrs(terminal_parser_t* parser) {
    parser->attrs = 0;
    if (parser->terminal) {
        terminal_setColors(parser->terminal, s_default_fg, s_default_bg);
        parser->terminal->current_attr = 0;
    }
}

void terminal_parser_reset(terminal_parser_t* parser) {
    parser->state = TERM_STATE_NORMAL;
    parser->param_count = 0;
    parser->current_param = 0;
    parser->has_intermediate = false;
    parser->private_marker = 0;
    terminal_parser_reset_attrs(parser);
}

// Map recognized Unicode codepoints to extended cell indices 0x80-0x9F
static uint8_t map_unicode_to_cell(uint32_t cp) {
    switch (cp) {
        // Light box drawing
        case 0x2500: return 0x80;  // ─
        case 0x2502: return 0x81;  // │
        case 0x250C: return 0x82;  // ┌
        case 0x2510: return 0x83;  // ┐
        case 0x2514: return 0x84;  // └
        case 0x2518: return 0x85;  // ┘
        case 0x251C: return 0x86;  // ├
        case 0x2524: return 0x87;  // ┤
        case 0x252C: return 0x88;  // ┬
        case 0x2534: return 0x89;  // ┴
        case 0x253C: return 0x8A;  // ┼
        // Heavy box drawing
        case 0x2501: return 0x8B;  // ━
        case 0x2503: return 0x8C;  // ┃
        case 0x250F: return 0x8D;  // ┏
        case 0x2513: return 0x8E;  // ┓
        case 0x2517: return 0x8F;  // ┗
        case 0x251B: return 0x90;  // ┛
        case 0x2523: return 0x91;  // ┣
        case 0x252B: return 0x92;  // ┫
        case 0x2533: return 0x93;  // ┳
        case 0x253B: return 0x94;  // ┻
        case 0x254B: return 0x95;  // ╋
        // Double-line box drawing
        case 0x2550: return 0x96;  // ═
        case 0x2551: return 0x97;  // ║
        case 0x2554: return 0x98;  // ╔
        case 0x2557: return 0x99;  // ╗
        case 0x255A: return 0x9A;  // ╚
        case 0x255D: return 0x9B;  // ╝
        // Block elements
        case 0x2588: return 0x9C;  // █
        case 0x2591: return 0x9D;  // ░
        case 0x2592: return 0x9E;  // ▒
        case 0x2593: return 0x9F;  // ▓
        default:     return '?';
    }
}

static void terminal_parser_process_csi(terminal_parser_t* parser, char cmd) {
    terminal_t* term = parser->terminal;
    if (!term) return;

    int p1 = parser->param_count > 0 ? parser->params[0] : 0;
    int p2 = parser->param_count > 1 ? parser->params[1] : 0;

    // DEC private mode sequences (ESC[?...h / ESC[?...l)
    if (parser->private_marker == '?') {
        bool set = (cmd == 'h');  // 'h' = set, 'l' = reset
        if (cmd == 'h' || cmd == 'l') {
            for (int i = 0; i < parser->param_count; i++) {
                switch (parser->params[i]) {
                    case 1:   // DECCKM - cursor key mode
                        parser->decckm = set;
                        break;
                    case 25:  // DECTCEM - cursor visible
                        terminal_setCursorVisible(set);
                        break;
                    case 1049: // Alt screen buffer (simplified: just clear)
                        if (set) {
                            terminal_eraseDisplay(term, 2);
                            terminal_setCursor(term, 0, 0);
                        }
                        break;
                }
            }
        }
        return;
    }

    // SGR - Select Graphic Rendition
    if (cmd == 'm') {
        for (int i = 0; i < parser->param_count; i++) {
            int param = parser->params[i];

            switch (param) {
                case 0: // Reset
                    terminal_parser_reset_attrs(parser);
                    break;
                case 1: // Bold
                    parser->attrs |= TERM_ATTR_BOLD;
                    break;
                case 2: // Dim
                    parser->attrs |= TERM_ATTR_DIM;
                    break;
                case 3: // Italic
                    parser->attrs |= TERM_ATTR_ITALIC;
                    break;
                case 4: // Underline
                    parser->attrs |= TERM_ATTR_UNDERLINE;
                    break;
                case 5: // Blink
                case 6:
                    parser->attrs |= TERM_ATTR_BLINK;
                    break;
                case 7: // Inverse
                    parser->attrs |= TERM_ATTR_INVERSE;
                    break;
                case 8: // Hidden
                    parser->attrs |= TERM_ATTR_HIDDEN;
                    break;
                case 9: // Strikethrough
                    parser->attrs |= TERM_ATTR_STRIKE;
                    break;
                case 22: // Normal (not bold/dim)
                    parser->attrs &= ~(TERM_ATTR_BOLD | TERM_ATTR_DIM);
                    break;
                case 23: // Not italic
                    parser->attrs &= ~TERM_ATTR_ITALIC;
                    break;
                case 24: // Not underline
                    parser->attrs &= ~TERM_ATTR_UNDERLINE;
                    break;
                case 25: // Not blink
                    parser->attrs &= ~TERM_ATTR_BLINK;
                    break;
                case 27: // Not inverse
                    parser->attrs &= ~TERM_ATTR_INVERSE;
                    break;
                case 29: // Not strikethrough
                    parser->attrs &= ~TERM_ATTR_STRIKE;
                    break;
                case 30: case 31: case 32: case 33:
                case 34: case 35: case 36: case 37: {
                    uint16_t cur_fg, cur_bg;
                    terminal_getColors(term, &cur_fg, &cur_bg);
                    terminal_setColors(term, color256_palette[param - 30], cur_bg);
                    break;
                }
                case 38: {
                    // Extended foreground color
                    uint16_t cur_fg, cur_bg;
                    terminal_getColors(term, &cur_fg, &cur_bg);
                    if (i + 4 < parser->param_count && parser->params[i + 1] == 2) {
                        // 24-bit color: 38;2;R;G;B
                        uint16_t fg = rgb888_to_rgb565(
                            parser->params[i + 2],
                            parser->params[i + 3],
                            parser->params[i + 4]
                        );
                        terminal_setColors(term, fg, cur_bg);
                        i += 4;
                    } else if (i + 2 < parser->param_count && parser->params[i + 1] == 5) {
                        // 256 color: 38;5;N
                        int color_idx = parser->params[i + 2];
                        if (color_idx >= 0 && color_idx < 256) {
                            terminal_setColors(term, color256_palette[color_idx], cur_bg);
                        }
                        i += 2;
                    }
                    break;
                }
                case 39: { // Default foreground
                    uint16_t cur_fg, cur_bg;
                    terminal_getColors(term, &cur_fg, &cur_bg);
                    terminal_setColors(term, s_default_fg, cur_bg);
                    break;
                }
                case 40: case 41: case 42: case 43:
                case 44: case 45: case 46: case 47: {
                    uint16_t cur_fg, cur_bg;
                    terminal_getColors(term, &cur_fg, &cur_bg);
                    terminal_setColors(term, cur_fg, color256_palette[param - 40]);
                    break;
                }
                case 48: {
                    // Extended background color
                    uint16_t cur_fg, cur_bg;
                    terminal_getColors(term, &cur_fg, &cur_bg);
                    if (i + 4 < parser->param_count && parser->params[i + 1] == 2) {
                        // 24-bit color: 48;2;R;G;B
                        uint16_t bg = rgb888_to_rgb565(
                            parser->params[i + 2],
                            parser->params[i + 3],
                            parser->params[i + 4]
                        );
                        terminal_setColors(term, cur_fg, bg);
                        i += 4;
                    } else if (i + 2 < parser->param_count && parser->params[i + 1] == 5) {
                        // 256 color: 48;5;N
                        int color_idx = parser->params[i + 2];
                        if (color_idx >= 0 && color_idx < 256) {
                            terminal_setColors(term, cur_fg, color256_palette[color_idx]);
                        }
                        i += 2;
                    }
                    break;
                }
                case 49: { // Default background
                    uint16_t cur_fg, cur_bg;
                    terminal_getColors(term, &cur_fg, &cur_bg);
                    terminal_setColors(term, cur_fg, s_default_bg);
                    break;
                }
                case 90: case 91: case 92: case 93:
                case 94: case 95: case 96: case 97: {
                    uint16_t cur_fg, cur_bg;
                    terminal_getColors(term, &cur_fg, &cur_bg);
                    terminal_setColors(term, color256_palette[param - 90 + 8], cur_bg);
                    break;
                }
                case 100: case 101: case 102: case 103:
                case 104: case 105: case 106: case 107: {
                    uint16_t cur_fg, cur_bg;
                    terminal_getColors(term, &cur_fg, &cur_bg);
                    terminal_setColors(term, cur_fg, color256_palette[param - 100 + 8]);
                    break;
                }
            }
        }
        return;
    }

    switch (cmd) {
        case 'A': // CUU - Cursor Up
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x, term->cursor_y - p1);
            break;

        case 'B': // CUD - Cursor Down
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x, term->cursor_y + p1);
            break;

        case 'C': // CUF - Cursor Forward
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x + p1, term->cursor_y);
            break;

        case 'D': // CUB - Cursor Back
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x - p1, term->cursor_y);
            break;

        case 'E': // CNL - Cursor Next Line
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, 0, term->cursor_y + p1);
            break;

        case 'F': // CPL - Cursor Previous Line
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, 0, term->cursor_y - p1);
            break;

        case 'G': // CHA - Cursor Horizontal Absolute
            terminal_setCursor(term, p1 - 1, term->cursor_y);
            break;

        case 'H': // CUP - Cursor Position
        case 'f': // Also CUP
            if (p1 == 0) p1 = 1;
            if (p2 == 0) p2 = 1;
            terminal_setCursor(term, p2 - 1, p1 - 1);
            break;

        case 'J': // ED - Erase Display
            terminal_eraseDisplay(term, p1);
            break;

        case 'K': // EL - Erase Line
            terminal_eraseLine(term, p1);
            break;

        case 'L': { // IL - Insert Lines
            if (p1 == 0) p1 = 1;
            int saved_top = term->scroll_top;
            term->scroll_top = term->cursor_y;
            for (int i = 0; i < p1; i++) {
                terminal_scrollDown(term);
            }
            term->scroll_top = saved_top;
            break;
        }

        case 'M': { // DL - Delete Lines
            if (p1 == 0) p1 = 1;
            int saved_top = term->scroll_top;
            term->scroll_top = term->cursor_y;
            for (int i = 0; i < p1; i++) {
                terminal_scrollUp(term);
            }
            term->scroll_top = saved_top;
            break;
        }

        case '@': // ICH - Insert Characters
            if (p1 == 0) p1 = 1;
            terminal_insertChars(term, p1);
            break;

        case 'P': // DCH - Delete Characters
            if (p1 == 0) p1 = 1;
            terminal_deleteChars(term, p1);
            break;

        case 'r': // DECSTBM - Set Scrolling Region
            if (p1 == 0) p1 = 1;
            if (p2 == 0) p2 = term->rows;
            terminal_setScrollRegion(term, p1 - 1, p2 - 1);
            terminal_setCursor(term, 0, 0);  // cursor goes home after DECSTBM
            break;

        case 'd': // VPA - Vertical Position Absolute
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x, p1 - 1);
            break;

        case 'X': { // ECH - Erase Characters
            if (p1 == 0) p1 = 1;
            for (int j = 0; j < p1 && term->cursor_x + j < term->cols; j++) {
                int idx = term->cursor_y * term->cols + term->cursor_x + j;
                term->cells[idx] = TERM_BLANK_CELL;
                term->fg_colors[idx] = term->fg_color;
                term->bg_colors[idx] = term->bg_color;
            }
            term->row_dirty[term->cursor_y] = 1;
            break;
        }

        case 'S': // SU - Scroll Up
            if (p1 == 0) p1 = 1;
            terminal_scroll(term, p1);
            break;

        case 'T': // SD - Scroll Down
            if (p1 == 0) p1 = 1;
            terminal_scroll(term, -p1);
            break;

        case 's': // SCP - Save Cursor Position
            parser->saved_x = term->cursor_x;
            parser->saved_y = term->cursor_y;
            parser->saved_cursor_x = true;
            parser->saved_cursor_y = true;
            break;

        case 'u': // RCP - Restore Cursor Position
            if (parser->saved_cursor_x && parser->saved_cursor_y) {
                terminal_setCursor(term, parser->saved_x, parser->saved_y);
            }
            break;

        case 'n': // DSR - Device Status Report (ignore)
        case 'c': // DA - Device Attributes (ignore)
            break;
    }
}

void terminal_parser_parse(terminal_parser_t* parser, const char* data, int len) {
    if (!parser || !parser->terminal || !data) return;

    terminal_t* term = parser->terminal;

    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)data[i];

        // A NUL does nothing, in any state: a terminal ignores it, even inside an
        // escape sequence.
        if (c == 0) continue;

        switch (parser->state) {
            case TERM_STATE_NORMAL:
                // UTF-8 continuation byte handling
                if (parser->utf8_remaining > 0) {
                    if ((c & 0xC0) == 0x80) {
                        parser->utf8_codepoint = (parser->utf8_codepoint << 6) | (c & 0x3F);
                        parser->utf8_remaining--;
                        if (parser->utf8_remaining == 0) {
                            uint8_t mapped = map_unicode_to_cell(parser->utf8_codepoint);
                            term->current_attr = parser->attrs;
                            terminal_putChar(term, (char)mapped);
                        }
                    } else {
                        // Malformed sequence — reset and reprocess byte
                        parser->utf8_remaining = 0;
                        i--;
                    }
                    break;
                }

                if (c == 0x1B) { // ESC
                    parser->state = TERM_STATE_ESC;
                } else if (c == '\n' || c == '\r' || c == '\t' || c == '\b') {
                    terminal_putChar(term, (char)c);
                } else if (c >= 0xC0 && c <= 0xF7) {
                    // UTF-8 lead byte
                    if (c >= 0xF0) {
                        parser->utf8_remaining = 3;
                        parser->utf8_codepoint = c & 0x07;
                    } else if (c >= 0xE0) {
                        parser->utf8_remaining = 2;
                        parser->utf8_codepoint = c & 0x0F;
                    } else {
                        parser->utf8_remaining = 1;
                        parser->utf8_codepoint = c & 0x1F;
                    }
                } else if (c >= 0x20) {
                    // Regular printable ASCII character
                    term->current_attr = parser->attrs;
                    terminal_putChar(term, (char)c);
                }
                break;

            case TERM_STATE_ESC:
                if (c == '[') {
                    parser->state = TERM_STATE_CSI;
                    parser->param_count = 0;
                    parser->current_param = 0;
                    parser->has_intermediate = false;
                    parser->private_marker = 0;
                    memset(parser->params, 0, sizeof(parser->params));
                } else if (c == ']') {
                    parser->state = TERM_STATE_OSC;
                } else if (c == '7') {
                    // Save cursor (ESC 7)
                    parser->saved_x = term->cursor_x;
                    parser->saved_y = term->cursor_y;
                    parser->saved_cursor_x = true;
                    parser->saved_cursor_y = true;
                    parser->state = TERM_STATE_NORMAL;
                } else if (c == '8') {
                    // Restore cursor (ESC 8)
                    if (parser->saved_cursor_x && parser->saved_cursor_y) {
                        terminal_setCursor(term, parser->saved_x, parser->saved_y);
                    }
                    parser->state = TERM_STATE_NORMAL;
                } else {
                    // Unknown escape sequence, ignore
                    parser->state = TERM_STATE_NORMAL;
                }
                break;

            case TERM_STATE_CSI:
                if (c >= '0' && c <= '9') {
                    // Digit - accumulate parameter
                    parser->current_param = parser->current_param * 10 + (c - '0');
                } else if (c == ';') {
                    // Parameter separator
                    if (parser->param_count < TERM_PARSER_MAX_PARAMS) {
                        parser->params[parser->param_count++] = parser->current_param;
                    }
                    parser->current_param = 0;
                } else if (c >= 0x3C && c <= 0x3F) {
                    // Private parameter prefix: '<', '=', '>', '?'
                    parser->private_marker = c;
                } else if ((c >= '@' && c <= '~') && c != '[' && c != ']') {
                    // Final character - process command
                    if (parser->param_count < TERM_PARSER_MAX_PARAMS && parser->current_param > 0) {
                        parser->params[parser->param_count++] = parser->current_param;
                    } else if (parser->param_count < TERM_PARSER_MAX_PARAMS) {
                        parser->params[parser->param_count++] = 0;
                    }
                    terminal_parser_process_csi(parser, (char)c);
                    parser->state = TERM_STATE_NORMAL;
                } else if (c >= ' ' && c <= '/') {
                    // Intermediate character
                    parser->has_intermediate = true;
                    parser->intermediate = c;
                }
                break;

            case TERM_STATE_OSC:
                // OSC (Operating System Command) - ignore for now
                if (c == 0x07 || c == 0x1B) { // BEL or ESC
                    parser->state = TERM_STATE_NORMAL;
                }
                break;

            case TERM_STATE_ESC_DIGIT:
                // For private sequences, just go back to normal
                parser->state = TERM_STATE_NORMAL;
                break;
        }
    }
}

void terminal_parser_write(terminal_parser_t* parser, const char* str) {
    if (!parser || !str) return;
    terminal_parser_parse(parser, str, strlen(str));
}

uint16_t terminal_parser_getFG(terminal_parser_t* parser) {
    if (!parser) return s_default_fg;
    uint16_t fg, bg;
    terminal_getColors(parser->terminal, &fg, &bg);
    if (parser->attrs & TERM_ATTR_INVERSE) return bg;
    return fg;
}

uint16_t terminal_parser_getBG(terminal_parser_t* parser) {
    if (!parser) return s_default_bg;
    uint16_t fg, bg;
    terminal_getColors(parser->terminal, &fg, &bg);
    if (parser->attrs & TERM_ATTR_INVERSE) return fg;
    return bg;
}
