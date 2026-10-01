// hardware/watchdog.h stub
#ifndef HARDWARE_WATCHDOG_H
#define HARDWARE_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

static inline void watchdog_enable(uint32_t delay_ms, bool pause_on_debug) {
    (void)delay_ms; (void)pause_on_debug;
}

static inline void watchdog_disable(void) {}

#ifdef __EMSCRIPTEN__
// The web build's yield point for Lua loops that never sleep, flush or poll
// input: the Lua count hook (lua_bridge.c, lua_service) kicks the watchdog
// on every call, and a browser tab freezes unless the OS hands it back. Every
// other OS caller (launcher, modal loops) yields here too, when one is due.
#include "../../hal/web_platform.h"
static inline void watchdog_update(void) { web_yield_if_due(); }
#else
static inline void watchdog_update(void) {}
#endif

static inline uint32_t watchdog_get_count(void) { return 0; }

static inline void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
    (void)pc; (void)sp; (void)delay_ms;
}

static inline bool watchdog_caused_reboot(void) { return false; }

static inline bool watchdog_enable_caused_reboot(void) { return false; }

#endif // HARDWARE_WATCHDOG_H
