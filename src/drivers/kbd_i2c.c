// The asynchronous STM32 keyboard bus engine (Core 0).
//
// All STM32 traffic after kbd_init() (FIFO reads, the battery read, backlight
// writes) runs here as a state machine driven by the I2C1 interrupt (a
// transaction's STOP, or an abort) and one-shot timeouts on the engine's own
// TIMER0 hardware alarm (the STM32's 1 ms reply delay, the idle interval and
// per-phase timeouts). Nothing waits on the 10 kHz bus: kbd_poll() drains a
// ring of raw FIFO items. kbd_bus.c decides what runs next and holds that
// state.
//
// Why its own alarm, not the default alarm pool (issue #58): SDK 2.2.0's pool
// loses a slot whenever its two earliest alarms are both cancelled before its
// interrupt handler runs (cmake/picodeck_pico_time.cmake now patches that).
// The engine cancelled and re-added an alarm at almost every step, often with
// interrupts masked, so beside the CYW43 driver's cross-core wake-ups the
// pool leaked a few slots per video session with WiFi on. Once it was full,
// every re-arm failed and the engine sat in KI_ERROR, retrying ~10 times a
// second, until a reboot. A dedicated alarm cannot run out, and the engine no
// longer adds to the pool's traffic.
//
// Engine steps run with Core 0 interrupts disabled (a few us each) and task
// calls touch the shared state under the same mask, so the I2C handler, the
// alarm callback and the caller never interleave whatever the priorities.
// Reads stop when kbd_poll() has not run for a second (KBD_BUS_UNPOLLED_US)
// and resume at its next call, so a watchdog reset after Core 0 stalls finds
// the bus idle; kbd_i2c_halt() (the HardFault handler) stops the engine at
// the next job boundary for good.
// While the engine runs (not KI_OFF or KI_ERROR) exactly one timeout is armed,
// due at s_due_us; it is always armed last in a step. A callback for one
// since moved or cancelled is ignored (s_armed, s_due_us). A failed
// transaction stops the engine (KI_ERROR); the ~12 ms bit-banged recovery
// runs in task context from kbd_i2c_service(), which also fails a running
// engine whose timeout is missing or KI_ALARM_LATE_US overdue (lost), so a
// stuck state ends at the first poll after that. Stays on Core 0:
// sys.pauseBackground stops Core 1.
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

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdio.h>

#define KI_REPLY_DELAY_US 1000u    // STM32 prepares a register reply
#define KI_PHASE_TIMEOUT_US 10000u // one transaction: ~2-3 ms at 10 kHz
#define KI_PAUSE_WAIT_US 20000u    // how long a pause waits for a job to end
#define KI_ALARM_LATE_US 100000u   // a timeout this overdue was lost
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
static int s_alarm_num = -1;  // the engine's TIMER0 hardware alarm
static bool s_armed;          // a timeout is set, due at s_due_us
static uint32_t s_due_us;
static uint32_t s_arms;       // timeouts set: a step that re-armed made progress
static uint32_t s_isr_us, s_max_read_us, s_recoveries;
static uint64_t s_stats_at_us;  // 64-bit: the soak's window must not wrap
static uint32_t s_warned_streak;
// kbdstat: failures by cause, the first failure of the latest streak and the
// latest one, and the event trace (kbd_i2c_event_t).
static uint32_t s_why[KBD_I2C_WHY_COUNT], s_cuts;
static kbd_i2c_fail_t s_first_fail, s_last_fail;
static kbd_i2c_event_t s_trace[KBD_I2C_TRACE_LEN];
static uint32_t s_trace_n;  // events recorded (index = n % LEN)

static inline i2c_hw_t *ki_hw(void) { return i2c_get_hw(KBD_I2C_PORT); }

static void ki_fail(uint8_t why, uint32_t abrt_src);
static void ki_stop_locked(void);

static uint8_t ki_lines(void) {
  return (uint8_t)((gpio_get(KBD_PIN_SDA) ? 1u : 0u) |
                   (gpio_get(KBD_PIN_SCL) ? 2u : 0u));
}

// Interrupts disabled by the caller.
static void ki_trace(char ev, uint8_t state, uint32_t arg) {
  kbd_i2c_event_t *e = &s_trace[s_trace_n % KBD_I2C_TRACE_LEN];
  e->t_us = time_us_32();
  e->ev = ev;
  e->state = state;
  e->arg = arg > 0xFFFFu ? 0xFFFFu : (uint16_t)arg;
  s_trace_n++;
}

