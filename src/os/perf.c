#include "perf.h"
#include "os_overlay.h"
#include "core0_idle.h"
#include "pico/stdlib.h"
#ifndef PICODECK_SIMULATOR
#include "xip_stats.h"
#endif
#include <stdio.h>
#include <string.h>

#define PERF_SAMPLES 30

// getFPS: frames presented per second, from the last PERF_SAMPLES periods
// between endFrame returns, so setTargetFPS's wait counts (a game paced at
// 30 reports 30 however little each frame works). getFrameTime: the last
// frame's work in ms, from the previous endFrame's return (or beginFrame,
// for the first frame) to this endFrame's call, excluding the wait.
static uint32_t s_perf_periods_us[PERF_SAMPLES] = {0};
static int s_perf_index = 0;
static uint64_t s_perf_frame_start_us = 0;   // when this frame's work began
static uint64_t s_perf_last_end_us = 0;      // when the previous endFrame returned
static uint32_t s_perf_last_frame_time = 0;  // ms, excluding the pacing wait
static int s_perf_fps = 0;
static uint32_t s_perf_target_us = 0;        // 0 = unpaced
static uint64_t s_perf_next_end_us = 0;      // when the current frame should end

static uint64_t perf_now_us(void) { return to_us_since_boot(get_absolute_time()); }

void perf_init(void) {
    s_perf_frame_start_us = 0;
    s_perf_last_end_us = 0;
    s_perf_index = 0;
    s_perf_fps = 0;
    s_perf_last_frame_time = 0;
    s_perf_target_us = 0;
    s_perf_next_end_us = 0;
    memset(s_perf_periods_us, 0, sizeof(s_perf_periods_us));
}

void perf_begin_frame(void) {
    if (s_perf_frame_start_us == 0)
        s_perf_frame_start_us = perf_now_us();
}

void perf_end_frame(void) {
    uint64_t now = perf_now_us();
    if (s_perf_frame_start_us != 0)
        s_perf_last_frame_time = (uint32_t)((now - s_perf_frame_start_us + 500) / 1000);
    // Deadline pacing: frame N should end at end(N-1) + target. On time, wait
    // for the deadline; late, restart the schedule from now (never sprint to
    // catch up).
    if (s_perf_target_us > 0) {
        if (s_perf_next_end_us == 0)
            s_perf_next_end_us = now;
        else
            s_perf_next_end_us += s_perf_target_us;
        now = perf_now_us();
        if (now < s_perf_next_end_us) {
            // Core 0 sleeps until the deadline: Core 1 may decode MP3 ahead
            // meanwhile, off Core 0's working time (core0_idle.h, #28).
            core0_idle_begin(now, s_perf_next_end_us);
            sleep_us(s_perf_next_end_us - now);
            core0_idle_end(perf_now_us());
        } else {
            s_perf_next_end_us = now;
        }
    }
    uint64_t end = perf_now_us();
    if (s_perf_last_end_us != 0) {
        uint64_t period = end - s_perf_last_end_us;
        s_perf_periods_us[s_perf_index] = period > UINT32_MAX ? UINT32_MAX : (uint32_t)period;
        s_perf_index = (s_perf_index + 1) % PERF_SAMPLES;
        uint64_t sum = 0;
        uint32_t count = 0;
        for (int i = 0; i < PERF_SAMPLES; i++) {
            if (s_perf_periods_us[i] > 0) {
                sum += s_perf_periods_us[i];
                count++;
            }
        }
        s_perf_fps = sum > 0 ? (int)((1000000ull * count + sum / 2) / sum) : 0;
    }
    s_perf_last_end_us = end;
    s_perf_frame_start_us = end;
    os_overlay_frame_tick((uint32_t)(end / 1000));  // the Show FPS counter
}

int perf_get_fps(void) {
    return s_perf_fps;
}

uint32_t perf_get_frame_time(void) {
    return s_perf_last_frame_time;
}

void perf_set_target_fps(uint32_t fps) {
    s_perf_target_us = fps > 0 ? 1000000u / fps : 0;
    s_perf_next_end_us = 0;
}

// The XIP cache's hit rate since the previous call (sys.getStats()). The
// counters are cleared by a write, not a read: xip_stats.c owns them.
int perf_xip_cache_hit_rate(void) {
#ifdef PICODECK_SIMULATOR
    return -1;  // no XIP cache to count
#else
    return xip_stats_hit_rate_since_last();
#endif
}
