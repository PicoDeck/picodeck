#include "lua_bridge_internal.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../os/os.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TERM_COLS 53
#define TERM_INPUT_ROW 37
// Scrollback: a ring of fixed-width lines (REPL_SCROLLBACK * (TERM_COLS + 1)
// = 13,824 bytes), allocated from the PSRAM heap on first use.
#define REPL_SCROLLBACK 256
#define MAX_INPUT_LEN 256

typedef struct {
    char lines[REPL_SCROLLBACK][TERM_COLS + 1];
    int head;           // ring index of the oldest line
    int line_count;
    int scroll_offset;  // lines scrolled back from the newest (0 = bottom)
    char input_buffer[MAX_INPUT_LEN];
    int input_len;
} repl_state_t;

// Allocated lazily (umm_malloc) by the first repl.* call that needs it and
// freed by repl.clear() and at app exit (lua_bridge_repl_release): an app
// that never uses the REPL costs nothing, and it never touches the SRAM heap.
static repl_state_t *s_repl = NULL;
static bool s_echo = true;

// The state, allocated on first use; raises a Lua error when out of memory.
static repl_state_t *repl_state(lua_State *L) {
    if (!s_repl) {
        s_repl = (repl_state_t *)umm_malloc(sizeof(repl_state_t));
        if (!s_repl)
            luaL_error(L, "repl: out of memory");
        memset(s_repl, 0, sizeof(*s_repl));
    }
    return s_repl;
}

static void lua_bridge_repl_release_state(void) {
    if (s_repl) {
        umm_free(s_repl);
        s_repl = NULL;
    }
}

void lua_bridge_repl_release(void) {
    lua_bridge_repl_release_state();
    s_echo = true;
}

static const char *repl_line(const repl_state_t *r, int i) {
    return r->lines[(r->head + i) % REPL_SCROLLBACK];
}

static void term_clear(void) {
    display_clear(COLOR_BLACK);
}

static void term_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg) {
    char s[2] = {c, '\0'};
    int saved_font = display_get_font();   // 6x8 cells: the built-in font
    display_set_font(0);
    display_draw_text(x * 6, y * 8, s, fg, bg);
    display_set_font(saved_font);
}

static void term_draw_line(int row, const char *line, int start_col, uint16_t fg, uint16_t bg) {
    int x = start_col;
    for (int i = 0; line[i] && x < TERM_COLS; i++, x++) {
        term_draw_char(x, row, line[i], fg, bg);
    }
    for (; x < TERM_COLS; x++) {
        term_draw_char(x, row, ' ', fg, bg);
    }
}

// The input row shows the tail of the line when it is wider than the screen.
static void term_draw_input(repl_state_t *r) {
    int start = r->input_len > TERM_COLS - 1 ? r->input_len - (TERM_COLS - 1) : 0;
    term_draw_line(TERM_INPUT_ROW, r->input_buffer + start, 0, COLOR_WHITE, COLOR_DKGRAY);
    term_draw_char(r->input_len - start, TERM_INPUT_ROW, ' ', COLOR_BLACK, COLOR_WHITE);
}

static void term_scroll_up(repl_state_t *r) {
    if (r->scroll_offset < r->line_count - TERM_INPUT_ROW) {
        r->scroll_offset++;
    }
}

static void term_scroll_down(repl_state_t *r) {
    if (r->scroll_offset > 0) {
        r->scroll_offset--;
    }
}

static void term_redraw(repl_state_t *r) {
    display_clear(COLOR_BLACK);
    // The newest TERM_INPUT_ROW lines, moved back by scroll_offset.
    int first = r->line_count - TERM_INPUT_ROW - r->scroll_offset;
    if (first < 0) first = 0;
    int row = 0;
    for (int i = first; i < r->line_count && row < TERM_INPUT_ROW; i++) {
        term_draw_line(row, repl_line(r, i), 0, COLOR_WHITE, COLOR_BLACK);
        row++;
    }
    for (; row < TERM_INPUT_ROW; row++) {
        term_draw_line(row, "", 0, COLOR_GRAY, COLOR_BLACK);
    }
    display_draw_line(0, TERM_INPUT_ROW * 8, 320, TERM_INPUT_ROW * 8, COLOR_DKGRAY);
    term_draw_input(r);
    display_flush();
}

