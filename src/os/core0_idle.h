#ifndef CORE0_IDLE_H
#define CORE0_IDLE_H

// Core 0's idle windows (issue #28). perf_end_frame's pacing wait (Lua
// perf.endFrame, native perf->endFrame with a target FPS) tells Core 1 when
// it will wake, so memory-heavy Core 1 work (the MP3 decoder) can run while
// Core 0 sleeps and leaves the shared XIP cache and QMI alone.
//
// Lock-free: Core 0 writes, Core 1 reads two atomics. A window Core 1 reads
// just as Core 0 closes it costs at most one frame's overlap, nothing else.

#include <stdbool.h>
#include <stdint.h>

// Core 0: idle from `now_us` until `until_us` (the time_us_64 clock), then
// working again at `now_us`.
void core0_idle_begin(uint64_t now_us, uint64_t until_us);
void core0_idle_end(uint64_t now_us);

// Any core: microseconds left in the open window at `now_us`, 0 when Core 0
// is working.
uint32_t core0_idle_left_us(uint64_t now_us);

// Windows closed and their total length since boot (or the last reset).
void core0_idle_stats(uint32_t *windows, uint64_t *idle_us);
void core0_idle_reset_stats(void);

#endif  // CORE0_IDLE_H
