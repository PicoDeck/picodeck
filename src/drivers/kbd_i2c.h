#ifndef KBD_I2C_H
#define KBD_I2C_H

// The asynchronous STM32 keyboard bus engine (Core 0). See kbd_i2c.c.
// Every function here except kbd_i2c_halt() is task context (not for IRQ
// handlers). Requests made before kbd_i2c_start() (backlight, interval,
// discard) are dropped: its kbd_bus_init() clears them. No caller does that.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  const char *state;     // "off", "wait", "busy" or "error"
  uint32_t window_ms;    // since start-up or kbd_i2c_reset_stats()
  uint32_t reads, items, dropped, bat_reads, bl_writes, errors, recoveries;
  uint32_t max_gap_us;   // empty FIFO answer -> next FIFO read done
  uint32_t max_read_us;  // longest FIFO read, first byte to result
  uint32_t isr_us;       // time spent in the engine's interrupt handlers;
                         // excludes the SDK's timer IRQ dispatch, so it
                         // undercounts a little
  uint32_t interval_us;  // current FIFO re-read interval
  int battery;
  // Why transactions failed (they add up to errors), and the pauses that cut
  // a job mid-flight (not errors: the next service pass clears the bus).
  uint32_t aborts;       // the I2C block aborted (kbd_i2c_fail_t.abrt_src)
  uint32_t timeouts;     // a phase did not finish in time
  uint32_t short_reads;  // a read ended with fewer than 2 bytes
  uint32_t lost_alarms;  // a running engine's timeout never fired
  uint32_t cuts;
} kbd_i2c_stats_t;

// A failed transaction, captured as it failed (kbdstat): the first failure of
// the latest failure streak, and the latest failure.
// 16 bytes: the registers' widths (RP2350) bound the fields.
typedef struct {
  uint32_t at_us;     // time_us_32() of the failure; 0: none yet
  uint32_t abrt_src;  // IC_TX_ABRT_SOURCE (aborts only)
  uint16_t raw_intr;  // IC_RAW_INTR_STAT (13 bits)
  uint16_t sys_mhz;   // clk_sys then
  uint8_t status;     // IC_STATUS (7 bits)
  uint8_t why : 3;    // KBD_I2C_WHY_*
  uint8_t state : 3;  // the engine state it failed in (kbd_i2c_state_name)
  uint8_t lines : 2;  // bit 0 SDA, bit 1 SCL (1 = high)
  uint8_t job : 2;    // kbd_job_t
  uint8_t txflr : 5;  // IC_TXFLR (0-16)
  uint8_t rxflr;      // IC_RXFLR (0-16)
} kbd_i2c_fail_t;

enum {
  KBD_I2C_WHY_ABORT,
  KBD_I2C_WHY_TIMEOUT,
  KBD_I2C_WHY_SHORT,
  KBD_I2C_WHY_LOST,
  KBD_I2C_WHY_COUNT,
};

// The engine's recent history (kbdstat log): pauses, resumes and clock
// changes, the first failure of each streak, bus clears and the end of each
// streak. KBD_I2C_TRACE_LEN entries, oldest overwritten.
#define KBD_I2C_TRACE_LEN 8
typedef struct {
  uint32_t t_us;   // time_us_32()
  char ev;         // P pause, C cut, R resume, K clock, F failure,
                   // X bus clear, O streak over
  uint8_t state;   // engine state (P: on entry, R: after)
  uint16_t arg;    // P: us waited / 10; C: job; R: recovery pending;
                   // K: clk_sys MHz; F: why | job << 8; X: lines before
                   // (bits 0-1) and after (2-3), failures in a row << 4;
                   // O: failures in the streak (capped)
} kbd_i2c_event_t;

// The I2C block, the pins and the engine's alarm right now (kbdstat hw).
typedef struct {
  uint32_t hcnt, lcnt;      // IC_FS_SCL_HCNT/LCNT: the divider for clk_sys
  uint32_t tar, enable_status, status, raw_intr, intr_mask;
  uint32_t pad_sda, pad_scl;    // PADS_BANK0 GPIOn
  uint32_t ctrl_sda, ctrl_scl;  // IO_BANK0 GPIOn_CTRL (funcsel, overrides)
  int32_t due_in_us;        // the armed timeout, from now
  int8_t alarm_num;         // the engine's TIMER0 hardware alarm
  uint8_t lines;            // bit 0 SDA, bit 1 SCL (1 = high)
  bool irq_on, armed;
} kbd_i2c_hw_t;

void kbd_i2c_start(void);        // after kbd_init()'s probe; idempotent
void kbd_i2c_pause(void);        // stop at a transaction boundary (<= ~20 ms)
void kbd_i2c_resume(void);       // restart after kbd_i2c_pause()
void kbd_i2c_recover(void);      // pause, bit-banged bus clear + re-init, resume
void kbd_i2c_apply_clock(void);  // pause, re-init the divider for clk_sys, resume
void kbd_i2c_service(void);      // from kbd_poll(): notes the poll (reads
                                 // stop after 1 s without one), fails a
                                 // running engine whose timeout was lost and
                                 // runs a due recovery
// Stop for good at the next transaction boundary, for a reset nobody
// prepared (the HardFault handler, either core): one store, no locks, no SDK
// calls. Never cleared; kbd_i2c_resume() and the recovery stay off after it.
void kbd_i2c_halt(void);
bool kbd_i2c_pop(uint8_t *state, uint8_t *keycode);
void kbd_i2c_discard(void);
void kbd_i2c_set_backlight(uint8_t level);
void kbd_i2c_set_interval_us(uint32_t us);  // 0 = default (KBD_BUS_IDLE_US)
int kbd_i2c_battery(void);       // percent, -1 until the first read
bool kbd_i2c_charging(void);
void kbd_i2c_get_stats(kbd_i2c_stats_t *out);
// first/last: see kbd_i2c_fail_t. Either may be NULL.
void kbd_i2c_get_failures(kbd_i2c_fail_t *first, kbd_i2c_fail_t *last);
// Copies up to max events, oldest first; returns how many.
int kbd_i2c_get_trace(kbd_i2c_event_t *out, int max);
void kbd_i2c_get_hw(kbd_i2c_hw_t *out);
const char *kbd_i2c_state_name(uint8_t state);
void kbd_i2c_reset_stats(void);  // the counters, failure records and trace
void kbd_i2c_inject_fault(void); // the next transaction is NACKed (kbdstat fault)

#endif
