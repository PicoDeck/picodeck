// HAL Input - SDL2 Implementation

#include "hal_input.h"
#include "hal_timing.h"
#include "../../src/drivers/kbd_event_queue.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

// Minimum wall-clock duration an injected one-shot ("click") press stays
// asserted before hal_input_read_buttons()/hal_input_update() auto-release
// it. This mirrors src/drivers/keyboard.c's KBD_INJECT_HOLD_MS, and for the
// exact same reason: simulator/CMakeLists.txt links THIS file (plus
// stubs/keyboard_stub.c) instead of src/drivers/keyboard.c, so the simulator
// has its own, separate injected-button state machine with the same
// "auto-release on the very next read" bug — an extra read landing between
// injection and the app's actual button sample (e.g. a watchdog-feed pump
// calling kbd_poll() more than once per frame) can retire the press before
// anything ever observes it. See keyboard.c's KBD_INJECT_HOLD_MS comment for
// the full history; this is the same fix applied to the code path every e2e
// test actually exercises.
//
// hal_get_time_ms() (SDL_GetTicks) is real wall-clock time — it is NOT
// scaled by hal_set_time_multiplier(), which only stretches/shrinks
// hal_sleep_ms()/hal_sleep_us() delays — so this hold behaves identically
// regardless of sim speed, just like the device's to_ms_since_boot().
#define HAL_INJECT_HOLD_MS 80

static uint32_t g_buttons = 0;
static uint32_t g_buttons_pressed = 0;
static uint32_t g_injected_click = 0;    // one-shot injection(s), held >= HAL_INJECT_HOLD_MS before auto-release
static uint32_t g_injected_click_since_ms = 0; // wall time g_injected_click was last (re)published
static uint32_t g_injected_click_pending = 0;  // same-button re-injection queued while still active
static uint32_t g_injected_latched = 0;  // held injections, released only by hal_input_release_buttons
static char g_char_buffer[256];
static int g_char_head = 0;
static int g_char_tail = 0;
static pthread_mutex_t s_input_mutex = PTHREAD_MUTEX_INITIALIZER;

// Injection seqs (see hal_input.h). 0 = nothing outstanding in that slot.
// Button events that are published but not yet read share one "lowest
// unread" seq; queued same-button re-injections keep their own until they
// are published; each char slot carries its injection's seq.
static uint32_t g_seq_issued = 0;
static uint32_t g_btn_unread_seq = 0;
static uint32_t g_btn_pending_seq = 0;
static uint32_t g_char_seq[256];
static uint32_t g_menu_seq = 0;

// Key events staged for kbd_poll (keyboard_stub.c), which moves them into the
// same kbd_event_queue_t the firmware driver fills — so, as on hardware, an
// app sees them only after input.update(). Written by the SDL thread and the
// RPC thread under s_input_mutex; sized generously (host memory), the app
// facing queue keeps the device's KBD_EVENT_QUEUE_LEN.
#define HAL_EVENT_STAGE_LEN 128
static kbd_event_t g_ev_stage[HAL_EVENT_STAGE_LEN];
static int g_ev_head = 0;   // next write
static int g_ev_count = 0;

// Must be called with s_input_mutex held.
static void stage_event_locked(uint8_t type, uint8_t key, char ch, uint8_t flags) {
    if (!key || kbd_key_is_os_only(key)) return;
    if (g_ev_count == HAL_EVENT_STAGE_LEN) g_ev_count--;  // drop the oldest
    kbd_event_t e = {type, key, (uint8_t)ch,
                     (uint8_t)(flags | kbd_mods_from_buttons(g_buttons))};
    g_ev_stage[g_ev_head] = e;
    g_ev_head = (g_ev_head + 1) % HAL_EVENT_STAGE_LEN;
    g_ev_count++;
}

// Down/up events for every button bit in `bits`. Call after g_buttons has
// been updated, so the mods byte reflects the new state.
static void stage_buttons_locked(uint32_t bits, uint8_t type) {
    bits &= ~BTN_MENU;  // the OS's
    for (uint32_t b = 1; b && b <= bits; b <<= 1)
        if (bits & b) stage_event_locked(type, kbd_button_to_keycode(b), 0, 0);
}

// A typed char as the device reports a tap: down, char, up.
static void stage_char_tap_locked(char c) {
    uint8_t key = (uint8_t)c;
    stage_event_locked(KBD_EV_DOWN, key, 0, 0);
    stage_event_locked(KBD_EV_CHAR, key, c, 0);
    stage_event_locked(KBD_EV_UP, key, 0, 0);
}

