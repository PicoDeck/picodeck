// umm_* on the host heap for unit tests (declared by simulator/stubs/umm_malloc.h).
// umm_fake_live() counts allocations not yet freed (test_sound checks that
// sample structs come from here, not the SRAM heap).
#include "umm_malloc.h"

static size_t s_live;

size_t umm_fake_live(void) { return s_live; }

void *umm_malloc(size_t size) {
  void *p = malloc(size);
  if (p) s_live++;
  return p;
}

void *umm_calloc(size_t num, size_t size) {
  void *p = calloc(num, size);
  if (p) s_live++;
  return p;
}

void *umm_realloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size);
  if (!ptr && p) s_live++;  // realloc(NULL, n) allocates
  return p;
}

void umm_free(void *ptr) {
  if (ptr) s_live--;
  free(ptr);
}
size_t umm_free_heap_size(void) { return SIM_UMM_HEAP_SIZE; }
size_t umm_max_free_block_size(void) { return SIM_UMM_HEAP_SIZE; }
