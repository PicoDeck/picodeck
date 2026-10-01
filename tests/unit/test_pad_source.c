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
  test_nav_buttons();
  return check_report("test_pad_source");
}
