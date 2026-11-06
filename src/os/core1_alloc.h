#ifndef CORE1_ALLOC_H
#define CORE1_ALLOC_H

#include <stddef.h>
#include <stdbool.h>

// Simple first-fit allocator for Core 1 (WiFi/Mongoose).
// Operates on a dedicated PSRAM region — no cross-core locking needed.
// NOT thread-safe: must only be called from Core 1.

void  core1_alloc_init(void *pool, size_t pool_size);
void *core1_malloc(size_t size);
void *core1_calloc(size_t count, size_t size);
void  core1_free(void *ptr);

// Returns true if ptr is within the Core 1 pool (for debug assertions)
bool  core1_owns(const void *ptr);
// Free bytes in total and in the largest block. Core 1 only: it walks the pool.
void  core1_alloc_stats(size_t *free_total, size_t *largest);

#endif // CORE1_ALLOC_H
