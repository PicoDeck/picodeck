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
// splash_finish() waits at least this long for the last status: past the end
// Core 1 is idle and needs one render plus a flush start to show it.
#define STATUS_GRACE_US 100000u
#define STOP_ACK_US 100000u

typedef struct {
  char status[STATUS_MAX], subtext[STATUS_MAX];
  uint32_t seq;  // bumped by every splash_set_status()
} text_t;

// done: splash_finish() after Core 1 showed the final frame with the latest
// status; stale: splash_finish() before it did; stopped: splash_stop().
enum { ST_NONE, ST_SKIPPED, ST_RUNNING, ST_DONE, ST_STALE, ST_STOPPED, ST_TIMEOUT };
static const char *const s_state_names[] = {"none",  "skipped", "running", "done",
                                            "stale", "stopped", "timeout"};

// Guards s_text. Claimed by splash_start(), given back by halt() once Core 1
// is reset; NULL outside, when splash_set_status() does nothing.
static spin_lock_t *s_lock;
static text_t s_text;
static uint64_t s_t0_us;             // the animation's t = 0
static volatile bool s_running;      // Core 1 owns the display
static volatile bool s_stop_req, s_stopped;
static volatile bool s_final;        // Core 1 has shown a frame at SPLASH_T_END...
static volatile uint32_t s_final_seq;  // ...with this status
static volatile bool s_core1_faulted;  // Core 1's HardFault handler is running
// Written by Core 1 while it runs, read by Core 0 after it stops.
static struct {
  int state;
  uint32_t first_light_ms, frames, anim_frames, render_sum_us, render_max_us;
  uint32_t render_max_at_ms;  // uptime at the slowest render's start
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
    if (us > s_st.render_max_us) {
      s_st.render_max_us = us;
      s_st.render_max_at_ms = (uint32_t)(a / 1000u);
    }
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

// Whether Core 1 has shown a final frame with the latest status.
static bool latest_shown(void) {
  const uint32_t irq = spin_lock_blocking(s_lock);
  const uint32_t seq = s_text.seq;
  spin_unlock(s_lock, irq);
  return s_final && s_final_seq == seq;
}

static void halt(bool wait_for_end) {
  if (!s_running) return;
  const uint64_t a = time_us_64();
  if (wait_for_end) {
    // Until the animation's end plus a margin or, when the boot outlasts
    // that (as it normally does), a grace from now for the last status.
    uint64_t until = s_t0_us + END_US + FINISH_MARGIN_US;
    if (until < a + STATUS_GRACE_US) until = a + STATUS_GRACE_US;
    while (time_us_64() < until && !latest_shown()) {
      watchdog_update();
      busy_wait_us_32(1000);
    }
  }
  const bool shown = wait_for_end && latest_shown();
  s_stop_req = true;
  const uint64_t ack = time_us_64();
  while (!s_stopped && !s_core1_faulted && time_us_64() - ack < STOP_ACK_US)
    busy_wait_us_32(100);
  if (s_core1_faulted) {
    // Resetting Core 1 now would cut off its fault screen and its reboot, and
    // the boot would go on to clear the crash record. Its handler reboots
    // after its 3 s screen; the boot watchdog, no longer fed, is the backstop.
    // Nothing returns from here, so the lock stays claimed.
    printf("[SPLASH] Core 1 faulted: waiting for its fault handler to reboot\n");
    for (;;) tight_loop_contents();
  }
  const bool acked = s_stopped;
  multicore_reset_core1();
  // Core 1 no longer touches the lock: give it back (only 8 are claimable).
  // Released first, in case the reset caught Core 1 holding it (a stop that
  // was not acknowledged).
  spin_lock_t *const lock = s_lock;
  s_lock = NULL;
  spin_unlock_unsafe(lock);
  spin_lock_unclaim(spin_lock_get_num(lock));
  display_wait_for_flush();
  display_set_owner_core(0);
  s_running = false;
  s_st.waited_ms = (uint32_t)((time_us_64() - a) / 1000u);
  s_st.handover_ms = now_ms();
  s_st.state = !acked ? ST_TIMEOUT : !wait_for_end ? ST_STOPPED
              : shown ? ST_DONE : ST_STALE;
  char line[256];
  splash_stats_format(line, sizeof line);
  printf("[SPLASH] %s\n", line);
}

void splash_finish(void) { halt(true); }
void splash_stop(void) { halt(false); }

void splash_fault_stop(void) {
  if (!s_running) return;
  if (get_core_num() != 0) {
    // Core 1 faulted while playing the splash: halt() must not reset it.
    s_core1_faulted = true;
    __dmb();
    return;
  }
  multicore_reset_core1();
  display_set_owner_core(0);
  s_running = false;
}

void splash_stats_format(char *buf, size_t n) {
  const uint32_t frames = s_st.frames;
  snprintf(buf, n,
           "state=%s first_light_ms=%lu frames=%lu anim_frames=%lu render_avg_us=%lu "
           "render_max_us=%lu render_max_at_ms=%lu ended_ms=%lu handover_ms=%lu waited_ms=%lu "
           "foreign_flushes=%lu",
           s_state_names[s_st.state], (unsigned long)s_st.first_light_ms,
           (unsigned long)frames, (unsigned long)s_st.anim_frames,
           (unsigned long)(frames ? s_st.render_sum_us / frames : 0),
           (unsigned long)s_st.render_max_us, (unsigned long)s_st.render_max_at_ms,
           (unsigned long)s_st.ended_ms,
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
