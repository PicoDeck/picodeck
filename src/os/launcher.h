#pragma once
#include <stdbool.h>
#include <stdint.h>

// Runs the app launcher loop (never returns).
// Scans /apps/ on SD card, renders a scrollable menu, and launches selected apps.
void launcher_run(void);

// Re-scans the SD card for apps and resets the selection.
void launcher_refresh_apps(void);

// List all installed apps to serial output (shows name and ID). `all` adds
// the hidden apps under /apps/.test and /apps/.dev, tagged with their root.
void launcher_list_apps(bool all);

// Launch an app by ID. Returns true if app was found and launched.
// Returns false if app not found.
bool launcher_launch_by_id(const char *id);

// Launch an app by name or ID. Returns true if app was found and launched.
// Tries ID match first, then falls back to name match, then the directory
// name, then the hidden roots (/apps/.test, /apps/.dev; dev console only).
// Returns false if app not found.
bool launcher_launch_by_name(const char *name);

// Get the name of the currently running app (NULL if no app running)
const char* launcher_get_running_app_name(void);

// Get milliseconds since the current app was launched (0 if no app running)
uint32_t launcher_get_app_uptime_ms(void);

// Change system clock (handles voltage, peripherals, Core 1 pause).
// khz=0 resets to the default 200 MHz.
void launcher_apply_clock(uint32_t khz);
