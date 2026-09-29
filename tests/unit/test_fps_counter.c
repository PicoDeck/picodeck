// Host unit tests for src/os/fps_counter.c: the counting rules of the OS FPS
// counter (Show FPS). Times are made up; nothing reads a clock.
#include "check.h"
#include "fps_counter.h"

// n events, one every period_us (microsecond steps, so 59.73 Hz is exact),
// from start_ms; returns the time of the last one in ms.
typedef void (*event_fn)(fps_counter_t *c, uint32_t now);

static uint32_t run(fps_counter_t *c, event_fn ev, uint32_t start_ms,
                    uint32_t period_us, int n) {
  uint32_t t = start_ms;
  for (int i = 0; i < n; i++) {
    t = start_ms + (uint32_t)(((uint64_t)i * period_us) / 1000u);
    ev(c, t);
  }
  return t;
}

static void present(fps_counter_t *c, uint32_t now) { fps_counter_present(c, now); }
static void tick(fps_counter_t *c, uint32_t now) { fps_counter_tick(c, now); }

static void test_nothing_until_a_window_completes(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  CHECK_EQ_INT(fps_counter_value(&c, 0), -1);
  // 60 presents at 60 Hz span 983 ms: still under a second.
  uint32_t t = run(&c, present, 100, 16667, 60);
  CHECK_EQ_INT(t, 1083);
  CHECK_EQ_INT(fps_counter_value(&c, t), -1);
  // The 61st closes the first window: 60 frames after the first, in 1000 ms.
  fps_counter_present(&c, 100 + 1000);
  CHECK_EQ_INT(fps_counter_value(&c, 1100), 60);
}

static void test_present_rates(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  uint32_t t = run(&c, present, 0, 16667, 60 * 3 + 1);  // 60 Hz for 3 s
  CHECK_EQ_INT(fps_counter_value(&c, t), 60);
  fps_counter_reset(&c);
  t = run(&c, present, 5000, 33333, 30 * 3 + 1);        // 30 Hz
  CHECK_EQ_INT(fps_counter_value(&c, t), 30);
  fps_counter_reset(&c);
  t = run(&c, present, 0, 16742, 200);                  // 59.73 Hz: rounds
  CHECK_EQ_INT(fps_counter_value(&c, t), 60);
  fps_counter_reset(&c);
  t = run(&c, present, 0, 4000, 1001);                  // 250 Hz
  CHECK_EQ_INT(fps_counter_value(&c, t), 250);
}

static void test_value_changes_once_per_window(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  uint32_t t = run(&c, present, 0, 20000, 51);  // 50 Hz, one window
  CHECK_EQ_INT(fps_counter_value(&c, t), 50);
  // Speed up to 100 Hz: the value holds until the open window completes.
  for (int i = 1; i < 100; i++) {
    fps_counter_present(&c, t + (uint32_t)i * 10);
    CHECK_EQ_INT(fps_counter_value(&c, t + (uint32_t)i * 10), 50);
  }
  fps_counter_present(&c, t + 1000);
  CHECK_EQ_INT(fps_counter_value(&c, t + 1000), 100);
}

static void test_stall_reads_low(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  uint32_t t = run(&c, present, 0, 16667, 121);
  CHECK_EQ_INT(fps_counter_value(&c, t), 60);
  // Nothing for 10 s (a modal), then one present: one frame in 10 s.
  fps_counter_present(&c, t + 10000);
  CHECK_EQ_INT(fps_counter_value(&c, t + 10000), 0);
  // Back to 60 Hz: the next window reads 60 again.
  uint32_t t2 = run(&c, present, t + 10000 + 16, 16667, 61);
  CHECK_EQ_INT(fps_counter_value(&c, t2), 60);
}

