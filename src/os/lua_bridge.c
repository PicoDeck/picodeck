#include "lua_bridge_internal.h"
#include "app_identity.h"
#include "lua_hook_arm.h"
#include "lua_psram_alloc.h"
#include "crashlog.h"
#include "sim_hooks.h"

char lua_bridge_exit_tag; // address used as sentinel, value irrelevant
#include "../drivers/display.h"
#include "../drivers/http.h"
#include "../drivers/keyboard.h"
#include "../drivers/wifi.h"
#include "../os/clock.h"
#include "../os/config.h"
#include "../os/os.h"
#include "../os/screenshot.h"
#include "../os/system_menu.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "../os/ui.h"

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "../dev_commands.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_decoders.h"
#include "umm_malloc.h"
#include "umm_malloc_cfg.h"


// ── Colour helpers
// ──────────────────────────────────────────────────────────── Lua passes
// colours as RGB565 integers (or we provide helper constructors)

uint16_t l_checkcolor(lua_State *L, int idx) {
  return (uint16_t)luaL_checkinteger(L, idx);
}

// ── Integer quantity arguments
// ──────────────────────────────────────────────────────────── lua_Number is
// float32, so ordinary app arithmetic (x + dx*dt, w/2) yields 199.99998 where
// double arithmetic landed on 200, and luaL_checkinteger rejects it with
// "number has no integer representation". Quantity parameters (coordinates,
// sizes, durations, volumes, ...) use these instead:
//   - integers pass through unchanged, including exact integers above 2^24;
//   - floats round to nearest, ties toward +inf: floor(x + 0.5), computed as
//     floor(x) plus a fraction test so no addition can round the input;
//   - numeric strings convert through luaL_checknumber, i.e. to a float
//     first, so they round like floats and an integer-valued string beyond
//     2^24 ("20000000") is an error even though the integer itself is not;
//   - NaN / +-inf raise "number is NaN or infinite";
//   - floats beyond +-2^24 raise "number out of integer range": past that
//     float32 no longer holds every integer, so there is nothing meaningful to
//     round to. The bound does NOT make narrow casts safe: 256..2^24 still
//     wraps a uint8_t, and any negative value wraps an unsigned sink, so
//     such sinks clamp with lb_clamp_int.
// Discrete identifiers (handles, enums, colours, masks, byte counts, ports)
// keep luaL_checkinteger.
#define LB_FLOAT_INT_LIMIT 16777216.0f  // 2^24

// Raises an argument error on `arg`; `what` (e.g. "field 'x'") prefixes the
// message when the value came from a table rather than the argument itself.
static int lb_argfail(lua_State *L, int arg, const char *what,
                      const char *msg) {
  if (what)
    msg = lua_pushfstring(L, "%s: %s", what, msg);
  return luaL_argerror(L, arg, msg);
}

// Converts the value at stack index idx; errors blame argument `arg`.
static lua_Integer lb_toint(lua_State *L, int idx, int arg, const char *what) {
  if (lua_isinteger(L, idx))
    return lua_tointeger(L, idx);
  lua_Number n;
  if (!what) {
    n = luaL_checknumber(L, idx);  // positional: the standard type error
  } else {
    int isnum;
    n = lua_tonumberx(L, idx, &isnum);
    if (!isnum)
      lb_argfail(L, arg, what,
                 lua_pushfstring(L, "number expected, got %s",
                                 luaL_typename(L, idx)));
  }
  if (isnan(n) || isinf(n))
    lb_argfail(L, arg, what, "number is NaN or infinite");
  if (n > LB_FLOAT_INT_LIMIT || n < -LB_FLOAT_INT_LIMIT)
    lb_argfail(L, arg, what, "number out of integer range");
  lua_Number f = floorf(n);
  // The fraction test cannot misround: for |n| >= 0.5 the fraction is a
  // multiple of 2^-24 below 1, so exact; for n in (-0.5, 0) it is above 0.5
  // and for n in [0, 0.5) it is n itself.
  if (n - f >= 0.5f)
    f += 1.0f;
  return (lua_Integer)f;
}

lua_Integer lb_checkint(lua_State *L, int idx) {
  return lb_toint(L, idx, idx, NULL);
}

lua_Integer lb_optint(lua_State *L, int idx, lua_Integer def) {
  return lua_isnoneornil(L, idx) ? def : lb_checkint(L, idx);
}

