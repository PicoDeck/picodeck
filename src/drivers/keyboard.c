#include "keyboard.h"
#include "kbd_event_queue.h"
#include "kbd_i2c.h"
#include "kbd_bus.h"
#include "../hardware.h"
#include "../os/idle_dim.h"
#include "../os/os.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

_Static_assert(KBD_BUS_FIFO_IDLE == KBD_FIFO_IDLE, "kbd_bus.h idle state");

// The STM32 uses a STOP-based protocol (not repeated-start):
//   1. Write register address as a complete transaction (nostop=false)
//   2. Wait for the STM32 to prepare its response
//   3. Read in a separate transaction
// Only kbd_init()'s boot probe still does this blocking dance (everything
// after it runs through the bus engine, kbd_i2c.c). pelrun/uf2loader used
// sleep_ms(16); 1ms is reliable and keeps the probe's ~5s budget short.
#define KBD_REG_DELAY_MS 1
#define KBD_I2C_TIMEOUT_US 5000  // 5ms per probe transfer (a 1-2 byte
                                 // transfer takes 2-3 ms at 10 kHz); used only
                                 // by kbd_init()'s boot probe (50ms once caused
                                 // ~150ms stalls per probe step on failure)

// At most this many FIFO items are decoded per poll, so one poll's worth of
// events always fits the event queue (see kbd_poll_impl); the rest wait in
// the bus engine's ring, and once that fills, in the STM32 FIFO itself.
#define KBD_POLL_MAX_ITEMS 8
_Static_assert(KBD_POLL_MAX_ITEMS * 2 <= KBD_EVENT_QUEUE_LEN,
              "one poll's events must fit the queue");

// Minimum wall-clock duration an injected one-shot button stays "active"
// before kbd_poll() auto-releases it. Some apps call kbd_poll() more than
// once per logical frame (watchdog-feed pumps), so a poll-count-based hold
// (e.g. "released on the next poll") isn't safe — the extra polls can retire
// the press before the app ever samples it. A wall-time hold makes delivery
// independent of poll cadence, like a real human keypress. Must stay below
// the MCP `keypress` tool's default 100ms inter-key delay so back-to-back
// injected presses of different buttons don't get delayed into merging.
// NOTE: The simulator carries an independent parallel implementation in
// simulator/hal/hal_input.c (HAL_INJECT_HOLD_MS) — changes here must be mirrored there.
#define KBD_INJECT_HOLD_MS 80

// ── Internal state
// ────────────────────────────────────────────────────────────

// Button masks (held / previous poll / tap bookkeeping, see kbd_event_queue.h)
static kbd_buttons_t s_btn;
// Event queue, key-down and unseen-press sets and getChar backlog. 140 bytes
// of SRAM: 16 four-byte events, two 256-bit key sets, 4 backlog chars and
// counters.
static kbd_input_t s_in;
static char s_last_char = 0;
static uint8_t s_last_raw_key =
    0; // raw keycode of last press this frame (0 = none)
static bool s_menu_pressed =
    false; // set on BTN_MENU rising edge; cleared by kbd_consume_menu_press()
static bool s_screenshot_pressed =
    false; // set on KEY_BRK press; cleared by kbd_consume_screenshot_press()

// Injected input (dev commands). Injection happens asynchronously — the
// dev-command pump runs from the Lua debug hook, at an arbitrary point in the
// app's frame — so a one-shot press must stay *pending* until kbd_poll()
// publishes it, and then stay *active* for a minimum wall-clock hold
// (KBD_INJECT_HOLD_MS) rather than for just one poll call. (The original
// scheme cleared the injection at the top of every kbd_poll, so the app's
// own input.update() usually wiped it before the app read the button state;
// a later fix held it for exactly one poll-to-poll cycle, but extra
// kbd_poll() calls within that cycle — e.g. watchdog-feed pumps added since
// — could still retire it early. See KBD_INJECT_HOLD_MS.)
// The bookkeeping (pending / active / held / char) and its arithmetic are in
// kbd_event_queue.h (kbd_inject_*), host-tested in test_kbd_event_queue.c.
static kbd_inject_t s_inj;

