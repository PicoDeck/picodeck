#include "tz_picker.h"
#include "config.h"
#include "os.h"
#include "../dev_commands.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"

#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

// ── Timezone table ─────────────────────────────────────────────────────────────

typedef struct { const char *label; int offset_min; } tz_entry_t;

static const tz_entry_t s_zones[] = {
    { "UTC-12 (Baker Island)",          -720 },
    { "UTC-11 (Samoa/Niue)",            -660 },
    { "UTC-10 (Hawaii)",                -600 },
    { "UTC-9  (Alaska)",                -540 },
    { "UTC-8  (Pacific US/Canada)",     -480 },
    { "UTC-7  (Mountain US/Canada)",    -420 },
    { "UTC-6  (Central US/Canada)",     -360 },
    { "UTC-5  (Eastern US/Canada)",     -300 },
    { "UTC-4  (Atlantic/Venezuela)",    -240 },
    { "UTC-3  (Brazil/Argentina)",      -180 },
    { "UTC-2  (Mid-Atlantic)",          -120 },
    { "UTC-1  (Azores/Cape Verde)",      -60 },
    { "UTC+0  (London/Dublin/UTC)",        0 },
    { "UTC+1  (Paris/Berlin/Rome)",       60 },
    { "UTC+2  (Athens/Cairo/Helsinki)",  120 },
    { "UTC+3  (Moscow/Nairobi/Riyadh)", 180 },
    { "UTC+3:30 (Tehran)",              210 },
    { "UTC+4  (Dubai/Baku)",            240 },
    { "UTC+4:30 (Kabul)",               270 },
    { "UTC+5  (Karachi/Tashkent)",      300 },
    { "UTC+5:30 (India/Sri Lanka)",     330 },
    { "UTC+5:45 (Nepal)",               345 },
    { "UTC+6  (Dhaka/Almaty)",          360 },
    { "UTC+6:30 (Yangon)",              390 },
    { "UTC+7  (Bangkok/Jakarta)",       420 },
    { "UTC+8  (Beijing/Singapore)",     480 },
    { "UTC+8:45 (Eucla)",               525 },
    { "UTC+9  (Tokyo/Seoul)",           540 },
    { "UTC+9:30 (Darwin/Adelaide)",     570 },
    { "UTC+10 (Sydney/Brisbane)",       600 },
    { "UTC+10:30 (Lord Howe)",          630 },
    { "UTC+11 (Solomon Islands)",       660 },
    { "UTC+12 (Auckland/Fiji)",         720 },
    { "UTC+13 (Samoa DST)",             780 },
    { "UTC+14 (Kiribati)",              840 },
};

#define ZONE_COUNT ((int)(sizeof(s_zones) / sizeof(s_zones[0])))

// ── Visual constants (match system_menu.c) ────────────────────────────────────

#define TZ_PANEL_W   300
#define TZ_TITLE_H    16
#define TZ_ITEM_H     13
#define TZ_FOOTER_H   12
#define TZ_VISIBLE     8

// border(1) + title(16) + divider(1) + search(13) + divider(1)
// + items(8*13=104) + divider(1) + footer(12) + border(1) = 150
#define TZ_PANEL_H  (1 + TZ_TITLE_H + 1 + TZ_ITEM_H + 1 + \
                     TZ_VISIBLE * TZ_ITEM_H + 1 + TZ_FOOTER_H + 1)

#define C_PANEL_BG  RGB565(20,  28,  50)
#define C_TITLE_BG  RGB565(10,  14,  30)
#define C_SEL_BG    RGB565(40,  80, 160)
#define C_BORDER    RGB565(80, 100, 150)

// ── Case-insensitive substring match ─────────────────────────────────────────

