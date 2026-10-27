#include "lua_bridge_internal.h"
#include "../os/terminal.h"
#include "../os/terminal_parser.h"
#include "../os/terminal_render.h"
#include "../fonts/font_scientifica.h"
#include "../dev_commands.h"
#include "umm_malloc.h"
#include "pico/time.h"

#ifdef PICODECK_SIMULATOR
#include "sim_socket_handler.h"
#endif

#define TERMINAL_MT "picocalc.terminal"

typedef struct {
    terminal_t* term;
    terminal_parser_t parser;
} lua_terminal_t;

// The live terminal at idx, or a Lua error. term is NULL once __gc freed it
// (reachable only from a later finaliser: __gc is not a method).
static lua_terminal_t* check_terminal(lua_State* L, int idx) {
    lua_terminal_t* t = (lua_terminal_t*)lb_checkudata(L, idx, TERMINAL_MT);
    if (!t->term)
        luaL_error(L, "attempt to use a destroyed terminal");
    return t;
}

static int l_terminal_gc(lua_State* L) {
    lua_terminal_t* t = (lua_terminal_t*)lb_checkudata(L, 1, TERMINAL_MT);
    if (t->term) {
#ifdef PICODECK_SIMULATOR
        if (sim_get_active_terminal() == t->term)
            sim_set_active_terminal(NULL);
#endif
        terminal_free(t->term);
        t->term = NULL;
    }
    return 0;
}

// The panel is 320x320; a terminal cell is FONT_SCI_WIDTH x FONT_SCI_HEIGHT.
#define TERM_MAX_COLS (320 / FONT_SCI_WIDTH)   // 53
#define TERM_MAX_ROWS (320 / FONT_SCI_HEIGHT)  // 26

static int l_terminal_new(lua_State* L) {
    lua_Integer cols = lb_optint(L, 1, TERM_DEFAULT_COLS);
    lua_Integer rows = lb_optint(L, 2, TERM_DEFAULT_ROWS);
    lua_Integer scrollback = lb_optint(L, 3, TERM_DEFAULT_SCROLLBACK);
    if (cols < 1 || cols > TERM_MAX_COLS)
        return luaL_argerror(L, 1, lua_pushfstring(L, "cols must be 1-%d",
                                                   TERM_MAX_COLS));
    if (rows < 1 || rows > TERM_MAX_ROWS)
        return luaL_argerror(L, 2, lua_pushfstring(L, "rows must be 1-%d",
                                                   TERM_MAX_ROWS));
    // <= 0 keeps meaning "the default"; terminal_new caps the maximum.
    if (scrollback > TERM_MAX_SCROLLBACK) scrollback = TERM_MAX_SCROLLBACK;
    if (scrollback < 0) scrollback = 0;

    // The userdata first, so a failure below leaves nothing to leak: its
    // __gc ignores a NULL term.
    lua_terminal_t* t = (lua_terminal_t*)lua_newuserdatauv(L, sizeof(lua_terminal_t), 0);
    t->term = NULL;
    luaL_setmetatable(L, TERMINAL_MT);

    terminal_t* term = terminal_new((int)cols, (int)rows, (int)scrollback);
    if (!term) {
        return luaL_error(L, "failed to create terminal (out of memory)");
    }
    t->term = term;
    terminal_parser_init(&t->parser, term);

#ifdef PICODECK_SIMULATOR
    sim_set_active_terminal(term);
#endif

    return 1;
}

static int l_terminal_write(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    const char* str = luaL_checkstring(L, 2);
    
    terminal_parser_write(&t->parser, str);
    return 0;
}

static int l_terminal_clear(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    terminal_clear(t->term);
    return 0;
}

static int l_terminal_setCursor(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int x = (int)lb_checkint(L, 2);
    int y = (int)lb_checkint(L, 3);
    terminal_setCursor(t->term, x, y);
    return 0;
}

static int l_terminal_getCursor(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int x, y;
    terminal_getCursor(t->term, &x, &y);
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    return 2;
}

