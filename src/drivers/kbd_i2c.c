// The asynchronous STM32 keyboard bus engine (Core 0).
//
// All STM32 traffic after kbd_init() (FIFO reads, the battery read, backlight
// writes) runs here as a state machine driven by the I2C1 interrupt (a
// transaction's STOP, or an abort) and one-shot alarms on the default alarm
// pool (the STM32's 1 ms reply delay, the idle interval and per-phase
// timeouts). Nothing waits on the 10 kHz bus: kbd_poll() drains a ring of raw
// FIFO items. kbd_bus.c decides what runs next and holds that state.
//
// Engine steps run with Core 0 interrupts disabled (a few us each) and task
// calls touch the shared state under the same mask, so the I2C handler, the
// alarm callback and the caller never interleave whatever the priorities.
// Reads stop when kbd_poll() has not run for a second (KBD_BUS_UNPOLLED_US)
// and resume at its next call, so a watchdog reset after Core 0 stalls finds
// the bus idle; kbd_i2c_halt() (the HardFault handler) stops the engine at
// the next job boundary for good.
// Exactly one alarm is outstanding while the engine runs, tagged with s_seq
// so a stale one (cancelled too late) is ignored. The next alarm is always
// armed last in a step. A failed transaction stops the engine
// (KI_ERROR); the ~12 ms bit-banged recovery runs in task context from
// kbd_i2c_service(). Stays on Core 0: sys.pauseBackground stops Core 1.
//
// pause/resume/recover, in words:
//   - kbd_i2c_pause returns with the engine OFF: no alarm, I2C interrupts
//     masked, NVIC line off. If a job was mid-flight it waits for it to
//     finish, up to 20 ms; if it has to cut the job, it sets s_need_recover.
//   - kbd_i2c_resume does nothing unless the engine is OFF. With
//     s_need_recover set, it parks the engine in KI_ERROR so the next
//     kbd_i2c_service() clears the bus before any new transaction.
//   - kbd_i2c_recover is pause -> bus clear -> clear s_need_recover -> resume.

#include "kbd_i2c.h"
#include "kbd_bus.h"
#include "../hardware.h"
#include "wifi.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdio.h>

#define KI_REPLY_DELAY_US 1000u    // STM32 prepares a register reply
#define KI_PHASE_TIMEOUT_US 10000u // one transaction: ~2-3 ms at 10 kHz
#define KI_PAUSE_WAIT_US 20000u    // how long a pause waits for a job to end
#define KI_FAULT_ADDR 0x7E         // kbdstat fault: an address nobody answers

typedef enum {
  KI_OFF,       // not started, or paused: interrupts masked, no alarm
  KI_WAIT,      // bus idle, alarm armed for the next job
  KI_REG_WRITE, // register address sent, waiting for its STOP
  KI_REG_DELAY, // STM32 preparing its reply (alarm)
  KI_READ,      // 2-byte read issued, waiting for its STOP
  KI_BL_WRITE,  // backlight write issued, waiting for its STOP
  KI_ERROR,     // stopped after a failure; kbd_i2c_service() recovers
} ki_state_t;

static kbd_bus_t s_bus;
static volatile ki_state_t s_state = KI_OFF;
static volatile bool s_pause_req;
static volatile bool s_halted;  // kbd_i2c_halt(): set once, never cleared
static bool s_started;
static bool s_need_recover;
static bool s_fault_once;
static kbd_job_t s_job;
static uint8_t s_job_value;
static uint32_t s_job_start_us;
static uint32_t s_seq;
static alarm_id_t s_alarm;
static uint32_t s_isr_us, s_max_read_us, s_recoveries;
static uint64_t s_stats_at_us;  // 64-bit: the soak's window must not wrap
static uint32_t s_warned_streak;

static inline i2c_hw_t *ki_hw(void) { return i2c_get_hw(KBD_I2C_PORT); }

static int64_t ki_alarm_cb(alarm_id_t id, void *user);
static void ki_fail(void);
static void ki_stop_locked(void);