lua_Integer lb_checkint_at(lua_State *L, int idx, int arg, const char *what) {
  return lb_toint(L, idx, arg, what);
}

lua_Integer lb_optint_at(lua_State *L, int idx, int arg, const char *what,
                         lua_Integer def) {
  return lua_isnoneornil(L, idx) ? def : lb_toint(L, idx, arg, what);
}

// Real-valued arguments (world coordinates, angles, scales): any finite
// number; NaN and infinities are argument errors.
float lb_checkfloat(lua_State *L, int idx) {
  lua_Number n = luaL_checknumber(L, idx);
  if (!isfinite(n)) lb_argfail(L, idx, NULL, "number is NaN or infinite");
  return (float)n;
}

float lb_optfloat(lua_State *L, int idx, float def) {
  return lua_isnoneornil(L, idx) ? def : lb_checkfloat(L, idx);
}

// ── Registration
// ──────────────────────────────────────────────────────────────

// Create a sub-table from a luaL_Reg and attach it to the `picocalc` table
// that is already on the stack at index -1.
void register_subtable(lua_State *L, const char *name,
                              const luaL_Reg *funcs) {
  lua_newtable(L);
  luaL_setfuncs(L, funcs, 0);
  lua_setfield(L, -2, name);
}

// Register a userdata (or table) type under the registry key `mtname`.
// The metatable holds only the metamethods in `meta` (__gc, __close,
// __tostring, ...); the methods live in a separate table that becomes
// __index, so obj:__gc() is "attempt to call a nil value" instead of a
// second finaliser run. __metatable = false hides the metatable from
// getmetatable/setmetatable. Every function in `meta` gets the methods
// table as upvalue 1, so a type with a custom __index function (sprite,
// animator) looks methods up with lua_rawget(L, lua_upvalueindex(1)) and
// never through the metatable. Leaves the stack as it found it.
void lb_register_type(lua_State *L, const char *mtname,
                      const luaL_Reg *methods, const luaL_Reg *meta) {
  luaL_newmetatable(L, mtname);  // mt
  lua_newtable(L);               // mt, methods
  if (methods)
    luaL_setfuncs(L, methods, 0);
  if (meta) {
    lua_pushvalue(L, -2);        // mt, methods, mt
    lua_pushvalue(L, -2);        // mt, methods, mt, methods (upvalue)
    luaL_setfuncs(L, meta, 1);   // mt, methods, mt
    lua_pop(L, 1);               // mt, methods
  }
  if (lua_getfield(L, -2, "__index") == LUA_TNIL) {
    lua_pop(L, 1);               // mt, methods
    lua_setfield(L, -2, "__index");
  } else {
    lua_pop(L, 2);               // meta supplied its own __index
  }
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 1);
}

// ── Exit request (see lua_bridge.h) ─────────────────────────────────────────
// Core 0 only: set by lua_bridge_raise_exit, cleared by the runner through
// lua_bridge_exit_reset once the app's VM has returned.
static bool s_exit_requested = false;

// The Lua thread executing on Core 0: the main thread, or the coroutine that
// lua_corolib.c resumed into. Written only by Core 0 thread code.
static lua_State *volatile s_running_L = NULL;

void lua_bridge_set_running(lua_State *L) { s_running_L = L; }

// Instructions between two count-hook calls of the SYNCHRONOUS count hook
// (the simulator, the web build, and firmware's rare "no alarm slot"
// fallback — see lua_bridge_hook_start below). Firmware's normal timer-armed
// hook always installs a count of 1 (the 1 ms timer re-arms it), so this
// adaptive count never applies there. Each call is a few flag reads unless
// the full service pass is due (see lua_service below). The count adapts
// (menu_lua_hook) so the hook fires about every LUA_HOOK_TARGET_US of wall
// time: up to LUA_HOOK_COUNT_MAX in compute-bound code (the old fixed 256
// cost ~10-25% of VM time), down to LUA_HOOK_COUNT_MIN in apps that spend
// their time in C calls (a draw loop runs a few instructions per frame, so a
// large fixed count would delay exit_app and dev commands by seconds).
#define LUA_HOOK_COUNT_MIN 128
#define LUA_HOOK_COUNT_MAX 4096
// At registration: start low; compute-bound code reaches the maximum within
// a couple of hook calls, while an app that is idle from the start is not
// left waiting for a large first count.
#define LUA_HOOK_COUNT LUA_HOOK_COUNT_MIN
#define LUA_HOOK_TARGET_US 1000u
// Longest gap between two full service passes while the hook fires.
#define LUA_SERVICE_PERIOD_US 5000u

