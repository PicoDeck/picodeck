// Host unit tests for src/drivers/pad_source.c: whole-state pad reports
// (the `pad` dev command, the simulator's game controller, later a BT or USB
// pad) ORed into the logical gamepad with the keyboard aliases' rules.
#include "check.h"
#include "pad_source.h"
#include "../os/os.h"

static uint32_t s_now;

// A foreground poll (kbd_poll) / a background one (kbd_poll_background).
static bool fg(void) { return pad_sources_poll(false, s_now); }
static bool bg(void) { return pad_sources_poll(true, s_now); }

// The app's view with no key held.
static uint32_t held(void) { return pad_sources_combine_held(0); }
static uint32_t pressed(void) { return pad_sources_combine_pressed(0, 0); }
static uint32_t released(void) { return pad_sources_combine_released(0, 0); }

static void reset(void) {
  pad_sources_reset();
  s_now = 1000;
}

static void test_press_hold_release(void) {
  reset();
  CHECK_EQ_U32(pad_sources_connected(), 0);
  pad_source_publish(PAD_SOURCE_HOST, PAD_A);
  CHECK(pad_source_is_connected(PAD_SOURCE_HOST));
  CHECK_EQ_U32(pad_sources_connected(), 1u << PAD_SOURCE_HOST);
  CHECK(!fg());
  CHECK_EQ_U32(pressed(), PAD_A);
  CHECK_EQ_U32(held(), PAD_A);
  CHECK_EQ_U32(pad_sources_pressed(), PAD_A);
  // Held: no repeat edge, however often it is reported again.
  pad_source_publish(PAD_SOURCE_HOST, PAD_A);
  fg();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(held(), PAD_A);
  fg();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);
  // A second button joins: only it gets an edge.
  pad_source_publish(PAD_SOURCE_HOST, PAD_A | PAD_RIGHT);
  fg();
  CHECK_EQ_U32(pressed(), PAD_RIGHT);
  CHECK_EQ_U32(held(), PAD_A | PAD_RIGHT);
  pad_source_publish(PAD_SOURCE_HOST, PAD_RIGHT);
  fg();
  CHECK_EQ_U32(released(), PAD_A);
  CHECK_EQ_U32(held(), PAD_RIGHT);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  fg();
  CHECK_EQ_U32(released(), PAD_RIGHT);
  CHECK_EQ_U32(held(), 0);
  CHECK(pad_source_is_connected(PAD_SOURCE_HOST));  // released, not gone
  fg();
  CHECK_EQ_U32(released(), 0);
}

// A press and its release both between two polls read as held for one poll.
static void test_tap_between_polls(void) {
  reset();
  fg();
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  pad_source_publish(PAD_SOURCE_BT, 0);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  CHECK_EQ_U32(held(), PAD_B);
  fg();
  CHECK_EQ_U32(released(), PAD_B);
  CHECK_EQ_U32(held(), 0);
  fg();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);
  // Released and pressed again while held: still held, no second edge (as a
  // key released and pressed inside one poll).
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  pad_source_publish(PAD_SOURCE_BT, 0);
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);
  CHECK_EQ_U32(held(), PAD_B);
}

// Sources and the keyboard OR together: a button stays held while any of
// them holds it, with one press edge and one release edge.
static void test_sources_or_together(void) {
  reset();
  pad_source_publish(PAD_SOURCE_TEST, PAD_A);
  fg();
  CHECK_EQ_U32(pressed(), PAD_A);
  pad_source_publish(PAD_SOURCE_HOST, PAD_A | PAD_UP);
  fg();
  CHECK_EQ_U32(pressed(), PAD_UP);
  pad_source_publish(PAD_SOURCE_TEST, 0);
  fg();
  CHECK_EQ_U32(released(), 0);
  CHECK_EQ_U32(held(), PAD_A | PAD_UP);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  fg();
  CHECK_EQ_U32(released(), PAD_A | PAD_UP);

  // The keyboard's F4 (PAD_A) held, then a pad presses A too: no new edge;
  // F4 released while the pad holds A: still held; the pad lets go: released.
  reset();
  uint16_t kc = PAD_A, kp = 0;
  fg();
  CHECK_EQ_U32(pad_sources_combine_pressed(kc, kp), PAD_A);
  kp = kc;
  pad_source_publish(PAD_SOURCE_HOST, PAD_A);
  fg();
  CHECK_EQ_U32(pad_sources_combine_pressed(kc, kp), 0);
  CHECK_EQ_U32(pad_sources_combine_held(kc), PAD_A);
  kc = 0;  // F4 up
  fg();
  CHECK_EQ_U32(pad_sources_combine_released(kc, kp), 0);
  CHECK_EQ_U32(pad_sources_combine_held(kc), PAD_A);
  kp = kc;
  pad_source_publish(PAD_SOURCE_HOST, 0);
  fg();
  CHECK_EQ_U32(pad_sources_combine_released(kc, kp), PAD_A);
  // pad_sources_pressed is the sources' alone.
  kc = PAD_UP;
  fg();
  CHECK_EQ_U32(pad_sources_combine_pressed(kc, kp), PAD_UP);
  CHECK_EQ_U32(pad_sources_pressed(), 0);
}

