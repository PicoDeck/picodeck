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
// passing over the C frames above it. That is enough for a hook that fires
// once and disarms: the running Lua frame sees its trap at its next jump,
// loop, call or return; a running C function that calls Lua enters
// luaV_execute, which reads L->hookmask; one that returns lands in the
// trapped Lua frame. What it leaves to a later tick is a return across a C
// boundary into an outer, untrapped Lua frame (or an error unwinding past
// the trapped one): the next tick finds that frame innermost and traps it.
//
// Async-signal safe in the same way as lua_sethook (lua.c's SIGINT handler
// relies on it): it stores the hook, the counts and the mask, then trap
// flags of frames on the live chain. Part of the Lua library
// (cmake/picodeck_lua.cmake PICODECK_LUA_SOURCES), since it uses VM internals.
#pragma once

#include "lua.h"

// Frames examined from the top of the chain: a safety bound, never reached
// on a sound chain. Each C frame stacked on another C frame was made by a
// call through C (luaD_call, or lua_resume at a coroutine's base), and each
// of those counts towards LUAI_MAXCCALLS (overflow handling allows ~10%
// more), so the innermost Lua frame is at most ~68 frames from the top
// (lua_hook_arm.c asserts it): ~70 us of PSRAM misses for a contrived
// C-to-C chain, one or two frames in practice. A lower cap could leave a
// loop that spends its time under a deep C chain without its hook for good.
#define PICODECK_LUA_ARM_MAX_FRAMES 80

void picodeck_lua_arm_hook(lua_State *L, lua_Hook hook);