// ── Public API
// ────────────────────────────────────────────────────────────────

// Clear a stuck bus (9 clocks + STOP) and re-init I2C1 at KBD_I2C_BAUD; the
// bus engine is paused around it (kbd_i2c_recover). Safe during kbd_init().
void kbd_recover_i2c_bus(void) { kbd_i2c_recover(); }

bool kbd_init(void) {
  // ── Step 1: Unconditional bus clear ───────────────────────────────────────
  // Sample SDA before recovery so we can log whether the bus was already stuck.
  gpio_init(KBD_PIN_SDA);
  gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
  gpio_pull_up(KBD_PIN_SDA);
  sleep_us(200);

  bool sda_stuck = !gpio_get(KBD_PIN_SDA);
  printf("[KBD] SDA=GP%d before init: %s\n", KBD_PIN_SDA,
         sda_stuck ? "LOW (bus stuck)" : "HIGH (idle)");

  // ── Step 2: Run 9-clock recovery + I2C re-init ────────────────────────────
  kbd_recover_i2c_bus();

  // ── Step 3: Wait for STM32 keyboard scanning to start ─────────────────────
  // The STM32's I2C peripheral starts ~100ms after power-on, but its keyboard
  // FIFO scanning doesn't start until ~2.5s from power-on. If we poll before
  // scanning is active, I2C ACKs but the FIFO is always empty (keys don't
  // work). From observation, 2.5s from RP2350 boot is reliable.
  {
    uint32_t boot_ms = to_ms_since_boot(get_absolute_time());
    if (boot_ms < 2500) {
      printf("[KBD] boot=%lums — waiting %lums for STM32 keyboard scanning\n",
             (unsigned long)boot_ms, (unsigned long)(2500 - boot_ms));
      sleep_ms(2500 - boot_ms);
    }
  }

  // ── Step 4: Poll for STM32 presence (up to 5 seconds) ───────────────────
  printf("[KBD] polling 0x%02X on I2C%d at %dkHz...\n", KBD_I2C_ADDR,
         KBD_I2C_PORT == i2c0 ? 0 : 1, KBD_I2C_BAUD / 1000);

  uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  bool ok = false;
  uint8_t ver = 0;

  for (int poll = 0; poll < 50; poll++) {
    sleep_ms(100);

    uint8_t reg = 0x01;
    int wret = i2c_write_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR, &reg, 1, false,
                                    KBD_I2C_TIMEOUT_US);
    uint32_t t = to_ms_since_boot(get_absolute_time()) - start_ms;

    if (wret == 1) {
      sleep_ms(KBD_REG_DELAY_MS);
      int rret = i2c_read_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR, &ver, 1, false,
                                     KBD_I2C_TIMEOUT_US);
      printf("[KBD] t+%lums: write OK, read ret=%d ver=0x%02X\n",
             (unsigned long)t, rret, ver);
      if (rret == 1) {
        ok = true;
        break;
      }
    } else {
      if (poll == 0 || poll % 10 == 9)
        printf("[KBD] t+%lums: NACK (ret=%d)\n", (unsigned long)t, wret);
    }
  }

  if (ok) {
    printf("[KBD] init OK — I2C%d SDA=GP%d SCL=GP%d fw=0x%02X\n",
           KBD_I2C_PORT == i2c0 ? 0 : 1, KBD_PIN_SDA, KBD_PIN_SCL, ver);
  } else {
    printf("[KBD] FAILED — STM32 never responded in 5s\n");
  }

  // From here on every STM32 transaction goes through the asynchronous bus
  // engine; it retries (and backs off) if the controller did not answer.
  kbd_i2c_start();

  return ok;
}