// A disconnect releases everything the source held; nothing sticks.
static void test_disconnect_releases(void) {
  reset();
  pad_source_publish(PAD_SOURCE_USB, PAD_A | PAD_RIGHT | PAD_L);
  pad_source_publish(PAD_SOURCE_HOST, PAD_L);
  fg();
  CHECK_EQ_U32(held(), PAD_A | PAD_RIGHT | PAD_L);
  pad_source_disconnect(PAD_SOURCE_USB);
  CHECK(!pad_source_is_connected(PAD_SOURCE_USB));
  CHECK_EQ_U32(pad_sources_connected(), 1u << PAD_SOURCE_HOST);
  fg();
  CHECK_EQ_U32(released(), PAD_A | PAD_RIGHT);
  CHECK_EQ_U32(held(), PAD_L);
  pad_source_disconnect(PAD_SOURCE_HOST);
  fg();
  CHECK_EQ_U32(released(), PAD_L);
  CHECK_EQ_U32(held(), 0);
  CHECK_EQ_U32(pad_sources_connected(), 0);
  // Reconnecting with a button already down is a fresh press.
  pad_source_publish(PAD_SOURCE_USB, PAD_A);
  fg();
  CHECK_EQ_U32(pressed(), PAD_A);
  // A tap published just before the disconnect still reads for one poll.
  fg();
  pad_source_publish(PAD_SOURCE_USB, PAD_A | PAD_B);
  pad_source_disconnect(PAD_SOURCE_USB);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  CHECK_EQ_U32(released(), PAD_A);
  fg();
  CHECK_EQ_U32(released(), PAD_B);
  CHECK_EQ_U32(held(), 0);
}

// Home is not a PAD_* button: its press is reported once by the poll.
static void test_home(void) {
  reset();
  pad_source_publish(PAD_SOURCE_HOST, PAD_SOURCE_HOME);
  CHECK(fg());
  CHECK_EQ_U32(held(), 0);
  CHECK_EQ_U32(pressed(), 0);
  CHECK(!fg());  // held: no second menu press
  pad_source_publish(PAD_SOURCE_HOST, PAD_SOURCE_HOME | PAD_A);
  CHECK(!fg());
  CHECK_EQ_U32(pressed(), PAD_A);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  CHECK(!fg());
  // A tap between polls.
  pad_source_publish(PAD_SOURCE_HOST, PAD_SOURCE_HOME);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  CHECK(fg());
  CHECK(!fg());
  // In a background poll (sys.sleep) it acts at once, and only once.
  pad_source_publish(PAD_SOURCE_HOST, PAD_SOURCE_HOME);
  CHECK(bg());
  CHECK(!bg());
  CHECK(!fg());
  // Unknown bits are ignored.
  pad_source_publish(PAD_SOURCE_HOST, 0xFFFFFFFFu & ~PAD_SOURCE_HOME);
  CHECK(!fg());
  CHECK_EQ_U32(held(), PAD_SOURCE_BUTTONS);
  // So is an unknown source.
  pad_source_publish(PAD_SOURCE_COUNT, PAD_SOURCE_HOME);
  pad_source_disconnect(PAD_SOURCE_COUNT);
  CHECK(!pad_source_is_connected(PAD_SOURCE_COUNT));
  CHECK(!fg());
}

