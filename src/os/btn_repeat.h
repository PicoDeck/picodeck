#pragma once
// Button auto-repeat: a press edge passes through, and a button still held
// repeats once the delay has run out, then every `rate` ms (the timing
// picocalc.input.setRepeat() sets). Pure: no SDK calls, host-tested in
// tests/unit/test_btn_repeat.c. picocalc.input.getButtonsRepeated() and
// picocalc.gamepad.getButtonsRepeated() each keep one btn_repeat_t.
#include <stdint.h>

#define BTN_REPEAT_BITS 32

typedef struct {
  // Per bit: when its next repeat is due (ms since boot). 0 = not armed:
  // released, or held since before a press this clock saw, so it must not
  // repeat until it is pressed again.
  uint32_t next[BTN_REPEAT_BITS];
} btn_repeat_t;

void btn_repeat_reset(btn_repeat_t *r);

// The press edges in `pressed`, plus a repeat edge for each button in `held`
// whose deadline has passed. delay_ms 0 turns repeat off (presses only).
// Advances the deadlines, so call it once per poll.
uint32_t btn_repeat_step(btn_repeat_t *r, uint32_t held, uint32_t pressed,
                         uint32_t now_ms, uint32_t delay_ms, uint32_t rate_ms);
