#pragma once

#include <stdint.h>
#include <stdbool.h>

// System-wide toast notification queue.
// Thread-safe: toast_push() can be called from any core.
// toast_update/toast_rect/toast_render are Core 0 only (display access).

// Toast style constants (controls background color)
#define TOAST_STYLE_INFO    0   // Default blue-gray
#define TOAST_STYLE_SUCCESS 1   // Green
#define TOAST_STYLE_WARNING 2   // Amber/orange
#define TOAST_STYLE_ERROR   3   // Red

// Initialize the toast subsystem. Call once at boot before Core 1 launch.
void toast_init(void);

// Push a toast message to the system queue. Thread-safe (spinlock protected).
// Message is copied (up to 63 chars). Auto-dismissed after ~3 seconds.
// style: TOAST_STYLE_INFO, _SUCCESS, _WARNING, or _ERROR.
void toast_push(const char *msg, uint8_t style);

// Advance the queue: expire the active toast and show the next queued one.
// Returns an id for the active toast (0 = none); a different id is a
// different toast. Core 0 only, like the two below.
uint32_t toast_update(void);

// The active toast's rectangle in the current font (x is negative when the
// message is wider than the screen). Valid while toast_update() is non-zero.
void toast_rect(int *x, int *y, int *w, int *h);

// Draw the active toast (if any) into the back buffer. The OS overlay pass
// (os_overlay_draw) calls these on every app present.
void toast_render(void);