// Background polls (sys.sleep) gather; the app's next poll gets the edges.
static void test_background_polls(void) {
  reset();
  fg();
  pad_source_publish(PAD_SOURCE_HOST, PAD_X);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  CHECK(!bg());
  CHECK_EQ_U32(pressed(), 0);  // the app's view did not move
  CHECK_EQ_U32(pad_sources_fresh(), PAD_X);
  pad_source_publish(PAD_SOURCE_HOST, PAD_Y);
  bg();
  CHECK_EQ_U32(pad_sources_fresh(), PAD_Y);
  bg();
  CHECK_EQ_U32(pad_sources_fresh(), 0);
  fg();
  CHECK_EQ_U32(pressed(), PAD_X | PAD_Y);  // the tap and the hold
  fg();
  CHECK_EQ_U32(released(), PAD_X);
  CHECK_EQ_U32(held(), PAD_Y);
  // Released during the run: a release edge at the next foreground poll.
  pad_source_publish(PAD_SOURCE_HOST, 0);
  bg();
  CHECK_EQ_U32(held(), PAD_Y);
  fg();
  CHECK_EQ_U32(released(), PAD_Y);
}

// kbd_clear_state: a button the app saw stays held without a new edge; one
// pressed but not polled yet keeps its edge.
static void test_clear(void) {
  reset();
  pad_source_publish(PAD_SOURCE_HOST, PAD_A);
  fg();
  pad_source_publish(PAD_SOURCE_HOST, PAD_A | PAD_B);  // B not polled yet
  pad_sources_clear();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);
  CHECK_EQ_U32(held(), PAD_A);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  CHECK_EQ_U32(held(), PAD_A | PAD_B);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  fg();
  CHECK_EQ_U32(released(), PAD_A | PAD_B);
  // A button released before the clear is not held after it.
  pad_source_publish(PAD_SOURCE_HOST, PAD_UP);
  fg();
  pad_source_publish(PAD_SOURCE_HOST, 0);
  pad_sources_clear();
  CHECK_EQ_U32(held(), 0);
  fg();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);
}

// kbd_discard_pending: nothing pressed before it gives a press edge after.
static void test_discard(void) {
  reset();
  fg();
  pad_source_publish(PAD_SOURCE_HOST, PAD_A);        // held, unpolled
  pad_source_publish(PAD_SOURCE_TEST, PAD_B);        // a tap, unpolled
  pad_source_publish(PAD_SOURCE_TEST, PAD_SOURCE_HOME);
  pad_sources_discard();
  CHECK_EQ_U32(held(), PAD_A);
  CHECK(!fg());  // the Home press went too
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(held(), PAD_A);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  fg();
  CHECK_EQ_U32(released(), PAD_A);
  pad_source_publish(PAD_SOURCE_HOST, PAD_A);        // a fresh press counts
  fg();
  CHECK_EQ_U32(pressed(), PAD_A);
}

// The idle-dim wake swallow: the waking press never reaches the app; the
// button then reads held with no edge until it is released.
static void test_swallow(void) {
  reset();
  fg();
  pad_source_publish(PAD_SOURCE_HOST, PAD_START);
  fg();
  CHECK_EQ_U32(pad_sources_fresh(), PAD_START);
  pad_sources_swallow();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(held(), 0);
  fg();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(held(), PAD_START);
  fg();
  CHECK_EQ_U32(pressed(), 0);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  fg();
  CHECK_EQ_U32(released(), PAD_START);
  pad_source_publish(PAD_SOURCE_HOST, PAD_START);  // no longer quiet
  fg();
  CHECK_EQ_U32(pressed(), PAD_START);

  // Woken in a background poll: the gathered press goes too.
  reset();
  fg();
  pad_source_publish(PAD_SOURCE_HOST, PAD_R);
  bg();
  CHECK_EQ_U32(pad_sources_fresh(), PAD_R);
  pad_sources_swallow();
  fg();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(held(), PAD_R);
  // A tap swallowed in a background poll is gone for good.
  pad_source_publish(PAD_SOURCE_HOST, 0);
  fg();
  pad_source_publish(PAD_SOURCE_HOST, PAD_L);
  pad_source_publish(PAD_SOURCE_HOST, 0);
  bg();
  pad_sources_swallow();
  fg();
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(held(), 0);
}

