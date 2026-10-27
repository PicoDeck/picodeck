// Keyboard driver stub for simulator
// Bridges SDL2 input to PicoDeck keyboard driver interface

#include "../../src/drivers/keyboard.h"
#include "../../src/drivers/kbd_event_queue.h"
#include "../../src/drivers/pad_source.h"
#include "../hal/hal_input.h"
#include "../hal/hal_timing.h"
#include "../hal/web_platform.h"  // web_yield_if_due (web build)
#include "../../src/os/os.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>

// Global exit flag
static volatile int *g_simulator_running = NULL;

void set_simulator_exit_flag(volatile int *flag) {
    g_simulator_running = flag;
}

void request_simulator_exit(void) {
    // Set the global g_running flag so the Lua debug hook sees it
    extern volatile int g_running;
    g_running = 0;
    if (g_simulator_running) {
        *g_simulator_running = 0;
    }
}

static uint32_t s_buttons = 0;
static uint32_t s_buttons_pressed = 0;
static uint32_t s_buttons_released = 0;
static char s_last_char = 0;
static uint8_t s_raw_key = 0;
static bool s_screenshot_pressed = false;
// The firmware's event queue + key-down set (kbd_event_queue.h). The HAL
// stages events as keys are typed/injected; kbd_poll moves them in here, so
// pollEvent sees them only after input.update(), as on hardware.
static kbd_input_t s_in;
// The gamepad: the firmware's masks and map, driven by the same staged
// events (kbd_pad_event), so it resolves keys exactly as the device does.
static kbd_pad_t s_pad;
static kbd_padmap_t s_padmap = KBD_PAD_DEFAULT_MAP;

// Key mapping from SDL to keyboard keycodes
static struct {
    uint32_t btn_mask;
    uint8_t keycode;
} btn_to_keycode[] = {
    {BTN_UP, KEY_UP},
    {BTN_DOWN, KEY_DOWN},
    {BTN_LEFT, KEY_LEFT},
    {BTN_RIGHT, KEY_RIGHT},
    {BTN_ENTER, KEY_ENTER},
    {BTN_ESC, KEY_ESC},
    {BTN_F1, KEY_F1},
    {BTN_F2, KEY_F2},
    {BTN_F3, KEY_F3},
    {BTN_F4, KEY_F4},
    {BTN_TAB, KEY_TAB},
    {BTN_BACKSPACE, KEY_BKSPC},
    {BTN_FN, KEY_MOD_SYM},
    {0, 0}
};

bool kbd_init(void) {
    printf("[KBD] Keyboard initialized (simulator)\n");
    return true;
}

void kbd_poll(void) {
#ifdef __EMSCRIPTEN__
    // Input-polling loops that never flush must still let key events arrive.
    web_yield_if_due();
#endif
    kbd_pad_begin_poll(&s_pad);

    // Pull pending events from the Wayland socket into SDL's internal queue.
    // Must be called before SDL_PollEvent to avoid blocking on compositor I/O.
    SDL_PumpEvents();

    // Process SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            // Handle quit - set both the running flag and dev_commands exit
            extern void request_simulator_exit(void);
            extern void dev_commands_set_exit(void);
            request_simulator_exit();
            dev_commands_set_exit();
        } else {
            hal_input_handle_event(&event);
        }
    }
    
    // Get current button state from HAL (atomic snapshot: pressed edges are
    // cleared and injected clicks auto-release as part of the same locked
    // read, so RPC-injected presses can't be dropped mid-poll)
    uint32_t new_buttons = 0, new_pressed = 0;
    hal_input_read_buttons(&new_buttons, &new_pressed);
    
    // Calculate released buttons
    s_buttons_released = s_buttons & ~new_buttons;
    
    // Update state
    s_buttons = new_buttons;
    s_buttons_pressed = new_pressed;
    
    // Check for menu key (F10 or BTN_MENU)
    if (s_buttons_pressed & BTN_MENU) {
        hal_input_raise_menu(false);
        // Hide MENU from apps, matching the hardware driver's intercept
        s_buttons &= ~BTN_MENU;
        s_buttons_pressed &= ~BTN_MENU;
    }
    
    // Check for screenshot key (F12 mapped to BTN_F9)
    if (s_buttons_pressed & BTN_F9) {
        s_screenshot_pressed = true;
    }
    
    // Staged key events, in order, into the app-facing queue and the gamepad.
    kbd_event_t ev;
    while (hal_input_pop_event(&ev)) {
        kbd_input_accept(&s_in, ev);
        kbd_pad_event(&s_pad, &s_padmap, ev);
    }

    // The other gamepad sources (the `pad` dev command, the host's game
    // controllers), as keyboard.c does; a source's Home raises the menu.
    if (pad_sources_poll(false, hal_get_time_ms()))
        hal_input_raise_menu(false);

    // Get character input
    s_last_char = hal_input_get_char();
    if (s_last_char) {
        s_raw_key = (uint8_t)s_last_char;
    } else if (s_buttons_pressed) {
        // Map button to keycode for raw key
        for (int i = 0; btn_to_keycode[i].btn_mask != 0; i++) {
            if (s_buttons_pressed & btn_to_keycode[i].btn_mask) {
                s_raw_key = btn_to_keycode[i].keycode;
                break;
            }
        }
    } else {
        s_raw_key = 0;
    }
}

// sys.sleep's poll: keep SDL (window events, quit) serviced. Key edges stay
// in the HAL until the app's next kbd_poll(), as on the device; injected
// Sym presses set the menu flag directly (kbd_inject_buttons), and a host
// F10 during a sleep takes effect at the next kbd_poll().
void kbd_poll_background(void) {
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            extern void request_simulator_exit(void);
            extern void dev_commands_set_exit(void);
            request_simulator_exit();
            dev_commands_set_exit();
        } else {
            hal_input_handle_event(&event);
        }
    }
    // A source's Home acts at once (as the device's background poll sets the
    // menu flag); its buttons wait for the app's next kbd_poll().
    if (pad_sources_poll(true, hal_get_time_ms()))
        hal_input_raise_menu(false);
}