// Wall time of the last hook call, any thread (Core 0). Only maintained by
// the synchronous count hook's adaptive rescale below: firmware's normal
// timer-armed hook never updates it. The count itself is per thread
// (lua_newthread copies it; lua_sethook sets only the running thread's), so
// the hook reads it back with lua_gethookcount.
static uint32_t s_last_hook_us = 0;

static void menu_lua_hook(lua_State *L, lua_Debug *ar);

static void lua_bridge_install_hook(lua_State *L, int count) {
  lua_sethook(L, menu_lua_hook, LUA_MASKCOUNT, count);
}

// With any count hook installed, Lua 5.4 calls luaG_traceexec before EVERY
// instruction (vmfetch's `trap`), whatever the count: on the device that
// tax was ~60% of VM time (P0: an empty loop 491 ns/iter with it, 189
// without). So on firmware the VM runs hook-free and a 1 ms Core 0 repeating
// timer arms a count-1 hook on the running thread; menu_lua_hook services
// and disarms it. The simulator and the web build have no Core 0 interrupt (a
// host thread arming the hook would race the VM), so they keep the adaptive
// synchronous count hook.
//
// The arming is picodeck_lua_arm_hook (lua_hook_arm.h), not lua_sethook:
// lua_sethook also sets the trap of every frame on the thread's call chain,
// and ~1000 frames deep (the LUAI_MAXSTACK limit with small frames) that
// walk of QMI PSRAM outlasts the 1 ms period. Issue #21: the timer then
// re-fired inside its own IRQ forever, the VM never ran again and the
// watchdog reset the device. The period is also counted from the end of
// each callback (a positive delay), not fixed-rate, so a late or slow
// callback can never catch up back-to-back and starve the VM.
#ifndef PICODECK_SIMULATOR
#define LUA_BRIDGE_ASYNC_HOOK 1
#define LUA_ARM_PERIOD_US 1000
static volatile bool s_async_active = false;  // an app's VM is running
static repeating_timer_t s_arm_timer;
static bool s_arm_timer_started = false;

static bool lua_bridge_arm_cb(repeating_timer_t *t) {
  (void)t;
  lua_State *L = s_running_L;
  if (s_async_active && L)
    picodeck_lua_arm_hook(L, menu_lua_hook);
  return true;
}
#else
#define LUA_BRIDGE_ASYNC_HOOK 0
#endif

// Called once per app, as the last step of registration.
static void lua_bridge_hook_start(lua_State *L) {
  s_running_L = L;
#if LUA_BRIDGE_ASYNC_HOOK
  lua_sethook(L, NULL, 0, 0);
  if (!s_arm_timer_started)  // default alarm pool: created on Core 0
    s_arm_timer_started =
        add_repeating_timer_us(LUA_ARM_PERIOD_US, lua_bridge_arm_cb, NULL,
                               &s_arm_timer);
  s_async_active = s_arm_timer_started;
  if (s_async_active)
    return;
  printf("[LUA] no alarm slot for the service timer: synchronous hook\n");
#endif
  lua_bridge_install_hook(L, LUA_HOOK_COUNT);
}

bool lua_bridge_exit_requested(void) { return s_exit_requested; }

