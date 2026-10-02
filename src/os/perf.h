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

// perf.drawFPS, for Lua and native apps alike: "FPS: n" (n = perf_get_fps())
// at (x, y) in the current font, in perf_fps_color(n) on black. It draws
// nothing while the OS counter (Show FPS) is on, which replaces the app's
// own: otherwise an app's counter anywhere but under the OS box (issue #66:
// the SDK Showcase's, cut off at the right edge) shows as a second one.
void perf_draw_fps(int x, int y);

// The FPS colour code of drawFPS and the OS counter (RGB565): green >= 55,
// yellow >= 30, red below.
uint16_t perf_fps_color(int fps);

// The XIP cache's hit rate in percent since the previous call, -1 when
// nothing was counted (or in the simulator). See xip_stats.h.
int perf_xip_cache_hit_rate(void);

#ifdef __cplusplus
}
#endif

#endif // PERF_H
