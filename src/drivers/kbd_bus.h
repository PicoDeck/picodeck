#ifndef KBD_BUS_H
#define KBD_BUS_H

// Scheduling and bookkeeping for the asynchronous STM32 keyboard bus
// (kbd_i2c.c): which transaction runs next, the ring of raw FIFO items
// between the engine and kbd_poll(), discard generations, the battery and
// backlight state, the recovery back-off and the counters.
//
// Pure C (no SDK), host-tested in tests/unit/test_kbd_bus.c. Not
// thread-safe: kbd_i2c.c touches a kbd_bus_t only with Core 0 interrupts
// disabled. Times are time_us_32() values, which wrap every ~71.6 min, so
// every comparison is an unsigned elapsed-time test, never "now >= deadline".

#include <stdbool.h>
#include <stdint.h>

#define KBD_BUS_FIFO_IDLE 0  // item state for "FIFO empty" (KBD_FIFO_IDLE)
#define KBD_BUS_RING_LEN 16u // raw items buffered between polls (power of 2)
#define KBD_BUS_IDLE_US 10000u  // re-read the FIFO this long after it read empty
#define KBD_BUS_MIN_WAIT_US 500u  // shortest idle alarm
#define KBD_BUS_BATTERY_US 5000000u  // battery read period
#define KBD_BUS_BATTERY_RETRY_US 2000000u  // after a failed battery read
#define KBD_BUS_FAST_RECOVERIES 10u  // failures in a row recovered at once,
#define KBD_BUS_BACKOFF_US 100000u   // then at most one recovery per 100 ms

typedef enum {
  KBD_JOB_NONE = 0,  // nothing due: wait *wait_us, then ask again
  KBD_JOB_BACKLIGHT, // write *value to the backlight register
  KBD_JOB_BATTERY,   // read the battery register (2 bytes, level in byte 1)
  KBD_JOB_FIFO,      // read one FIFO item (2 bytes: state, keycode)
} kbd_job_t;

typedef struct {
  uint8_t state, key;
} kbd_bus_item_t;

typedef struct {
  uint32_t fifo_reads;       // FIFO transactions completed
  uint32_t items;            // non-empty items received
  uint32_t dropped;          // items discarded (kbd_bus_discard)
  uint32_t battery_reads;
  uint32_t backlight_writes;
  uint32_t errors;           // failed transactions
  uint32_t max_gap_us;       // longest empty answer -> next FIFO read done
} kbd_bus_stats_t;

typedef struct {
  kbd_bus_item_t ring[KBD_BUS_RING_LEN];
  uint8_t head, tail;       // free-running; queued = (uint8_t)(head - tail)
  uint8_t discard_gen;      // bumped by kbd_bus_discard()
  uint8_t discard_done;     // == discard_gen once a read begun after it read empty
  uint8_t read_gen;         // discard_gen when the FIFO read in flight began
  bool fifo_now;            // read the FIFO at the next chance
  bool idle_valid;          // fifo_at_us is an empty answer to time the next read from
  bool charging;
  int16_t backlight_req;    // level waiting to be written, -1 = none
  int16_t battery;          // percent, -1 until the first good read
  uint32_t idle_us;         // FIFO re-read interval once it reads empty
  uint32_t fifo_at_us;      // when the FIFO last read empty
  uint32_t battery_at_us;   // last battery attempt
  uint32_t battery_wait_us; // next attempt due this long after it (0 = now)
  uint32_t recover_at_us;   // last bus recovery
  uint32_t fail_streak;     // failed transactions in a row
  kbd_bus_stats_t stats;
} kbd_bus_t;

void kbd_bus_init(kbd_bus_t *b, uint32_t now_us);

// The next transaction, in priority order: a pending backlight level, a due
// battery read, a FIFO read (while items keep coming, or once the idle
// interval has passed, and only while the ring has room). KBD_JOB_NONE sets
// *wait_us instead. A FIFO job records the discard generation it began in.
kbd_job_t kbd_bus_next_job(kbd_bus_t *b, uint32_t now_us, uint8_t *value,
                           uint32_t *wait_us);

// Transaction outcomes. Any success ends a failure streak.
void kbd_bus_fifo_result(kbd_bus_t *b, uint8_t state, uint8_t key,
                         uint32_t now_us);
void kbd_bus_battery_result(kbd_bus_t *b, uint8_t raw, uint32_t now_us);
void kbd_bus_backlight_done(kbd_bus_t *b);
// A failed backlight write is retried unless a newer level is waiting; a
// failed battery read is retried after KBD_BUS_BATTERY_RETRY_US.
void kbd_bus_job_failed(kbd_bus_t *b, kbd_job_t job, uint8_t value);

// Bus recovery policy: at once for the first KBD_BUS_FAST_RECOVERIES
// failures in a row, then at most every KBD_BUS_BACKOFF_US.
bool kbd_bus_recover_due(const kbd_bus_t *b, uint32_t now_us);
void kbd_bus_recovered(kbd_bus_t *b, uint32_t now_us);

// After the engine was paused or recovered: read the FIFO at once, and do
// not time the pause as a polling gap.
void kbd_bus_restart(kbd_bus_t *b);

// Consumer side (kbd_poll): oldest item first.
bool kbd_bus_pop(kbd_bus_t *b, kbd_bus_item_t *out);

// Drop every queued item and every item the STM32 queued before this call:
// the engine drops what it reads until a FIFO read begun after this call
// reads empty (ui_confirm relies on it). Drains promptly.
void kbd_bus_discard(kbd_bus_t *b);

void kbd_bus_request_backlight(kbd_bus_t *b, uint8_t value);  // latest wins
void kbd_bus_set_idle_us(kbd_bus_t *b, uint32_t idle_us);  // 0 = default
void kbd_bus_reset_stats(kbd_bus_t *b);

#endif