void lua_bridge_raise_exit(lua_State *L) {
  bool first = !s_exit_requested;
  if (first) {
    s_exit_requested = true;
    // Modal loops (ui_confirm, text input, the system menu) watch the dev
    // flag; they unwind instead of waiting for a key the app will never read.
    dev_commands_set_exit();
  }
  // Re-raise before every instruction from now on: a count-1 hook installed
  // with lua_sethook, which traps every Lua frame on the chain (here, in
  // thread context, its walk may take as long as it needs), so whichever
  // frame a pcall returns to raises at its next instruction. The raising
  // thread on every raise, even over an armed count-1 hook: hook state is
  // per thread, and a coroutine that existed before the first raise (or never
  // ran since) carries at most the timer's arming, which traps only its
  // innermost Lua frame (picodeck_lua_arm_hook). The main thread as well: a
  // coroutine that raised is dead once resume returns, and the thread that
  // resumed it has its own hook. Main is installed on the first raise and
  // again whenever its hook is not armed; check the mask too, not just the
  // count: a hook disarmed mid-way (count already 1, but mask 0 — e.g. the
  // async timer's window between menu_lua_hook disarming it and the next
  // 1 ms re-arm) must be re-armed immediately rather than waiting for the
  // timer. Once installed, a thread's mask and traps stay set until the
  // runner's lua_bridge_exit_reset.
  lua_bridge_install_hook(L, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
  lua_State *main = lua_tothread(L, -1);
  lua_pop(L, 1);
  if (main && main != L &&
      (first || lua_gethookcount(main) != 1 ||
       !(lua_gethookmask(main) & LUA_MASKCOUNT)))
    lua_bridge_install_hook(main, 1);
  lua_pushlightuserdata(L, &lua_bridge_exit_tag);
  lua_error(L);
#if defined(__GNUC__)
  __builtin_unreachable();
#endif
}

void lua_bridge_exit_reset(lua_State *L) {
  s_exit_requested = false;
  dev_commands_clear_exit();
#if LUA_BRIDGE_ASYNC_HOOK
  // The VM is about to close (or has): stop arming it. lua_close's __gc
  // handlers run under the synchronous count hook installed below.
  s_async_active = false;
#endif
  if (L)
    lua_bridge_install_hook(L, LUA_HOOK_COUNT);
}

// ── Service pass (see lua_bridge.h) ─────────────────────────────────────────
// The count hook fires every LUA_HOOK_COUNT instructions on the synchronous
// hook (simulator, web build, firmware's no-alarm-slot fallback); on
// firmware's normal timer-armed hook it fires as a count-1 hook about once
// every 1 ms instead (the arming timer, see lua_bridge_hook_start). Either
// way, it always does the cheap part (watchdog, exit request, Sym press:
// flag reads) and runs the full pass (HTTP/TCP slot scans, sound callbacks,
// the serial dev-command poll, which takes the stdio mutex and TinyUSB on
// firmware, reboot flags, screenshots, low-memory GC) only when work was
// flagged pending or LUA_SERVICE_PERIOD_US has passed since the last full
// pass. The period is the latency bound for anything that does not flag
// itself (HTTP/TCP events, serial dev commands on firmware).
volatile bool g_lua_service_pending = false;
static uint32_t s_last_full_pass_us = 0;

static inline uint32_t service_now_us(void) {
#ifdef PICODECK_SIMULATOR
  extern uint64_t hal_get_time_us(void);
  return (uint32_t)hal_get_time_us();
#else
  return time_us_32();
#endif
}

// The expensive part. May raise (exit) and may run Lua callbacks.
static void lua_service_full(lua_State *L) {
  g_lua_service_pending = false;
  s_last_full_pass_us = service_now_us();
  http_lua_fire_pending(L); // fire any queued HTTP Lua callbacks
  tcp_lua_fire_pending(L);  // fire any queued TCP Lua callbacks
  lua_bridge_sound_poll(L); // fire any pending sound finish/loop callbacks
  dev_commands_poll();
  dev_commands_process();
#ifdef PICODECK_SIMULATOR
  // Socket polling is handled by the dedicated socket thread.
  // Check the global running flag (set by signal handler or shutdown RPC).
  extern volatile int g_running;
  if (!g_running) {
    dev_commands_set_exit();
  }
#endif
  // A callback above may have called sys.exit() inside its own pcall.
  if (s_exit_requested || dev_commands_wants_exit())
    lua_bridge_raise_exit(L);
  if (dev_commands_wants_reboot()) {
    printf("[DEV] Rebooting...\n");
    crashlog_clear_running(); // intentional — not an unclean exit
    stdio_flush();
    sleep_ms(100);
    kbd_prepare_reset();
    watchdog_reboot(0, 0, 0);
  }
  if (dev_commands_wants_reboot_flash()) {
    printf("[DEV] Rebooting to BOOTSEL mode...\n");
    crashlog_clear_running();
    stdio_flush();
    sleep_ms(100);
    kbd_prepare_reset();
    reset_usb_boot(0, 0);
  }
  if (dev_commands_wants_reboot_ota()) {
    // Launcher-only: drop it rather than let it fire when the app exits.
    dev_commands_clear_reboot_ota();
    printf("[DEV] reboot-ota ignored: an app is running (exit it first)\n");
  }
  // Both screenshot triggers set s_screenshot_pending so the capture fires
  // inside l_display_flush — always on a fully-drawn, flushed frame.
  if (kbd_consume_screenshot_press())
    s_screenshot_pending = true;
  if (screenshot_check_scheduled())
    s_screenshot_pending = true;

  // Low-memory GC trigger: when the PSRAM heap drops below PSRAM_LOW_WATERMARK,
  // force a full GC cycle to reclaim dead Lua objects before allocations start
  // failing.  s_gc_triggered prevents hammering GC on every pass while
  // memory stays low; it resets once the heap recovers above the watermark.
  static bool s_gc_triggered = false;
  if (lua_psram_alloc_is_low()) {
    if (!s_gc_triggered) {
      printf("[LUA] Memory low (%zu KB free), triggering emergency GC\n",
             lua_psram_alloc_free_size() / 1024);
      lua_gc(L, LUA_GCCOLLECT, 0);
      s_gc_triggered = true;
      printf("[LUA] After GC: %zu KB free\n",
             lua_psram_alloc_free_size() / 1024);
    }
  } else {
    s_gc_triggered = false;
  }
}

// The cheap part plus, when due, the full pass.
static void lua_service(lua_State *L, bool force) {
  watchdog_update(); // kick watchdog
  if (s_exit_requested || dev_commands_wants_exit())
    lua_bridge_raise_exit(L);  // new, or an earlier one that was swallowed
  if (force || g_lua_service_pending ||
      (uint32_t)(service_now_us() - s_last_full_pass_us) >= LUA_SERVICE_PERIOD_US)
    lua_service_full(L);
  if (kbd_consume_menu_press())
    system_menu_show(L);
}

void lua_bridge_service(lua_State *L) { lua_service(L, true); }

void lua_bridge_service_poll(lua_State *L) { lua_service(L, false); }

// Instruction-count hook. On the synchronous hook (simulator, web build,
// firmware's no-alarm-slot fallback) it fires every LUA_HOOK_COUNT Lua
// opcodes, adaptively rescaled (every opcode once an exit was requested). On
// firmware's normal timer-armed hook it always fires as a count-1 hook: the
// 1 ms timer (lua_bridge_arm_cb) re-arms it, so it services once and disarms
// until the next tick.
static void menu_lua_hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
#if LUA_BRIDGE_ASYNC_HOOK
  if (s_async_active && !s_exit_requested) {
    lua_sethook(L, NULL, 0, 0);  // disarm first: the timer re-arms in 1 ms
    lua_service(L, false);
    return;
  }
#endif
  if (!s_exit_requested) {
    // Rescale the count when the gap since the last call is off target by
    // more than 2x either way (so a steady app is left alone).
    uint32_t now = service_now_us();
    uint32_t dt = now - s_last_hook_us;
    s_last_hook_us = now;
    if (dt < LUA_HOOK_TARGET_US / 2 || dt > LUA_HOOK_TARGET_US * 2) {
      int cur = lua_gethookcount(L);  // this thread's count
      uint64_t want = (uint64_t)(cur > 0 ? cur : 1) * LUA_HOOK_TARGET_US /
                      (dt ? dt : 1);
      int count = want < LUA_HOOK_COUNT_MIN   ? LUA_HOOK_COUNT_MIN
                  : want > LUA_HOOK_COUNT_MAX ? LUA_HOOK_COUNT_MAX
                                              : (int)want;
      if (count != cur)
        lua_bridge_install_hook(L, count);
    }
  }
  lua_service(L, false);
}


