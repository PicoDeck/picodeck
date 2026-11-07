#pragma once

// =============================================================================
// App stack: run an app runtime on its own Process Stack (PSP)
//
// Core 0's main stack (MSP) is only 4 KB (SCRATCH_Y) and also takes every
// interrupt. Both app runtimes therefore run in Thread mode on the PSP, on a
// stack the runner allocates at launch:
//   - native ELF apps (native_loader.c): the app's entry point
//   - Lua apps (lua_runner.c): the whole VM lifetime (state creation, load,
//     run, error screen, lua_close and its __gc handlers)
//   - OS work from the launcher (app_stack_run_os): dev commands and
//     screenshot save, on a short-lived 32 KB stack
// Only one runner is active at a time, and app_stack_run_os() runs inline
// when an app stack is already active, so one PSP user exists at a time.
// Interrupt handlers always run on the MSP, whatever the thread uses.
//
// app_stack_run() paints the stack, arms PSPLIM above a 32-byte guard at the
// bottom (a push past it is a STKOF HardFault instead of silent corruption),
// switches Thread mode to the PSP, calls fn(arg), and switches back. The
// HardFault handler reads g_app_stack_base/owner to name the stack.
//
// Simulator: there is no PSP; fn(arg) runs directly on the host stack.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  APP_STACK_NONE = 0,
  APP_STACK_NATIVE = 1,
  APP_STACK_LUA = 2,
  APP_STACK_OS = 3,  // app_stack_run_os(): OS work run from the launcher
} app_stack_owner_t;

// Bottom-of-stack guard kept below PSPLIM, and the paint word used both for
// the guard and for high-water measurement.
#define APP_STACK_GUARD_BYTES 32u
#define APP_STACK_PAINT 0xDEADBEEFu

// The stack installed as PSP right now; NULL / APP_STACK_NONE when no app
// runtime is on it. Read by the HardFault handler (main.c).
extern uint8_t *volatile g_app_stack_base;
extern volatile uint32_t g_app_stack_size;
extern volatile uint8_t g_app_stack_owner;

// Run fn(arg) on [base, base+size) as the Thread-mode PSP. Blocks until fn
// returns. base must stay allocated until then.
void app_stack_run(uint8_t *base, uint32_t size, app_stack_owner_t owner,
                   void (*fn)(void *), void *arg);

// Size of the stack app_stack_run_os() allocates.  unzip needs ~5 KB
// (miniz's central-directory read alone has a 4 KB local buffer) and
// sdcard_delete_recursive ~1.4 KB per directory level; 32 KB leaves room for
// ~20 levels.  PSRAM, allocated per call: nothing is reserved while idle.
#define APP_STACK_OS_SIZE (32u * 1024u)

// Run fn(arg) on an app stack instead of Core 0's 4 KB main stack, for OS
// work with deep or large frames (dev commands, screenshot save) that the
// launcher would otherwise run on the MSP.
//   - Already on an app stack (called from inside a running app: a Lua hook,
//     sys.sleep, a native sys->poll, a modal): fn(arg) runs inline on that
//     stack. app_stack_run() is never nested.
//   - Otherwise: allocates APP_STACK_OS_SIZE from the PSRAM heap, runs
//     fn(arg) there via app_stack_run(), records the peak, frees it.
// Returns false (fn not called) only if the stack could not be allocated.
bool app_stack_run_os(void (*fn)(void *), void *arg);

// Peak bytes used by the last app_stack_run_os() call that switched stacks
// (0 before any, and always 0 in the simulator).
uint32_t app_stack_os_last_peak(void);

// True if Thread mode is running on an app stack right now.
bool app_stack_active(void);

// Run fn(arg) on [lo, lo+size) instead of the app stack it is called on, for one call, and
// return true. For a call that wants a fast stack: the app stacks are PSRAM, so every spill
// and saved register goes through the XIP cache, and [lo, lo+size) can be SRAM lent for the
// call (gfx3d_end's raster, gfx3d_stack_call). The region is painted, PSP moves to its top and
// PSPLIM above a guard at its bottom (overflow is a STKOF HardFault, as for app_stack_run),
// and g_app_stack_base/size name it until fn returns, so the profiler and the HardFault
// handler see the stack in use; then all of it is put back. An interrupt taken meanwhile
// stacks its frame here (up to 104 B with the FP context). False, running nothing, when
// Thread mode is not on an app stack (the MSP) and always in the simulator. fn must return:
// a longjmp out of it would leave PSPLIM at this stack and the globals naming it.
bool app_stack_call_lent(uint8_t *lo, uint32_t size, void (*fn)(void *), void *arg);

// The largest peak of app_stack_call_lent's calls since boot, and the size of the last one
// (the `stack` dev command's raster_peak and raster_size). 0 in the simulator.
uint32_t app_stack_lent_peak(void);
uint32_t app_stack_lent_size(void);

// True if the guard words at the bottom of the stack are untouched.
bool app_stack_guard_intact(const uint8_t *base);

// Peak bytes used since the stack was painted (scan from the bottom for the
// first word that is no longer the paint pattern). 0 in the simulator.
uint32_t app_stack_high_water(const uint8_t *base, uint32_t size);

// Core 0 MSP high-water: main() paints the free part of the 4 KB main stack
// at boot (app_stack_paint_msp); app_stack_msp_high_water() reports the peak
// bytes used since then. Both no-ops / 0 in the simulator.
void app_stack_paint_msp(void);
uint32_t app_stack_msp_high_water(void);

// Core 1 stack high-water (SCRATCH_X, 4 KB): core1_entry paints it first
// thing (app_stack_paint_core1, called ON Core 1); the `stack` dev command
// reports app_stack_core1_high_water() — peak bytes used since then — so the
// verified-TLS handshake depth on Core 1 can be measured on hardware.
// No-ops / 0 in the simulator.
void app_stack_paint_core1(void);
uint32_t app_stack_core1_high_water(void);
uint32_t app_stack_core1_size(void);
