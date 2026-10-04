#include "btn_repeat.h"

#include <string.h>

void btn_repeat_reset(btn_repeat_t *r) { memset(r, 0, sizeof(*r)); }

// 0 means "not armed", so a deadline that lands on it moves to 1.
static uint32_t deadline(uint32_t t) { return t ? t : 1; }

uint32_t btn_repeat_step(btn_repeat_t *r, uint32_t held, uint32_t pressed,
                         uint32_t now_ms, uint32_t delay_ms, uint32_t rate_ms) {
  uint32_t out = pressed; // real edges always pass through unchanged
  if (delay_ms == 0)
    return out;
  for (int i = 0; i < BTN_REPEAT_BITS; i++) {
    uint32_t bit = 1u << i;
    if (!(held & bit)) {
      r->next[i] = 0; // released: disarm
      continue;
    }
    if (pressed & bit) {
      r->next[i] = deadline(now_ms + delay_ms); // fresh press: the delay
      continue;
    }
    if (r->next[i] == 0)
      continue; // held since before a press this clock saw
    // Signed difference, so this stays right across the 32-bit ms wrap
    // (~49.7 days of uptime).
    if ((int32_t)(now_ms - r->next[i]) >= 0) {
      out |= bit;
      r->next[i] = deadline(now_ms + rate_ms);
    }
  }
  return out;
}
