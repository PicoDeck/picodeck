// Host unit tests for src/drivers/kbd_bus.c: what the asynchronous keyboard
// bus engine (kbd_i2c.c) runs next, the raw-item ring kbd_poll() drains,
// discard generations, battery/backlight state, the recovery back-off and
// the stop while nobody polls. Times are time_us_32() values; T0 is an
// arbitrary start.
#include "check.h"
#include "kbd_bus.h"

#define T0 1000000u

static kbd_bus_t b;

static kbd_job_t next(uint32_t now, uint32_t *wait) {
  uint8_t v = 0;
  uint32_t w = 0;
  kbd_job_t j = kbd_bus_next_job(&b, now, &v, &w);
  if (wait)
    *wait = w;
  return j;
}

// Init and run the start-up FIFO and battery reads, then restart (as after a
// pause), so a FIFO read is what comes next and no gap is being timed.
static void start(uint32_t now) {
  kbd_bus_init(&b, now);
  CHECK_EQ_INT(next(now, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, now);
  CHECK_EQ_INT(next(now, NULL), KBD_JOB_BATTERY);
  kbd_bus_battery_result(&b, 50, now);
  kbd_bus_restart(&b);
  kbd_bus_reset_stats(&b);
}

static void test_startup_reads_fifo_then_battery(void) {
  kbd_bus_init(&b, T0);
  CHECK_EQ_INT(b.battery, -1);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 5700);
  CHECK_EQ_INT(next(T0 + 5700, NULL), KBD_JOB_BATTERY);  // soon after start
  kbd_bus_battery_result(&b, 0x80 | 57, T0 + 11400);
  CHECK_EQ_INT(b.battery, 57);
  CHECK(b.charging);
  CHECK_EQ_INT(next(T0 + 11400, NULL), KBD_JOB_NONE);  // FIFO idle, battery done
}

static void test_empty_fifo_waits_the_idle_interval(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 5700);
  uint32_t w;
  CHECK_EQ_INT(next(T0 + 5700, &w), KBD_JOB_NONE);
  CHECK_EQ_U32(w, KBD_BUS_IDLE_US);
  CHECK_EQ_INT(next(T0 + 5700 + 4000, &w), KBD_JOB_NONE);
  CHECK_EQ_U32(w, KBD_BUS_IDLE_US - 4000);
  CHECK_EQ_INT(next(T0 + 5700 + KBD_BUS_IDLE_US - 200, &w), KBD_JOB_NONE);
  CHECK_EQ_U32(w, KBD_BUS_MIN_WAIT_US);  // never a busy-short alarm
  CHECK_EQ_INT(next(T0 + 5700 + KBD_BUS_IDLE_US, NULL), KBD_JOB_FIFO);
}

static void test_items_are_read_back_to_back_and_queued_in_order(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, 1, 'a', T0 + 100);
  CHECK_EQ_INT(next(T0 + 100, NULL), KBD_JOB_FIFO);  // more may wait: no idle gap
  kbd_bus_fifo_result(&b, 3, 'a', T0 + 200);
  kbd_bus_item_t it;
  CHECK(kbd_bus_pop(&b, &it));
  CHECK_EQ_INT(it.state, 1);
  CHECK_EQ_INT(it.key, 'a');
  CHECK(kbd_bus_pop(&b, &it));
  CHECK_EQ_INT(it.state, 3);
  CHECK(!kbd_bus_pop(&b, &it));
  CHECK_EQ_U32(b.stats.items, 2);
}

// Review Focus 5: an app that stops polling for seconds loses nothing: the
// engine stops reading once the ring is full, and the STM32 keeps the rest.
static void test_full_ring_leaves_items_in_the_stm32(void) {
  start(T0);
  uint32_t t = T0;
  for (unsigned i = 0; i < KBD_BUS_RING_LEN; i++) {
    CHECK_EQ_INT(next(t, NULL), KBD_JOB_FIFO);
    kbd_bus_fifo_result(&b, 1, (uint8_t)('a' + i), t);
    t += 6000;
  }
  uint32_t w;
  CHECK_EQ_INT(next(t, &w), KBD_JOB_NONE);
  CHECK_EQ_U32(w, KBD_BUS_IDLE_US);
  CHECK_EQ_U32(b.stats.dropped, 0);
  kbd_bus_item_t it;
  CHECK(kbd_bus_pop(&b, &it));
  CHECK_EQ_INT(it.key, 'a');
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_FIFO);  // room again: carry on reading
}