// Appends one line (at most TERM_COLS chars of it); the oldest line is
// dropped once REPL_SCROLLBACK lines are held.
static void term_add_line(repl_state_t *r, const char *line, size_t len) {
    if (len > TERM_COLS) len = TERM_COLS;
    int slot;
    if (r->line_count < REPL_SCROLLBACK) {
        slot = (r->head + r->line_count) % REPL_SCROLLBACK;
        r->line_count++;
    } else {
        slot = r->head;
        r->head = (r->head + 1) % REPL_SCROLLBACK;
    }
    memcpy(r->lines[slot], line, len);
    r->lines[slot][len] = '\0';
    r->scroll_offset = 0;
}

// Splits text at newlines and at TERM_COLS columns.
static void term_print(repl_state_t *r, const char *text, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n' || i - start == TERM_COLS) {
            if (i < len || i > start)
                term_add_line(r, text + start, i - start);
            if (i < len && text[i] == '\n') {
                start = i + 1;
            } else {
                start = i;
            }
        }
    }
}

static int l_repl_readline(lua_State *L) {
    repl_state_t *r = repl_state(L);
    kbd_poll();

    uint32_t pressed = kbd_get_buttons_pressed();

    if (pressed & BTN_ESC) {
        lua_pushnil(L);
        return 1;
    }

    if (pressed & BTN_UP) {
        term_scroll_up(r);
        term_redraw(r);
        kbd_clear_state();
        lua_pushnil(L);
        return 1;
    }

    if (pressed & BTN_DOWN) {
        term_scroll_down(r);
        term_redraw(r);
        kbd_clear_state();
        lua_pushnil(L);
        return 1;
    }

    if (pressed & BTN_ENTER) {
        lua_pushlstring(L, r->input_buffer, (size_t)r->input_len);
        if (s_echo) {
            term_print(r, r->input_buffer, (size_t)r->input_len);
        }
        r->input_len = 0;
        r->input_buffer[0] = '\0';
        term_redraw(r);
        return 1;
    }

    if (pressed & BTN_BACKSPACE) {
        if (r->input_len > 0) {
            r->input_buffer[--r->input_len] = '\0';
            term_draw_input(r);
            display_flush();
        }
        kbd_clear_state();
        lua_pushnil(L);
        return 1;
    }

    char c = kbd_get_char();
    if (c && r->input_len < MAX_INPUT_LEN - 1) {
        r->input_buffer[r->input_len++] = c;
        r->input_buffer[r->input_len] = '\0';
        term_draw_input(r);
        display_flush();
    }

    lua_pushnil(L);
    return 1;
}

// repl.print(...): the values as tostring() shows them, tab-separated,
// built in a Lua buffer (no fixed-size C buffer to overrun).
static int l_repl_print(lua_State *L) {
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1)
            luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);  // pushes the string (honours __tostring)
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    // The state only now: a __tostring above may have run repl.clear(),
    // which frees it.
    repl_state_t *r = repl_state(L);
    size_t len;
    const char *text = lua_tolstring(L, -1, &len);
    term_print(r, text, len);
    term_redraw(r);
    return 0;
}

// Clears the screen and frees the scrollback (the next repl call starts a
// fresh one).
static int l_repl_clear(lua_State *L) {
    (void)L;
    lua_bridge_repl_release_state();
    term_clear();
    display_flush();
    return 0;
}

static int l_repl_echo(lua_State *L) {
    s_echo = lua_toboolean(L, 1);
    return 0;
}

static const luaL_Reg l_repl_lib[] = {
    {"readline", l_repl_readline},
    {"print", l_repl_print},
    {"clear", l_repl_clear},
    {"echo", l_repl_echo},
    {NULL, NULL}
};

void lua_bridge_repl_init(lua_State *L) {
    // Nothing is allocated until the app uses the REPL; a previous app's
    // state is normally gone already (freed at its exit).
    lua_bridge_repl_release();
    register_subtable(L, "repl", l_repl_lib);
}