// Arm the one outstanding alarm. Always the last action of a step: with
// fire_if_past, add_alarm_in_us forces the alarm IRQ pending rather than
// running the callback inline, and that IRQ can be served the instant
// interrupts are re-enabled (e.g. by our own caller's restore_interrupts) —
// arming last keeps the step's state consistent for every path.
static void ki_arm(uint32_t us) {
  if (s_alarm > 0)
    cancel_alarm(s_alarm);
  s_seq++;
  s_alarm = add_alarm_in_us(us, ki_alarm_cb, (void *)(uintptr_t)s_seq, true);
  if (s_alarm < 0) {  // pool exhausted: stop; the service pass restarts us
    s_alarm = 0;
    ki_fail();
  }
}

static void ki_fail(void) {
  ki_hw()->intr_mask = 0;
  (void)ki_hw()->clr_intr;
  if (s_alarm > 0)
    cancel_alarm(s_alarm);
  s_alarm = 0;
  s_seq++;
  kbd_bus_job_failed(&s_bus, s_job, s_job_value);
  s_need_recover = true;
  s_state = KI_ERROR;
}

// The bus is free: start the next job, or wait.
static void ki_next(void) {
  if (s_halted) {  // a fault handler is about to reset: stay off the bus
    ki_stop_locked();
    return;
  }
  if (s_pause_req) {
    s_state = KI_OFF;  // kbd_i2c_pause() finishes the stop
    return;
  }
  uint32_t now = time_us_32();
  uint8_t value = 0;
  uint32_t wait = 0;
  kbd_job_t job = kbd_bus_next_job(&s_bus, now, &value, &wait);
  s_job = job;
  s_job_value = value;
  s_job_start_us = now;
  if (job == KBD_JOB_NONE) {
    s_state = KI_WAIT;
    ki_arm(wait);
    return;
  }
  i2c_hw_t *h = ki_hw();
  (void)h->clr_intr;
  if (s_fault_once) {  // kbdstat fault: this transaction goes unanswered
    s_fault_once = false;
    h->enable = 0;
    h->tar = KI_FAULT_ADDR;
    h->enable = 1;
  }
  if (job == KBD_JOB_BACKLIGHT) {
    h->data_cmd = (uint32_t)(KBD_REG_BL | KBD_WRITE_MASK);
    h->data_cmd = I2C_IC_DATA_CMD_STOP_BITS | value;
    s_state = KI_BL_WRITE;
  } else {
    h->data_cmd = I2C_IC_DATA_CMD_STOP_BITS |
                  (job == KBD_JOB_FIFO ? KBD_REG_FIF : KBD_REG_BAT);
    s_state = KI_REG_WRITE;
  }
  ki_arm(KI_PHASE_TIMEOUT_US);
}

// Has the transaction on the bus finished? An abort (NACK) fails the job.
static void ki_check(void) {
  i2c_hw_t *h = ki_hw();
  uint32_t raw = h->raw_intr_stat;
  if (raw & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
    (void)h->clr_tx_abrt;
    ki_fail();
    return;
  }
  if (!(raw & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS))
    return;
  (void)h->clr_stop_det;
  uint32_t now = time_us_32();
  if (s_state == KI_REG_WRITE) {
    s_state = KI_REG_DELAY;
    ki_arm(KI_REPLY_DELAY_US);
  } else if (s_state == KI_READ) {
    if (h->rxflr < 2) {
      ki_fail();
      return;
    }
    uint8_t b0 = (uint8_t)h->data_cmd;
    uint8_t b1 = (uint8_t)h->data_cmd;
    if (s_job == KBD_JOB_FIFO) {
      uint32_t d = now - s_job_start_us;
      if (d > s_max_read_us)
        s_max_read_us = d;
      kbd_bus_fifo_result(&s_bus, b0, b1, now);
    } else {
      kbd_bus_battery_result(&s_bus, b1, now);
    }
    ki_next();
  } else if (s_state == KI_BL_WRITE) {
    kbd_bus_backlight_done(&s_bus);
    ki_next();
  }
}