static int l_terminal_setColors(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    uint16_t fg = (uint16_t)luaL_checkinteger(L, 2);
    uint16_t bg = (uint16_t)luaL_checkinteger(L, 3);
    terminal_setColors(t->term, fg, bg);
    return 0;
}

static int l_terminal_getColors(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    uint16_t fg, bg;
    terminal_getColors(t->term, &fg, &bg);
    lua_pushinteger(L, fg);
    lua_pushinteger(L, bg);
    return 2;
}

static int l_terminal_scroll(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int lines = (int)lb_checkint(L, 2);
    terminal_scroll(t->term, lines);
    return 0;
}

static int l_terminal_render(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    terminal_render(t->term);
    return 0;
}

static int l_terminal_renderDirty(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    terminal_renderDirty(t->term);
    return 0;
}

static int l_terminal_getCols(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getCols(t->term));
    return 1;
}

static int l_terminal_getRows(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getRows(t->term));
    return 1;
}

static int l_terminal_setCursorVisible(lua_State* L) {
    bool visible = lua_toboolean(L, 2);
    terminal_setCursorVisible(visible);
    return 0;
}

static int l_terminal_setCursorBlink(lua_State* L) {
    bool blink = lua_toboolean(L, 2);
    terminal_setCursorBlink(blink);
    return 0;
}

static int l_terminal_setFont(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    const char* font_name = luaL_checkstring(L, 2);
    
    if (strcmp(font_name, "scientifica") == 0) {
        terminal_setFont(t->term, TERM_FONT_SCIENTIFICA);
    } else if (strcmp(font_name, "scientifica_bold") == 0) {
        terminal_setFont(t->term, TERM_FONT_SCIENTIFICA_BOLD);
    } else {
        return luaL_error(L, "unknown font: %s (use 'scientifica' or 'scientifica_bold')", font_name);
    }
    return 0;
}

static int l_terminal_getFont(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    enum terminal_font font = terminal_getFont(t->term);
    
    switch (font) {
        case TERM_FONT_SCIENTIFICA_BOLD:
            lua_pushstring(L, "scientifica_bold");
            break;
        case TERM_FONT_SCIENTIFICA:
        default:
            lua_pushstring(L, "scientifica");
            break;
    }
    return 1;
}

static int l_terminal_markAllDirty(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    terminal_markAllDirty(t->term);
    return 0;
}

static int l_terminal_isFullDirty(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushboolean(L, terminal_isFullDirty(t->term));
    return 1;
}

static int l_terminal_getDirtyRange(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int first, last;
    terminal_getDirtyRange(t->term, &first, &last);
    lua_pushinteger(L, first);
    lua_pushinteger(L, last);
    return 2;
}

static int l_terminal_getScrollbackCount(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getScrollbackCount(t->term));
    return 1;
}

static int l_terminal_getScrollbackLine(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int line = (int)lb_checkint(L, 2) - 1;
    
    int count = terminal_getScrollbackCount(t->term);
    if (line < 0 || line >= count) {
        return luaL_error(L, "scrollback line out of range");
    }
    
    uint16_t* cells = (uint16_t*)umm_malloc(t->term->cols * sizeof(uint16_t));
    if (!cells) return luaL_error(L, "out of memory");
    
    terminal_getScrollbackLine(t->term, line, cells);
    
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 0; i < t->term->cols; i++) {
        char c = cells[i] & 0xFF;
        if (c == 0) c = ' ';
        luaL_addchar(&b, c);
    }
    umm_free(cells);
    
    luaL_pushresult(&b);
    return 1;
}

static int l_terminal_getScrollbackOffset(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getScrollbackOffset(t->term));
    return 1;
}

static int l_terminal_setScrollbackOffset(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int offset = (int)lb_checkint(L, 2);
    terminal_setScrollbackOffset(t->term, offset);
    return 0;
}

// Service the system while waiting in a blocking C loop: the instruction
// hook's own pass (lua_bridge_service), so the waits cannot drift from it.
static void terminal_service_hooks(lua_State* L) {
    lua_bridge_service(L);
}

