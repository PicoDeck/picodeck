// Keyboard driver stub for simulator
// Bridges SDL2 input to PicoDeck keyboard driver interface

#include "../../src/drivers/keyboard.h"
#include "../../src/drivers/kbd_event_queue.h"
#include "../../src/drivers/pad_source.h"
#include "../hal/hal_input.h"
#include "../hal/hal_timing.h"
#include "../hal/web_platform.h"  // web_yield_if_due (web build)
#include "../../src/os/idle_dim.h"
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
static char s_injected_char = 0;  // until getChar reads it, as keyboard.c's s_inj.ch
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
// Keys down when the system menu opened or closed, hidden until released
// (kbd_event_queue.h, "Hidden keys"). The HAL's staged events go through the
// same filter as the device's FIFO items; button masks are masked by it below.
static kbd_hide_t s_hide;
static uint32_t s_poll_count; // foreground polls (kbd_poll_count)

bool kbd_init(void) {
    printf("[KBD] Keyboard initialized (simulator)\n");
    return true;
}

// The buttons of the hidden keys, to take out of the HAL's masks. The HAL knows
// their level, so one that is up ends here too: a discard drops staged "up"
// events, so this is what the device's quiet timer is for its FIFO.
static uint32_t hide_buttons(uint32_t level) {
    uint32_t hidden = 0;
    for (int i = s_hide.count - 1; i >= 0; i--) {
        uint32_t btn = kbd_keycode_to_button(s_hide.key[i]);
        if (btn & level)
            hidden |= btn;
        else if (btn)
            kbd_hide_remove_at(&s_hide, i);
    }
    return hidden;
}

void kbd_poll(void) {
    s_poll_count++;
#ifdef __EMSCRIPTEN__
    // Input-polling loops that never flush must still let key events arrive.
    web_yield_if_due();
#endif
    kbd_pad_begin_poll(&s_pad);
    kbd_pad_t pad_before = s_pad;
    kbd_keyset_t down_before = s_in.down;
    s_in.ev_pushed = 0;

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

    // Staged key events, in order, into the app-facing queue and the gamepad.
    // First, since an "up" ends a hidden key: one released and pressed again
    // since the last poll reads as the press it is.
    //
    // The HAL's press edges and chars are not in order with these events, and a
    // repeat makes both (an arrow's edge, Enter's or a letter's char). So a
    // repeat and the release that ends the hiding can land in one poll (always
    // possible, as sys.sleep's background polls gather them for the next
    // kbd_poll) and the repeat's edge must still go: of the keys hidden when
    // this poll began, only a down that passes the filter is a press. Its char
    // goes as its event does. The device has none of this: kbd_hide_item drops
    // the item before anything is made of it.
    uint32_t hidden_before = hide_buttons(UINT32_MAX);
    uint32_t fresh = 0;
    kbd_event_t ev;
    s_raw_key = 0;  // getRawKey: the last key to go down or repeat in this poll
    while (hal_input_pop_event(&ev)) {
        if (kbd_hide_event(&s_hide, &s_pad, &s_padmap, ev)) {
            if (ev.type == KBD_EV_CHAR)
                hal_input_drop_host_char((char)ev.ch);
            continue;
        }
        kbd_input_accept(&s_in, ev);
        kbd_pad_event(&s_pad, &s_padmap, ev);
        if (ev.type == KBD_EV_DOWN) {
            s_raw_key = ev.key;
            fresh |= kbd_keycode_to_button(ev.key);
        }
    }
    new_buttons &= ~hide_buttons(new_buttons);
    new_pressed &= ~(hidden_before & ~fresh);

    // Calculate released buttons
    s_buttons_released = s_buttons & ~new_buttons;
    
    // Update state
    s_buttons = new_buttons;
    s_buttons_pressed = new_pressed;
    
    // Check for menu key (F10 or BTN_MENU)
    bool menu_key = (s_buttons_pressed & BTN_MENU) != 0;
    if (menu_key) {
        hal_input_raise_menu(false);
        // Hide MENU from apps, matching the hardware driver's intercept
        s_buttons &= ~BTN_MENU;
        s_buttons_pressed &= ~BTN_MENU;
    }
    
    // Check for screenshot key (F12 mapped to BTN_F9)
    if (s_buttons_pressed & BTN_F9) {
        s_screenshot_pressed = true;
    }
    
    // The other gamepad sources (the `pad` dev command, the host's game
    // controllers), as keyboard.c does; a source's Home raises the menu.
    bool pad_home = pad_sources_poll(false, hal_get_time_ms());
    if (pad_home)
        hal_input_raise_menu(false);

    // Get character input (a hidden key's chars have left the queue already)
    bool injected;
    s_last_char = hal_input_get_char_ex(&injected);
    if (injected) {
        s_injected_char = s_last_char;
        s_last_char = 0;
    }

    // Idle screen dimming, as keyboard.c's kbd_poll_impl: fresh input is
    // activity, and the input that wakes a dimmed screen never reaches the
    // app. An injected char is not activity there either: kbd_inject_char
    // queues it outside the poll. Off unless --idle-dim (main.c): with a
    // timeout of 0 the screen never dims, so nothing is ever swallowed.
    bool activity = s_buttons_pressed || menu_key || hal_input_take_host_key() ||
                    pad_sources_fresh() || pad_home;
    if (activity && idle_dim_note_activity()) {
        s_buttons &= ~s_buttons_pressed;
        s_buttons_pressed = 0;
        kbd_pad_swallow(&s_pad, false, pad_before);
        pad_sources_swallow();
        // This poll's presses and chars; a key the app saw go down still
        // comes up.
        kbd_evq_drop_newest_keep_ups(&s_in.q, s_in.ev_pushed, &down_before);
        kbd_hide_swallowed(&s_hide, &s_pad, &pad_before, &s_in.down, &down_before,
                           hal_input_injected_down());
        for (int i = 0; i < 8; i++)
            s_in.down.bits[i] &= down_before.bits[i];
        // The waking click is retired for good (kbd_inject_drop_oneshots):
        // its hold would otherwise read as held on the next polls.
        hal_input_discard_pending();
        s_last_char = 0;
        s_raw_key = 0;
    }
    idle_dim_poll();
}