// The `pad` dev command's source: a timed hold counts from the first
// foreground poll after the command.
static void test_test_source(void) {
  reset();
  pad_source_test_set(PAD_A | PAD_RIGHT, 100);
  CHECK(pad_source_is_connected(PAD_SOURCE_TEST));
  s_now = 5000;  // the app polls late
  fg();
  CHECK_EQ_U32(pressed(), PAD_A | PAD_RIGHT);
  s_now = 5099;
  fg();
  CHECK_EQ_U32(held(), PAD_A | PAD_RIGHT);
  bg();  // background polls do not end it
  s_now = 5100;
  fg();
  CHECK_EQ_U32(released(), PAD_A | PAD_RIGHT);
  CHECK(pad_source_is_connected(PAD_SOURCE_TEST));
  // Untimed: held until the next command.
  pad_source_test_set(PAD_UP, 0);
  s_now = 90000;
  fg();
  CHECK_EQ_U32(pressed(), PAD_UP);
  s_now = 190000;
  fg();
  CHECK_EQ_U32(held(), PAD_UP);
  // A new command replaces a timed hold's state and timer.
  pad_source_test_set(PAD_B, 50);
  pad_source_test_set(PAD_X, 0);
  fg();
  s_now += 1000;
  fg();
  CHECK_EQ_U32(held(), PAD_X);
  pad_source_test_off();
  CHECK(!pad_source_is_connected(PAD_SOURCE_TEST));
  fg();
  CHECK_EQ_U32(released(), PAD_X);
  // A timed hold whose source was switched off does not reconnect it.
  pad_source_test_set(PAD_Y, 10);
  fg();
  pad_source_test_off();
  s_now += 100;
  fg();
  CHECK(!pad_source_is_connected(PAD_SOURCE_TEST));
  // A timed Home tap opens the menu once.
  pad_source_test_set(PAD_SOURCE_HOME, 30);
  CHECK(fg());
  s_now += 30;
  CHECK(!fg());
  CHECK(!fg());
}

// kbd_hide_held (the system menu, opening and closing): a button down then is
// out of every mask, with no edges, until it comes up. Held on, however often
// it is reported again, it stays out; pressed again it is a press.
static void test_hide_held_until_released(void) {
  reset();
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);  // the menu reads it, and closes
  pad_sources_hide_held();
  CHECK_EQ_U32(held(), 0);
  CHECK_EQ_U32(released(), 0);
  CHECK_EQ_U32(pad_sources_pressed(), 0);
  for (int i = 0; i < 5; i++) {
    pad_source_publish(PAD_SOURCE_BT, PAD_B);  // a report per frame, unchanged
    fg();
    CHECK_EQ_U32(held(), 0);
    CHECK_EQ_U32(pressed(), 0);
    CHECK_EQ_U32(released(), 0);
    CHECK_EQ_U32(pad_sources_pressed(), 0);
    CHECK_EQ_U32(pad_sources_fresh(), 0);
  }
  pad_source_publish(PAD_SOURCE_BT, 0);
  fg();
  CHECK_EQ_U32(held(), 0);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);  // the app never saw it down
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  CHECK_EQ_U32(held(), PAD_B);
  CHECK_EQ_U32(pad_sources_pressed(), PAD_B);
  pad_source_publish(PAD_SOURCE_BT, 0);
  fg();
  CHECK_EQ_U32(released(), PAD_B);
}

