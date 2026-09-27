#include "kbd_bus.h"

#include <string.h>

static uint32_t elapsed(uint32_t now_us, uint32_t then_us) {
  return now_us - then_us;  // unsigned: right across the 32-bit wrap
}

static uint8_t queued(const kbd_bus_t *b) {
  return (uint8_t)(b->head - b->tail);
}

void kbd_bus_init(kbd_bus_t *b, uint32_t now_us) {
  memset(b, 0, sizeof(*b));
  b->backlight_req = -1;
  b->battery = -1;
  b->idle_us = KBD_BUS_IDLE_US;
  b->fifo_now = true;
  b->fifo_at_us = now_us;
  b->battery_at_us = now_us;  // battery_wait_us 0: read it after the FIFO
  b->recover_at_us = now_us;
  b->polled_at_us = now_us;
}

kbd_job_t kbd_bus_next_job(kbd_bus_t *b, uint32_t now_us, uint8_t *value,
                           uint32_t *wait_us) {
  if (b->backlight_req >= 0) {
    *value = (uint8_t)b->backlight_req;
    b->backlight_req = -1;
    return KBD_JOB_BACKLIGHT;
  }
  // Nobody has polled for a second: leave the STM32 alone (it keeps the
  // items) until kbd_bus_note_poll(). The flag is sticky so a stretch longer
  // than the 71.6 min wrap cannot look fresh again.
  if (!b->unpolled &&
      elapsed(now_us, b->polled_at_us) >= KBD_BUS_UNPOLLED_US)
    b->unpolled = true;
  bool room = queued(b) < KBD_BUS_RING_LEN;
  uint32_t since = elapsed(now_us, b->fifo_at_us);
  if (!b->unpolled && room && (b->fifo_now || since >= b->idle_us)) {
    b->fifo_now = false;
    b->read_gen = b->discard_gen;
    return KBD_JOB_FIFO;
  }
  // After a due FIFO read, never before it: a battery read that fell due
  // during the idle wait must not add its ~6 ms to a key's latency.
  if (!b->unpolled &&
      elapsed(now_us, b->battery_at_us) >= b->battery_wait_us) {
    b->battery_at_us = now_us;
    b->battery_wait_us = KBD_BUS_BATTERY_US;
    return KBD_JOB_BATTERY;
  }
  // A full ring waits a whole interval for kbd_poll() to drain it; the STM32
  // keeps what does not fit. Unpolled, the engine only looks for backlight
  // requests, once an interval.
  uint32_t w = (!b->unpolled && room && since < b->idle_us) ? b->idle_us - since
                                                            : b->idle_us;
  *wait_us = w < KBD_BUS_MIN_WAIT_US ? KBD_BUS_MIN_WAIT_US : w;
  return KBD_JOB_NONE;
}

bool kbd_bus_note_poll(kbd_bus_t *b, uint32_t now_us) {
  bool resumed = b->unpolled ||
                 elapsed(now_us, b->polled_at_us) >= KBD_BUS_UNPOLLED_US;
  b->polled_at_us = now_us;
  b->unpolled = false;
  if (resumed) {
    b->fifo_now = true;
    b->idle_valid = false;
  }
  return resumed;
}

void kbd_bus_fifo_result(kbd_bus_t *b, uint8_t state, uint8_t key,
                         uint32_t now_us) {
  b->fail_streak = 0;
  b->stats.fifo_reads++;
  if (b->idle_valid) {
    uint32_t gap = elapsed(now_us, b->fifo_at_us);
    if (gap > b->stats.max_gap_us)
      b->stats.max_gap_us = gap;
  }
  bool discarding = b->discard_done != b->discard_gen;
  if (state == KBD_BUS_FIFO_IDLE) {
    // Only an empty answer to a read begun after the discard proves that
    // nothing from before it is left in the STM32.
    if (discarding && b->read_gen == b->discard_gen)
      b->discard_done = b->discard_gen;
    b->fifo_at_us = now_us;
    b->idle_valid = true;
    return;
  }
  b->stats.items++;
  b->fifo_now = true;
  b->idle_valid = false;
  if (discarding || queued(b) >= KBD_BUS_RING_LEN) {
    b->stats.dropped++;
    return;
  }
  b->ring[b->head % KBD_BUS_RING_LEN] = (kbd_bus_item_t){state, key};
  b->head++;
}

void kbd_bus_battery_result(kbd_bus_t *b, uint8_t raw, uint32_t now_us) {
  (void)now_us;  // the period runs from the attempt (battery_at_us)
  b->fail_streak = 0;
  b->stats.battery_reads++;
  b->battery = (int16_t)(raw & 0x7F);
  b->charging = (raw & 0x80) != 0;
}

void kbd_bus_backlight_done(kbd_bus_t *b) {
  b->fail_streak = 0;
  b->stats.backlight_writes++;
}

void kbd_bus_job_failed(kbd_bus_t *b, kbd_job_t job, uint8_t value) {
  b->fail_streak++;
  b->stats.errors++;
  b->idle_valid = false;
  if (job == KBD_JOB_BACKLIGHT && b->backlight_req < 0)
    b->backlight_req = value;
  if (job == KBD_JOB_BATTERY)
    b->battery_wait_us = KBD_BUS_BATTERY_RETRY_US;
}

bool kbd_bus_recover_due(const kbd_bus_t *b, uint32_t now_us) {
  return b->fail_streak <= KBD_BUS_FAST_RECOVERIES ||
         elapsed(now_us, b->recover_at_us) >= KBD_BUS_BACKOFF_US;
}

void kbd_bus_recovered(kbd_bus_t *b, uint32_t now_us) {
  b->recover_at_us = now_us;
}

void kbd_bus_restart(kbd_bus_t *b) {
  b->fifo_now = true;
  b->idle_valid = false;
}

bool kbd_bus_pop(kbd_bus_t *b, kbd_bus_item_t *out) {
  if (b->head == b->tail)
    return false;
  *out = b->ring[b->tail % KBD_BUS_RING_LEN];
  b->tail++;
  return true;
}

void kbd_bus_discard(kbd_bus_t *b) {
  b->stats.dropped += queued(b);
  b->tail = b->head;
  b->discard_gen++;
  b->fifo_now = true;
}

void kbd_bus_request_backlight(kbd_bus_t *b, uint8_t value) {
  b->backlight_req = value;
}

void kbd_bus_set_idle_us(kbd_bus_t *b, uint32_t idle_us) {
  b->idle_us = idle_us ? idle_us : KBD_BUS_IDLE_US;
  b->fifo_now = true;
  b->idle_valid = false;
}

void kbd_bus_reset_stats(kbd_bus_t *b) {
  memset(&b->stats, 0, sizeof(b->stats));
}
