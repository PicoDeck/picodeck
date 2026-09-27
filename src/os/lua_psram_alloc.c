#include "lua_psram_alloc.h"
#include "crashlog.h"
#include "small_alloc.h"
#include "launcher.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "umm_malloc.h"
#include "umm_malloc_cfg.h"
#include "hardware/watchdog.h"
#include "pico/critical_section.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Hardware spinlock protecting all umm_malloc heap operations across both cores.
// Referenced by UMM_CRITICAL_ENTRY/EXIT in umm_malloc_cfgport.h.
critical_section_t g_umm_critsec;

// Allocate PSRAM for the Lua VM heap.
// On RP2350, reserve 128KB at the end of the 6MB region for Core 1's
// dedicated allocator (WiFi/Mongoose) to eliminate cross-core contention.
#ifdef PICODECK_SIMULATOR
  #define CORE1_POOL_SIZE 0
  static uint8_t *s_lua_psram_heap = NULL;
  uint32_t UMM_MALLOC_CFG_HEAP_SIZE = 8 * 1024 * 1024;
#elif defined(PICO_RP2350)
  #define CORE1_POOL_SIZE (128 * 1024)
  static uint8_t *s_lua_psram_heap = (uint8_t *)0x11200000;
  uint32_t UMM_MALLOC_CFG_HEAP_SIZE = 6 * 1024 * 1024 - CORE1_POOL_SIZE;
#else
  #define CORE1_POOL_SIZE 0
  static uint8_t s_lua_psram_heap[256 * 1024]; // Fallback to 256K on regular Pico
  uint32_t UMM_MALLOC_CFG_HEAP_SIZE = sizeof(s_lua_psram_heap);
#endif

// Satisfy umm_malloc.c externs even if we explicitly use umm_init_heap
#ifdef PICO_RP2350
void *UMM_MALLOC_CFG_HEAP_ADDR = NULL; // We initialize umm_malloc manually
#else
void *UMM_MALLOC_CFG_HEAP_ADDR = NULL; // Initialized in lua_psram_alloc_init
#endif

