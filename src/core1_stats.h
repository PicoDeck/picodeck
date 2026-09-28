#pragma once

#include <stdint.h>

// Core 1's 1 ms service tick (main.c core1_entry), for the `audiostat` dev
// command: how long each tick's work took, and the timer fires that found
// the previous tick still waiting to start (merged into it: lost ticks).
typedef struct {
  uint32_t window_ms;  // since the last reset (since boot before one)
  uint32_t ticks;      // ticks run
  uint32_t over;       // ticks whose work took over 1 ms
  uint32_t missed;     // timer fires merged into a late tick
  uint32_t max_us;     // the longest tick
} core1_tick_stats_t;

void core1_get_tick_stats(core1_tick_stats_t *out);
// Core 1 zeroes the counters at its next tick (within ~1 ms, or after the
// tick in progress).
void core1_reset_tick_stats(void);
