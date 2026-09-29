#include "perf.h"
#include "os_overlay.h"
#include "pico/stdlib.h"
#include "hardware/structs/xip.h"
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
        if (now < s_perf_next_end_us)
            sleep_us(s_perf_next_end_us - now);
        else
            s_perf_next_end_us = now;
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

// ── XIP cache performance counters ──────────────────────────────────────────
// RP2350 XIP controller has two counters:
//   ctr_acc — total cache accesses (reads from XIP address space)
//   ctr_hit — cache hits (served from cache without flash/PSRAM fetch)
// Both are 32-bit and auto-reset on read of ctr_acc.

void perf_xip_cache_reset(void) {
    // Reading ctr_acc clears both counters (datasheet §10.4)
    (void)xip_ctrl_hw->ctr_acc;
}

int perf_xip_cache_hit_rate(void) {
    uint32_t hits = xip_ctrl_hw->ctr_hit;
    uint32_t total = xip_ctrl_hw->ctr_acc;  // clears both
    if (total == 0) return -1;
    return (int)((uint64_t)hits * 100 / total);
}

void perf_xip_cache_report(void) {
    uint32_t hits = xip_ctrl_hw->ctr_hit;
    uint32_t total = xip_ctrl_hw->ctr_acc;  // clears both
    if (total == 0) {
        printf("[XIP] No cache accesses recorded\n");
        return;
    }
    int rate = (int)((uint64_t)hits * 100 / total);
    printf("[XIP] Cache: %lu accesses, %lu hits (%d%% hit rate)\n",
           (unsigned long)total, (unsigned long)hits, rate);
}