char kbd_get_char(void) {
    char c = s_last_char;
    s_last_char = 0;
    return c;
}

uint8_t kbd_get_raw_key(void) {
    uint8_t k = s_raw_key;
    s_raw_key = 0;
    return k;
}

uint32_t kbd_get_buttons(void) {
    return s_buttons;
}

uint32_t kbd_get_buttons_pressed(void) {
    return s_buttons_pressed;
}

uint32_t kbd_get_buttons_released(void) {
    return s_buttons_released;
}

// The keyboard's aliases ORed with every other source (pad_source.h).
uint32_t kbd_get_pad(void) {
    return pad_sources_combine_held(s_pad.curr);
}

uint32_t kbd_get_pad_pressed(void) {
    return pad_sources_combine_pressed(s_pad.curr, s_pad.prev);
}

uint32_t kbd_get_pad_released(void) {
    return pad_sources_combine_released(s_pad.curr, s_pad.prev);
}

uint32_t kbd_get_pad_nav_pressed(void) {
    return pad_nav_buttons(pad_sources_pressed());
}

void kbd_set_pad_map(const kbd_padmap_t *map) {
    s_padmap = *map;
    memset(&s_pad, 0, sizeof(s_pad));
}

const kbd_padmap_t *kbd_get_pad_map(void) {
    return &s_padmap;
}

bool kbd_poll_event(kbd_event_t *out) {
    return kbd_evq_pop(&s_in.q, out);
}

bool kbd_is_key_down(uint8_t keycode) {
    return kbd_keyset_test(&s_in.down, keycode);
}

void kbd_flush_events(void) {
    kbd_evq_clear(&s_in.q);
}

// Full and not charging unless a test sets them (set_battery RPC). Written
// by the socket thread, read by Core 0.
static int s_sim_battery = 100;
static bool s_sim_charging = false;

void sim_kbd_set_battery(int percent, bool charging) {
    __atomic_store_n(&s_sim_battery, percent, __ATOMIC_RELAXED);
    __atomic_store_n(&s_sim_charging, charging, __ATOMIC_RELAXED);
}

int kbd_get_battery_percent(void) {
    return __atomic_load_n(&s_sim_battery, __ATOMIC_RELAXED);
}

bool kbd_is_charging(void) {
    return __atomic_load_n(&s_sim_charging, __ATOMIC_RELAXED);
}

void kbd_set_backlight(uint8_t brightness) {
    (void)brightness;
}

void kbd_apply_clock(void) {
    // No-op in simulator
}

void kbd_pause_bus(void) {}

void kbd_resume_bus(void) {}

void kbd_prepare_reset(void) {}

void kbd_set_poll_interval_ms(uint32_t ms) { (void)ms; }

bool kbd_consume_menu_press(void) {
    return hal_input_take_menu();
}

bool kbd_consume_screenshot_press(void) {
    if (s_screenshot_pressed) {
        s_screenshot_pressed = false;
        return true;
    }
    return false;
}

void kbd_clear_state(void) {
    s_buttons = 0;
    s_buttons_pressed = 0;
    s_buttons_released = 0;
    s_last_char = 0;
    s_raw_key = 0;
    kbd_input_clear(&s_in);
    // An injected key still down keeps its gamepad button held, without a
    // press edge; its retire/keyup event releases it (as on the device).
    memset(&s_pad, 0, sizeof(s_pad));
    s_pad.down = kbd_pad_slots_from_buttons(&s_padmap, hal_input_injected_down());
    s_pad.curr = s_pad.prev = kbd_pad_buttons_of(s_pad.down);
    pad_sources_clear();
}

void kbd_discard_pending(void) {
    hal_input_discard_pending();
    kbd_clear_state();
    pad_sources_discard();
}

void kbd_recover_i2c_bus(void) {
    // No-op in simulator
}

void kbd_inject_buttons(uint32_t buttons) {
    // Mirror the device driver: BTN_MENU is an OS-level trigger — set the
    // menu flag here and strip the bit so it never enters the HAL's 80ms
    // injected-click hold. (Pre-hold, kbd_poll's press-edge intercept caught
    // it; with the hold, the bit would leak into s_buttons on the hold
    // frames after the edge-only intercept stripped the first frame.)
    if (buttons & BTN_MENU) {
        buttons &= ~BTN_MENU;
        // One critical section notes the seq and raises the flag, and the
        // consumer clears both at once (hal_input.c).
        hal_input_raise_menu(true);
        if (!buttons) return;
    }
    // Inject through HAL only — the next kbd_poll picks them up atomically.
    // (Writing s_buttons/s_buttons_pressed directly here would double-fire
    // the pressed edge and race with kbd_poll's assignment.)
    hal_input_inject_buttons(buttons);
}

void kbd_inject_char(char c) {
    // Route through the HAL char ring so kbd_poll picks it up on the next
    // frame. Writing s_last_char directly raced with kbd_poll, which
    // overwrites s_last_char from hal_input_get_char() every frame — injected
    // chars were wiped before any app could read them.
    hal_input_inject_char(c);
}

void kbd_hold_buttons(uint32_t buttons) {
    buttons &= ~BTN_MENU;  // menu is click-only, matches hardware driver
    hal_input_hold_buttons(buttons);
}

void kbd_release_buttons(uint32_t buttons) {
    hal_input_release_buttons(buttons);
}
