#pragma once

#include "lua.h"
#include "small_alloc.h"
#include <stdbool.h>

// Zero the heap by DMA through the uncached alias (firmware with QMI PSRAM;
// a no-op elsewhere): start it once the QMI PSRAM is in quad mode, and wait
// for it before Core 1 runs from flash (the clear saturates the QMI). Nothing
// may touch the heap in between.
void lua_psram_alloc_clear_start(void);
void lua_psram_alloc_clear_wait(void);

// Initialize the PSRAM allocator (waits for the clear first). Call once on boot.
void lua_psram_alloc_init(void);

// Allocator function compatible with lua_Alloc. Requests of up to
// SMALL_ALLOC_MAX bytes are packed into small_alloc slabs carved from umm;
// larger ones are umm blocks. Core 0 (the Lua VM) only.
void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

// The running VM's small-object pools (all zero when no VM is running).
// Their slabs are umm blocks, so psram_free / largest block already count them.
void lua_psram_alloc_small_stats(small_stats_t *out);

// Create a new Lua state using the PSRAM allocator
lua_State *lua_psram_newstate(void);

// Memory stats for the PSRAM Lua heap
size_t lua_psram_alloc_free_size(void);
size_t lua_psram_alloc_total_size(void);
// Largest single allocation that would currently succeed.  Total free bytes
// can be large while this is small (fragmentation) — this is the number that
// decides whether a big app image or buffer will load.
size_t lua_psram_alloc_largest_block(void);
// umm_malloc fragmentation metric, 0 (contiguous) .. 100 (shattered).
int    lua_psram_alloc_fragmentation(void);

// Returns true when free heap has fallen below PSRAM_LOW_WATERMARK.
// Used by the Lua debug hook to trigger GC before allocations start failing.
#define PSRAM_LOW_WATERMARK (512u * 1024u)
bool lua_psram_alloc_is_low(void);

// Core 1 dedicated memory pool accessors.
// Returns a pointer to the 128KB region at the end of the PSRAM allocation
// (NULL on boards without RP2350 PSRAM).
void  *lua_psram_get_core1_pool(void);
size_t lua_psram_get_core1_pool_size(void);