// bg: a background poll (kbd_poll_background) that reads the STM32 FIFO into
// the event queue, the char backlog and the button masks WITHOUT starting a
// new app-facing poll (kbd_poll_begin in kbd_event_queue.h): taps, deferred
// releases and the raw key accumulate against the curr the app last saw,
// the char the app has not read yet stays, and injected one-shots are
// neither retired nor published. The app's next kbd_poll() then delivers
// the press/release edges, chars and raw key of what arrived meanwhile;
// only the OS-level Sym and Brk flags are set right away.
static void kbd_poll_impl(bool bg);

void kbd_poll(void) { kbd_poll_impl(false); }

void kbd_poll_background(void) { kbd_poll_impl(true); }

static void kbd_poll_impl(bool bg) {
  bool bg_run = bg || s_btn.in_bg;  // this poll is part of a background run
  kbd_poll_begin(&s_btn, bg, &s_last_raw_key);
  uint32_t curr_before = s_btn.curr, prev_before = s_btn.prev;
  uint8_t raw_before = s_last_raw_key;
  if (!bg)
    s_last_char = 0;
  bool new_key = false;  // a press/repeat or char decoded in THIS poll
  s_in.ev_pushed = 0;
  s_in.char_pushed = 0;
  kbd_keyset_t down_before = s_in.down;

  uint32_t now_ms = to_ms_since_boot(get_absolute_time());

  // Retire the previous one-shot injection (once it has been held for at
  // least KBD_INJECT_HOLD_MS) and publish any pending one. Folding injected
  // buttons into s_btn.curr (rather than OR-ing them in the getters)
  // gives them real press AND release edges.
  //
  // Retiring on wall time rather than "the next poll" is the fix: apps now
  // call kbd_poll() more than once per logical frame in places (the C-Dogs
  // SDL_Delay pump, the blit-path pump, picodeck_asset_load_tick — all added to
  // feed the watchdog during long-running work). Any such extra poll landing
  // between publish and the app's actual getButtons()/read call used to eat
  // the press before the app ever saw it. A minimum wall-clock hold makes
  // delivery independent of how many times kbd_poll() happens to run.
  // A one-shot is retired only after a full poll-to-poll cycle in which a
  // re-injected key reads released (kbd_inject_poll).
  kbd_inject_poll(&s_inj, &s_btn, &s_in, bg, now_ms, KBD_INJECT_HOLD_MS);

  // Decode up to KBD_POLL_MAX_ITEMS raw FIFO items the bus engine (kbd_i2c.c)
  // has read since the last poll, in FIFO order (kbd_fifo_apply): nothing
  // between two polls is lost to "net state". A key pressed and released
  // inside one poll reads as held for this poll; a HOLD is a repeat, never a
  // new press. The engine reads the STM32 from interrupts, so this never
  // waits on the 10 kHz bus; the service pass first runs a bus recovery if a
  // transaction failed. Anything past the cap stays in the ring (and, once
  // that fills, in the STM32) for the next poll.
  kbd_i2c_service();
  uint8_t state, keycode;
  for (int i = 0; i < KBD_POLL_MAX_ITEMS && kbd_i2c_pop(&state, &keycode);
       i++) {
#ifdef KBD_DEBUG
    const char *state_str = state == KBD_FIFO_PRESSED    ? "PRESS"
                            : state == KBD_FIFO_HOLD     ? "HOLD"
                            : state == KBD_FIFO_RELEASED ? "RELEASE"
                                                         : "?";
    if (keycode >= 0x20 && keycode < 0x7F)
      printf("[KBD] %s 0x%02X ('%c')\n", state_str, keycode, keycode);
    else
      printf("[KBD] %s 0x%02X\n", state_str, keycode);
#endif
    uint8_t raw = kbd_fifo_apply(&s_in, &s_btn, state, keycode);
    if (raw) {
      s_last_raw_key = raw;
      new_key = true;
    }
    // Brk (screenshot) on its press only: checked per item, so a key that
    // follows it in the same poll cannot hide it.
    if (state == KBD_FIFO_PRESSED && keycode == KEY_BRK)
      s_screenshot_pressed = true;
  }

  // Intercept BTN_MENU: detect rising edge, flag it for the OS, hide from apps.
  if ((s_btn.curr & BTN_MENU) && !(s_btn.prev & BTN_MENU))
    s_menu_pressed = true;
  s_btn.curr &= ~BTN_MENU;
  s_btn.deferred &= ~BTN_MENU;

  // Idle screen dimming: any fresh input counts as activity. If the activity
  // woke a dimmed screen, swallow the waking event so it doesn't reach the
  // running app.
  // In a background poll prev does not advance and the raw key is kept, so
  // only what this poll decoded counts (once per key, not every pass).
  bool activity = bg ? (new_key || s_in.char_pushed)
                     : ((s_btn.curr & ~s_btn.prev) || s_in.char_pushed ||
                        s_last_raw_key);
  if (activity) {
    if (idle_dim_note_activity()) {
      // Drop this poll's fresh press edges (in a background run, only this
      // poll's: earlier polls of the run stay for the app).
      kbd_buttons_swallow(&s_btn, bg_run, curr_before, prev_before);
      s_last_raw_key = bg_run ? raw_before : 0;
      // ...and this poll's queued presses and chars. Releases of keys that
      // were down before this poll stay, so a key the app saw go down still
      // comes up; ups of keys pressed in this poll go with their downs. Keys
      // first seen down in this poll are forgotten (their HOLD then reads as
      // a quiet hold).
      kbd_evq_drop_newest_keep_ups(&s_in.q, s_in.ev_pushed, &down_before);
      kbd_chars_drop_newest(&s_in, s_in.char_pushed);
      for (int i = 0; i < 8; i++)
        s_in.down.bits[i] &= down_before.bits[i];
      // A waking injected one-shot must be retired for good here, not just
      // masked out of s_btn.curr for this one poll. The active one-shot is
      // held across multiple polls (KBD_INJECT_HOLD_MS), so if we left it
      // set, the very next poll's fold in kbd_inject_poll would OR it
      // straight back in — and with s_btn.prev now 0 (we just cleared it),
      // that reads as a brand new rising edge, leaking the "swallowed" wake
      // press to the app one poll late. Dropping active/pending here matches
      // the pre-hold behavior, where a swallowed wake press was gone for good.
      kbd_inject_drop_oneshots(&s_inj, &s_btn);
    }
  }
  // One char per poll, oldest first: a second key in the same poll is kept
  // for the next poll instead of overwriting the first.
  if (!bg)
    s_last_char = kbd_chars_pop(&s_in);
  idle_dim_poll();
}