void lua_bridge_game_init(lua_State *L);
void lua_bridge_terminal_init(lua_State *L);
void lua_bridge_register_3d(lua_State *L);
void lua_bridge_zip_init(lua_State *L);

// The VM and this bridge must agree on the number types. cmake/picodeck_lua.cmake
// patches luaconf.h so LUA_32BITS=1 takes effect and applies it PUBLIC; if
// either step regresses, Lua silently reverts to 64-bit integers and doubles
// (apps then behave differently on the simulator and the device). Fail the
// build instead.
_Static_assert(sizeof(lua_Integer) == 4,
               "lua_Integer must be 32-bit: luaconf.h not patched or "
               "PICODECK_LUA_DEFINITIONS not applied (see cmake/picodeck_lua.cmake)");
_Static_assert(sizeof(lua_Number) == 4,
               "lua_Number must be float: luaconf.h not patched or "
               "PICODECK_LUA_DEFINITIONS not applied (see cmake/picodeck_lua.cmake)");
_Static_assert(LUAI_MAXSTACK == 1000,
               "LUAI_MAXSTACK override not applied (see cmake/picodeck_lua.cmake)");
// The Lua VM stack (lua_runner.c LUA_VM_STACK_SIZE) is sized for this many
// nested C calls; raising it without growing that stack turns the clean
// "C stack overflow" error into a stack-limit HardFault.
_Static_assert(LUAI_MAXCCALLS == 60,
               "LUAI_MAXCCALLS override not applied (see cmake/picodeck_lua.cmake)");