// Unprotected Lua error (an error raised outside any pcall — typically an
// allocation failure inside lua_newstate/lua_close or a bridge bug).  Lua
// would call abort() when this returns, which halts the core until the 10s
// watchdog fires and reboots with no record of what happened.  Instead:
// record the error and heap state to /system/error.log, show it on screen,
// then reboot deliberately.
static int l_panic(lua_State *L) {
  const char *msg = (lua_type(L, -1) == LUA_TSTRING)
                        ? lua_tostring(L, -1)
                        : "error object is not a string";
  const char *app = launcher_get_running_app_name();
  char heap[96];
  crashlog_describe_heap(heap, sizeof(heap));
  printf("PANIC: unprotected error in call to Lua API (%s) [%s]\n", msg, heap);

  crashlog_write("LUA PANIC", app, "unprotected error in call to Lua API", msg);
  // The panic is now on record with its app name; drop the dirty-exit marker
  // so the next boot does not report the same event a second time.
  crashlog_clear_running();

  display_clear(COLOR_BLACK);
  display_draw_text(4, 4, "Lua panic (unprotected error):", COLOR_RED, COLOR_BLACK);
  // Word-wrap the message at 52 columns.
  int col = 0, row = 1;
  char line[54] = {0};
  for (int i = 0; msg[i] && row < 30; i++) {
    if (msg[i] != '\n') line[col++] = msg[i];
    if (col >= 52 || msg[i] == '\n') {
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
    row++;
  }
  row++;
  display_draw_text(4, 4 + row * 9, heap, COLOR_GRAY, COLOR_BLACK);
  if (app) {
    row++;
    display_draw_text(4, 4 + row * 9, app, COLOR_GRAY, COLOR_BLACK);
  }
  display_draw_text(4, FB_HEIGHT - 12, "Logged to /system/error.log - rebooting",
                    COLOR_GRAY, COLOR_BLACK);
  display_flush();

  for (int i = 0; i < 40; i++) {
    watchdog_update();
    sleep_ms(100);
  }
  stdio_flush();
  kbd_prepare_reset();
  watchdog_reboot(0, 0, 0);
  for (;;) tight_loop_contents();
  return 0;
}

static void l_warnfoff(void *ud, const char *message, int tocont) {
  (void)ud;
  (void)message;
  (void)tocont;
}

void lua_psram_alloc_init(void) {
  critical_section_init(&g_umm_critsec);
#ifdef PICODECK_SIMULATOR
  // 8 MB for the counting allocator, the device size under --real-umm
  // (the simulator's umm owns its own arena; this buffer is never used).
  UMM_MALLOC_CFG_HEAP_SIZE = (uint32_t)sim_umm_heap_size();
  if (!s_lua_psram_heap) {
    s_lua_psram_heap = malloc(UMM_MALLOC_CFG_HEAP_SIZE);
    if (!s_lua_psram_heap) {
      printf("FATAL: Failed to allocate Lua PSRAM heap in simulator\n");
      return;
    }
  }
#endif
  umm_init_heap(s_lua_psram_heap, UMM_MALLOC_CFG_HEAP_SIZE);
  printf("PSRAM Lua Allocator Initialized: %d bytes\n",
         (int)UMM_MALLOC_CFG_HEAP_SIZE);
  
  // Debug: check free size immediately after init
  size_t free_after_init = umm_free_heap_size();
  printf("[PSRAM] Free immediately after init: %zu bytes (%zuK)\n", 
         free_after_init, free_after_init / 1024);
}

#ifdef PICODECK_LUA_ALLOC_HISTOGRAM
// Opt-in measurement build (-DPICODECK_LUA_ALLOC_HISTOGRAM): histograms the Lua
// allocator's request sizes and prints them when the VM's last block is freed
// (lua_close). Lua passes the old block size on every realloc/free, so live
// objects per size bin are exact. Bins: 8-byte steps up to 256, then powers
// of two. Not for release builds (static counters).
#define HIST_FINE_BINS 32          // 1..256 in 8-byte steps
#define HIST_BINS (HIST_FINE_BINS + 6)  // 257-512 .. >8K
static uint32_t s_hist_events[HIST_BINS];   // allocations requested per bin
static int32_t  s_hist_live[HIST_BINS];     // live objects per bin now
static int32_t  s_hist_at_peak[HIST_BINS];  // live objects per bin at peak
static int32_t  s_hist_live_total, s_hist_peak_total;
static size_t   s_hist_live_bytes, s_hist_peak_bytes;

static int hist_bin(size_t n) {
  if (n <= 256) return (int)((n + 7) / 8) - 1;
  int b = HIST_FINE_BINS;
  for (size_t lim = 512; n > lim && b < HIST_BINS - 1; lim <<= 1) b++;
  return b;
}

static void hist_label(int b, char *out, size_t len) {
  if (b < HIST_FINE_BINS) {
    snprintf(out, len, "%d-%d", b * 8 + 1, b * 8 + 8);
  } else if (b == HIST_BINS - 1) {
    snprintf(out, len, ">%d", 256 << (HIST_BINS - 1 - HIST_FINE_BINS));
  } else {
    int lo = 256 << (b - HIST_FINE_BINS);
    snprintf(out, len, "%d-%d", lo + 1, lo * 2);
  }
}

static void hist_dump(void) {
  printf("[ALLOCHIST] bin events live_at_peak (peak objects=%ld bytes=%lu)\n",
         (long)s_hist_peak_total, (unsigned long)s_hist_peak_bytes);
  for (int b = 0; b < HIST_BINS; b++) {
    if (!s_hist_events[b]) continue;
    char label[24];
    hist_label(b, label, sizeof(label));
    printf("[ALLOCHIST] %s %lu %ld\n", label, (unsigned long)s_hist_events[b],
           (long)s_hist_at_peak[b]);
  }
  memset(s_hist_events, 0, sizeof(s_hist_events));
  memset(s_hist_at_peak, 0, sizeof(s_hist_at_peak));
  s_hist_peak_total = 0;
  s_hist_peak_bytes = 0;
}

static void hist_record(void *ptr, size_t osize, size_t nsize) {
  if (ptr) {
    s_hist_live[hist_bin(osize)]--;
    s_hist_live_total--;
    s_hist_live_bytes -= osize;
  }
  if (nsize) {
    s_hist_events[hist_bin(nsize)]++;
    s_hist_live[hist_bin(nsize)]++;
    s_hist_live_total++;
    s_hist_live_bytes += nsize;
    if (s_hist_live_bytes > s_hist_peak_bytes) {
      s_hist_peak_bytes = s_hist_live_bytes;
      s_hist_peak_total = s_hist_live_total;
      memcpy(s_hist_at_peak, s_hist_live, sizeof(s_hist_live));
    }
  }
  if (ptr && !nsize && s_hist_live_total == 0) hist_dump();
}
#endif

// Small Lua objects live in size-class slabs carved from umm (small_alloc.c):
// a 32-byte table no longer costs a whole 200-byte umm block, and the heap is
// no longer capped at ~30,800 live objects by umm's 15-bit block index.
// Larger requests go to umm as before. The pools are created on the VM's
// first small request and destroyed when its last pooled object is freed
// (lua_close), so no slab or bookkeeping block outlives the app and the heap
// is whole again for the next one (C-Dogs needs one ~5.9 MB block).
// Core 0 only (the Lua VM); umm's own critical section guards the slabs'
// allocation against Core 1's umm use.
// -DPICODECK_LUA_SMALL_POOLS=0 builds a firmware or simulator without them, for
// before/after measurements.
#ifndef PICODECK_LUA_SMALL_POOLS
#define PICODECK_LUA_SMALL_POOLS 1
#endif
_Static_assert(SMALL_UMM_BLOCK_BYTES == UMM_BLOCK_BODY_SIZE,
               "small_alloc slabs are sized in umm blocks");
static const small_backing_t k_umm_backing = {umm_malloc, umm_realloc, umm_free};
static small_heap_t *s_small;

void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  // Lua passes a type tag, not a size, as osize when ptr is NULL.
  if (!ptr) osize = 0;
#ifdef PICODECK_LUA_ALLOC_HISTOGRAM
  hist_record(ptr, osize, nsize);
#endif

  if (PICODECK_LUA_SMALL_POOLS && !s_small && nsize > 0 && nsize <= SMALL_ALLOC_MAX)
    s_small = small_create(&k_umm_backing);  // NULL: plain umm until it fits

  void *result;
  if (s_small) {
    // Every block of this VM goes through the pools' entry points, pooled or
    // not: they route by address, so nothing reaches the wrong free.
    result = small_realloc(s_small, ptr, osize, nsize);
    if (ptr && small_live_objects(s_small) == 0) {
      small_destroy(s_small);
      s_small = NULL;
    }
  } else if (nsize == 0) {
    umm_free(ptr);  // no pools exist, so ptr is a umm block
    return NULL;
  } else {
    result = umm_realloc(ptr, nsize);
  }
  if (!result && nsize) {
    printf("[PSRAM] OOM: failed to allocate %zu bytes (free=%zu largest=%zu frag=%d%%)\n",
           nsize, umm_free_heap_size(), lua_psram_alloc_largest_block(),
           lua_psram_alloc_fragmentation());
  }
  return result;
}

void lua_psram_alloc_small_stats(small_stats_t *out) {
  small_stats(s_small, out);
}

size_t lua_psram_alloc_free_size(void) {
  return umm_free_heap_size();
}

bool lua_psram_alloc_is_low(void) {
  return umm_free_heap_size() < PSRAM_LOW_WATERMARK;
}

size_t lua_psram_alloc_total_size(void) {
  return (size_t)UMM_MALLOC_CFG_HEAP_SIZE;
}

size_t lua_psram_alloc_largest_block(void) {
  return umm_max_free_block_size();
}

int lua_psram_alloc_fragmentation(void) {
  return umm_fragmentation_metric();
}

lua_State *lua_psram_newstate(void) {
  lua_State *L = lua_newstate(lua_psram_alloc, NULL);
  if (L) {
    lua_atpanic(L, &l_panic);
    lua_setwarnf(L, l_warnfoff, L);
  }
  return L;
}

void *lua_psram_get_core1_pool(void) {
#if CORE1_POOL_SIZE > 0
    return s_lua_psram_heap + UMM_MALLOC_CFG_HEAP_SIZE;
#else
    return NULL;
#endif
}

size_t lua_psram_get_core1_pool_size(void) {
    return CORE1_POOL_SIZE;
}