static void test_backlight_goes_first_and_latest_wins(void) {
  start(T0);
  kbd_bus_request_backlight(&b, 100);
  kbd_bus_request_backlight(&b, 40);
  uint8_t v = 0;
  uint32_t w;
  CHECK_EQ_INT(kbd_bus_next_job(&b, T0, &v, &w), KBD_JOB_BACKLIGHT);
  CHECK_EQ_INT(v, 40);
  kbd_bus_backlight_done(&b);
  CHECK_EQ_U32(b.stats.backlight_writes, 1);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
}

static void test_failed_backlight_is_retried_unless_superseded(void) {
  start(T0);
  uint8_t v = 0;
  uint32_t w;
  kbd_bus_request_backlight(&b, 80);
  CHECK_EQ_INT(kbd_bus_next_job(&b, T0, &v, &w), KBD_JOB_BACKLIGHT);
  kbd_bus_job_failed(&b, KBD_JOB_BACKLIGHT, v);
  CHECK_EQ_INT(kbd_bus_next_job(&b, T0, &v, &w), KBD_JOB_BACKLIGHT);
  CHECK_EQ_INT(v, 80);
  kbd_bus_request_backlight(&b, 20);  // arrives while 80 is on the bus
  kbd_bus_job_failed(&b, KBD_JOB_BACKLIGHT, 80);
  CHECK_EQ_INT(kbd_bus_next_job(&b, T0, &v, &w), KBD_JOB_BACKLIGHT);
  CHECK_EQ_INT(v, 20);
}

// Polled every 10 ms (as by an app's input.update()), reading the FIFO
// whenever it is due, from `from` to `to`: returns the first job that is
// neither a FIFO read nor a wait (the FIFO reads empty), or KBD_JOB_NONE.
static kbd_job_t run_polled(uint32_t from, uint32_t to, uint32_t *at) {
  for (uint32_t t = from; t - from <= to - from; t += 10000) {
    kbd_bus_note_poll(&b, t);
    kbd_job_t j = next(t, NULL);
    if (j == KBD_JOB_FIFO) {
      kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t);
      j = next(t, NULL);
    }
    if (j != KBD_JOB_NONE) {
      *at = t;
      return j;
    }
  }
  return KBD_JOB_NONE;
}

static void test_battery_period_and_retry(void) {
  start(T0);  // battery read at T0
  uint32_t at = 0;
  CHECK_EQ_INT(run_polled(T0, T0 + KBD_BUS_BATTERY_US - 1, &at), KBD_JOB_NONE);
  uint32_t t = T0 + KBD_BUS_BATTERY_US;
  CHECK_EQ_INT(run_polled(t, t, &at), KBD_JOB_BATTERY);
  kbd_bus_job_failed(&b, KBD_JOB_BATTERY, 0);
  CHECK_EQ_INT(run_polled(t + 10000, t + KBD_BUS_BATTERY_RETRY_US - 1, &at),
               KBD_JOB_NONE);
  CHECK_EQ_INT(run_polled(t + KBD_BUS_BATTERY_RETRY_US,
                          t + KBD_BUS_BATTERY_RETRY_US, &at),
               KBD_JOB_BATTERY);
  CHECK_EQ_INT(b.battery, 50);  // a failed read keeps the last good level
}

