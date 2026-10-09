// The boot splash (specs/2026-10-10-boot-splash-design.md). At boot, Core 1
// plays the CRT power-on animation (splash_render.c) while Core 0 boots;
// outside it (USB mode, a firmware update, picocalc.ui.splash) the splash
// is the animation's final frame, drawn once.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Draws the still frame (the animation's last) with `status` and `subtext`
// under the logo (NULL or "" for none) on the calling core, and flushes.
void splash_draw_static(const char *status, const char *subtext);

// Core 0, once, right after display_init(). With `animate`, Core 1 plays the
// animation until splash_finish() or splash_stop(); without (after a failed
// boot) the still frame is drawn and nothing runs. The simulator (no Core 1,
// no main.c boot) only draws the still frame.
void splash_start(bool animate);
// Whether Core 1 owns the display. While it does Core 0 must not draw:
// ui_draw_splash() hands it the status through splash_set_status().
bool splash_running(void);
// The status and subtext Core 1 draws under the settled logo (copied, cut to
// 53 characters; NULL clears).
void splash_set_status(const char *status, const char *subtext);
// Core 0, at the end of boot: waits until Core 1 has shown the final frame
// with the latest status (at most SPLASH_END_MS + 500 ms after the start),
// stops and resets Core 1, and logs the [SPLASH] line. The frame stays up.
void splash_finish(void);
// Core 0, before an error or update screen: stops and resets Core 1 now.
void splash_stop(void);
// From the HardFault handler, first thing; it does nothing unless the splash
// is running. On Core 0 it resets Core 1 at once (no waits, no printing), so
// the fault screen is not drawn over. On Core 1 it only records the fault:
// splash_finish() and splash_stop() then leave Core 1 alone and Core 0 spins,
// unfed watchdog and all, until Core 1's handler reboots after its screen.
void splash_fault_stop(void);
// The animation's measurements, "state=… first_light_ms=… …": the [SPLASH]
// line and the `splash` dev command.
void splash_stats_format(char *buf, size_t n);
