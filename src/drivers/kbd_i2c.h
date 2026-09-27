#ifndef KBD_I2C_H
#define KBD_I2C_H

// The asynchronous STM32 keyboard bus engine (Core 0). See kbd_i2c.c.
// Every function here is task context (not for IRQ handlers).

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  const char *state;     // "off", "wait", "busy" or "error"
  uint32_t window_ms;    // since start-up or kbd_i2c_reset_stats()
  uint32_t reads, items, dropped, bat_reads, bl_writes, errors, recoveries;
  uint32_t max_gap_us;   // empty FIFO answer -> next FIFO read done
  uint32_t max_read_us;  // longest FIFO read, first byte to result
  uint32_t isr_us;       // time spent in the engine's interrupt handlers
  uint32_t interval_us;  // current FIFO re-read interval
  int battery;
} kbd_i2c_stats_t;

void kbd_i2c_start(void);        // after kbd_init()'s probe; idempotent
void kbd_i2c_pause(void);        // stop at a transaction boundary (<= ~20 ms)
void kbd_i2c_resume(void);       // restart after kbd_i2c_pause()
void kbd_i2c_recover(void);      // pause, bit-banged bus clear + re-init, resume
void kbd_i2c_apply_clock(void);  // pause, re-init the divider for clk_peri, resume
void kbd_i2c_service(void);      // from kbd_poll(): runs a due recovery
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
