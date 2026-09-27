// Host unit tests for src/drivers/kbd_bus.c: what the asynchronous keyboard
// bus engine (kbd_i2c.c) runs next, the raw-item ring kbd_poll() drains,
// discard generations, battery/backlight state and the recovery back-off.
// Times are time_us_32() values; T0 is an arbitrary start.
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

// Init and run the start-up battery read, so a FIFO read is what comes next.
static void start(uint32_t now) {
  kbd_bus_init(&b, now);
  CHECK_EQ_INT(next(now, NULL), KBD_JOB_BATTERY);
  kbd_bus_battery_result(&b, 50, now);
}

static void test_startup_reads_battery_then_fifo(void) {
  kbd_bus_init(&b, T0);
  CHECK_EQ_INT(b.battery, -1);
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_BATTERY);
  kbd_bus_battery_result(&b, 0x80 | 57, T0 + 6000);
  CHECK_EQ_INT(b.battery, 57);
  CHECK(b.charging);
  CHECK_EQ_INT(next(T0 + 6000, NULL), KBD_JOB_FIFO);
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

static void test_battery_period_and_retry(void) {
  start(T0);  // battery read at T0
  CHECK_EQ_INT(next(T0, NULL), KBD_JOB_FIFO);
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 5700);
  CHECK(next(T0 + KBD_BUS_BATTERY_US - 1, NULL) != KBD_JOB_BATTERY);
  uint32_t t = T0 + KBD_BUS_BATTERY_US;
  CHECK_EQ_INT(next(t, NULL), KBD_JOB_BATTERY);
  kbd_bus_job_failed(&b, KBD_JOB_BATTERY, 0);
  CHECK(next(t + KBD_BUS_BATTERY_RETRY_US - 1, NULL) != KBD_JOB_BATTERY);
  CHECK_EQ_INT(next(t + KBD_BUS_BATTERY_RETRY_US, NULL), KBD_JOB_BATTERY);
  CHECK_EQ_INT(b.battery, 50);  // a failed read keeps the last good level
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
  for (unsigned i = 0; i < KBD_BUS_FAST_RECOVERIES - 1; i++) {
    kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);
    CHECK(kbd_bus_recover_due(&b, T0));
    kbd_bus_recovered(&b, T0);
  }
  kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);  // the 10th failure in a row
  CHECK(!kbd_bus_recover_due(&b, T0 + KBD_BUS_BACKOFF_US - 1));
  CHECK(kbd_bus_recover_due(&b, T0 + KBD_BUS_BACKOFF_US));
  kbd_bus_fifo_result(&b, KBD_BUS_FIFO_IDLE, 0, T0 + 1);  // one success
  CHECK_EQ_U32(b.fail_streak, 0);
  CHECK(kbd_bus_recover_due(&b, T0 + 1));
  CHECK_EQ_U32(b.stats.errors, KBD_BUS_FAST_RECOVERIES);
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
  for (unsigned i = 0; i < KBD_BUS_FAST_RECOVERIES; i++)
    kbd_bus_job_failed(&b, KBD_JOB_FIFO, 0);
  kbd_bus_recovered(&b, t);
  CHECK(kbd_bus_recover_due(&b, t + 3000000000u));  // 50 min later: due
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

int main(void) {
  test_startup_reads_battery_then_fifo();
  test_empty_fifo_waits_the_idle_interval();
  test_items_are_read_back_to_back_and_queued_in_order();
  test_full_ring_leaves_items_in_the_stm32();
  test_backlight_goes_first_and_latest_wins();
  test_failed_backlight_is_retried_unless_superseded();
  test_battery_period_and_retry();
  test_discard_drops_an_item_already_on_the_bus();
  test_discard_waits_for_an_empty_read_begun_after_it();
  test_discard_clears_queued_items();
  test_recovery_backs_off_after_repeated_failures();
  test_times_wrap();
  test_gap_counts_only_waits_after_an_empty_read();
  test_idle_interval_can_change();
  return check_report("test_kbd_bus");
}