char kbd_get_char(void) {
  char c = s_last_char ? s_last_char : s_inj.ch;
  s_inj.ch = 0; // consume injected char
  return c;
}

uint8_t kbd_get_raw_key(void) { return s_last_raw_key; }

uint32_t kbd_get_buttons(void) { return s_btn.curr; }

uint32_t kbd_get_buttons_pressed(void) {
  return (s_btn.curr & ~s_btn.prev);
}

uint32_t kbd_get_buttons_released(void) {
  return (~s_btn.curr & s_btn.prev);
}

bool kbd_poll_event(kbd_event_t *out) { return kbd_evq_pop(&s_in.q, out); }

bool kbd_is_key_down(uint8_t keycode) {
  return kbd_keyset_test(&s_in.down, keycode);
}

void kbd_flush_events(void) { kbd_evq_clear(&s_in.q); }

int kbd_get_battery_percent(void) { return kbd_i2c_battery(); }

bool kbd_is_charging(void) { return kbd_i2c_charging(); }

void kbd_set_backlight(uint8_t brightness) {
  kbd_i2c_set_backlight(brightness);
}

void kbd_apply_clock(void) { kbd_i2c_apply_clock(); }

void kbd_pause_bus(void) { kbd_i2c_pause(); }

void kbd_resume_bus(void) { kbd_i2c_resume(); }