// load(chunk [, chunkname [, mode [, env]]]) with mode forced to "t".
// Precompiled bytecode is not verified by the VM, so a crafted chunk can read
// and write arbitrary memory; apps may only load source text. The original
// base-library load is upvalue 1.
static int l_base_load_text(lua_State *L) {
  if (lua_gettop(L) < 3)
    lua_settop(L, 3);  // pad chunkname/mode with nil; env (arg 4) stays absent
  lua_pushliteral(L, "t");
  lua_replace(L, 3);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
  return lua_gettop(L);
}

void lua_bridge_register(lua_State *L) {
  printf("[LUA] lua_bridge_register start, PSRAM free=%lu\n",
         (unsigned long)umm_free_heap_size());








  // Open standard Lua libs. io, os, package and debug stay closed (sandbox).
  printf("[LUA] registering _G...\n");
  luaL_requiref(L, "_G", luaopen_base, 1);
  lua_pop(L, 1);
  // dofile/loadfile read the host filesystem through C stdio: on firmware
  // there is no such filesystem, in the simulator they bypass the SD root
  // and the app sandbox. Apps load code with load() or picocalc.sys.loadlib.
  lua_pushnil(L);
  lua_setglobal(L, "dofile");
  lua_pushnil(L);
  lua_setglobal(L, "loadfile");
  lua_bridge_require_init(L);
  printf("[LUA] registering table...\n");
  luaL_requiref(L, "table", luaopen_table, 1);
  lua_pop(L, 1);
  printf("[LUA] registering string...\n");
  luaL_requiref(L, "string", luaopen_string, 1);
  lua_pop(L, 1);
  printf("[LUA] registering math...\n");
  luaL_requiref(L, "math", luaopen_math, 1);
#ifdef PICODECK_SIMULATOR
  // Simulator --test-mode: a fixed seed, so math.random repeats run to run.
  if (sim_test_mode()) {
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, SIM_TEST_RANDOM_SEED);
    lua_call(L, 1, 0);
  }