// Minor 2: a battery read that fell due during the idle wait goes after the
// FIFO read that is due at the same moment, never ahead of it.
static void test_due_fifo_read_goes_before_a_due_battery_read(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  uint32_t t = T0 + KBD_BUS_BATTERY_US - 5000;
  kbd_bus_note_poll(&b, t);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t);  // empty: idle wait begins
  t += KBD_BUS_IDLE_US;  // the battery fell due during the wait
  kbd_bus_note_poll(&b, t);
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, 1, 'k', t + 5700);  // a key: read on at once...
  CHECK_EQ_INT(next(t + 5700, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t + 11400);
  CHECK_EQ_INT(next(t + 11400, NULL), KBD_JOB_BATTERY);  // ...then its turn
}

// Review Focus 2: a key queued before ui_confirm opened must never answer it,
// even when the read carrying it was already on the bus.
static void test_discard_drops_an_item_already_on_the_bus(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);  // read in flight...
  kbd_bus_discard(&b);                          // ...when the dialog opens
  kbd_bus_fifo_result(&b, 1, 'y', T0 + 5700);   // queued before it: dropped
  kbd_bus_item_t it;
  CHECK(!kbd_bus_pop(&b, &it));
  CHECK_EQ_INT(next(T0 + 5700, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 11400);  // drained
  CHECK_EQ_INT(next(T0 + 11400 + KBD_BUS_IDLE_US, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, 1, '\r', T0 + 30000);  // typed at the dialog: kept
  CHECK(kbd_bus_pop(&b, &it));
  CHECK_EQ_INT(it.key, '\r');
  CHECK_EQ_U32(b.stats.dropped, 1);
}

static void test_discard_waits_for_an_empty_read_begun_after_it(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);  // begun before the discard
  kbd_bus_discard(&b);
  // Empty, but the STM32 answered before the discard: items queued between
  // its answer and the discard may still be in the FIFO.
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 5700);
  CHECK_EQ_INT(next(T0 + 5700, NULL), KBD_JOB_FIFO);  // discard drains at once
  kbd_bus_fifo_result(&b, 2, 'y', T0 + 11400);        // queued earlier: dropped
  CHECK_EQ_INT(next(T0 + 11400, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 17100);
  CHECK_EQ_INT(next(T0 + 17100 + KBD_BUS_IDLE_US, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, 1, 'n', T0 + 33000);
  kbd_bus_item_t it;
  CHECK(kbd_bus_pop(&b, &it));
  CHECK_EQ_INT(it.key, 'n');
  CHECK(!kbd_bus_pop(&b, &it));
}

static void test_discard_clears_queued_items(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, 1, 'a', T0 + 100);
  kbd_bus_discard(&b);
  kbd_bus_item_t it;
  CHECK(!kbd_bus_pop(&b, &it));
  CHECK_EQ_U32(b.stats.dropped, 1);
}

static void test_recovery_backs_off_after_repeated_failures(void) {
  start(T0);
  for (unsigned i = 0; i < KBD_BUS_FAST_RECOVERIES; i++) {  // 1st..10th
    kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);
    CHECK(kbd_bus_recover_due(&b, T0));
    kbd_bus_recovered(&b, T0);
  }
  kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);  // the 11th failure in a row
  CHECK(!kbd_bus_recover_due(&b, T0 + KBD_BUS_BACKOFF_US - 1));
  CHECK(kbd_bus_recover_due(&b, T0 + KBD_BUS_BACKOFF_US));
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 1);  // one success
  CHECK_EQ_U32(b.fail_streak, 0);
  CHECK(kbd_bus_recover_due(&b, T0 + 1));
  CHECK_EQ_U32(b.stats.errors, KBD_BUS_FAST_RECOVERIES + 1);
}

