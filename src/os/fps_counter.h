#pragma once

// The counting rules of the OS FPS counter (system menu → Settings → Show
// FPS, drawn by os_overlay.c). Pure: no clock, no display; the host tests in
// tests/unit/test_fps_counter.c drive it with made-up times. Times are ms
// since any epoch; differences wrap correctly.
//
// The number shown is a rate over a window of at least FPS_WINDOW_MS: the
// events after the one that opened the window, per second of its span,
// rounded. The event that closes a window opens the next. Which events:
//   - perf.endFrame ticks, while the app ticks perf (its logical frame);
//   - otherwise presents: flush and flushRegion count one frame each, and a
//     flushRows call counts only when it starts a new sweep (its y0 is at or
//     above the previous call's) and no flush/flushRegion came within the
//     last FPS_WINDOW_MS (an app mixing the two is counted by its flushes).

#include <stdint.h>

#define FPS_WINDOW_MS   1000u
#define FPS_RATE_NONE   0xFFFFu   // rate: no window has completed yet
#define FPS_RATE_MAX    9999u

typedef struct {
  uint32_t t0;    // when the open window started
  uint16_t n;     // events since t0; FPS_RATE_NONE = no window open
  uint16_t rate;  // the last completed window's rate, or FPS_RATE_NONE
} fps_meter_t;

typedef struct {
  fps_meter_t presents;
  fps_meter_t ticks;
  uint32_t last_tick;  // the latest perf.endFrame
  uint32_t last_full;  // the latest flush / flushRegion
  int16_t rows_y0;     // the previous flushRows call's y0; -1 = none
  uint8_t flags;       // FPS_SEEN_*
} fps_counter_t;

void fps_counter_reset(fps_counter_t *c);

// flush() or flushRegion(): one frame.
void fps_counter_present(fps_counter_t *c, uint32_t now);

// flushRows(y0, ...): one frame when it starts a new sweep (see above).
void fps_counter_rows(fps_counter_t *c, int y0, uint32_t now);

// perf.endFrame(): one logical frame.
void fps_counter_tick(fps_counter_t *c, uint32_t now);

// The number to show: the tick rate while the app has ticked perf in the
// last 2 s and a tick window has completed, else the present rate; -1 until
// a window has completed.
int fps_counter_value(const fps_counter_t *c, uint32_t now);