static void ki_irq(void) {
  uint32_t irq = save_and_disable_interrupts();
  uint32_t t0 = time_us_32();
  if (s_state == KI_REG_WRITE || s_state == KI_READ || s_state == KI_BL_WRITE)
    ki_check();
  else
    (void)ki_hw()->clr_intr;  // e.g. a STOP after a timeout: nothing waits
  s_isr_us += time_us_32() - t0;
  restore_interrupts(irq);
}

static int64_t ki_alarm_cb(alarm_id_t id, void *user) {
  (void)id;
  uint32_t irq = save_and_disable_interrupts();
  uint32_t t0 = time_us_32();
  if ((uint32_t)(uintptr_t)user == s_seq) {
    s_alarm = 0;
    ki_state_t st = s_state;
    if (st == KI_WAIT) {
      ki_next();
    } else if (st == KI_REG_DELAY) {
      ki_hw()->data_cmd = I2C_IC_DATA_CMD_CMD_BITS;
      ki_hw()->data_cmd = I2C_IC_DATA_CMD_CMD_BITS | I2C_IC_DATA_CMD_STOP_BITS;
      s_state = KI_READ;
      ki_arm(KI_PHASE_TIMEOUT_US);
    } else if (st == KI_REG_WRITE || st == KI_READ || st == KI_BL_WRITE) {
      // Timeout, unless it finished while interrupts were masked (the timer
      // IRQ can be served before the I2C one).
      uint32_t seq = s_seq;
      ki_check();
      if (s_state == st && s_seq == seq)
        ki_fail();
    }
  }
  s_isr_us += time_us_32() - t0;
  restore_interrupts(irq);
  return 0;
}

// Interrupts disabled by the caller.
static void ki_stop_locked(void) {
  if (s_alarm > 0)
    cancel_alarm(s_alarm);
  s_alarm = 0;
  s_seq++;
  ki_hw()->intr_mask = 0;
  irq_set_enabled(KBD_I2C_IRQ, false);
  s_state = KI_OFF;
  s_pause_req = false;
}

void kbd_i2c_pause(void) {
  if (!s_started)
    return;
  uint32_t t0 = time_us_32();
  for (;;) {
    uint32_t irq = save_and_disable_interrupts();
    ki_state_t st = s_state;
    bool idle = st == KI_OFF || st == KI_WAIT || st == KI_ERROR;
    if (idle || time_us_32() - t0 >= KI_PAUSE_WAIT_US) {
      if (!idle)
        s_need_recover = true;  // cut mid-transaction: the STM32 may be mid-reply
      ki_stop_locked();
      restore_interrupts(irq);
      return;
    }
    s_pause_req = true;  // ki_next() stops at the job boundary
    restore_interrupts(irq);
    tight_loop_contents();
  }
}

void kbd_i2c_resume(void) {
  if (!s_started || s_halted)
    return;
  uint32_t irq = save_and_disable_interrupts();
  if (s_state == KI_OFF) {
    if (s_need_recover) {
      s_state = KI_ERROR;  // kbd_i2c_service() clears the bus first
    } else {
      i2c_hw_t *h = ki_hw();
      h->enable = 0;
      h->tar = KBD_I2C_ADDR;
      h->enable = 1;
      (void)h->clr_intr;
      // i2c_init() leaves every source unmasked (reset value 0x8ff).
      h->intr_mask =
          I2C_IC_INTR_MASK_M_STOP_DET_BITS | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;
      irq_set_enabled(KBD_I2C_IRQ, true);
      kbd_bus_restart(&s_bus);
      s_job = KBD_JOB_NONE;
      s_state = KI_WAIT;
      ki_arm(KBD_BUS_MIN_WAIT_US);
    }
  }
  restore_interrupts(irq);
}

void kbd_i2c_start(void) {
  if (s_started)
    return;
  kbd_bus_init(&s_bus, time_us_32());
  s_stats_at_us = time_us_64();
  irq_set_exclusive_handler(KBD_I2C_IRQ, ki_irq);
  s_started = true;
  kbd_i2c_resume();
}