// A controller that stays silent (#58: every transaction failing for
// minutes) is retried once a second past KBD_BUS_SLOW_RECOVERIES failures in
// a row, not ten times: each try holds Core 0 for a ~12 ms bus clear. It is
// never given up on, and one answer restores the fast policy.
static void test_recovery_slows_down_but_never_stops(void) {
  start(T0);
  uint32_t t = T0;
  for (unsigned i = 1; i <= KBD_BUS_SLOW_RECOVERIES; i++) {
    kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);
    if (i > KBD_BUS_FAST_RECOVERIES) {
      CHECK(!kbd_bus_recover_due(&b, t + KBD_BUS_BACKOFF_US - 1));
      t += KBD_BUS_BACKOFF_US;
    }
    CHECK(kbd_bus_recover_due(&b, t));
    kbd_bus_recovered(&b, t);
  }
  for (unsigned i = 0; i < 1000; i++) {  // ~17 minutes of failures
    kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);
    CHECK(!kbd_bus_recover_due(&b, t + KBD_BUS_BACKOFF_US));
    CHECK(!kbd_bus_recover_due(&b, t + KBD_BUS_SLOW_BACKOFF_US - 1));
    t += KBD_BUS_SLOW_BACKOFF_US;
    CHECK(kbd_bus_recover_due(&b, t));
    kbd_bus_recovered(&b, t);
  }
  CHECK_EQ_U32(b.fail_streak, KBD_BUS_SLOW_RECOVERIES + 1000);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t + 5700);  // it answers
  CHECK_EQ_U32(b.fail_streak, 0);
  kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);  // a later failure: at once again
  CHECK(kbd_bus_recover_due(&b, t + 5700));
}

// Review Focus 1: time_us_32() wraps every ~71.6 min; scheduling must not
// stall across the wrap or on a stamp that is an hour old.
static void test_times_wrap(void) {
  uint32_t t = 0xFFFFF000u;
  start(t);
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t);
  uint32_t w;
  CHECK_EQ_INT(next(t + 5000u, &w), KBD_JOB_NONE);  // past the wrap
  CHECK_EQ_U32(w, KBD_BUS_IDLE_US - 5000u);
  CHECK_EQ_INT(next(t + KBD_BUS_IDLE_US, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t + KBD_BUS_IDLE_US + 5700u);
  CHECK_EQ_U32(b.stats.max_gap_us, KBD_BUS_IDLE_US + 5700u);
  for (unsigned i = 0; i <= KBD_BUS_FAST_RECOVERIES; i++)  // backing off
    kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);
  kbd_bus_recovered(&b, t);
  CHECK(!kbd_bus_recover_due(&b, t + KBD_BUS_BACKOFF_US - 1));
  CHECK(kbd_bus_recover_due(&b, t + 3000000000u));  // 50 min later: due
  // A poll across the wrap: reading carries on for a second after it.
  uint32_t p = 0xFFFFFFF0u;
  kbd_bus_init(&b, p - KBD_BUS_UNPOLLED_US / 2);
  kbd_bus_note_poll(&b, p);
  CHECK_EQ_INT(next(p + KBD_BUS_UNPOLLED_US - 1, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, p + KBD_BUS_UNPOLLED_US - 1);
  CHECK_EQ_INT(next(p + KBD_BUS_UNPOLLED_US + KBD_BUS_IDLE_US, NULL),
               KBD_JOB_NONE);
  CHECK(b.unpolled);
}

static void test_gap_counts_only_waits_after_an_empty_read(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, 1, 'a', T0 + 5700);  // no empty read before it
  CHECK_EQ_U32(b.stats.max_gap_us, 0);
  CHECK_EQ_INT(next(T0 + 5700, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 11400);
  CHECK_EQ_INT(next(T0 + 11400 + KBD_BUS_IDLE_US, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0,
                      T0 + 11400 + KBD_BUS_IDLE_US + 5700);
  CHECK_EQ_U32(b.stats.max_gap_us, KBD_BUS_IDLE_US + 5700);
  kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);  // a failure breaks the chain
  kbd_bus_restart(&b);
  CHECK_EQ_INT(next(T0 + 40000, NULL), KBD_JOB_FIFO);  // restart: read at once
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 900000);
  CHECK_EQ_U32(b.stats.max_gap_us, KBD_BUS_IDLE_US + 5700);  // not ~900 ms
  kbd_bus_reset_stats(&b);
  CHECK_EQ_U32(b.stats.fifo_reads, 0);
  CHECK_EQ_INT(b.battery, 50);  // state survives a stats reset
}