// Only the button that was down. A second one, on the same source or another,
// and the same one pressed on another source, are new presses.
static void test_hide_held_other_buttons(void) {
  reset();
  pad_source_publish(PAD_SOURCE_HOST, PAD_B);
  fg();
  pad_sources_hide_held();
  pad_source_publish(PAD_SOURCE_HOST, PAD_B | PAD_A);
  fg();
  CHECK_EQ_U32(pressed(), PAD_A);
  CHECK_EQ_U32(held(), PAD_A);
  pad_source_publish(PAD_SOURCE_HOST, PAD_B);
  fg();
  CHECK_EQ_U32(released(), PAD_A);
  CHECK_EQ_U32(held(), 0);
  // A held button on a second source too: it is hidden while any holds it.
  reset();
  pad_source_publish(PAD_SOURCE_HOST, PAD_B);
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  pad_sources_hide_held();
  pad_source_publish(PAD_SOURCE_HOST, 0);
  fg();
  CHECK_EQ_U32(held(), 0);
  pad_source_publish(PAD_SOURCE_BT, 0);
  fg();
  CHECK_EQ_U32(held(), 0);
  pad_source_publish(PAD_SOURCE_HOST, PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  // Another source pressing the hidden button is a press of its own.
  reset();
  pad_source_publish(PAD_SOURCE_HOST, PAD_B);
  fg();
  pad_sources_hide_held();
  fg();
  CHECK_EQ_U32(held(), 0);
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
}

// Released and pressed again between two polls: the state alone would still
// say held; the latch shows the new press.
static void test_hide_held_new_press_between_polls(void) {
  reset();
  pad_source_publish(PAD_SOURCE_TEST, PAD_B);
  fg();
  pad_sources_hide_held();
  fg();
  pad_source_publish(PAD_SOURCE_TEST, 0);
  pad_source_publish(PAD_SOURCE_TEST, PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  CHECK_EQ_U32(held(), PAD_B);
  // A tap after it was released, whole between two polls, reads as one.
  reset();
  pad_source_publish(PAD_SOURCE_TEST, PAD_B);
  fg();
  pad_sources_hide_held();
  pad_source_publish(PAD_SOURCE_TEST, 0);
  fg();
  pad_source_publish(PAD_SOURCE_TEST, PAD_B);
  pad_source_publish(PAD_SOURCE_TEST, 0);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  fg();
  CHECK_EQ_U32(released(), PAD_B);
}

// A press not polled before the menu closed goes with the rest (it was typed
// at the menu); so does a press gathered in a background run, and Home.
static void test_hide_held_drops_what_was_not_polled(void) {
  reset();
  pad_source_publish(PAD_SOURCE_BT, PAD_A);
  pad_source_publish(PAD_SOURCE_BT, 0);
  pad_source_publish(PAD_SOURCE_BT, PAD_START | PAD_SOURCE_HOME);
  pad_sources_hide_held();
  CHECK(!fg());
  CHECK_EQ_U32(held(), 0);
  CHECK_EQ_U32(pressed(), 0);
  reset();
  pad_source_publish(PAD_SOURCE_BT, PAD_A);
  pad_source_publish(PAD_SOURCE_BT, 0);
  bg();
  pad_sources_hide_held();
  fg();
  CHECK_EQ_U32(held(), 0);
  CHECK_EQ_U32(pressed(), 0);
}

// Background polls (sys.sleep) see the release too: a press after it, gathered
// by them, reaches the next foreground poll.
static void test_hide_held_background_polls(void) {
  reset();
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  pad_sources_hide_held();
  bg();
  fg();
  CHECK_EQ_U32(held(), 0);
  pad_source_publish(PAD_SOURCE_BT, 0);
  bg();
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  bg();
  CHECK_EQ_U32(pad_sources_fresh(), PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  // Still held and never let go, a run leaves it hidden.
  reset();
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  pad_sources_hide_held();
  bg();
  bg();
  CHECK_EQ_U32(pad_sources_fresh(), 0);
  fg();
  CHECK_EQ_U32(held(), 0);
}

// A disconnect ends it: the reconnected pad's press is new.
static void test_hide_held_disconnect(void) {
  reset();
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  pad_sources_hide_held();
  pad_source_disconnect(PAD_SOURCE_BT);
  fg();
  CHECK_EQ_U32(held(), 0);
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
}

// The test source's timed hold runs on while hidden and its release ends it.
static void test_hide_held_timed_hold(void) {
  reset();
  pad_source_test_set(PAD_B, 100);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
  pad_sources_hide_held();
  s_now += 50;
  fg();
  CHECK_EQ_U32(held(), 0);
  s_now += 60;
  fg();
  CHECK_EQ_U32(held(), 0);
  CHECK_EQ_U32(released(), 0);
  pad_source_test_set(PAD_B, 100);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
}

// A later clear or discard (input.clearState(), a modal) does not bring it
// back; a discard lets one that is up go.
static void test_hide_held_survives_clear_and_discard(void) {
  reset();
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  pad_sources_hide_held();
  pad_sources_clear();
  fg();
  CHECK_EQ_U32(held(), 0);
  pad_sources_discard();
  CHECK_EQ_U32(held(), 0);
  fg();
  CHECK_EQ_U32(held(), 0);
  pad_source_publish(PAD_SOURCE_BT, 0);
  pad_sources_discard();
  pad_source_publish(PAD_SOURCE_BT, PAD_B);
  fg();
  CHECK_EQ_U32(pressed(), PAD_B);
}

static void test_nav_buttons(void) {
  CHECK_EQ_U32(pad_nav_buttons(0), 0);
  CHECK_EQ_U32(pad_nav_buttons(PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT),
               BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
  CHECK_EQ_U32(pad_nav_buttons(PAD_A), BTN_ENTER);
  CHECK_EQ_U32(pad_nav_buttons(PAD_B), BTN_ESC);
  CHECK_EQ_U32(pad_nav_buttons(PAD_X | PAD_Y | PAD_L | PAD_R | PAD_START |
                               PAD_SELECT),
               0);
}

// ── Axes ────────────────────────────────────────────────────────────────────

static bool near(float got, float want) {
  float d = got - want;
  return d > -0.012f && d < 0.012f;
}

#define CHECK_AXIS(axis, dpad, want)                                          \
  do {                                                                        \
    float _v = pad_sources_axis((axis), (dpad));                              \
    if (!near(_v, (want)))                                                    \
      printf("  axis %d read %f, want %f\n", (axis), (double)_v,              \
             (double)(want));                                                 \
    CHECK(near(_v, (want)));                                                  \
  } while (0)

static void sticks(pad_source_id_t id, int lx, int ly, int rx, int ry) {
  pad_axes_t a = {(int8_t)lx, (int8_t)ly, (int8_t)rx, (int8_t)ry, 0, 0};
  pad_source_publish_axes(id, &a);
}

static void triggers(pad_source_id_t id, int lt, int rt) {
  pad_axes_t a = {0, 0, 0, 0, (uint8_t)lt, (uint8_t)rt};
  pad_source_publish_axes(id, &a);
}

static void test_axes_rest_and_range(void) {
  reset();
  fg();
  for (int a = AXIS_LX; a <= AXIS_RT; a++)
    CHECK_AXIS(a, 0, 0.0f);
  // Not an axis: 0, whatever is held.
  CHECK_AXIS(-1, PAD_LEFT, 0.0f);
  CHECK_AXIS(6, PAD_LEFT, 0.0f);
  // Full deflection reads exactly +-1, right and down positive.
  sticks(PAD_SOURCE_HOST, 127, 0, 0, -127);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == 1.0f);
  CHECK(pad_sources_axis(AXIS_LY, 0) == 0.0f);
  CHECK(pad_sources_axis(AXIS_RY, 0) == -1.0f);
  // -128 (a raw value nothing should send) stays in range.
  sticks(PAD_SOURCE_HOST, -128, 0, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == -1.0f);
  triggers(PAD_SOURCE_HOST, 255, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LT, 0) == 1.0f);
  CHECK(pad_sources_axis(AXIS_RT, 0) == 0.0f);
}

static void test_stick_deadzone(void) {
  reset();
  // The inner dead zone: nothing at all.
  sticks(PAD_SOURCE_HOST, 15, 0, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == 0.0f);
  sticks(PAD_SOURCE_HOST, -10, 10, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == 0.0f);
  CHECK(pad_sources_axis(AXIS_LY, 0) == 0.0f);
  // Radial: each axis alone is inside, the stick's distance is not.
  sticks(PAD_SOURCE_HOST, 14, 14, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) > 0.0f);
  CHECK(pad_sources_axis(AXIS_LY, 0) > 0.0f);
  // The rest is rescaled to start at 0: just outside reads next to nothing,
  // no jump, and it rises with the stick up to the outer clamp.
  float prev = 0.0f;
  for (int v = 0; v <= 127; v++) {
    sticks(PAD_SOURCE_HOST, v, 0, 0, 0);
    fg();
    float got = pad_sources_axis(AXIS_LX, 0);
    CHECK(got >= prev);
    CHECK(got - prev < 0.03f);
    prev = got;
    // Mirror image on the other side.
    sticks(PAD_SOURCE_HOST, -v, 0, 0, 0);
    fg();
    CHECK(pad_sources_axis(AXIS_LX, 0) == -got);
  }
  // A worn stick that stops short still reaches full.
  sticks(PAD_SOURCE_HOST, 122, 0, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == 1.0f);
  // Past half way it reads what the shape says, not 1.
  sticks(PAD_SOURCE_HOST, 76, 0, 0, 0);
  fg();
  CHECK_AXIS(AXIS_LX, 0, 0.575f);
  // Square-gated corner: the direction is kept, the length clamped.
  sticks(PAD_SOURCE_HOST, 127, 127, 0, 0);
  fg();
  CHECK_AXIS(AXIS_LX, 0, 0.707f);
  CHECK_AXIS(AXIS_LY, 0, 0.707f);
  // The right stick has the same shape, on its own: the left at rest.
  sticks(PAD_SOURCE_HOST, 0, 0, 10, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_RX, 0) == 0.0f);
  sticks(PAD_SOURCE_HOST, 10, 0, 122, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == 0.0f);
  CHECK(pad_sources_axis(AXIS_RX, 0) == 1.0f);
}

