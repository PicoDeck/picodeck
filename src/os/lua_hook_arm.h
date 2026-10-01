// Arming the Lua service hook from an interrupt, in constant time.
//
// Firmware runs the Lua VM hook-free; a 1 ms Core 0 timer arms a count-1
// hook on the running thread (src/os/lua_bridge.c, lua_bridge_arm_cb), and
// the hook disarms itself after servicing. Stock lua_sethook() cannot do
// that arming: besides setting the hook, it walks the thread's whole
// CallInfo chain (ldebug.c settraps) to set every Lua frame's trap flag.
// In deep recursion that is ~1000 frames of QMI PSRAM, a few cache misses
// each, on every tick: the walk outlasts the 1 ms period and the timer IRQ
// never returns, so the VM never runs again and the watchdog resets the
// device (GitHub issue #21).
//
// picodeck_lua_arm_hook() sets the hook exactly as lua_sethook(L, hook,
// LUA_MASKCOUNT, 1) does, but sets the trap of only the innermost Lua frame,
// looking past at most PICODECK_LUA_ARM_MAX_FRAMES frames from the top. That
// is enough for a hook that fires once and disarms: the running Lua frame
// sees its trap at its next jump, loop, call or return; a running C function
// that calls Lua enters luaV_execute, which reads L->hookmask; one that
// returns lands in the trapped Lua frame. The only case it leaves to the
// next tick is a return across a C boundary into an outer, untrapped Lua
// frame, so the hook fires at most one period later than stock would.
//
// Async-signal safe in the same way as lua_sethook (lua.c's SIGINT handler
// relies on it): it stores the hook, the counts and the mask, then trap
// flags of frames on the live chain. Part of the Lua library
// (cmake/picodeck_lua.cmake PICODECK_LUA_SOURCES), since it uses VM internals.
#pragma once

#include "lua.h"

// Frames examined from the top of the chain: the running frame plus the C
// functions above the innermost Lua frame (each nested C-to-C call also
// counts towards LUAI_MAXCCALLS, so a deep pure-C stack is rare).
#define PICODECK_LUA_ARM_MAX_FRAMES 16

void picodeck_lua_arm_hook(lua_State *L, lua_Hook hook);