static void test_idle_interval_can_change(void) {
  start(T0);
  kbd_bus_set_idle_us(&b, 500000);  // USB storage mode polls slowly
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);  // ...starting with a read now
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0);
  uint32_t w;
  CHECK_EQ_INT(next(T0 + 10000, &w), KBD_JOB_NONE);
  CHECK_EQ_U32(w, 490000);
  kbd_bus_set_idle_us(&b, 0);  // 0 restores the default
  CHECK_EQ_U32(b.idle_us, KBD_BUS_IDLE_US);
  CHECK_EQ_INT(next(T0 + 10001, NULL), KBD_JOB_FIFO);
}

// Important 1(b): no kbd_poll() for KBD_BUS_UNPOLLED_US stops the FIFO and
// battery reads, so a reset nobody prepared for (the watchdog after Core 0
// stalls) finds the bus idle. The STM32 keeps the items meanwhile.
static void test_reads_stop_after_a_second_unpolled(void) {
  start(T0);  // start() polls at T0 (kbd_bus_init)
  uint32_t t = T0;
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t + 5700);
  t = T0 + KBD_BUS_UNPOLLED_US - 1;  // still within the second: reads go on
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t);
  CHECK(!b.unpolled);
  uint32_t reads = b.stats.fifo_reads, w = 0;
  t = T0 + KBD_BUS_UNPOLLED_US + KBD_BUS_IDLE_US;
  CHECK_EQ_INT(next(t, &w), KBD_JOB_NONE);
  CHECK(b.unpolled);
  CHECK_EQ_U32(w, KBD_BUS_IDLE_US);  // still wakes for backlight requests
  // Neither the FIFO (idle interval long past) nor the battery (due at
  // T0 + 5 s) is read, however long it lasts; the flag stays set.
  for (uint32_t s = 2; s <= 20; s++)
    CHECK_EQ_INT(next(T0 + s * KBD_BUS_UNPOLLED_US, NULL), KBD_JOB_NONE);
  // 71.6 min after the poll time_us_32() has wrapped and the stamp looks
  // half a second old again: the flag holds.
  CHECK_EQ_INT(next(T0 + KBD_BUS_UNPOLLED_US / 2, NULL), KBD_JOB_NONE);
  CHECK_EQ_U32(b.stats.fifo_reads, reads);
  CHECK_EQ_U32(b.stats.battery_reads, 0);
  // An item that arrived before the stop stays in the ring for the poll.
  kbd_bus_item_t it;
  CHECK(!kbd_bus_pop(&b, &it));
}

static void test_backlight_is_written_while_unpolled(void) {
  start(T0);
  uint32_t t = T0 + 3 * KBD_BUS_UNPOLLED_US;
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_NONE);
  CHECK(b.unpolled);
  kbd_bus_request_backlight(&b, 30);  // e.g. idle dimming at boot
  uint8_t v = 0;
  uint32_t w;
  CHECK_EQ_INT(kbd_bus_next_job(&b, t + KBD_BUS_IDLE_US, &v, &w),
               KBD_JOB_BACKLIGHT);
  CHECK_EQ_INT(v, 30);
  kbd_bus_job_failed(&b, KBD_JOB_BACKLIGHT, v);  // retried while unpolled too
  CHECK_EQ_INT(kbd_bus_next_job(&b, t + 2 * KBD_BUS_IDLE_US, &v, &w),
               KBD_JOB_BACKLIGHT);
  kbd_bus_backlight_done(&b);
  CHECK_EQ_U32(b.stats.backlight_writes, 1);
  CHECK_EQ_INT(next(t + 3 * KBD_BUS_IDLE_US, NULL), KBD_JOB_NONE);
}