static void test_rows_sweeps(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  // Four bands per frame, top to bottom, 30 frames a second: 30, not 120.
  uint32_t t = 0;
  for (int f = 0; f <= 60; f++) {
    t = (uint32_t)f * 1000u / 30u;
    for (int b = 0; b < 4; b++)
      fps_counter_rows(&c, b * 80, t);
  }
  CHECK_EQ_INT(fps_counter_value(&c, t), 30);

  // Full-screen flushRows every frame: each call is a frame (y0 == previous).
  fps_counter_reset(&c);
  for (int f = 0; f <= 120; f++)
    fps_counter_rows(&c, 0, (uint32_t)f * 1000u / 60u);
  CHECK_EQ_INT(fps_counter_value(&c, 2000), 60);

  // One band that moves down the screen: the same sweep until it wraps.
  fps_counter_reset(&c);
  for (int f = 0; f <= 100; f++)
    fps_counter_rows(&c, (f % 10) * 30, (uint32_t)f * 20u);  // 50 calls/s, 5 sweeps/s
  CHECK_EQ_INT(fps_counter_value(&c, 2000), 5);
}

static void test_rows_ignored_next_to_flushes(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  // A status band via flushRows plus a full flush, every frame at 30 Hz:
  // the flushes are the frames.
  uint32_t t = 0;
  for (int f = 0; f <= 90; f++) {
    t = (uint32_t)f * 1000u / 30u;
    fps_counter_rows(&c, 0, t);
    fps_counter_present(&c, t + 1);
  }
  CHECK_EQ_INT(fps_counter_value(&c, t + 1), 30);

  // The app stops flushing and presents with flushRows only: after a
  // window without flushes its sweeps count again.
  uint32_t t0 = t + 1;
  for (int f = 1; f <= 180; f++) {
    t = t0 + (uint32_t)f * 1000u / 60u;
    fps_counter_rows(&c, 0, t);
  }
  CHECK_EQ_INT(fps_counter_value(&c, t), 60);
}

static void test_ticks_win_over_presents(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  // gbc: two game frames (perf ticks) per present, presents at 30 Hz.
  uint32_t t = 0;
  for (int f = 0; f <= 90; f++) {
    t = (uint32_t)f * 1000u / 30u;
    fps_counter_tick(&c, t);
    fps_counter_tick(&c, t + 16);
    fps_counter_present(&c, t + 16);
  }
  CHECK_EQ_INT(fps_counter_value(&c, t + 16), 60);

  // Ticks stop (a pause screen that still presents at 30 Hz): after 2 s
  // without a tick the present rate is shown.
  uint32_t last_tick = t + 16;
  for (int f = 1; f <= 90; f++) {
    t = last_tick + (uint32_t)f * 1000u / 30u;
    fps_counter_present(&c, t);
  }
  CHECK(t - last_tick >= 2000);
  CHECK_EQ_INT(fps_counter_value(&c, t), 30);
  CHECK_EQ_INT(fps_counter_value(&c, last_tick + 1999), 60);
}

static void test_ticks_shown_only_after_a_tick_window(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  uint32_t t = run(&c, present, 0, 50000, 41);  // 20 Hz presents, 2 s
  CHECK_EQ_INT(fps_counter_value(&c, t), 20);
  // The app starts ticking: until a tick window completes, presents rule.
  fps_counter_tick(&c, t + 1);
  fps_counter_tick(&c, t + 500);
  CHECK_EQ_INT(fps_counter_value(&c, t + 500), 20);
  fps_counter_tick(&c, t + 1001);
  CHECK_EQ_INT(fps_counter_value(&c, t + 1001), 2);

  // Ticks alone (no presents at all): the game-frame rate, 59.73 Hz.
  fps_counter_reset(&c);
  t = run(&c, tick, 0, 16742, 200);
  CHECK_EQ_INT(fps_counter_value(&c, t), 60);
}

static void test_clock_wrap(void) {
  fps_counter_t c;
  fps_counter_reset(&c);
  uint32_t t = run(&c, present, 0xFFFFFFFFu - 500u, 16667, 121);
  CHECK(t < 2000);  // wrapped past zero
  CHECK_EQ_INT(fps_counter_value(&c, t), 60);
}

int main(void) {
  test_nothing_until_a_window_completes();
  test_present_rates();
  test_value_changes_once_per_window();
  test_stall_reads_low();
  test_rows_sweeps();
  test_rows_ignored_next_to_flushes();
  test_ticks_win_over_presents();
  test_ticks_shown_only_after_a_tick_window();
  test_clock_wrap();
  return check_report("test_fps_counter");
}