// sys.sleep's poll: keep SDL (window events, quit) serviced. Key edges stay
// in the HAL until the app's next kbd_poll(), as on the device; injected
// Menu key presses set the menu flag directly (kbd_inject_buttons), and a host
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
    bool pad_home = pad_sources_poll(true, hal_get_time_ms());
    if (pad_home)
        hal_input_raise_menu(false);
    // Idle dim runs here too, so an app that only sleeps still dims (keys
    // count at the next kbd_poll(), where they are decoded here).
    if ((pad_sources_fresh() || pad_home) && idle_dim_note_activity())
        pad_sources_swallow();
    idle_dim_poll();
}

// As in keyboard.c: both keep their value until the next kbd_poll, so a loop
// that reads until nil never ends there and must not end here. Only an injected
// char is read once.
char kbd_get_char(void) {
    char c = s_last_char ? s_last_char : s_injected_char;
    s_injected_char = 0;
    return c;
}

uint8_t kbd_get_raw_key(void) {
    return s_raw_key;
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

float kbd_get_pad_axis(int axis) {
    return pad_sources_axis(axis, kbd_get_pad());
}

uint32_t kbd_get_pad_nav_pressed(void) {
    return pad_nav_buttons(pad_sources_pressed());
}

uint32_t kbd_poll_count(void) {
    return s_poll_count;
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

// What the panel's backlight was last set to (nothing draws with it): the
// get_backlight RPC. Written by Core 0, read by the socket thread.
static int s_sim_backlight = 0;

int sim_kbd_get_backlight(void) {
    return __atomic_load_n(&s_sim_backlight, __ATOMIC_RELAXED);
}

void kbd_set_backlight(uint8_t brightness) {
    __atomic_store_n(&s_sim_backlight, brightness, __ATOMIC_RELAXED);
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
    s_injected_char = 0;
    s_raw_key = 0;
    kbd_input_clear(&s_in);
    // An injected key still down keeps its gamepad button held, without a
    // press edge; its retire/keyup event releases it (as on the device).
    memset(&s_pad, 0, sizeof(s_pad));
    uint32_t down = hal_input_injected_down() & ~hide_buttons(UINT32_MAX);
    s_pad.down = kbd_pad_slots_from_buttons(&s_padmap, down);
    s_pad.curr = s_pad.prev = kbd_pad_buttons_of(s_pad.down);
    pad_sources_clear();
}

void kbd_discard_pending(void) {
    hal_input_discard_pending();
    // Its staged "up" may have gone with the rest, and a key with no button
    // has no level to tell (hide_buttons has the others).
    for (int i = s_hide.count - 1; i >= 0; i--)
        if (!kbd_keycode_to_button(s_hide.key[i]))
            kbd_hide_remove_at(&s_hide, i);
    kbd_clear_state();
    pad_sources_discard();
}

void kbd_hide_held(void) {
    kbd_hide_add_down(&s_hide, &s_in.down, 0);
    uint32_t down = hal_input_read_down();
    for (uint32_t b = 1; b && b <= down; b <<= 1)
        if (down & b)
            kbd_hide_add(&s_hide, kbd_button_to_keycode(b));
    // Chars of theirs not read yet go too, as the device's clear drops its
    // backlog (the other keys' stay, as across any clear here).
    for (int i = 0; i < s_hide.count; i++)
        while (hal_input_drop_host_char((char)s_hide.key[i]))
            ;
    kbd_clear_state();
    pad_sources_hide_held();
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
