#pragma once

// =============================================================================
// Pad sources: whatever feeds the logical gamepad (picocalc.gamepad,
// g_api.gamepad; PAD_* in os.h) besides the keyboard.
//
// The keyboard's aliases are the built-in source (kbd_event_queue.h,
// "Gamepad"): keys resolved through the pad map, with their own tap, repeat
// and Shift/Alt rules. Every other source reports its whole PAD_* state
// here: the `pad` dev command (tests without hardware), the simulator's SDL
// game controller, and a Bluetooth pad (bt_pad.c; issue #26). The
// keyboard driver ORs them all into the gamepad at every kbd_poll(), so the
// keyboard keeps working while a pad is connected and an app cannot tell
// which source a press came from (kbd_get_pad* return
// pad_sources_combine_*). getLabel is unchanged: it names the keyboard key.
//
// Producer side (a transport): pad_source_publish(id, state) with the
// source's whole state at every report (not a sequence of press / release
// calls), pad_source_disconnect(id) when the pad goes away. Safe from any
// core, task or interrupt: a publish is one atomic 32-bit store of the state
// plus, when a button went down, one atomic OR of those buttons into the
// source's press latch (LDREX/STREX on the RP2350's SRAM, whose global
// exclusive monitor serves both cores). One producer per source: `last`
// below is the producer's own. The latch is what keeps a tap: a press and
// its release that both land between two polls still read as held for one
// poll, as a key's do.
//
// Consumer side (Core 0 only: the keyboard driver): pad_sources_poll at
// every kbd_poll / kbd_poll_background, the combine getters, and the clear
// / discard / swallow hooks that mirror the keyboard's. The rules match the
// keyboard aliases':
//   - a button reads held while its source holds it, with one press edge
//     and one release edge (sources report no repeats; neither do keys on
//     the gamepad);
//   - a tap between two polls reads as held for one poll, then released;
//   - background polls (sys.sleep) only gather: the app's next foreground
//     poll gets the edges;
//   - after kbd_clear_state a button the app saw and is still held stays
//     held with no new press edge; one pressed since then gets its edge;
//     kbd_discard_pending makes every held button quiet (no press edge);
//   - after kbd_hide_held a button down then reads as not held, with no edges,
//     until it comes up (or any source presses it anew);
//   - a disconnect clears the source's state, so nothing stays held;
//   - Home (PAD_SOURCE_HOME) is not a PAD_* button: its press raises the
//     system menu through kbd_consume_menu_press, like the Sym key.
//
// Axes (AXIS_* in os.h; issue #68). A source also reports its sticks and
// triggers, pad_source_publish_axes, with its raw readings: the dead zone is
// applied here, once, so no transport or game has its own. They are levels,
// not edges: no latch, and a poll that catches the button word and the axis
// words from different reports does not matter. Each source has two words, one
// atomic store each (four int8 sticks, two uint8 triggers), so a reader never
// sees half of a stick. At every poll, foreground or background, Core 0
// combines them into what getAxis reads until the next poll, so reading an axis
// costs a shift and a division, and takes no lock:
//   - stick shape: a radial inner dead zone (a stick inside it reads 0 on both
//     axes), the rest rescaled to start at 0, and an outer clamp, so a worn
//     stick still reaches +-1. A trigger has the same at either end;
//   - sources: each axis reads the largest magnitude among the sources;
//   - with the left stick at rest on every source, AXIS_LX and AXIS_LY read the
//     D-pad instead, -1, 0 or +1: the keyboard's aliases or a hat. The D-pad
//     cannot simply win, since a stick past half way sets PAD_LEFT too: every
//     strong push would snap back to 1. The right stick and triggers read 0.
//   - a disconnect zeroes the source's axes.
//
// A transport now implements: discovery, pairing and the link (a BT HID or
// USB HID report parser), the physical -> logical mapping (face buttons,
// shoulders, Start/Select, D-pad, a stick to the D-pad past half way, the raw
// sticks and triggers, Home), and one pad_source_publish and
// pad_source_publish_axes per report plus pad_source_disconnect on link loss.
// Nothing in the gamepad core or the apps changes.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  PAD_SOURCE_TEST = 0, // the `pad` dev command (dev_ops.c): tests, no pad
  PAD_SOURCE_HOST,     // the simulator's SDL game controller (hal_pad.c)
  PAD_SOURCE_BT,       // a Bluetooth pad (bt_pad.c)
  PAD_SOURCE_USB,      // reserved: a USB-host pad
  PAD_SOURCE_COUNT
} pad_source_id_t;

// A source's state: PAD_* bits (os.h: bit i is button i) and Home.
#define PAD_SOURCE_BUTTONS 0x0FFFu    // PAD_UP .. PAD_SELECT
#define PAD_SOURCE_HOME (1u << 16)    // the pad's Home/guide: the system menu

