#include "core1_alloc.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// Block header: sits before every allocation (free or used).
// Free blocks form a singly-linked list sorted by address.
// Adjacent free blocks are coalesced on free().

#define BLOCK_MAGIC  0xC1A110C1u  // "C1 ALLOC 1"
#define ALIGN        8u           // 8-byte alignment (ARM requirement)
#define ALIGN_UP(x)  (((x) + ALIGN - 1) & ~(ALIGN - 1))

typedef struct block {
    uint32_t       magic;   // BLOCK_MAGIC when valid
    uint32_t       size;    // total block size including header
    struct block  *next;    // next block in pool (used or free); NULL at end
    uint32_t       used;    // 1 = allocated, 0 = free
} block_t;

#define HEADER_SIZE  ALIGN_UP(sizeof(block_t))

static uint8_t *s_pool_start;
static uint8_t *s_pool_end;

void core1_alloc_init(void *pool, size_t pool_size) {
    s_pool_start = (uint8_t *)pool;
    s_pool_end   = s_pool_start + pool_size;

    // Single free block spanning the entire pool
    block_t *b  = (block_t *)s_pool_start;
    b->magic    = BLOCK_MAGIC;
    b->size     = pool_size;
    b->next     = NULL;
    b->used     = 0;
}

void *core1_malloc(size_t size) {
    if (size == 0) return NULL;

    uint32_t needed = HEADER_SIZE + ALIGN_UP(size);

    // First-fit search
    block_t *b = (block_t *)s_pool_start;

    while (b) {
        if (!b->used && b->size >= needed) {
            // Split if remainder is large enough for another block
            uint32_t remain = b->size - needed;
            if (remain >= HEADER_SIZE + ALIGN) {
                block_t *split = (block_t *)((uint8_t *)b + needed);
                split->magic = BLOCK_MAGIC;
                split->size  = remain;
                split->next  = b->next;
                split->used  = 0;
                b->size = needed;
                b->next = split;
            }
            b->used = 1;
            return (uint8_t *)b + HEADER_SIZE;
        }
        b = b->next;
    }

    printf("[CORE1_ALLOC] OOM: requested %u bytes\n", (unsigned)size);
    return NULL;
}

void *core1_calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) return NULL;
    if (size > SIZE_MAX / count) {
        printf("[CORE1_ALLOC] calloc overflow: %zu * %zu\n", count, size);
        return NULL;
    }
    size_t total = count * size;
    void *p = core1_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void core1_free(void *ptr) {
    if (!ptr) return;

    block_t *b = (block_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (!core1_owns(b)) {
        printf("[CORE1_ALLOC] free out of range: %p\n", ptr);
        return;
    }
    if (b->magic != BLOCK_MAGIC) {
        printf("[CORE1_ALLOC] bad free: %p (magic=%08x)\n", ptr, (unsigned)b->magic);
        return;
    }
    b->used = 0;

    // Coalesce with next block if free
    block_t *next = b->next;
    if (next && !next->used) {
        b->size += next->size;
        b->next  = next->next;
        next->magic = 0; // invalidate merged header
    }

    // Coalesce with previous block if free (scan from start)
    block_t *scan = (block_t *)s_pool_start;
    while (scan && scan != b) {
        if (!scan->used && scan->next == b) {
            scan->size += b->size;
            scan->next  = b->next;
            b->magic = 0;
            break;
        }
        scan = scan->next;
    }
}

void core1_alloc_stats(size_t *free_total, size_t *largest) {
    size_t total = 0, big = 0;
    for (const block_t *b = (const block_t *)s_pool_start; b; b = b->next) {
        if (!b->used) {
            size_t n = b->size - HEADER_SIZE;
            total += n;
            if (n > big) big = n;
        }
    }
    *free_total = total;
    *largest = big;
}

bool core1_owns(const void *ptr) {
    return (const uint8_t *)ptr >= s_pool_start &&
           (const uint8_t *)ptr <  s_pool_end;
}