static bool str_contains_ci(const char *haystack, const char *needle) {
    if (!needle[0]) return true;
    int hlen = (int)strlen(haystack);
    int nlen = (int)strlen(needle);
    for (int i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (int j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// ── Filter rebuild ────────────────────────────────────────────────────────────

static int rebuild_filter(const char *search, int *filtered) {
    int count = 0;
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (str_contains_ci(s_zones[i].label, search))
            filtered[count++] = i;
    }
    return count;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool tz_picker_show(void) {
    int px = (FB_WIDTH  - TZ_PANEL_W) / 2;
    int py = (FB_HEIGHT - TZ_PANEL_H) / 2;

    char search[32];
    search[0] = '\0';
    int  search_len = 0;

    int filtered[ZONE_COUNT];
    int fcount = rebuild_filter(search, filtered);

    int  sel         = 0;
    int  scroll      = 0;
    bool running     = true;
    bool need_redraw = true;
    bool changed     = false;

    display_darken();

    // Start from a clean keyboard: edges gathered by sys.sleep's background
    // polls, or injections queued before this modal opened, were not typed
    // at this picker and must not filter or select on its behalf.
    kbd_clear_state();

    while (running) {
        if (need_redraw) {
            // Outer border
            display_draw_rect(px, py, TZ_PANEL_W, TZ_PANEL_H, C_BORDER);

            // Title bar
            display_fill_rect(px + 1, py + 1, TZ_PANEL_W - 2, TZ_TITLE_H, C_TITLE_BG);
            const char *title = "Select Timezone";
            int tw = display_text_width(title);
            display_draw_text(px + (TZ_PANEL_W - tw) / 2, py + 5,
                              title, COLOR_WHITE, C_TITLE_BG);

            // Divider after title
            int div1_y = py + 1 + TZ_TITLE_H;
            display_fill_rect(px + 1, div1_y, TZ_PANEL_W - 2, 1, C_BORDER);

            // Search row
            int search_y = div1_y + 1;
            display_fill_rect(px + 1, search_y, TZ_PANEL_W - 2, TZ_ITEM_H, C_PANEL_BG);
            char disp[36];
            snprintf(disp, sizeof(disp), "/ %s_", search);
            display_draw_text(px + 4, search_y + 2, disp, COLOR_YELLOW, C_PANEL_BG);

            // Divider after search
            int div2_y = search_y + TZ_ITEM_H;
            display_fill_rect(px + 1, div2_y, TZ_PANEL_W - 2, 1, C_BORDER);

            // List items
            int items_y = div2_y + 1;
            for (int i = 0; i < TZ_VISIBLE; i++) {
                int idx = i + scroll;
                int iy  = items_y + i * TZ_ITEM_H;
                bool sel_row = (idx == sel);
                uint16_t bg = (idx < fcount && sel_row) ? C_SEL_BG : C_PANEL_BG;
                display_fill_rect(px + 1, iy, TZ_PANEL_W - 2, TZ_ITEM_H, bg);
                if (idx < fcount) {
                    display_draw_text(px + 4,  iy + 2, sel_row ? ">" : " ",
                                      COLOR_WHITE, bg);
                    display_draw_text(px + 10, iy + 2, s_zones[filtered[idx]].label,
                                      COLOR_WHITE, bg);
                }
            }

            // Divider before footer
            int fdiv_y = items_y + TZ_VISIBLE * TZ_ITEM_H;
            display_fill_rect(px + 1, fdiv_y, TZ_PANEL_W - 2, 1, C_BORDER);

            // Footer
            int footer_y = fdiv_y + 1;
            display_fill_rect(px + 1, footer_y, TZ_PANEL_W - 2, TZ_FOOTER_H, C_TITLE_BG);
            display_draw_text(px + 4, footer_y + 2,
                              "Enter:select  Esc:cancel", COLOR_GRAY, C_TITLE_BG);

            display_flush();
            need_redraw = false;
        }

        kbd_poll();

        // Pump dev commands (keypress/screenshot/exit) so they don't stall
        // for the entire time this modal is open.
        dev_commands_poll();
        dev_commands_process();
        // Let a dev "exit" unwind this modal (cancel; the Lua hook handles exit).
        if (dev_commands_wants_exit())
            return false;
        // The menu key cancels, like Esc, and takes the latch.
        if (kbd_consume_menu_press())
            return false;

        char     ch      = kbd_get_char();
        uint32_t pressed = kbd_get_buttons_pressed();

        // Typing: append printable chars to search, backspace removes last
        if (pressed & BTN_BACKSPACE) {
            if (search_len > 0) {
                search[--search_len] = '\0';
                fcount = rebuild_filter(search, filtered);
                sel = 0; scroll = 0;
                need_redraw = true;
            }
        } else if (ch >= 0x20 && ch < 0x7F) {
            if (search_len < (int)sizeof(search) - 1) {
                search[search_len++] = ch;
                search[search_len]   = '\0';
                fcount = rebuild_filter(search, filtered);
                sel = 0; scroll = 0;
                need_redraw = true;
            }
        }

        // Navigation
        if ((pressed & BTN_UP) && sel > 0) {
            sel--;
            if (sel < scroll) scroll = sel;
            need_redraw = true;
        }
        if ((pressed & BTN_DOWN) && sel < fcount - 1) {
            sel++;
            if (sel >= scroll + TZ_VISIBLE) scroll = sel - TZ_VISIBLE + 1;
            need_redraw = true;
        }

        // Confirm selection
        if ((pressed & BTN_ENTER) && fcount > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", s_zones[filtered[sel]].offset_min);
            config_set("tz_offset", buf);
            config_save();
            changed = true;
            running = false;
        }

        if (pressed & BTN_ESC) running = false;

        watchdog_update();
        sleep_ms(16);
    }

    return changed;
}