#endif
  lua_pop(L, 1);
  printf("[LUA] registering coroutine...\n");
  luaL_requiref(L, LUA_COLIBNAME, luaopen_picodeck_coroutine, 1);
  lua_pop(L, 1);
  printf("[LUA] registering utf8...\n");
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(L, 1);
  // Replace base load with the text-only wrapper.
  lua_getglobal(L, "load");
  lua_pushcclosure(L, l_base_load_text, 1);
  lua_setglobal(L, "load");
  printf("[LUA] stdlib done, PSRAM free=%lu\n",
         (unsigned long)umm_free_heap_size());

  // Create the top-level `picocalc` table
  lua_newtable(L);
  printf("[LUA] registering display...\n");
  lua_bridge_display_init(L);
  printf("[LUA] registering input...\n");
  lua_bridge_input_init(L);
  printf("[LUA] registering gamepad...\n");
  lua_bridge_gamepad_init(L);
  printf("[LUA] registering sys...\n");
  lua_bridge_sys_init(L);
  printf("[LUA] registering fs...\n");
  lua_bridge_fs_init(L);
  printf("[LUA] registering network...\n");
  lua_bridge_network_init(L);
  printf("[LUA] registering tcp...\n");
  lua_bridge_tcp_init(L);
  printf("[LUA] registering config...\n");
  lua_bridge_config_init(L);
  printf("[LUA] registering appconfig...\n");
  lua_bridge_appconfig_init(L);
  printf("[LUA] registering perf...\n");
  lua_bridge_perf_init(L);
  printf("[LUA] registering graphics...\n");
  lua_bridge_graphics_init(L);
  printf("[LUA] registering 3D extensions...\n");
  lua_bridge_register_3d(L);
  printf("[LUA] registering gfx3d...\n");
  lua_bridge_gfx3d_init(L);
  printf("[LUA] registering ui...\n");
  lua_bridge_ui_init(L);
  printf("[LUA] registering audio...\n");
  lua_bridge_audio_init(L);
  printf("[LUA] registering sound...\n");
  lua_bridge_sound_init(L);
  printf("[LUA] registering repl...\n");
  lua_bridge_repl_init(L);
  printf("[LUA] registering video...\n");
  lua_bridge_video_init(L);
  printf("[LUA] registering game...\n");
  lua_bridge_game_init(L);
  printf("[LUA] registering terminal...\n");
  lua_bridge_terminal_init(L);
  printf("[LUA] registering crypto...\n");
  lua_bridge_crypto_init(L);
  printf("[LUA] registering modplayer...\n");
  lua_bridge_mod_init(L);
  printf("[LUA] registering zip...\n");
  lua_bridge_zip_init(L);
  printf("[LUA] registering json...\n");
  lua_bridge_json_init(L);
  printf("[LUA] all modules done, PSRAM free=%lu\n",
         (unsigned long)umm_free_heap_size());
  // Set as global
  lua_setglobal(L, "picocalc");

  lua_bridge_hook_start(L);
  printf("[LUA] lua_bridge_register complete\n");
}

void lua_bridge_show_error(lua_State *L, const char *context) {
  const char *err = lua_tostring(L, -1);
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", err ? err : "unknown error");

  // Lua's own memory error is just "not enough memory" — attach the heap
  // state so the on-screen message says how much was free and whether the
  // failure was fragmentation rather than exhaustion.
  if (err && strstr(err, "not enough memory")) {
    size_t n = strlen(buf);
    if (n < sizeof(buf) - 2) {
      buf[n++] = '\n';
      crashlog_describe_heap(buf + n, sizeof(buf) - n);
    }
  }

  // Log to /system/error.log on SD card
  // Attributed from app_identity: the APP_NAME global is the app's to change.
  const app_identity_t *me = app_identity_current();
  crashlog_write_lua_error(me ? me->name : "unknown", context, buf);
  sim_app_report_error(context, buf);

  display_clear(COLOR_BLACK);
  display_draw_text(4, 4, context, COLOR_RED, COLOR_BLACK);

  // Word-wrap the error message at ~52 chars (320px / 6px per char)
  int col = 0, row = 1;
  char line[54] = {0};
  for (int i = 0; buf[i] && row < 38; i++) {
    if (buf[i] != '\n') line[col++] = buf[i];
    if (col >= 52 || buf[i] == '\n') {
      line[col] = '\0';
      display_draw_text(4, 4 + row * 9, line, COLOR_WHITE, COLOR_BLACK);
      row++;
      col = 0;
      memset(line, 0, sizeof(line));
    }
  }
  if (col > 0) {
    line[col] = '\0';
    display_draw_text(4, 4 + row * 9, line, COLOR_WHITE, COLOR_BLACK);
  }

  display_draw_text(4, FB_HEIGHT - 12, "Press Esc to continue", COLOR_GRAY,
                    COLOR_BLACK);
  display_flush();

  if (sim_test_mode()) {  // tests read the error from the log, not the screen
    lua_pop(L, 1);
    return;
  }

  // Drain any keys already held when the error occurred.
  // Timeout after ~2s in case the keyboard I2C is dead and state is stale.
  for (int drain = 0; drain < 125 && kbd_get_buttons(); drain++) {
    kbd_poll();
    sleep_ms(16);
    watchdog_update();
  }
  kbd_clear_state();

  // Wait specifically for Esc before returning.
  // Timeout after ~30s so a dead keyboard doesn't block the launcher forever.
  for (int wait = 0; wait < 1875; wait++) {
    kbd_poll();
    if (kbd_get_buttons() & BTN_ESC)
      break;
    sleep_ms(16);
    watchdog_update();
  }
  lua_pop(L, 1);
}