// Point the hardware alarm at us from now. A target already past is not
// armed by the SDK, so its interrupt is forced instead: either way the
// callback runs in the alarm's IRQ, never inline.
static void ki_set_target(uint32_t us) {
  if (hardware_alarm_set_target((uint)s_alarm_num, make_timeout_time_us(us)))
    hardware_alarm_force_irq((uint)s_alarm_num);
}

// Arm the one outstanding timeout. Always the last action of a step: its IRQ
// can be served the instant interrupts are re-enabled (e.g. by our own
// caller's restore_interrupts), so arming last keeps the step's state
// consistent for every path.
static void ki_arm(uint32_t us) {
  s_arms++;
  s_armed = true;
  s_due_us = time_us_32() + us;
  ki_set_target(us);
}

static void ki_disarm(void) {
  s_armed = false;
  hardware_alarm_cancel((uint)s_alarm_num);
}

// abrt_src: IC_TX_ABRT_SOURCE, read before anything clears it.
static void ki_fail(uint8_t why, uint32_t abrt_src) {
  i2c_hw_t *h = ki_hw();
  kbd_i2c_fail_t f = {
      .at_us = time_us_32() | 1u,
      .abrt_src = abrt_src,
      .raw_intr = (uint16_t)h->raw_intr_stat,
      .sys_mhz = (uint16_t)(clock_get_hz(clk_sys) / 1000000u),
      .status = (uint8_t)h->status,
      .why = why & 7u,
      .state = (uint8_t)s_state & 7u,
      .lines = ki_lines() & 3u,
      .job = (uint8_t)s_job & 3u,
      .txflr = (uint8_t)h->txflr & 31u,
      .rxflr = (uint8_t)h->rxflr,
  };
  s_why[why]++;
  s_last_fail = f;
  if (s_bus.fail_streak == 0) {
    s_first_fail = f;
    ki_trace('F', f.state, (uint32_t)why | (uint32_t)f.job << 8);
  }
  h->intr_mask = 0;
  (void)h->clr_intr;
  ki_disarm();
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
    uint32_t src = h->tx_abrt_source;
    (void)h->clr_tx_abrt;
    ki_fail(KBD_I2C_WHY_ABORT, src);
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
      ki_fail(KBD_I2C_WHY_SHORT, 0);
      return;
    }
    uint8_t b0 = (uint8_t)h->data_cmd;
    uint8_t b1 = (uint8_t)h->data_cmd;
    if (s_bus.fail_streak)  // this success ends a failure streak
      ki_trace('O', KI_READ, s_bus.fail_streak);
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
    if (s_bus.fail_streak)
      ki_trace('O', KI_BL_WRITE, s_bus.fail_streak);
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

// The timeout's IRQ (hardware_alarm_set_callback).
static void ki_alarm_irq(uint alarm_num) {
  (void)alarm_num;
  uint32_t irq = save_and_disable_interrupts();
  uint32_t t0 = time_us_32();
  int32_t early = (int32_t)(s_due_us - t0);
  if (!s_armed) {
    // Cancelled since it fired (its IRQ was already pending): nothing waits.
  } else if (early > 0) {
    // An earlier target's IRQ, still pending when the engine re-armed (the
    // SDK checks only the high word of the time): wait out the new one.
    ki_set_target((uint32_t)early);
  } else {
    s_armed = false;
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
      uint32_t arms = s_arms;
      ki_check();
      if (s_state == st && s_arms == arms)
        ki_fail(KBD_I2C_WHY_TIMEOUT, 0);
    }
  }
  s_isr_us += time_us_32() - t0;
  restore_interrupts(irq);
}

// Interrupts disabled by the caller.
static void ki_stop_locked(void) {
  ki_disarm();
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
    uint32_t waited = time_us_32() - t0;
    if (idle || waited >= KI_PAUSE_WAIT_US) {
      if (st != KI_OFF)  // an engine already off is not news
        ki_trace('P', (uint8_t)st, waited / 10u);
      if (!idle) {
        s_need_recover = true;  // cut mid-transaction: the STM32 may be mid-reply
        s_cuts++;
        ki_trace('C', (uint8_t)st, (uint32_t)s_job);
      }
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
    ki_trace('R', (uint8_t)s_state, s_need_recover);
  }
  restore_interrupts(irq);
}