// Recover the I2C bus from a stuck state by:
//   1. De-initing the I2C peripheral to release GPIO control
//   2. Pulsing SCL 9 times to clock out any partial byte the STM32 is stuck in
//   3. Issuing an explicit STOP if SDA is still stuck low
//   4. Re-initing the I2C peripheral
// The engine is paused around it. Safe before kbd_i2c_start() (kbd_init).
static void ki_bus_clear(void) {
  // Release the I2C peripheral so we can drive the pins manually.
  i2c_deinit(KBD_I2C_PORT);

  // SDA as floating input (pulled high) — no START generated during SCL pulses.
  gpio_init(KBD_PIN_SDA);
  gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
  gpio_pull_up(KBD_PIN_SDA);
  sleep_us(200);

  // Pre-load SCL HIGH before driving it as an output.
  gpio_init(KBD_PIN_SCL);
  gpio_put(KBD_PIN_SCL, 1);
  gpio_set_dir(KBD_PIN_SCL, GPIO_OUT);
  sleep_us(50);

  // 9 clock pulses — clocks out any partial byte in the STM32's shift register.
  for (int i = 0; i < 9; i++) {
    gpio_put(KBD_PIN_SCL, 0);
    sleep_us(50);
    gpio_put(KBD_PIN_SCL, 1);
    sleep_us(50);
  }

  // If SDA is still stuck low after clocking, issue an explicit STOP.
  // CRITICAL: SCL must go LOW before SDA goes LOW — SDA falling while SCL
  // is HIGH generates a START condition, which would confuse the STM32.
  if (!gpio_get(KBD_PIN_SDA)) {
    gpio_set_dir(KBD_PIN_SDA, GPIO_OUT);
    gpio_put(KBD_PIN_SCL, 0);
    sleep_us(50); // SCL low first
    gpio_put(KBD_PIN_SDA, 0);
    sleep_us(50); // SDA low (SCL is low — no START)
    gpio_put(KBD_PIN_SCL, 1);
    sleep_us(50); // SCL high
    gpio_put(KBD_PIN_SDA, 1);
    sleep_us(50); // SDA high while SCL high → STOP
    gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
    gpio_pull_up(KBD_PIN_SDA);
  }

  // Check final bus state — log if still stuck (helps diagnose STM32 issues).
  bool sda_free = gpio_get(KBD_PIN_SDA);
  bool scl_free = gpio_get(KBD_PIN_SCL);
  if (!sda_free || !scl_free)
    printf("[KBD] bus recovery: SDA=%s SCL=%s after 9-clock sequence\n",
           sda_free ? "high" : "LOW-STUCK", scl_free ? "high" : "LOW-STUCK");

  // Give the STM32 time to recognise the bus-free condition before we
  // re-assert a START.  Without this pause the STM32 may miss the STOP.
  sleep_ms(10);

  // Re-initialize the I2C peripheral and restore GPIO functions.
  i2c_init(KBD_I2C_PORT, KBD_I2C_BAUD);
  gpio_set_function(KBD_PIN_SDA, GPIO_FUNC_I2C);
  gpio_set_function(KBD_PIN_SCL, GPIO_FUNC_I2C);
  gpio_pull_up(KBD_PIN_SDA);
  gpio_pull_up(KBD_PIN_SCL);
}

void kbd_i2c_recover(void) {
  kbd_i2c_pause();
  ki_bus_clear();
  if (s_started) {
    uint32_t irq = save_and_disable_interrupts();
    s_need_recover = false;
    s_recoveries++;
    kbd_bus_recovered(&s_bus, time_us_32());
    restore_interrupts(irq);
  }
  kbd_i2c_resume();
}

void kbd_i2c_apply_clock(void) {
  // i2c_init() derives the divider from clk_sys (i2c_set_baudrate).
  kbd_i2c_pause();
  i2c_init(KBD_I2C_PORT, KBD_I2C_BAUD);
  gpio_set_function(KBD_PIN_SDA, GPIO_FUNC_I2C);
  gpio_set_function(KBD_PIN_SCL, GPIO_FUNC_I2C);
  gpio_pull_up(KBD_PIN_SDA);
  gpio_pull_up(KBD_PIN_SCL);
  kbd_i2c_resume();
}

