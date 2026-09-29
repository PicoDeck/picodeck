#include "fps_counter.h"

#include <stdbool.h>

#define FPS_TICK_FRESH_MS 2000u  // ticks this recent make the tick rate the one shown

enum { FPS_SEEN_TICK = 1, FPS_SEEN_FULL = 2 };

static void meter_reset(fps_meter_t *m) {
  m->t0 = 0;
  m->n = FPS_RATE_NONE;
  m->rate = FPS_RATE_NONE;
}

static void meter_event(fps_meter_t *m, uint32_t now) {
  if (m->n == FPS_RATE_NONE) {  // the first event opens the first window
    m->t0 = now;
    m->n = 0;
    return;
  }
  if (m->n < FPS_RATE_NONE - 1)
    m->n++;
  uint32_t span = now - m->t0;
  if (span < FPS_WINDOW_MS)
    return;
  uint32_t rate = ((uint32_t)m->n * 1000u + span / 2) / span;
  m->rate = (uint16_t)(rate > FPS_RATE_MAX ? FPS_RATE_MAX : rate);
  m->t0 = now;
  m->n = 0;
}

void fps_counter_reset(fps_counter_t *c) {
  meter_reset(&c->presents);
  meter_reset(&c->ticks);
  c->last_tick = 0;
  c->last_full = 0;
  c->rows_y0 = -1;
  c->flags = 0;
}

void fps_counter_present(fps_counter_t *c, uint32_t now) {
  c->last_full = now;
  c->flags |= FPS_SEEN_FULL;
  c->rows_y0 = -1;
  meter_event(&c->presents, now);
}

void fps_counter_rows(fps_counter_t *c, int y0, uint32_t now) {
  bool new_sweep = c->rows_y0 < 0 || y0 <= c->rows_y0;
  c->rows_y0 = (int16_t)(y0 < 0 ? 0 : y0 > 0x7FFF ? 0x7FFF : y0);
  if ((c->flags & FPS_SEEN_FULL) && now - c->last_full < FPS_WINDOW_MS)
    return;  // the app presents with flush/flushRegion: those are its frames
  if (new_sweep)
    meter_event(&c->presents, now);
}

void fps_counter_tick(fps_counter_t *c, uint32_t now) {
  c->last_tick = now;
  c->flags |= FPS_SEEN_TICK;
  meter_event(&c->ticks, now);
}

int fps_counter_value(const fps_counter_t *c, uint32_t now) {
  if ((c->flags & FPS_SEEN_TICK) && c->ticks.rate != FPS_RATE_NONE &&
      now - c->last_tick < FPS_TICK_FRESH_MS)
    return c->ticks.rate;
  return c->presents.rate == FPS_RATE_NONE ? -1 : c->presents.rate;
}