// Resolve a key name string to a BTN_* mask. Returns 0 on unknown key.
static uint32_t resolve_key_mask(const char* key_name) {
    if (strcmp(key_name, "enter") == 0)      return BTN_ENTER;
    if (strcmp(key_name, "left") == 0)       return BTN_LEFT;
    if (strcmp(key_name, "right") == 0)      return BTN_RIGHT;
    if (strcmp(key_name, "up") == 0)         return BTN_UP;
    if (strcmp(key_name, "down") == 0)       return BTN_DOWN;
    if (strcmp(key_name, "esc") == 0)        return BTN_ESC;
    if (strcmp(key_name, "f1") == 0)         return BTN_F1;
    if (strcmp(key_name, "f2") == 0)         return BTN_F2;
    if (strcmp(key_name, "f3") == 0)         return BTN_F3;
    if (strcmp(key_name, "f4") == 0)         return BTN_F4;
    if (strcmp(key_name, "f5") == 0)         return BTN_F5;
    if (strcmp(key_name, "tab") == 0)        return BTN_TAB;
    if (strcmp(key_name, "backspace") == 0)  return BTN_BACKSPACE;
    return 0;
}

// Wait for release after a key press (debounce), servicing hooks.
static void wait_for_release(lua_State* L) {
    while (kbd_get_buttons() != 0) {
        kbd_poll();
        terminal_service_hooks(L);
        sleep_ms(10);
    }
}

// term:waitForAnyKey() — blocks until any button pressed, returns nil.
// System menu, HTTP callbacks, screenshots etc. all remain responsive.
static int l_terminal_waitForAnyKey(lua_State* L) {
    while (true) {
        kbd_poll();
        terminal_service_hooks(L);
        uint32_t pressed = kbd_get_buttons();
        if (pressed != 0) {
            wait_for_release(L);
            return 0;
        }
        sleep_ms(10);
    }
}

// term:waitForKey("enter") — blocks until a specific button pressed.
// Returns the button mask integer. System stays responsive.
static int l_terminal_waitForKey(lua_State* L) {
    const char* key_name = luaL_checkstring(L, 2);

    uint32_t target_mask = resolve_key_mask(key_name);
    if (target_mask == 0)
        return luaL_error(L, "Unknown key: %s", key_name);

    while (true) {
        kbd_poll();
        terminal_service_hooks(L);
        uint32_t pressed = kbd_get_buttons();
        if (pressed & target_mask) {
            wait_for_release(L);
            lua_pushinteger(L, target_mask);
            return 1;
        }
        sleep_ms(10);
    }
}

