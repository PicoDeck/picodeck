// Block body size determines sizeof(umm_block) and thus max addressable heap.
// Block indices are 15-bit (0x7FFF = 32767 max) because bit 15 is the free-
// list flag.  With body_size=128 → sizeof(umm_block)=128 → max heap ~4MB.
// At 200 bytes: sizeof(umm_block)=200, 6MB/200 = 31457 blocks — fits in 15 bits.
#define UMM_BLOCK_BODY_SIZE 200
#define UMM_INLINE_METRICS

// The firmware zeroes the heap before umm_init_heap() (lua_psram_alloc.c: by
// DMA through the uncached alias, before the boot splash; a static array in
// .bss on boards without PSRAM), so the init only writes its block headers:
// the firmware's umm_malloc.c is a copy whose clear honours this
// (cmake/picodeck_umm.cmake). The simulator's --real-umm includes the
// original source, which clears whatever this says.
#ifndef PICODECK_SIMULATOR
#define UMM_HEAP_PRECLEARED
#endif

// ── Multi-core safety ─────────────────────────────────────────────────────────
// umm_malloc is called from both Core 0 (Lua) and Core 1 (Mongoose/WiFi).
// We use a Pico SDK critical_section_t (hardware spinlock + IRQ save/restore)
// to serialise all heap operations across cores.
//
// critical_section_t is non-recursive.  umm_malloc/free do NOT nest in
// practice: umm_realloc handles the NULL-ptr and zero-size edge cases BEFORE
// acquiring the lock, so there is no nested acquisition path.
#include "pico/critical_section.h"

// Defined in lua_psram_alloc.c and initialised in lua_psram_alloc_init().
extern critical_section_t g_umm_critsec;

#define UMM_CRITICAL_DECL(tag)
#define UMM_CRITICAL_ENTRY(tag)  critical_section_enter_blocking(&g_umm_critsec)
#define UMM_CRITICAL_EXIT(tag)   critical_section_exit(&g_umm_critsec)
