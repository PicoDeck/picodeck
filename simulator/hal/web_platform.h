#ifndef WEB_PLATFORM_H
#define WEB_PLATFORM_H

// The web build's platform seam: the simulator core calls these from its wait
// points when compiled with Emscripten, and the web project implements them
// (web_platform.c in the directory PICODECK_WEB_DIR names; see
// simulator/CLAUDE.md, "Web build seam"). Only __EMSCRIPTEN__ code uses them.

#include <stdint.h>

// Run the simulated Core 1 service loop if its 5 ms period has elapsed.
void web_core1_tick(void);

// Hand the tab back to the browser for at least `ms` (0 = one event-loop turn).
void web_yield(uint32_t ms);

// Yield only if the OS has been running for longer than a frame without one.
void web_yield_if_due(void);

// Mount <sd_root>/data on IndexedDB and load saved data (blocks until loaded).
void web_fs_init(const char *sd_root);

#endif
