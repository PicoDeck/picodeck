#include "core0_idle.h"

#include <stdatomic.h>

// The window's end, low 32 bits of the time_us_64 clock (a window is far
// shorter than the 71-minute wrap), published before s_open.
static _Atomic uint32_t s_until;
static atomic_bool s_open;
// Core 0 only (stats; Core 1 may read them torn, which is harmless).
static uint64_t s_began_us;
static volatile uint32_t s_windows;
static volatile uint64_t s_idle_us;

void core0_idle_begin(uint64_t now_us, uint64_t until_us) {
  atomic_store_explicit(&s_until, (uint32_t)until_us, memory_order_relaxed);
  atomic_store_explicit(&s_open, true, memory_order_release);
  s_began_us = now_us;
}

void core0_idle_end(uint64_t now_us) {
  atomic_store_explicit(&s_open, false, memory_order_release);
  s_windows++;
  s_idle_us += now_us - s_began_us;
}

uint32_t core0_idle_left_us(uint64_t now_us) {
  if (!atomic_load_explicit(&s_open, memory_order_acquire))
    return 0;
  int32_t left = (int32_t)(atomic_load_explicit(&s_until, memory_order_relaxed) -
                           (uint32_t)now_us);
  return left > 0 ? (uint32_t)left : 0;
}

void core0_idle_stats(uint32_t *windows, uint64_t *idle_us) {
  *windows = s_windows;
  *idle_us = s_idle_us;
}

void core0_idle_reset_stats(void) {
  s_windows = 0;
  s_idle_us = 0;
}