void kbd_i2c_service(void) {
  if (!s_started || s_halted)
    return;
  uint32_t irq = save_and_disable_interrupts();
  uint32_t now = time_us_32();
  // After an unpolled stretch the FIFO is read now, not at the next idle
  // alarm (500 ms away in USB storage mode).
  if (kbd_bus_note_poll(&s_bus, now) && s_state == KI_WAIT)
    ki_arm(KBD_BUS_MIN_WAIT_US);
  uint32_t streak = s_bus.fail_streak;
  bool recover = s_state == KI_ERROR && kbd_bus_recover_due(&s_bus, now);
  restore_interrupts(irq);
  if (streak >= 5 && !s_warned_streak) {
    printf("[KBD] warning: %lu consecutive I2C failures (wifi=%d)\n",
           (unsigned long)streak, wifi_get_status());
    s_warned_streak = streak;
  } else if (streak == 0 && s_warned_streak) {
    printf("[KBD] I2C recovered after %lu+ failures\n",
           (unsigned long)s_warned_streak);
    s_warned_streak = 0;
  }
  if (recover)
    kbd_i2c_recover();
}

bool kbd_i2c_pop(uint8_t *state, uint8_t *keycode) {
  kbd_bus_item_t it;
  uint32_t irq = save_and_disable_interrupts();
  bool ok = kbd_bus_pop(&s_bus, &it);
  restore_interrupts(irq);
  if (ok) {
    *state = it.state;
    *keycode = it.key;
  }
  return ok;
}

void kbd_i2c_discard(void) {
  uint32_t irq = save_and_disable_interrupts();
  kbd_bus_discard(&s_bus);
  restore_interrupts(irq);
}

void kbd_i2c_set_backlight(uint8_t level) {
  uint32_t irq = save_and_disable_interrupts();
  kbd_bus_request_backlight(&s_bus, level);
  restore_interrupts(irq);
}

void kbd_i2c_set_interval_us(uint32_t us) {
  uint32_t irq = save_and_disable_interrupts();
  kbd_bus_set_idle_us(&s_bus, us);
  restore_interrupts(irq);
}

int kbd_i2c_battery(void) {
  return s_started ? s_bus.battery : -1;
}

bool kbd_i2c_charging(void) { return s_started && s_bus.charging; }

void kbd_i2c_get_stats(kbd_i2c_stats_t *out) {
  static const char *const k_names[] = {"off",  "wait", "busy", "busy",
                                        "busy", "busy", "error"};
  uint32_t irq = save_and_disable_interrupts();
  kbd_bus_stats_t st = s_bus.stats;
  out->state = k_names[s_state];
  out->window_ms = (uint32_t)((time_us_64() - s_stats_at_us) / 1000u);
  out->recoveries = s_recoveries;
  out->max_read_us = s_max_read_us;
  out->isr_us = s_isr_us;
  out->interval_us = s_bus.idle_us;
  out->battery = s_started ? s_bus.battery : -1;
  restore_interrupts(irq);
  out->reads = st.fifo_reads;
  out->items = st.items;
  out->dropped = st.dropped;
  out->bat_reads = st.battery_reads;
  out->bl_writes = st.backlight_writes;
  out->errors = st.errors;
  out->max_gap_us = st.max_gap_us;
}

void kbd_i2c_reset_stats(void) {
  uint32_t irq = save_and_disable_interrupts();
  kbd_bus_reset_stats(&s_bus);
  s_isr_us = 0;
  s_max_read_us = 0;
  s_recoveries = 0;
  s_stats_at_us = time_us_64();
  restore_interrupts(irq);
}

void kbd_i2c_halt(void) { s_halted = true; }

void kbd_i2c_inject_fault(void) {
  uint32_t irq = save_and_disable_interrupts();
  s_fault_once = true;
  restore_interrupts(irq);
}