bool hal_input_pop_event(kbd_event_t *out) {
    pthread_mutex_lock(&s_input_mutex);
    bool ok = g_ev_count > 0;
    if (ok) {
        int tail = (g_ev_head - g_ev_count + HAL_EVENT_STAGE_LEN) % HAL_EVENT_STAGE_LEN;
        if (out) *out = g_ev_stage[tail];
        g_ev_count--;
    }
    pthread_mutex_unlock(&s_input_mutex);
    return ok;
}

static uint32_t min_nonzero(uint32_t a, uint32_t b) {
    if (!a) return b;
    if (!b) return a;
    return a < b ? a : b;
}

// Key mapping table
static struct {
    SDL_Keycode sdl_key;
    uint32_t btn_mask;
} key_mappings[] = {
    {SDLK_UP, BTN_UP},
    {SDLK_DOWN, BTN_DOWN},
    {SDLK_LEFT, BTN_LEFT},
    {SDLK_RIGHT, BTN_RIGHT},
    {SDLK_RETURN, BTN_ENTER},
    {SDLK_ESCAPE, BTN_ESC},
    {SDLK_F1, BTN_F1},
    {SDLK_F2, BTN_F2},
    {SDLK_F3, BTN_F3},
    {SDLK_F4, BTN_F4},
    {SDLK_TAB, BTN_TAB},
    {SDLK_BACKSPACE, BTN_BACKSPACE},
    {SDLK_DELETE, BTN_DEL},
    {SDLK_HOME, BTN_FN},
    {SDLK_LCTRL, BTN_CTRL},
    {SDLK_RCTRL, BTN_CTRL},
    {SDLK_LSHIFT, BTN_SHIFT},
    {SDLK_RSHIFT, BTN_SHIFT},
    {SDLK_F12, BTN_F9},  // Screenshot key
    {SDLK_F10, BTN_MENU},  // System menu
    {0, 0}
};

bool hal_input_init(void) {
    memset(g_char_buffer, 0, sizeof(g_char_buffer));
    g_char_head = 0;
    g_char_tail = 0;
    pthread_mutex_init(&s_input_mutex, NULL);
    printf("[Input] Initialized\n");
    return true;
}

void hal_input_shutdown(void) {
    pthread_mutex_destroy(&s_input_mutex);
    printf("[Input] Shutdown\n");
}

void hal_input_handle_event(const SDL_Event* event) {
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        bool pressed = (event->type == SDL_KEYDOWN);
        SDL_Keycode key = event->key.keysym.sym;
        
        pthread_mutex_lock(&s_input_mutex);
        
        // SDL auto-repeat is the desktop's HOLD: flagged, not a new press.
        uint8_t rep = event->key.repeat ? KBD_EVF_REPEAT : 0;

        // Check button mappings
        for (int i = 0; key_mappings[i].sdl_key != 0; i++) {
            if (key_mappings[i].sdl_key == key) {
                uint32_t mask = key_mappings[i].btn_mask;
                uint8_t code = kbd_button_to_keycode(mask);
                if (pressed) {
                    g_buttons |= mask;
                    g_buttons_pressed |= mask;
                    if (!(mask & BTN_MENU))
                        stage_event_locked(KBD_EV_DOWN, code, 0, rep);
                    if (mask & BTN_ENTER)
                        stage_event_locked(KBD_EV_CHAR, code, '\n', rep);
                    else if (mask & BTN_BACKSPACE)
                        stage_event_locked(KBD_EV_CHAR, code, '\b', rep);
                } else {
                    g_buttons &= ~mask;
                    if (!(mask & BTN_MENU))
                        stage_event_locked(KBD_EV_UP, code, 0, 0);
                }
                pthread_mutex_unlock(&s_input_mutex);
                return;
            }
        }
        
        // Character input
        if (key >= 32 && key < 127) {
            if (pressed) {
                int next = (g_char_head + 1) % sizeof(g_char_buffer);
                if (next != g_char_tail) {
                    g_char_buffer[g_char_head] = (char)key;
                    g_char_seq[g_char_head] = 0;
                    g_char_head = next;
                }
                stage_event_locked(KBD_EV_DOWN, (uint8_t)key, 0, rep);
                stage_event_locked(KBD_EV_CHAR, (uint8_t)key, (char)key, rep);
            } else {
                stage_event_locked(KBD_EV_UP, (uint8_t)key, 0, 0);
            }
        }
        
        pthread_mutex_unlock(&s_input_mutex);
    }
}

