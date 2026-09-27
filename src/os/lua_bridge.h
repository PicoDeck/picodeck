#pragma once

#include "lua.h"
#include "../os/os.h"

// =============================================================================
// Lua ↔ OS Bridge
//
// This file declares all the C functions that are registered as Lua modules.
// Together they form the complete Lua-side API that app scripts call.
//
// In Lua, everything is accessed via the `picocalc` global table:
//
//   picocalc.display.clear(0x0000)
//   picocalc.display.drawText(10, 20, "Hello!", 0xFFFF, 0x0000)
//   picocalc.display.flush()
//
//   local btns = picocalc.input.getButtons()
//   if btns & picocalc.input.BTN_ENTER ~= 0 then ... end
//
//   picocalc.sys.log("starting app")
//   local bat = picocalc.sys.getBattery()
//
// =============================================================================

// Register all picocalc.* sub-modules into the Lua state.
// Call this once after lua_newstate(), before running any app code.
void lua_bridge_register(lua_State *L);

// ── App exit ─────────────────────────────────────────────────────────────────
// sys.exit(), the system menu's "Exit App" and the dev `exit` command
// (exit_app) all end in lua_bridge_raise_exit. The request is sticky: it sets
// a flag the Lua runner owns (cleared only when the app's VM has returned),
// raises the exit sentinel (a unique light userdata) as an ordinary Lua
// error, and drops the count hook to every instruction. A pcall/xpcall (or
// coroutine.resume, or a C callback's lua_pcall) that swallows the sentinel
// cannot keep the app alive: the next instruction outside it raises again,
// so the error climbs one protected call per instruction until it reaches
// the runner. Modal loops see the request through dev_commands_wants_exit(),
// which it also sets.
extern char lua_bridge_exit_tag; // address used as sentinel, value irrelevant
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void lua_bridge_raise_exit(lua_State *L);
// True from the first exit request until the runner resets it.
bool lua_bridge_exit_requested(void);
// Runner only, after the app's pcall returned: clears the request (and the
// dev exit flag) and restores the normal count hook, so __gc handlers run
// during lua_close are not interrupted.
void lua_bridge_exit_reset(lua_State *L);
// Records the Lua thread now executing on Core 0 (lua_corolib.c calls it on
// every resume, return and coroutine close). The firmware's service hook is
// armed on this thread.
void lua_bridge_set_running(lua_State *L);
// The coroutine library (fork of lcorolib.c, src/os/lua_corolib.c).
int luaopen_picodeck_coroutine(lua_State *L);
static inline bool lua_bridge_is_exit_sentinel(lua_State *L, int idx) {
  return lua_islightuserdata(L, idx) &&
         lua_touserdata(L, idx) == &lua_bridge_exit_tag;
}

// One service pass, shared by the count hook, sys.sleep and the terminal's
// blocking waits so they cannot drift apart: feeds the watchdog, raises a
// pending exit, fires HTTP/TCP/sound callbacks, runs dev commands (and their
// reboot/exit flags), opens the system menu on a pending Sym press, latches
// screenshot requests, and collects garbage when the PSRAM heap runs low.
// It does not poll the keyboard (that would steal the app's key edges); a
// menu press is seen once the app's own input polling has read it. May
// raise (exit) and may run Lua callbacks.
void lua_bridge_service(lua_State *L);

// The count hook's form of it, also called by input.update(): the watchdog,
// exit and Sym checks every time, the rest only when work is pending or the
// last full pass is more than 5 ms old. Apps that spend their time in C
// calls (few instructions, so few hook calls) are still served every frame.
void lua_bridge_service_poll(lua_State *L);

// Work for the service pass (a Lua callback to fire, a dev command to run).
// Set from any core or thread (Core 1's audio trampolines, the simulator's
// control socket); the next hook call runs the full pass.
extern volatile bool g_lua_service_pending;
static inline void lua_bridge_request_service(void) {
  g_lua_service_pending = true;
}

// Frees the REPL scrollback (allocated on first repl.* use). The runner calls
// it when the app exits.
void lua_bridge_repl_release(void);

// Run one update tick: poll input, check for menu button, yield to app.
// Returns false if the app requested exit (returned from its update()).
bool lua_bridge_tick(lua_State *L);

// Push a Lua error message as a formatted string to the display.
// Used when pcall() catches a Lua runtime error.
void lua_bridge_show_error(lua_State *L, const char *context);
