#include "text_input.h"
#include "../dev_commands.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "os.h"

#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include <string.h>

// ── Visual constants (match system_menu.c) ────────────────────────────────────

#define PANEL_W    260
#define TITLE_H     16   // title bar height (px)
#define ITEM_H      13   // row height (px): 8px font + 5px padding
#define FOOTER_H    12   // footer hint bar height (px)

#define C_PANEL_BG  RGB565(20,  28,  50)
#define C_TITLE_BG  RGB565(10,  14,  30)
#define C_BORDER    RGB565(80, 100, 150)
#define C_INPUT_BG  RGB565( 5,  10,  20)

// Panel height: border(1) + title(16) + divider(1) + prompt(13) + input(13)
//             + divider(1) + footer(12) + border(1) = 58
#define PANEL_H  (32 + 2 * ITEM_H)

// Maximum visible characters: text area width divided by font char width (6px)
#define MAX_VIS  ((PANEL_W - 16) / 6)

bool text_input_show(const char *title, const char *prompt,
                     const char *initial, char *out, int out_len)
{
    char buf[256];
    buf[0] = '\0';
    if (initial && initial[0]) {
        strncpy(buf, initial, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    }
    int len = (int)strlen(buf);

    int px = (FB_WIDTH  - PANEL_W) / 2;
    int py = (FB_HEIGHT - PANEL_H) / 2;

    display_darken();

    // OS modals draw full-screen even if the app set a clip rect.
    int saved_cx, saved_cy, saved_cw, saved_ch;
    display_get_clip_rect(&saved_cx, &saved_cy, &saved_cw, &saved_ch);
    display_clear_clip_rect();

    // Start from a clean keyboard: edges gathered by sys.sleep's background
    // polls, or injections queued before this modal opened, were not typed
    // at this dialog and must not accept/cancel it.
    kbd_clear_state();
    kbd_consume_menu_press();  // a stale press was not made here

    int  scroll      = 0;
    bool running     = true;
    bool confirmed   = false;
    bool need_redraw = true;

    while (running) {
        if (need_redraw) {
            // Outer border
            display_draw_rect(px, py, PANEL_W, PANEL_H, C_BORDER);

            // Title bar
            display_fill_rect(px + 1, py + 1, PANEL_W - 2, TITLE_H, C_TITLE_BG);
            int tw = display_text_width(title);
            display_draw_text(px + (PANEL_W - tw) / 2, py + 5,
                              title, COLOR_WHITE, C_TITLE_BG);

            // Divider after title
            int div1_y = py + 1 + TITLE_H;
            display_fill_rect(px + 1, div1_y, PANEL_W - 2, 1, C_BORDER);

            // Prompt row
            int prompt_y = div1_y + 1;
            display_fill_rect(px + 1, prompt_y, PANEL_W - 2, ITEM_H, C_PANEL_BG);
            display_draw_text(px + 8, prompt_y + 2, prompt, COLOR_GRAY, C_PANEL_BG);

            // Input field
            int input_y = prompt_y + ITEM_H;
            display_fill_rect(px + 1, input_y, PANEL_W - 2, ITEM_H, C_INPUT_BG);

            // Adjust scroll so cursor stays visible
            if (len - scroll >= MAX_VIS) scroll = len - MAX_VIS + 1;
            if (scroll < 0) scroll = 0;

            // Visible text slice
            int vis = len - scroll;
            if (vis < 0)       vis = 0;
            if (vis > MAX_VIS) vis = MAX_VIS;

            char visible[MAX_VIS + 1];
            memcpy(visible, buf + scroll, vis);
            visible[vis] = '\0';

            display_draw_text(px + 8, input_y + 2, visible, COLOR_WHITE, C_INPUT_BG);

            // Cursor bar
            int cursor_x = px + 8 + vis * 6;
            if (cursor_x < px + PANEL_W - 4)
                display_fill_rect(cursor_x, input_y + 2, 2, 8, COLOR_WHITE);

            // Divider before footer
            int div2_y = input_y + ITEM_H;
            display_fill_rect(px + 1, div2_y, PANEL_W - 2, 1, C_BORDER);

            // Footer
            int footer_y = div2_y + 1;
            display_fill_rect(px + 1, footer_y, PANEL_W - 2, FOOTER_H, C_TITLE_BG);
            display_draw_text(px + 4, footer_y + 2,
                              "Enter:confirm  Esc:cancel", COLOR_GRAY, C_TITLE_BG);

            display_flush();
            need_redraw = false;
        }

        kbd_poll();
        dev_commands_poll();
        dev_commands_process();
        // Let a dev "exit" unwind this modal (cancel; the Lua hook handles exit).
        if (dev_commands_wants_exit()) {
            running = false;
            continue;
        }

        // The menu key cancels, like Esc, and takes the latch.
        if (kbd_consume_menu_press()) {
            running = false;
            continue;
        }

        char     ch      = kbd_get_char();
        uint32_t pressed = kbd_get_buttons_pressed();

        if (pressed & BTN_ENTER) {
            confirmed = true;
            running   = false;
        } else if (pressed & BTN_ESC) {
            running = false;
        } else if (pressed & BTN_BACKSPACE) {
            if (len > 0) {
                buf[--len] = '\0';
                need_redraw = true;
            }
        } else if (ch >= 0x20 && ch < 0x7F) {
            int max_buf = out_len - 1 < (int)sizeof(buf) - 1
                          ? out_len - 1 : (int)sizeof(buf) - 1;
            if (len < max_buf) {
                buf[len++] = ch;
                buf[len]   = '\0';
                need_redraw = true;
            }
        }

        watchdog_update();
        sleep_ms(16);
    }

    if (confirmed) {
        strncpy(out, buf, out_len - 1);
        out[out_len - 1] = '\0';
    }

    display_set_clip_rect(saved_cx, saved_cy, saved_cw, saved_ch);
    return confirmed;
}