// Retire g_injected_click once it has been held for at least
// HAL_INJECT_HOLD_MS, and publish any queued same-button re-injection.
// Must be called with s_input_mutex already held. Shared by
// hal_input_update() and hal_input_read_buttons() so the two entry points
// (one currently unused, kept for API completeness) can't drift apart.
static void retire_and_publish_injected_click_locked(uint32_t now_ms) {
    bool retired_this_read = false;
    if (g_injected_click &&
        (now_ms - g_injected_click_since_ms >= HAL_INJECT_HOLD_MS)) {
        uint32_t retired = g_injected_click & ~g_injected_latched;
        g_buttons &= ~g_injected_click;
        g_injected_click = 0;
        retired_this_read = true;
        stage_buttons_locked(retired, KBD_EV_UP);
    }
    // Publish a queued re-injection only once active is empty AND we didn't
    // just retire it in this very call — guarantees at least one full read
    // where the button reads as released before it (or a same-button
    // re-injection received while still active) can be republished. A real
    // release-then-press edge, mirroring keyboard.c's
    // injected_retired_this_poll guard.
    if (!retired_this_read && !g_injected_click && g_injected_click_pending) {
        uint32_t fresh = g_injected_click_pending & ~g_buttons;
        g_buttons |= g_injected_click_pending;
        stage_buttons_locked(fresh, KBD_EV_DOWN);
        g_buttons_pressed |= g_injected_click_pending;
        g_injected_click = g_injected_click_pending;
        g_injected_click_pending = 0;
        g_injected_click_since_ms = now_ms;
        g_btn_unread_seq = min_nonzero(g_btn_unread_seq, g_btn_pending_seq);
        g_btn_pending_seq = 0;
    }
}

void hal_input_update(void) {
    pthread_mutex_lock(&s_input_mutex);
    // Clear pressed flags each frame (they're edge-triggered)
    g_buttons_pressed = 0;
    retire_and_publish_injected_click_locked(hal_get_time_ms());
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_read_buttons(uint32_t* out_buttons, uint32_t* out_pressed) {
    pthread_mutex_lock(&s_input_mutex);
    // Latched holds persist until an explicit hal_input_release_buttons —
    // enables modifier chords. One-shot (click) injections retire on their
    // own wall-clock hold below, not on every read.
    retire_and_publish_injected_click_locked(hal_get_time_ms());
    if (out_buttons) *out_buttons = g_buttons;
    if (out_pressed) *out_pressed = g_buttons_pressed;
    g_buttons_pressed = 0;
    g_btn_unread_seq = 0;  // everything published is now read
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_inject_buttons(uint32_t buttons) {
    pthread_mutex_lock(&s_input_mutex);
    // Retire any expired click before checking if it's still active, so an
    // expired click doesn't needlessly queue a fresh same-button injection.
    retire_and_publish_injected_click_locked(hal_get_time_ms());
    uint32_t already_active = buttons & g_injected_click;
    uint32_t fresh = buttons & ~already_active;
    uint32_t seq = ++g_seq_issued;
    if (fresh) g_btn_unread_seq = min_nonzero(g_btn_unread_seq, seq);
    if (already_active) g_btn_pending_seq = min_nonzero(g_btn_pending_seq, seq);
    if (fresh) {
        uint32_t newly_down = fresh & ~g_buttons;
        g_buttons |= fresh;
        g_buttons_pressed |= fresh;
        stage_buttons_locked(newly_down, KBD_EV_DOWN);
        g_injected_click |= fresh;
        g_injected_click_since_ms = hal_get_time_ms();
    }
    if (already_active) {
        // Same button injected again while its previous click is still
        // active — queue it instead of re-OR-ing an already-set bit (which
        // would produce no observable release/press edge at all). See
        // retire_and_publish_injected_click_locked() for the republish
        // rule. NOTE: like keyboard.c's s_injected_pending, this is a single
        // bitmask, not a per-button queue — two DIFFERENT buttons injected
        // within the same hold window merge into a momentary chord. The MCP
        // `keypress` tool's default 100ms inter-key delay is comfortably
        // above HAL_INJECT_HOLD_MS (80ms), so back-to-back sequence presses
        // never actually overlap in practice.
        g_injected_click_pending |= already_active;
    }
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_hold_buttons(uint32_t buttons) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t newly_down = buttons & ~g_buttons;
    g_buttons |= buttons;
    g_buttons_pressed |= buttons;
    g_injected_latched |= buttons;
    stage_buttons_locked(newly_down, KBD_EV_DOWN);
    g_btn_unread_seq = min_nonzero(g_btn_unread_seq, ++g_seq_issued);
    // A held button must not be auto-released by an earlier click of the
    // same key, nor resurrected later by a queued re-injection of it.
    g_injected_click &= ~buttons;
    g_injected_click_pending &= ~buttons;
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_release_buttons(uint32_t buttons) {
    pthread_mutex_lock(&s_input_mutex);
    g_btn_unread_seq = min_nonzero(g_btn_unread_seq, ++g_seq_issued);
    uint32_t going_up = buttons & g_buttons;
    g_buttons &= ~buttons;
    stage_buttons_locked(going_up, KBD_EV_UP);
    g_injected_click &= ~buttons;
    g_injected_click_pending &= ~buttons;
    g_injected_latched &= ~buttons;
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_inject_char(char c) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t seq = ++g_seq_issued;
    int next = (g_char_head + 1) % sizeof(g_char_buffer);
    if (next != g_char_tail) {
        g_char_buffer[g_char_head] = c;
        g_char_seq[g_char_head] = seq;
        g_char_head = next;
    }
    stage_char_tap_locked(c);
    pthread_mutex_unlock(&s_input_mutex);
}

uint32_t hal_input_get_buttons(void) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t result = g_buttons;
    pthread_mutex_unlock(&s_input_mutex);
    return result;
}

uint32_t hal_input_get_buttons_pressed(void) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t result = g_buttons_pressed;
    pthread_mutex_unlock(&s_input_mutex);
    return result;
}

char hal_input_get_char(void) {
    pthread_mutex_lock(&s_input_mutex);
    if (g_char_head == g_char_tail) {
        pthread_mutex_unlock(&s_input_mutex);
        return 0;
    }
    char c = g_char_buffer[g_char_tail];
    g_char_tail = (g_char_tail + 1) % sizeof(g_char_buffer);
    pthread_mutex_unlock(&s_input_mutex);
    return c;
}

bool hal_input_poll_char(char* out_char) {
    pthread_mutex_lock(&s_input_mutex);
    if (g_char_head == g_char_tail) {
        pthread_mutex_unlock(&s_input_mutex);
        return false;
    }
    if (out_char) {
        *out_char = g_char_buffer[g_char_tail];
    }
    g_char_tail = (g_char_tail + 1) % sizeof(g_char_buffer);
    pthread_mutex_unlock(&s_input_mutex);
    return true;
}

uint32_t hal_input_last_issued_seq(void) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t seq = g_seq_issued;
    pthread_mutex_unlock(&s_input_mutex);
    return seq;
}

