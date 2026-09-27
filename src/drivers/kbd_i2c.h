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
                         // excludes the alarm pool's own dispatch (a few
                         // timer IRQ entries per read), so it undercounts
  uint32_t interval_us;  // current FIFO re-read interval
  int battery;
} kbd_i2c_stats_t;

void kbd_i2c_start(void);        // after kbd_init()'s probe; idempotent
void kbd_i2c_pause(void);        // stop at a transaction boundary (<= ~20 ms)
void kbd_i2c_resume(void);       // restart after kbd_i2c_pause()
void kbd_i2c_recover(void);      // pause, bit-banged bus clear + re-init, resume
void kbd_i2c_apply_clock(void);  // pause, re-init the divider for clk_sys, resume
void kbd_i2c_service(void);      // from kbd_poll(): notes the poll (reads
                                 // stop after 1 s without one) and runs a
                                 // due recovery
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
void kbd_i2c_reset_stats(void);
void kbd_i2c_inject_fault(void); // the next transaction is NACKed (kbdstat fault)

#endif