static void test_a_poll_resumes_with_an_immediate_fifo_read(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  // The last read before the stop answered empty just before the second ran
  // out, so without the resume its idle interval would still be running.
  uint32_t t = T0 + KBD_BUS_UNPOLLED_US - 100;
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t);
  CHECK_EQ_INT(next(T0 + KBD_BUS_UNPOLLED_US, NULL), KBD_JOB_NONE);
  CHECK(b.unpolled);
  uint32_t p = T0 + KBD_BUS_UNPOLLED_US + 1000;  // 1.1 ms after that read
  CHECK(kbd_bus_note_poll(&b, p));                // ends the stretch
  CHECK(!b.unpolled);
  CHECK_EQ_INT(next(p, NULL), KBD_JOB_FIFO);      // at once, not in 8.9 ms
  kbd_bus_fifo_result(&b, 1, 'q', p + 5700);      // held in the STM32 meanwhile
  kbd_bus_item_t it;
  CHECK(kbd_bus_pop(&b, &it));
  CHECK_EQ_INT(it.key, 'q');
  CHECK_EQ_INT(next(p + 5700, NULL), KBD_JOB_FIFO);  // more may wait
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, p + 11400);
  uint32_t w;
  CHECK_EQ_INT(next(p + 11400, &w), KBD_JOB_NONE);  // back to the cadence
  CHECK_EQ_U32(w, KBD_BUS_IDLE_US);
  CHECK(!kbd_bus_note_poll(&b, p + 20000));  // an ordinary poll resumes nothing
}

static void test_no_gap_sample_spans_the_unpolled_stretch(void) {
  start(T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 5700);  // gap timing on
  uint32_t t = T0 + 5 * KBD_BUS_UNPOLLED_US;
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_NONE);
  kbd_bus_note_poll(&b, t);
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t + 5700);
  CHECK_EQ_U32(b.stats.max_gap_us, 0);  // not ~5 s
  CHECK_EQ_INT(next(t + 5700, NULL), KBD_JOB_BATTERY);  // overdue: its turn
  kbd_bus_battery_result(&b, 40, t + 5700);
  kbd_bus_note_poll(&b, t + 5700 + KBD_BUS_IDLE_US);
  CHECK_EQ_INT(next(t + 5700 + KBD_BUS_IDLE_US, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, t + 11400 + KBD_BUS_IDLE_US);
  CHECK_EQ_U32(b.stats.max_gap_us, KBD_BUS_IDLE_US + 5700);  // timed again
}

// Between kbd_init() (engine start) and the launcher's first kbd_poll() the
// engine reads for a second, then waits; config/backlight writes still land.
static void test_init_counts_as_a_poll(void) {
  kbd_bus_init(&b, T0);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 5700);
  CHECK_EQ_INT(next(T0 + 5700, NULL), KBD_JOB_BATTERY);
  kbd_bus_battery_result(&b, 88, T0 + 11400);
  CHECK_EQ_INT(next(T0 + KBD_BUS_UNPOLLED_US - 1, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + KBD_BUS_UNPOLLED_US - 1);
  CHECK(!b.unpolled);
  CHECK_EQ_INT(next(T0 + KBD_BUS_UNPOLLED_US + KBD_BUS_IDLE_US, NULL),
               KBD_JOB_NONE);
  CHECK(b.unpolled);
  CHECK_EQ_INT(b.battery, 88);  // the boot reading stays for the launcher
}

int main(void) {
  test_startup_reads_fifo_then_battery();
  test_empty_fifo_waits_the_idle_interval();
  test_items_are_read_back_to_back_and_queued_in_order();
  test_full_ring_leaves_items_in_the_stm32();
  test_backlight_goes_first_and_latest_wins();
  test_failed_backlight_is_retried_unless_superseded();
  test_battery_period_and_retry();
  test_due_fifo_read_goes_before_a_due_battery_read();
  test_discard_drops_an_item_already_on_the_bus();
  test_discard_waits_for_an_empty_read_begun_after_it();
  test_discard_clears_queued_items();
  test_recovery_backs_off_after_repeated_failures();
  test_recovery_slows_down_but_never_stops();
  test_times_wrap();
  test_gap_counts_only_waits_after_an_empty_read();
  test_idle_interval_can_change();
  test_reads_stop_after_a_second_unpolled();
  test_backlight_is_written_while_unpolled();
  test_a_poll_resumes_with_an_immediate_fifo_read();
  test_no_gap_sample_spans_the_unpolled_stretch();
  test_init_counts_as_a_poll();
  return check_report("test_kbd_bus");
}