void hal_input_get_seq_state(uint32_t *issued, uint32_t *consumed) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t lowest = min_nonzero(g_btn_unread_seq, g_btn_pending_seq);
    lowest = min_nonzero(lowest, g_menu_seq);
    for (int i = g_char_tail; i != g_char_head; i = (i + 1) % (int)sizeof(g_char_buffer)) {
        if (g_char_seq[i]) {  // the ring is FIFO: the first tracked char is the oldest
            lowest = min_nonzero(lowest, g_char_seq[i]);
            break;
        }
    }
    if (issued) *issued = g_seq_issued;
    if (consumed) *consumed = lowest ? lowest - 1 : g_seq_issued;
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_discard_pending(void) {
    pthread_mutex_lock(&s_input_mutex);
    g_char_tail = g_char_head;
    g_ev_count = 0;
    g_buttons &= ~g_injected_click;
    g_buttons_pressed = 0;
    g_injected_click = 0;
    g_injected_click_pending = 0;
    g_btn_unread_seq = 0;
    g_btn_pending_seq = 0;
    pthread_mutex_unlock(&s_input_mutex);
}

// The menu flag lives here, under the input mutex, so raising it and noting
// its seq (and taking it and clearing that seq) are each one critical section.
static bool g_menu_flag = false;

void hal_input_raise_menu(bool injected) {
    pthread_mutex_lock(&s_input_mutex);
    if (injected)
        g_menu_seq = min_nonzero(g_menu_seq, ++g_seq_issued);
    g_menu_flag = true;
    pthread_mutex_unlock(&s_input_mutex);
}

bool hal_input_take_menu(void) {
    pthread_mutex_lock(&s_input_mutex);
    bool was = g_menu_flag;
    if (was) {
        g_menu_flag = false;
        g_menu_seq = 0;
    }
    pthread_mutex_unlock(&s_input_mutex);
    return was;
}
