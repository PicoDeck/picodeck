// Host unit tests for src/os/btn_repeat.c: the auto-repeat clock behind
// picocalc.input.getButtonsRepeated() and picocalc.gamepad.getButtonsRepeated().
#include "check.h"
#include "btn_repeat.h"

#define DELAY 200
#define RATE 80
#define A (1u << 4)
#define B (1u << 5)

static btn_repeat_t r;

static uint32_t step(uint32_t held, uint32_t pressed, uint32_t now) {
  return btn_repeat_step(&r, held, pressed, now, DELAY, RATE);
}

static void test_press_then_repeats(void) {
  btn_repeat_reset(&r);
  CHECK_EQ_U32(step(A, A, 1000), A); // the press itself
  CHECK_EQ_U32(step(A, 0, 1100), 0); // inside the delay
  CHECK_EQ_U32(step(A, 0, 1199), 0);
  CHECK_EQ_U32(step(A, 0, 1200), A); // the delay is over: first repeat
  CHECK_EQ_U32(step(A, 0, 1279), 0);
  CHECK_EQ_U32(step(A, 0, 1280), A); // then one every RATE ms
  CHECK_EQ_U32(step(A, 0, 1360), A);
}

static void test_release_disarms(void) {
  btn_repeat_reset(&r);
  step(A, A, 1000);
  CHECK_EQ_U32(step(0, 0, 1100), 0); // released
  CHECK_EQ_U32(step(A, 0, 1300), 0); // held again, but no press edge seen
  CHECK_EQ_U32(step(A, A, 1400), A); // a new press arms it again
  CHECK_EQ_U32(step(A, 0, 1600), A);
}

static void test_held_before_reset_never_repeats(void) {
  btn_repeat_reset(&r);
  // Down since before the reset (an app launched with it held, or
  // input.clearState()): no press edge was seen, so it never repeats.
  CHECK_EQ_U32(step(A, 0, 1000), 0);
  CHECK_EQ_U32(step(A, 0, 5000), 0);
}

static void test_buttons_are_independent(void) {
  btn_repeat_reset(&r);
  CHECK_EQ_U32(step(A, A, 1000), A);
  CHECK_EQ_U32(step(A | B, B, 1100), B); // B pressed later
  CHECK_EQ_U32(step(A | B, 0, 1200), A); // A's delay ends first
  CHECK_EQ_U32(step(A | B, 0, 1280), A);
  CHECK_EQ_U32(step(A | B, 0, 1300), B); // B's delay ends 100 ms later
  CHECK_EQ_U32(step(A | B, 0, 1360), A);
  CHECK_EQ_U32(step(A | B, 0, 1380), B);
}

static void test_delay_zero_is_presses_only(void) {
  btn_repeat_reset(&r);
  CHECK_EQ_U32(btn_repeat_step(&r, A, A, 1000, 0, RATE), A);
  CHECK_EQ_U32(btn_repeat_step(&r, A, 0, 5000, 0, RATE), 0);
}

static void test_wraps_at_32_bits(void) {
  btn_repeat_reset(&r);
  uint32_t t = 0xFFFFFFFFu - 50;      // 51 ms before the clock wraps
  CHECK_EQ_U32(step(A, A, t), A);     // due at t + 200 = 149 after the wrap
  CHECK_EQ_U32(step(A, 0, 49), 0);    // t + 100
  CHECK_EQ_U32(step(A, 0, 149), A);
}

static void test_deadline_never_reads_as_unarmed(void) {
  btn_repeat_reset(&r);
  // now + DELAY wraps to exactly 0, the "not armed" value: stored as 1.
  CHECK_EQ_U32(step(A, A, 0u - DELAY), A);
  CHECK_EQ_U32(step(A, 0, 0), 0);
  CHECK_EQ_U32(step(A, 0, 1), A);
}

int main(void) {
  test_press_then_repeats();
  test_release_disarms();
  test_held_before_reset_never_repeats();
  test_buttons_are_independent();
  test_delay_zero_is_presses_only();
  test_wraps_at_32_bits();
  test_deadline_never_reads_as_unarmed();
  return check_report("test_btn_repeat");
}
