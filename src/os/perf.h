#ifndef PERF_H
#define PERF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the performance monitoring system.
void perf_init(void);

// Start timing a frame. Call at the beginning of the game loop.
void perf_begin_frame(void);

// End timing a frame and update FPS calculation. Call at the end of the game loop.
void perf_end_frame(void);

// Frames presented per second, averaged over the last 30 frames: the time
// from one endFrame to the next, including setTargetFPS's wait.
int perf_get_fps(void);

// The last frame's work in milliseconds: from the previous endFrame (or
// beginFrame, for the first frame) to this endFrame, without the pacing wait.
uint32_t perf_get_frame_time(void);

// Set target FPS for automatic frame pacing (0 = no limit).
void perf_set_target_fps(uint32_t fps);

// XIP cache performance counters (RP2350).
// Reset counters and start counting.
void perf_xip_cache_reset(void);
// Get cache hit rate as 0-100 percentage.  Returns -1 if no accesses recorded.
int perf_xip_cache_hit_rate(void);
// Log cache stats to UART (total accesses, hits, hit rate).
void perf_xip_cache_report(void);

#ifdef __cplusplus
}
#endif

#endif // PERF_H