// A source's raw axes: right and down positive, as AXIS_* read. No dead zone
// yet. Full deflection is +-127 (a stray -128 reads as -127) and 255.
typedef struct {
  int8_t lx, ly, rx, ry;
  uint8_t lt, rt;
} pad_axes_t;

// ── Producer side (any core, task or ISR; one producer per source) ──────────

// The source's whole state now (PAD_* | PAD_SOURCE_HOME; other bits are
// ignored). Connects the source if it was not.
void pad_source_publish(pad_source_id_t id, uint32_t state);

// The source's sticks and triggers now, raw (the shape above is applied here).
// Does not connect the source: publish its state as well.
void pad_source_publish_axes(pad_source_id_t id, const pad_axes_t *axes);

// The source went away: its buttons are released at the next poll, its axes
// read 0. A tap it published just before still reads for one poll.
void pad_source_disconnect(pad_source_id_t id);

// ── Either side ─────────────────────────────────────────────────────────────

bool pad_source_is_connected(pad_source_id_t id);

// Bit i set when source i is connected (the header's pad indicator).
uint32_t pad_sources_connected(void);

// ── The test source (the `pad` dev command; Core 0) ─────────────────────────

// Publish `state` on PAD_SOURCE_TEST (its axes: pad_source_publish_axes first).
// hold_ms > 0: publish 0 and zero its axes (released, still connected) once
// hold_ms have passed, counted from the first foreground poll after this call,
// so an app that polls late still sees the press. hold_ms 0: held until the
// next call.
void pad_source_test_set(uint32_t state, uint32_t hold_ms);

// Disconnect PAD_SOURCE_TEST (and cancel a timed hold).
void pad_source_test_off(void);

// ── Consumer side (Core 0: the keyboard driver) ─────────────────────────────

// Every kbd_poll (bg false) and kbd_poll_background (bg true), with the
// millisecond clock kbd_poll uses. A foreground poll moves the sources'
// masks on (prev = the last poll's, curr = what is held now plus the taps
// since); a background one only gathers them for the next foreground poll.
// Returns true when a source's Home went down since the previous poll (the
// caller raises the menu flag, as for the Sym key).
bool pad_sources_poll(bool bg, uint32_t now_ms);

// The presses that poll found new (a foreground poll: its press edges; a
// background one: the buttons pressed since the previous poll), for the
// idle dimmer's activity check.
uint32_t pad_sources_fresh(void);

// The idle-dim wake: the fresh presses of the last poll never reach the app;
// a button still held reads held from the next foreground poll, with no
// press edge (as a key held across the swallow comes back on its HOLD).
void pad_sources_swallow(void);

// kbd_clear_state: no edges; a button the app saw and is still held stays
// held without a new press edge, one pressed since keeps its edge for the
// next poll.
void pad_sources_clear(void);

// kbd_discard_pending: taps not yet polled are dropped and every held button
// is held quietly: only a fresh press after this call gives a press edge.
void pad_sources_discard(void);

// kbd_hide_held: taps not yet polled are dropped and every held button is hidden:
// out of the masks, with no edges, until it is released. A press after that
// (in any source, the same one released and pressed again between two polls
// included) ends it and reads normally. Other buttons are not touched.
void pad_sources_hide_held(void);

// The gamepad the apps read: the keyboard's masks (kbd_pad_t curr / prev)
// ORed with the sources'.
uint32_t pad_sources_combine_held(uint16_t kbd_curr);
uint32_t pad_sources_combine_pressed(uint16_t kbd_curr, uint16_t kbd_prev);
uint32_t pad_sources_combine_released(uint16_t kbd_curr, uint16_t kbd_prev);

// PAD_* pressed this poll by the sources alone (not the keyboard aliases,
// whose keys already report as BTN_*).
uint32_t pad_sources_pressed(void);

// The value of one AXIS_* as of the last poll, -1 to 1 (triggers 0 to 1); 0 for
// anything else. `held` is the D-pad to fall back on (kbd_get_pad: every source
// and the keyboard).
float pad_sources_axis(int axis, uint32_t held);

// A stick or trigger moved at the last poll (after its dead zone, so a stick at
// rest never does), for the idle dimmer's activity check. There is nothing to
// swallow on a wake: an axis is a level, and reads on.
bool pad_sources_axes_moved(void);

// The OS's own menus (launcher, system menu) read a pad as navigation: the
// BTN_* its PAD_* presses stand for (D-pad = arrows, A = Enter, B = Esc).
uint32_t pad_nav_buttons(uint32_t pad);

// Back to power-on state (host tests).
void pad_sources_reset(void);
