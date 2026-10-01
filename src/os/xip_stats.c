#include "xip_stats.h"
#include "launcher.h"

#include "hardware/structs/busctrl.h"
#include "hardware/structs/xip.h"
#include "hardware/regs/busctrl.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "umm_malloc.h"

#include <string.h>

// Bus fabric events (RP2350 datasheet 12.15.4.2; the SDK's
// bus_ctrl_perf_counter_t enum carries RP2040-style numbers, so the
// register header's values are used). Port 0 serves even 8-byte lines,
// port 1 odd ones.
static const uint8_t k_events[4] = {
    BUSCTRL_PERFSEL0_VALUE_XIP_MAIN0_STALL_DOWNSTREAM,
    BUSCTRL_PERFSEL0_VALUE_XIP_MAIN1_STALL_DOWNSTREAM,
    BUSCTRL_PERFSEL0_VALUE_XIP_MAIN0_ACCESS_CONTESTED,
    BUSCTRL_PERFSEL0_VALUE_XIP_MAIN1_ACCESS_CONTESTED,
};
#define PERFCTR_MAX 0x00ffffffu

typedef struct {
  uint64_t acc, hit, perf[4];
} totals_t;

// The window lives in QMI PSRAM, allocated by the first `xipstat reset`
// and kept: a dev tool gets no SRAM (only the totals sys.getStats needs
// are static).
typedef struct {
  totals_t base, end;             // the totals at its start and its freeze
  uint64_t t0_us, t1_us;
  bool frozen, app_seen, timer_on, saturated;
  repeating_timer_t timer;
} window_t;

static totals_t s_tot;            // everything harvested since boot
static window_t *s_win;
static uint64_t s_rate_acc, s_rate_hit;  // xip_stats_hit_rate_since_last's base
static bool s_rate_saturated;

// Reads and clears the hardware counters into s_tot (interrupts off).
static void harvest_locked(void) {
  uint32_t hit = xip_ctrl_hw->ctr_hit;
  uint32_t acc = xip_ctrl_hw->ctr_acc;
  xip_ctrl_hw->ctr_acc = 0;   // write to clear (WC); a read clears nothing
  xip_ctrl_hw->ctr_hit = 0;
  bool xip_sat = acc == UINT32_MAX || hit == UINT32_MAX;
  bool sat = xip_sat;
  s_tot.acc += acc;
  s_tot.hit += hit;
  if (busctrl_hw->perfctr_en) {
    for (int i = 0; i < 4; i++) {
      uint32_t v = busctrl_hw->counter[i].value;
      busctrl_hw->counter[i].value = 0;
      if (v >= PERFCTR_MAX)
        sat = true;
      s_tot.perf[i] += v;
    }
  }
  if (xip_sat)
    s_rate_saturated = true;
  if (sat && s_win)
    s_win->saturated = true;
}

static void freeze_locked(void) {
  s_win->end = s_tot;
  s_win->t1_us = time_us_64();
  s_win->frozen = true;
}

static bool harvest_cb(repeating_timer_t *rt) {
  (void)rt;
  uint32_t irq = save_and_disable_interrupts();
  harvest_locked();
  // The app the window measures: seen running, now gone.
  bool app = launcher_get_running_app_name() != NULL;
  if (app)
    s_win->app_seen = true;
  bool keep = true;
  if (!app && s_win->app_seen && !s_win->frozen) {
    freeze_locked();
    s_win->timer_on = keep = false;
  }
  restore_interrupts(irq);
  return keep;
}

void xip_stats_start(void) {
  if (!s_win) {
    s_win = umm_malloc(sizeof(*s_win));
    if (!s_win)
      return;
    memset(s_win, 0, sizeof(*s_win));
  }
  if (s_win->timer_on) {
    cancel_repeating_timer(&s_win->timer);
    s_win->timer_on = false;
  }
  uint32_t irq = save_and_disable_interrupts();
  if (!busctrl_hw->perfctr_en) {
    for (int i = 0; i < 4; i++)
      busctrl_hw->counter[i].sel = k_events[i];
    busctrl_hw->perfctr_en = 1;
  }
  harvest_locked();               // what came before is not this window's
  s_win->base = s_tot;
  s_win->t0_us = time_us_64();
  s_win->frozen = false;
  s_win->app_seen = launcher_get_running_app_name() != NULL;
  s_win->saturated = false;
  restore_interrupts(irq);
  // 10 ms: a port can stall at most 2 M cycles in it at 200 MHz (24-bit
  // counters hold 16.7 M); the XIP counters would take ~40 s to saturate.
  s_win->timer_on = add_repeating_timer_ms(-10, harvest_cb, NULL, &s_win->timer);
}

void xip_stats_stop(void) {
  if (!s_win)
    return;
  if (s_win->timer_on) {
    cancel_repeating_timer(&s_win->timer);
    s_win->timer_on = false;
  }
  uint32_t irq = save_and_disable_interrupts();
  harvest_locked();
  if (!s_win->frozen)
    freeze_locked();
  busctrl_hw->perfctr_en = 0;
  restore_interrupts(irq);
}

void xip_stats_get(xip_stats_t *out) {
  memset(out, 0, sizeof(*out));
  if (!s_win)
    return;
  uint32_t irq = save_and_disable_interrupts();
  harvest_locked();
  const window_t *w = s_win;
  totals_t end = w->frozen ? w->end : s_tot;
  uint64_t t1 = w->frozen ? w->t1_us : time_us_64();
  out->running = true;
  out->frozen = w->frozen;
  out->saturated = w->saturated;
  out->window_ms = (uint32_t)((t1 - w->t0_us) / 1000u);
  out->accesses = end.acc - w->base.acc;
  out->hits = end.hit - w->base.hit;
  out->stall[0] = end.perf[0] - w->base.perf[0];
  out->stall[1] = end.perf[1] - w->base.perf[1];
  out->contested[0] = end.perf[2] - w->base.perf[2];
  out->contested[1] = end.perf[3] - w->base.perf[3];
  restore_interrupts(irq);
}

int xip_stats_hit_rate_since_last(void) {
  uint32_t irq = save_and_disable_interrupts();
  harvest_locked();
  uint64_t acc = s_tot.acc - s_rate_acc;
  uint64_t hit = s_tot.hit - s_rate_hit;
  bool sat = s_rate_saturated;
  s_rate_acc = s_tot.acc;
  s_rate_hit = s_tot.hit;
  s_rate_saturated = false;
  restore_interrupts(irq);
  if (acc == 0 || sat)
    return -1;
  return (int)(hit * 100u / acc);
}

void xip_stats_set_core0_priority(bool high) {
  if (high)
    hw_set_bits(&busctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC0_BITS);
  else
    hw_clear_bits(&busctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC0_BITS);
}

bool xip_stats_core0_priority(void) {
  return (busctrl_hw->priority & BUSCTRL_BUS_PRIORITY_PROC0_BITS) != 0;
}