void kbd_i2c_start(void) {
  if (s_started)
    return;
  kbd_bus_init(&s_bus, time_us_32());
  s_stats_at_us = time_us_64();
  irq_set_exclusive_handler(KBD_I2C_IRQ, ki_irq);
  // Core 0 (kbd_init), before Core 1 starts: the lowest free alarm, 0 (the
  // default pool has 3; Core 1's audio pool claims 2 by number).
  s_alarm_num = hardware_alarm_claim_unused(true);
  hardware_alarm_set_callback((uint)s_alarm_num, ki_alarm_irq);
  s_started = true;
  kbd_i2c_resume();
}

// Recover the I2C bus from a stuck state by:
//   1. De-initing the I2C peripheral to release GPIO control
//   2. Pulsing SCL 9 times to clock out any partial byte the STM32 is stuck in
//   3. Issuing an explicit STOP if SDA is still stuck low
//   4. Re-initing the I2C peripheral
// The engine is paused around it. Safe before kbd_i2c_start() (kbd_init).
// Busy-waits rather than sleeps, so nothing here takes a default-pool alarm.
// Returns the SDA/SCL levels before (bits 0-1) and after (bits 2-3).
static uint32_t ki_bus_clear(void) {
  uint32_t before = ki_lines();
  // Release the I2C peripheral so we can drive the pins manually.
  i2c_deinit(KBD_I2C_PORT);

  // SDA as floating input (pulled high) — no START generated during SCL pulses.
  gpio_init(KBD_PIN_SDA);
  gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
  gpio_pull_up(KBD_PIN_SDA);
  busy_wait_us(200);

  // Pre-load SCL HIGH before driving it as an output.
  gpio_init(KBD_PIN_SCL);
  gpio_put(KBD_PIN_SCL, 1);
  gpio_set_dir(KBD_PIN_SCL, GPIO_OUT);
  busy_wait_us(50);

  // 9 clock pulses — clocks out any partial byte in the STM32's shift register.
  for (int i = 0; i < 9; i++) {
    gpio_put(KBD_PIN_SCL, 0);
    busy_wait_us(50);
    gpio_put(KBD_PIN_SCL, 1);
    busy_wait_us(50);
  }

  // If SDA is still stuck low after clocking, issue an explicit STOP.
  // CRITICAL: SCL must go LOW before SDA goes LOW — SDA falling while SCL
  // is HIGH generates a START condition, which would confuse the STM32.
  if (!gpio_get(KBD_PIN_SDA)) {
    gpio_set_dir(KBD_PIN_SDA, GPIO_OUT);
    gpio_put(KBD_PIN_SCL, 0);
    busy_wait_us(50); // SCL low first
    gpio_put(KBD_PIN_SDA, 0);
    busy_wait_us(50); // SDA low (SCL is low — no START)
    gpio_put(KBD_PIN_SCL, 1);
    busy_wait_us(50); // SCL high
    gpio_put(KBD_PIN_SDA, 1);
    busy_wait_us(50); // SDA high while SCL high → STOP
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
  busy_wait_ms(10);

  // Re-initialize the I2C peripheral and restore GPIO functions.
  i2c_init(KBD_I2C_PORT, KBD_I2C_BAUD);
  gpio_set_function(KBD_PIN_SDA, GPIO_FUNC_I2C);
  gpio_set_function(KBD_PIN_SCL, GPIO_FUNC_I2C);
  gpio_pull_up(KBD_PIN_SDA);
  gpio_pull_up(KBD_PIN_SCL);
  return before | (sda_free ? 4u : 0u) | (scl_free ? 8u : 0u);
}

void kbd_i2c_recover(void) {
  kbd_i2c_pause();
  uint32_t lines = ki_bus_clear();
  if (s_started) {
    uint32_t irq = save_and_disable_interrupts();
    // A streak's first few clears, then one in 16 (the back-off repeats
    // them up to 10 times a second).
    if (s_bus.fail_streak <= 3 || (s_recoveries & 15u) == 0)
      ki_trace('X', (uint8_t)s_state, lines | (s_bus.fail_streak & 0xFFFu) << 4);
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
  uint32_t irq = save_and_disable_interrupts();
  ki_trace('K', (uint8_t)s_state, clock_get_hz(clk_sys) / 1000000u);
  restore_interrupts(irq);
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
  // A running engine always has its timeout armed; one that is missing or
  // long overdue was lost, and nothing else would ever move the engine on.
  ki_state_t st = s_state;
  if (st != KI_OFF && st != KI_ERROR &&
      (!s_armed || (int32_t)(now - s_due_us) > (int32_t)KI_ALARM_LATE_US))
    ki_fail(KBD_I2C_WHY_LOST, 0);
  // After an unpolled stretch the FIFO is read now, not at the next idle
  // alarm (500 ms away in USB storage mode).
  if (kbd_bus_note_poll(&s_bus, now) && s_state == KI_WAIT)
    ki_arm(KBD_BUS_MIN_WAIT_US);
  uint32_t streak = s_bus.fail_streak;
  uint8_t why = s_last_fail.why;
  bool recover = s_state == KI_ERROR && kbd_bus_recover_due(&s_bus, now);
  restore_interrupts(irq);
  if (streak >= 5 && !s_warned_streak) {
    printf("[KBD] warning: %lu consecutive I2C failures (why=%u wifi=%d)\n",
           (unsigned long)streak, why, wifi_get_status());
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

const char *kbd_i2c_state_name(uint8_t state) {
  static const char *const k_names[] = {"off",  "wait", "regw", "delay",
                                        "read", "blw",  "error"};
  return state < sizeof(k_names) / sizeof(k_names[0]) ? k_names[state] : "?";
}

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
  out->aborts = s_why[KBD_I2C_WHY_ABORT];
  out->timeouts = s_why[KBD_I2C_WHY_TIMEOUT];
  out->short_reads = s_why[KBD_I2C_WHY_SHORT];
  out->lost_alarms = s_why[KBD_I2C_WHY_LOST];
  out->cuts = s_cuts;
  restore_interrupts(irq);
  out->reads = st.fifo_reads;
  out->items = st.items;
  out->dropped = st.dropped;
  out->bat_reads = st.battery_reads;
  out->bl_writes = st.backlight_writes;
  out->errors = st.errors;
  out->max_gap_us = st.max_gap_us;
}

void kbd_i2c_get_failures(kbd_i2c_fail_t *first, kbd_i2c_fail_t *last) {
  uint32_t irq = save_and_disable_interrupts();
  if (first)
    *first = s_first_fail;
  if (last)
    *last = s_last_fail;
  restore_interrupts(irq);
}

int kbd_i2c_get_trace(kbd_i2c_event_t *out, int max) {
  uint32_t irq = save_and_disable_interrupts();
  uint32_t n = s_trace_n;
  int have = n < KBD_I2C_TRACE_LEN ? (int)n : KBD_I2C_TRACE_LEN;
  if (have > max)
    have = max;
  for (int i = 0; i < have; i++)
    out[i] = s_trace[(n - (uint32_t)have + (uint32_t)i) % KBD_I2C_TRACE_LEN];
  restore_interrupts(irq);
  return have;
}

void kbd_i2c_get_hw(kbd_i2c_hw_t *out) {
  i2c_hw_t *h = ki_hw();
  uint32_t irq = save_and_disable_interrupts();
  out->hcnt = h->fs_scl_hcnt;
  out->lcnt = h->fs_scl_lcnt;
  out->tar = h->tar;
  out->enable_status = h->enable_status;
  out->status = h->status;
  out->raw_intr = h->raw_intr_stat;
  out->intr_mask = h->intr_mask;
  out->pad_sda = pads_bank0_hw->io[KBD_PIN_SDA];
  out->pad_scl = pads_bank0_hw->io[KBD_PIN_SCL];
  out->ctrl_sda = io_bank0_hw->io[KBD_PIN_SDA].ctrl;
  out->ctrl_scl = io_bank0_hw->io[KBD_PIN_SCL].ctrl;
  out->lines = ki_lines();
  out->irq_on = irq_is_enabled(KBD_I2C_IRQ);
  out->alarm_num = (int8_t)s_alarm_num;
  out->armed = s_armed;
  out->due_in_us = s_armed ? (int32_t)(s_due_us - time_us_32()) : 0;
  restore_interrupts(irq);
}

void kbd_i2c_reset_stats(void) {
  uint32_t irq = save_and_disable_interrupts();
  kbd_bus_reset_stats(&s_bus);
  for (unsigned i = 0; i < sizeof(s_why) / sizeof(s_why[0]); i++)
    s_why[i] = 0;
  s_cuts = 0;
  // A streak still going keeps its records: the next failure would only
  // update "last", leaving "first" empty until the streak ended.
  if (s_bus.fail_streak == 0) {
    s_first_fail = (kbd_i2c_fail_t){0};
    s_last_fail = (kbd_i2c_fail_t){0};
  }
  s_trace_n = 0;
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