// term:readKey() — non-blocking. Returns button mask if any key pressed, nil otherwise.
static int l_terminal_readKey(lua_State* L) {
    (void)L;
    uint32_t pressed = kbd_get_buttons_pressed();
    if (pressed != 0) {
        lua_pushinteger(L, pressed);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// term:readChar() — non-blocking. Returns a character string if a printable key
// was typed, nil otherwise. Useful for text input in terminal apps.
static int l_terminal_readChar(lua_State* L) {
    (void)L;
    char c = kbd_get_char();
    if (c != 0) {
        lua_pushlstring(L, &c, 1);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// term:waitForChar() — blocks until a printable character is typed.
// Returns the character as a string. System stays responsive.
static int l_terminal_waitForChar(lua_State* L) {
    while (true) {
        kbd_poll();
        terminal_service_hooks(L);
        char c = kbd_get_char();
        if (c != 0) {
            lua_pushlstring(L, &c, 1);
            return 1;
        }
        sleep_ms(10);
    }
}

// Line numbers bindings
static int l_terminal_setLineNumbers(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    bool enabled = lua_toboolean(L, 2);
    terminal_setLineNumbers(t->term, enabled);
    return 0;
}

static int l_terminal_setLineNumberStart(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int start = lb_checkint(L, 2);
    terminal_setLineNumberStart(t->term, start);
    return 0;
}

static int l_terminal_setLineNumberCols(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int cols = lb_checkint(L, 2);
    terminal_setLineNumberCols(t->term, cols);
    return 0;
}

static int l_terminal_setLineNumberColors(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    uint16_t fg = (uint16_t)luaL_checkinteger(L, 2);
    uint16_t bg = (uint16_t)luaL_checkinteger(L, 3);
    terminal_setLineNumberColors(t->term, fg, bg);
    return 0;
}

static int l_terminal_getContentCols(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getContentCols(t->term));
    return 1;
}

// Scrollbar bindings
static int l_terminal_setScrollbar(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    bool enabled = lua_toboolean(L, 2);
    terminal_setScrollbar(t->term, enabled);
    return 0;
}

static int l_terminal_setScrollbarColors(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    uint16_t bg = (uint16_t)luaL_checkinteger(L, 2);
    uint16_t thumb = (uint16_t)luaL_checkinteger(L, 3);
    terminal_setScrollbarColors(t->term, bg, thumb);
    return 0;
}

static int l_terminal_setScrollbarWidth(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int width = lb_checkint(L, 2);
    terminal_setScrollbarWidth(t->term, width);
    return 0;
}

static int l_terminal_setScrollInfo(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int total_lines = lb_checkint(L, 2);
    int scroll_position = lb_checkint(L, 3);
    terminal_setScrollInfo(t->term, total_lines, scroll_position);
    return 0;
}

// Render bounds
static int l_terminal_setRenderBounds(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int y_start = lb_checkint(L, 2);
    int y_end = lb_checkint(L, 3);
    terminal_setRenderBounds(t->term, y_start, y_end);
    return 0;
}

// Word wrap bindings
static int l_terminal_setWordWrap(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    bool enabled = lua_toboolean(L, 2);
    terminal_setWordWrap(t->term, enabled);
    return 0;
}

static int l_terminal_setWordWrapColumn(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int column = lb_checkint(L, 2);
    terminal_setWordWrapColumn(t->term, column);
    return 0;
}

static int l_terminal_setWrapIndicator(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    bool enabled = lua_toboolean(L, 2);
    terminal_setWrapIndicator(t->term, enabled);
    return 0;
}

static int l_terminal_getWordWrap(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushboolean(L, terminal_getWordWrap(t->term));
    return 1;
}

static int l_terminal_getVisualRowCount(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getVisualRowCount(t->term));
    return 1;
}

// Cell access (for syntax highlighting)
static int l_terminal_setCell(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int x = (int)lb_checkint(L, 2);
    int y = (int)lb_checkint(L, 3);
    const char* ch = luaL_checkstring(L, 4);
    if (x < 0 || x >= t->term->cols || y < 0 || y >= t->term->rows)
        return 0;
    terminal_setCell(t->term, x, y, (uint16_t)(unsigned char)ch[0]);
    return 0;
}

static int l_terminal_getCell(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int x = (int)lb_checkint(L, 2);
    int y = (int)lb_checkint(L, 3);
    if (x < 0 || x >= t->term->cols || y < 0 || y >= t->term->rows) {
        lua_pushlstring(L, " ", 1);
        return 1;
    }
    uint16_t cell = terminal_getCell(t->term, x, y);
    char ch = (char)(cell & 0xFF);
    if (ch == 0) ch = ' ';
    lua_pushlstring(L, &ch, 1);
    return 1;
}

static int l_terminal_setCellColors(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int x = (int)lb_checkint(L, 2);
    int y = (int)lb_checkint(L, 3);
    uint16_t fg = (uint16_t)luaL_checkinteger(L, 4);
    uint16_t bg = (uint16_t)luaL_checkinteger(L, 5);
    if (x < 0 || x >= t->term->cols || y < 0 || y >= t->term->rows)
        return 0;
    int idx = y * t->term->cols + x;
    t->term->fg_colors[idx] = fg;
    t->term->bg_colors[idx] = bg;
    t->term->row_dirty[y] = 1;
    return 0;
}

static int l_terminal_setRowColors(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int y = (int)lb_checkint(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    luaL_checktype(L, 4, LUA_TTABLE);
    int start_x = (int)lb_optint(L, 5, 0);
    int count = (int)lb_optint(L, 6, t->term->cols);

    if (y < 0 || y >= t->term->rows)
        return 0;
    if (start_x < 0) start_x = 0;
    if (start_x >= t->term->cols)
        return 0;
    if (start_x + count > t->term->cols)
        count = t->term->cols - start_x;

    for (int i = 0; i < count; i++) {
        int idx = y * t->term->cols + start_x + i;
        lua_rawgeti(L, 3, i + 1);
        lua_rawgeti(L, 4, i + 1);
        t->term->fg_colors[idx] = (uint16_t)lua_tointeger(L, -2);
        t->term->bg_colors[idx] = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 2);
    }
    t->term->row_dirty[y] = 1;
    return 0;
}

static const luaL_Reg terminal_methods[] = {
    {"write", l_terminal_write},
    {"clear", l_terminal_clear},
    {"setCursor", l_terminal_setCursor},
    {"getCursor", l_terminal_getCursor},
    {"setColors", l_terminal_setColors},
    {"getColors", l_terminal_getColors},
    {"scroll", l_terminal_scroll},
    {"render", l_terminal_render},
    {"renderDirty", l_terminal_renderDirty},
    {"getCols", l_terminal_getCols},
    {"getRows", l_terminal_getRows},
    {"setCursorVisible", l_terminal_setCursorVisible},
    {"setCursorBlink", l_terminal_setCursorBlink},
    {"setFont", l_terminal_setFont},
    {"getFont", l_terminal_getFont},
    {"markAllDirty", l_terminal_markAllDirty},
    {"isFullDirty", l_terminal_isFullDirty},
    {"getDirtyRange", l_terminal_getDirtyRange},
    {"getScrollbackCount", l_terminal_getScrollbackCount},
    {"getScrollbackLine", l_terminal_getScrollbackLine},
    {"getScrollbackOffset", l_terminal_getScrollbackOffset},
    {"setScrollbackOffset", l_terminal_setScrollbackOffset},
    {"waitForAnyKey", l_terminal_waitForAnyKey},
    {"waitForKey", l_terminal_waitForKey},
    {"readKey", l_terminal_readKey},
    {"readChar", l_terminal_readChar},
    {"waitForChar", l_terminal_waitForChar},
    // Line numbers
    {"setLineNumbers", l_terminal_setLineNumbers},
    {"setLineNumberStart", l_terminal_setLineNumberStart},
    {"setLineNumberCols", l_terminal_setLineNumberCols},
    {"setLineNumberColors", l_terminal_setLineNumberColors},
    {"getContentCols", l_terminal_getContentCols},
    // Scrollbar
    {"setScrollbar", l_terminal_setScrollbar},
    {"setScrollbarColors", l_terminal_setScrollbarColors},
    {"setScrollbarWidth", l_terminal_setScrollbarWidth},
    {"setScrollInfo", l_terminal_setScrollInfo},
    // Render bounds
    {"setRenderBounds", l_terminal_setRenderBounds},
    // Word wrap
    {"setWordWrap", l_terminal_setWordWrap},
    {"setWordWrapColumn", l_terminal_setWordWrapColumn},
    {"setWrapIndicator", l_terminal_setWrapIndicator},
    {"getWordWrap", l_terminal_getWordWrap},
    {"getVisualRowCount", l_terminal_getVisualRowCount},
    // Cell access (for syntax highlighting)
    {"setCell", l_terminal_setCell},
    {"getCell", l_terminal_getCell},
    {"setCellColors", l_terminal_setCellColors},
    {"setRowColors", l_terminal_setRowColors},
    {NULL, NULL}
};

static const luaL_Reg terminal_meta[] = {
    {"__gc", l_terminal_gc},
    {NULL, NULL}
};

static const luaL_Reg terminal_funcs[] = {
    {"new", l_terminal_new},
    {NULL, NULL}
};

void lua_bridge_terminal_init(lua_State* L) {
    lb_register_type(L, TERMINAL_MT, terminal_methods, terminal_meta);

    register_subtable(L, "terminal", terminal_funcs);
}