static void test_trigger_shape(void) {
  reset();
  // A trigger at rest may not read exactly 0, and a worn one still gets to 1.
  triggers(PAD_SOURCE_HOST, 7, 8);
  fg();
  CHECK(pad_sources_axis(AXIS_LT, 0) == 0.0f);
  CHECK(pad_sources_axis(AXIS_RT, 0) == 0.0f);
  triggers(PAD_SOURCE_HOST, 248, 128);
  fg();
  CHECK(pad_sources_axis(AXIS_LT, 0) == 1.0f);
  CHECK_AXIS(AXIS_RT, 0, 0.5f);
  float prev = 0.0f;
  for (int v = 0; v <= 255; v++) {
    triggers(PAD_SOURCE_HOST, 0, v);
    fg();
    float got = pad_sources_axis(AXIS_RT, 0);
    CHECK(got >= prev);
    CHECK(got - prev < 0.02f);
    prev = got;
  }
  // Triggers have no D-pad fallback, sticks do not reach them.
  CHECK_AXIS(AXIS_LT, PAD_LEFT | PAD_UP | PAD_A, 0.0f);
}

static void test_dpad_fallback(void) {
  reset();
  fg();
  // No stick in use: the left stick reads the D-pad (the keyboard's aliases,
  // a hat), -1 / 0 / +1. The right stick and triggers have no such fallback.
  CHECK(pad_sources_axis(AXIS_LX, PAD_LEFT) == -1.0f);
  CHECK(pad_sources_axis(AXIS_LX, PAD_RIGHT) == 1.0f);
  CHECK(pad_sources_axis(AXIS_LX, PAD_LEFT | PAD_RIGHT) == 0.0f);
  CHECK(pad_sources_axis(AXIS_LX, PAD_UP | PAD_DOWN | PAD_A) == 0.0f);
  CHECK(pad_sources_axis(AXIS_LY, PAD_UP) == -1.0f);
  CHECK(pad_sources_axis(AXIS_LY, PAD_DOWN) == 1.0f);
  CHECK(pad_sources_axis(AXIS_LY, PAD_UP | PAD_DOWN) == 0.0f);
  CHECK(pad_sources_axis(AXIS_RX, PAD_RIGHT) == 0.0f);
  CHECK(pad_sources_axis(AXIS_RY, PAD_DOWN) == 0.0f);
  CHECK(pad_sources_axis(AXIS_RT, PAD_RIGHT | PAD_A | PAD_R) == 0.0f);
  // A stick inside its dead zone is not in use: the D-pad still reads.
  sticks(PAD_SOURCE_BT, 12, 0, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, PAD_LEFT) == -1.0f);
  // A stick past half way also sets PAD_LEFT: it must not snap back to -1.
  sticks(PAD_SOURCE_BT, -76, 0, 0, 0);
  fg();
  CHECK_AXIS(AXIS_LX, PAD_LEFT, -0.575f);
  // The rule is per stick: while it is in use its other axis reads the stick
  // too (0 here), not the D-pad, so the reading never jumps with the noise.
  CHECK(pad_sources_axis(AXIS_LY, PAD_UP) == 0.0f);
  // The right stick in use does not hide the D-pad from the left.
  sticks(PAD_SOURCE_BT, 0, 0, 100, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, PAD_LEFT) == -1.0f);
}

