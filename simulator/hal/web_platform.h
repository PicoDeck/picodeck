#ifndef WEB_PLATFORM_H
#define WEB_PLATFORM_H

// The web build's seam: the whole contract between the simulator core and the
// web project that builds it for the browser (the directory PICODECK_WEB_DIR
// names: a PicoDeck/web-sim checkout; see simulator/CLAUDE.md, "Web build
// seam"). A change here is a change to that contract.
//
// The core provides, for the web sources to call:
//   * sim_core1_service() (below): one Core 1 tick.
//   * hal_input_inject_char() (hal/hal_input.h) and
//     hal_display_get_framebuffer() (hal/hal_display.h).
//
// The web sources must define:
//   * the four web_* functions below, which the core's __EMSCRIPTEN__ hooks
//     call from their wait points;
//   * the desktop-only hooks the core calls, as the browser has no RPC socket,
//     terminal tracking or Unicorn: sim_socket_notify() and
//     sim_socket_notify_log_subscribers() (sim_socket.h),
//     sim_set_active_terminal() and sim_get_active_terminal()
//     (sim_socket_handler.h), and unicorn_run_app() (unicorn_runner.h).

#include <stdint.h>

// ── The core provides ────────────────────────────────────────────────────────

// Run one Core 1 tick (audio, MOD player, network poll, HTTP callbacks) and
// return; no sleep. The desktop build's Core 1 thread runs it every 5 ms.
void sim_core1_service(void);

// ── The web sources define ───────────────────────────────────────────────────

// Run sim_core1_service() if its 5 ms period has elapsed. This is the web
// build's whole Core 1: it calls nothing else of Core 1's.
void web_core1_tick(void);

// Hand the tab back to the browser for at least `ms` (0 = one event-loop turn).
void web_yield(uint32_t ms);

// Yield only if the OS has been running for longer than a frame without one.
void web_yield_if_due(void);

// Mount <sd_root>/data on IndexedDB and load saved data (blocks until loaded).
void web_fs_init(const char *sd_root);

#endif