void kbd_prepare_reset(void) { kbd_i2c_pause(); }

void kbd_set_poll_interval_ms(uint32_t ms) {
  kbd_i2c_set_interval_us(ms * 1000u);
}

bool kbd_consume_menu_press(void) {
  bool val = s_menu_pressed;
  s_menu_pressed = false;
  return val;
}

bool kbd_consume_screenshot_press(void) {
  bool val = s_screenshot_pressed;
  s_screenshot_pressed = false;
  return val;
}

void kbd_discard_pending(void) {
  // Everything the STM32 queued before this call (the bus engine drops it:
  // see kbd_bus_discard) and everything already read into its ring, what
  // background polls (sys.sleep) already decoded into the queue, backlog and
  // button masks (kbd_clear_state below drops it), plus pending/active
  // one-shot injections: none of it was typed at whatever is about to be
  // shown.
  kbd_i2c_discard();
  kbd_inject_drop_oneshots(&s_inj, &s_btn);
  s_inj.ch = 0;
  kbd_clear_state();  // a keydown latch stays held, without an edge
}

void kbd_clear_state(void) {
  memset(&s_btn, 0, sizeof(s_btn));
  kbd_input_clear(&s_in);
  s_last_char = 0;
  s_last_raw_key = 0;
  // An injected key still down (an active one-shot — typically the injected
  // Enter that opened the modal calling this — or a keydown latch) stays
  // held with no press edge, and its retire/keyup still releases it; a
  // pending injection is published, with its edge, by the next poll; an
  // unread injected char is dropped. See kbd_inject_after_clear.
  kbd_inject_after_clear(&s_inj, &s_btn);
}

void kbd_inject_buttons(uint32_t buttons) {
  // Mirror the physical-key intercepts: BTN_MENU is an OS-level trigger that
  // must set the menu flag and stay hidden from apps (physical MENU is
  // intercepted in kbd_poll and stripped from s_btn.curr). Without this,
  // an injected MENU reached apps as a plain button and never opened the
  // system menu.
  if (buttons & BTN_MENU) {
    s_menu_pressed = true;
    buttons &= ~BTN_MENU;
  }
  // NOTE: pending is a single bitmask, not a per-button queue — two DIFFERENT
  // buttons injected within the same KBD_INJECT_HOLD_MS window merge into a
  // momentary chord (both alive in s_inj.active at once) instead of
  // arriving as two separate presses. The MCP `keypress` tool's default
  // 100ms inter-key delay is comfortably above KBD_INJECT_HOLD_MS (80ms), so
  // back-to-back sequence presses never actually overlap in practice.
  s_inj.pending |= buttons;
}

void kbd_hold_buttons(uint32_t buttons) {
  // Latch buttons held until kbd_release_buttons() — enables modifier chords
  // (e.g. hold ctrl, type 's', release ctrl). MENU is click-only.
  buttons &= ~BTN_MENU;
  kbd_inject_hold(&s_inj, &s_btn, &s_in, buttons);
}

void kbd_release_buttons(uint32_t buttons) {
  // Also retires active/pending one-shots of the same keys: otherwise the
  // next poll's fold (kbd_inject_poll) would resurrect them.
  kbd_inject_release(&s_inj, &s_btn, &s_in, buttons);
}

void kbd_inject_char(char c) {
  s_inj.ch = c;
  // The event queue sees a tap of that key, as the simulator's does.
  uint8_t mods = kbd_mods_from_buttons(s_btn.curr);
  kbd_event_t e = {KBD_EV_DOWN, (uint8_t)c, 0, mods};
  kbd_input_accept(&s_in, e);
  e.type = KBD_EV_CHAR;
  e.ch = (uint8_t)c;
  kbd_input_accept(&s_in, e);
  e.type = KBD_EV_UP;
  e.ch = 0;
  kbd_input_accept(&s_in, e);
}