static void test_axes_combine(void) {
  reset();
  // Largest magnitude wins, per axis, across the sources in use.
  sticks(PAD_SOURCE_TEST, 40, -100, 0, 0);
  sticks(PAD_SOURCE_BT, -90, 30, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) < -0.5f);
  CHECK(pad_sources_axis(AXIS_LY, 0) < -0.5f);
  // A source at rest changes nothing.
  sticks(PAD_SOURCE_BT, 0, 0, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) > 0.0f);
  CHECK(pad_sources_axis(AXIS_LY, 0) < -0.5f);
  // Triggers likewise, each on its own.
  triggers(PAD_SOURCE_TEST, 200, 40);
  triggers(PAD_SOURCE_BT, 60, 255);
  fg();
  CHECK_AXIS(AXIS_LT, 0, 0.8f);
  CHECK(pad_sources_axis(AXIS_RT, 0) == 1.0f);
}

static void test_axes_disconnect(void) {
  reset();
  pad_axes_t a = {127, 127, 127, 127, 255, 255};
  pad_source_publish(PAD_SOURCE_BT, PAD_LEFT);
  pad_source_publish_axes(PAD_SOURCE_BT, &a);
  fg();
  CHECK(pad_sources_axis(AXIS_RX, 0) != 0.0f);
  CHECK(pad_sources_axis(AXIS_RT, 0) == 1.0f);
  pad_source_disconnect(PAD_SOURCE_BT);
  fg();
  for (int i = AXIS_LX; i <= AXIS_RT; i++)
    CHECK(pad_sources_axis(i, 0) == 0.0f);
  // Another source's axes outlive it.
  pad_source_publish_axes(PAD_SOURCE_BT, &a);
  sticks(PAD_SOURCE_HOST, 0, 0, -127, 0);
  fg();
  pad_source_disconnect(PAD_SOURCE_BT);
  fg();
  CHECK(pad_sources_axis(AXIS_RX, 0) == -1.0f);
  CHECK(pad_sources_axis(AXIS_LT, 0) == 0.0f);
}

