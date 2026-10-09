// The boot splash (splash.h).
#include "splash.h"

#include "../drivers/display.h"
#include "../drivers/display_raster.h"
#include "splash_render.h"

#include <stdint.h>
#include <stdio.h>

void splash_draw_static(const char *status, const char *subtext) {
  display_raster_target_t r;
  display_get_raster_target(&r);
  splash_render(r.fb, r.stride, r.swap, SPLASH_T_END, status, subtext);
  display_flush();
}

#ifndef PICODECK_SIMULATOR

#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/time.h"

#define STATUS_MAX 54  // 53 characters of the 6x8 font span the screen
#define END_US ((uint64_t)SPLASH_END_MS * 1000u)
#define FINISH_MARGIN_US 500000u
#define STOP_ACK_US 100000u

typedef struct {
  char status[STATUS_MAX], subtext[STATUS_MAX];
  uint32_t seq;  // bumped by every splash_set_status()
} text_t;

enum { ST_NONE, ST_SKIPPED, ST_RUNNING, ST_DONE, ST_STOPPED, ST_TIMEOUT };
static const char *const s_state_names[] = {"none", "skipped", "running",
                                            "done", "stopped", "timeout"};

static spin_lock_t *s_lock;          // guards s_text
static text_t s_text;
static uint64_t s_t0_us;             // the animation's t = 0
static volatile bool s_running;      // Core 1 owns the display
static volatile bool s_stop_req, s_stopped;
static volatile bool s_final;        // Core 1 has shown a frame at SPLASH_T_END...
static volatile uint32_t s_final_seq;  // ...with this status
// Written by Core 1 while it runs, read by Core 0 after it stops.
static struct {
  int state;
  uint32_t first_light_ms, frames, anim_frames, render_sum_us, render_max_us;
  uint32_t ended_ms, handover_ms, waited_ms;
} s_st;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void copy_text(char *dst, const char *src) {
  size_t i = 0;
  if (src)
    for (; src[i] && i < STATUS_MAX - 1; i++) dst[i] = src[i];
  dst[i] = '\0';
}

void splash_set_status(const char *status, const char *subtext) {
  if (!s_lock) return;
  const uint32_t irq = spin_lock_blocking(s_lock);
  copy_text(s_text.status, status);
  copy_text(s_text.subtext, subtext);
  s_text.seq++;
  spin_unlock(s_lock, irq);
}

// Core 1: frames until asked to stop. Past SPLASH_T_END the picture no
// longer moves, so it redraws only when the status changes.
static void splash_core1(void) {
  text_t text;
  uint32_t drawn_seq = UINT32_MAX;
  bool drew_final = false;
  while (!s_stop_req) {
    const uint64_t el = time_us_64() - s_t0_us;
    const bool final = el >= END_US;
    const uint32_t irq = spin_lock_blocking(s_lock);
    text = s_text;
    spin_unlock(s_lock, irq);
    if (final && drew_final && text.seq == drawn_seq) {
      busy_wait_us_32(2000);
      continue;
    }
    const float t = final ? SPLASH_T_END : (float)(uint32_t)el * 1e-6f;
    display_raster_target_t r;
    display_get_raster_target(&r);
    const uint64_t a = time_us_64();
    splash_render(r.fb, r.stride, r.swap, t, text.status, text.subtext);
    const uint32_t us = (uint32_t)(time_us_64() - a);
    display_flush();
    if (s_st.frames++ == 0) s_st.first_light_ms = now_ms();
    if (!final) s_st.anim_frames++;
    s_st.render_sum_us += us;
    if (us > s_st.render_max_us) s_st.render_max_us = us;
    drawn_seq = text.seq;
    if (final) {
      if (!drew_final) s_st.ended_ms = now_ms();
      drew_final = true;
      s_final_seq = text.seq;
      __dmb();
      s_final = true;
    }
  }
  display_wait_for_flush();
  __dmb();
  s_stopped = true;
  for (;;) __wfe();  // parked until multicore_reset_core1()
}

void splash_start(bool animate) {
  if (!animate) {
    s_st.state = ST_SKIPPED;
    splash_draw_static(NULL, NULL);
    return;
  }
  s_lock = spin_lock_init((uint)spin_lock_claim_unused(true));
  s_t0_us = time_us_64();
  s_st.state = ST_RUNNING;
  display_set_owner_core(1);
  s_running = true;
  __dmb();
  multicore_launch_core1(splash_core1);
}

bool splash_running(void) { return s_running; }

static void halt(bool wait_for_end) {
  if (!s_running) return;
  const uint64_t a = time_us_64();
  if (wait_for_end) {
    while (time_us_64() - s_t0_us < END_US + FINISH_MARGIN_US) {
      const uint32_t irq = spin_lock_blocking(s_lock);
      const uint32_t seq = s_text.seq;
      spin_unlock(s_lock, irq);
      if (s_final && s_final_seq == seq) break;
      watchdog_update();
      busy_wait_us_32(1000);
    }
  }
  s_stop_req = true;
  const uint64_t ack = time_us_64();
  while (!s_stopped && time_us_64() - ack < STOP_ACK_US) busy_wait_us_32(100);
  const bool acked = s_stopped;
  multicore_reset_core1();
  display_wait_for_flush();
  display_set_owner_core(0);
  s_running = false;
  s_st.waited_ms = (uint32_t)((time_us_64() - a) / 1000u);
  s_st.handover_ms = now_ms();
  s_st.state = !acked ? ST_TIMEOUT : (wait_for_end && s_final) ? ST_DONE : ST_STOPPED;
  char line[224];
  splash_stats_format(line, sizeof line);
  printf("[SPLASH] %s\n", line);
}

void splash_finish(void) { halt(true); }
void splash_stop(void) { halt(false); }

void splash_fault_stop(void) {
  if (!s_running || get_core_num() != 0) return;
  multicore_reset_core1();
  display_set_owner_core(0);
  s_running = false;
}

void splash_stats_format(char *buf, size_t n) {
  const uint32_t frames = s_st.frames;
  snprintf(buf, n,
           "state=%s first_light_ms=%lu frames=%lu anim_frames=%lu render_avg_us=%lu "
           "render_max_us=%lu ended_ms=%lu handover_ms=%lu waited_ms=%lu foreign_flushes=%lu",
           s_state_names[s_st.state], (unsigned long)s_st.first_light_ms,
           (unsigned long)frames, (unsigned long)s_st.anim_frames,
           (unsigned long)(frames ? s_st.render_sum_us / frames : 0),
           (unsigned long)s_st.render_max_us, (unsigned long)s_st.ended_ms,
           (unsigned long)s_st.handover_ms, (unsigned long)s_st.waited_ms,
           (unsigned long)display_foreign_flushes());
}

#else  // PICODECK_SIMULATOR: no Core 1, and main.c's boot does not run here

void splash_start(bool animate) {
  (void)animate;
  splash_draw_static(NULL, NULL);
}
bool splash_running(void) { return false; }
void splash_set_status(const char *status, const char *subtext) {
  (void)status;
  (void)subtext;
}
void splash_finish(void) {}
void splash_stop(void) {}
void splash_fault_stop(void) {}
void splash_stats_format(char *buf, size_t n) { snprintf(buf, n, "state=none"); }

#endif