static void test_axes_test_source(void) {
  reset();
  pad_axes_t a = {-127, 0, 0, 0, 0, 255};
  // A timed hold ends for the axes too, and the source stays connected.
  pad_source_publish_axes(PAD_SOURCE_TEST, &a);
  pad_source_test_set(0, 100);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == -1.0f);
  s_now += 99;
  fg();
  CHECK(pad_sources_axis(AXIS_RT, 0) == 1.0f);
  s_now += 1;
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == 0.0f);
  CHECK(pad_sources_axis(AXIS_RT, 0) == 0.0f);
  CHECK(pad_source_is_connected(PAD_SOURCE_TEST));
  // Off: gone at once.
  pad_source_publish_axes(PAD_SOURCE_TEST, &a);
  pad_source_test_set(0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == -1.0f);
  pad_source_test_off();
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == 0.0f);
  CHECK(!pad_source_is_connected(PAD_SOURCE_TEST));
}

// Reads are of the last poll, whatever kind, so a frame sees one value; the
// buttons' clear, discard and swallow are for edges and leave levels alone.
static void test_axes_read_at_polls(void) {
  reset();
  fg();
  sticks(PAD_SOURCE_HOST, 127, 0, 0, 0);
  CHECK(pad_sources_axis(AXIS_LX, 0) == 0.0f);  // not polled yet
  bg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == 1.0f);
  sticks(PAD_SOURCE_HOST, -127, 0, 0, 0);
  fg();
  CHECK(pad_sources_axis(AXIS_LX, 0) == -1.0f);
  pad_sources_clear();
  CHECK(pad_sources_axis(AXIS_LX, 0) == -1.0f);
  pad_sources_discard();
  CHECK(pad_sources_axis(AXIS_LX, 0) == -1.0f);
  pad_sources_swallow();
  CHECK(pad_sources_axis(AXIS_LX, 0) == -1.0f);
}

// What the idle dimmer counts: a stick or trigger that moved. A stick at rest
// (inside its dead zone, however noisy) or held dead still does not.
static void test_axes_moved(void) {
  reset();
  fg();
  CHECK(!pad_sources_axes_moved());
  sticks(PAD_SOURCE_BT, 10, -9, 0, 0);
  fg();
  CHECK(!pad_sources_axes_moved());
  sticks(PAD_SOURCE_BT, 70, 0, 0, 0);
  fg();
  CHECK(pad_sources_axes_moved());
  fg();
  CHECK(!pad_sources_axes_moved());
  sticks(PAD_SOURCE_BT, 70, 0, 0, 0);
  bg();
  CHECK(!pad_sources_axes_moved());
  sticks(PAD_SOURCE_BT, 72, 0, 0, 0);
  bg();  // a background poll sees it too
  CHECK(pad_sources_axes_moved());
  triggers(PAD_SOURCE_BT, 0, 100);
  fg();
  CHECK(pad_sources_axes_moved());  // a trigger, and the stick let go
  pad_source_disconnect(PAD_SOURCE_BT);
  fg();
  CHECK(pad_sources_axes_moved());
  fg();
  CHECK(!pad_sources_axes_moved());
}

int main(void) {
  test_press_hold_release();
  test_tap_between_polls();
  test_sources_or_together();
  test_disconnect_releases();
  test_home();
  test_background_polls();
  test_clear();
  test_discard();
  test_swallow();
  test_test_source();
  test_hide_held_until_released();
  test_hide_held_other_buttons();
  test_hide_held_new_press_between_polls();
  test_hide_held_drops_what_was_not_polled();
  test_hide_held_background_polls();
  test_hide_held_disconnect();
  test_hide_held_timed_hold();
  test_hide_held_survives_clear_and_discard();
  test_nav_buttons();
  test_axes_rest_and_range();
  test_stick_deadzone();
  test_trigger_shape();
  test_dpad_fallback();
  test_axes_combine();
  test_axes_disconnect();
  test_axes_test_source();
  test_axes_read_at_polls();
  test_axes_moved();
  return check_report("test_pad_source");
}
