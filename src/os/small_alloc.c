// Small-object pools in front of umm for the Lua heap (see small_alloc.h).
//
// Layout: each slab is one backing allocation of SMALL_SLAB_BYTES holding a
// small_slab_t header followed by equal-sized slots of one size class. A slot
// is handed out from the slab's free list, or else from its never-used tail
// (`fresh`), so a new slab costs no initialisation pass over PSRAM.
//
// Every slab sits in `dir`, sorted by address; small_owns() is a binary search
// over it, which is how a pointer is routed on free/realloc (the caller's size
// is never trusted for that). Slabs of a class with a free slot sit on the
// class's `partial` list. A slab whose last object is freed is returned to the
// backing heap, except for one `spare` kept (in the directory, classless) to
// absorb alloc/free ping-pong at a slab boundary.
#include "small_alloc.h"

#include <string.h>

// ASan builds (simulator-asan, unit tests): free slots and the slack past
// each request are poisoned, so overflows and use-after-free inside a slab
// are still caught although the host malloc no longer sees each object.
#if defined(__SANITIZE_ADDRESS__)
#define SMALL_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SMALL_ASAN 1
#endif
#endif
#ifdef SMALL_ASAN
#include <sanitizer/asan_interface.h>
#define POISON(p, n)   ASAN_POISON_MEMORY_REGION((p), (n))
#define UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define POISON(p, n)   ((void)(p), (void)(n))
#define UNPOISON(p, n) ((void)(p), (void)(n))
#endif

#define NO_CLASS 0xFF

typedef struct small_slab {
  struct small_slab *next, *prev;  // class partial list
  void *free_list;                 // freed slots (first word links them)
  uint8_t *slots;                  // first slot, SMALL_ALLOC_ALIGN aligned
  uint16_t cap;                    // slots in this slab
  uint16_t used;                   // live objects
  uint16_t fresh;                  // slots [fresh, cap) never handed out
  uint8_t cls;                     // size class, NO_CLASS for the spare
  uint8_t on_partial;
} small_slab_t;

struct small_heap {
  small_backing_t backing;
  small_slab_t *partial[SMALL_CLASS_COUNT];
  small_slab_t *spare;
  small_slab_t **dir;   // every slab, ascending address
  small_slab_t *hint;   // the slab that last owned a pointer (slab_of_hint)
  uint32_t dir_len, dir_cap;
  uint32_t objects;
  uint32_t object_bytes;
};

// Class sizes in granules of SMALL_ALLOC_ALIGN (8 bytes on the device):
// 16 24 32 40 48 56 64 80 96 112 128 on the device, twice that on a 64-bit
// host. 8-byte steps up to 64 bytes cover tables (32), short strings, closures
// and upvalues tightly; 16-byte steps above that.
static const uint8_t k_class_granules[SMALL_CLASS_COUNT] = {
    2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16};
// Granules (1..16) -> class.
static const uint8_t k_class_of_granules[17] = {
    0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 7, 8, 8, 9, 9, 10, 10};

size_t small_class_size(int cls) {
  return (size_t)k_class_granules[cls] * SMALL_ALLOC_ALIGN;
}

int small_class_of(size_t size) {
  if (size == 0 || size > SMALL_ALLOC_MAX) return -1;
  return k_class_of_granules[(size + SMALL_ALLOC_ALIGN - 1) / SMALL_ALLOC_ALIGN];
}

// ── Slab directory ─────────────────────────────────────────────────────────

// Index of the last slab starting at or below p, or -1.
static int dir_floor(const small_heap_t *h, const void *p) {
  int lo = 0, hi = (int)h->dir_len - 1, found = -1;
  uintptr_t a = (uintptr_t)p;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if ((uintptr_t)h->dir[mid] <= a) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return found;
}

static small_slab_t *slab_of(const small_heap_t *h, const void *p) {
  if (!p || h->dir_len == 0) return NULL;
  int i = dir_floor(h, p);
  if (i < 0) return NULL;
  small_slab_t *s = h->dir[i];
  if ((uintptr_t)p >= (uintptr_t)s + SMALL_SLAB_BYTES) return NULL;
  // Inside the slab: only its slots are objects (the header never is).
  if ((uint8_t *)p < s->slots) return NULL;
  return s;
}

// slab_of for a pointer being freed or resized, which is mostly in the slab of
// the one before it (97.7% of the calls on a host run of Nova Rail's heap under
// churn, against 6.4 steps of the search over 40 slabs): one compare instead of
// the search's PSRAM reads. Sweeping frees one object at a time.
static small_slab_t *slab_of_hint(small_heap_t *h, const void *p) {
  small_slab_t *s = h->hint;
  if (s && (uintptr_t)p - (uintptr_t)s < SMALL_SLAB_BYTES && (uint8_t *)p >= s->slots)
    return s;
  s = slab_of(h, p);
  if (s) h->hint = s;
  return s;
}

static bool dir_insert(small_heap_t *h, small_slab_t *s) {
  if (h->dir_len == h->dir_cap) {
    uint32_t cap = h->dir_cap ? h->dir_cap * 2 : 16;
    small_slab_t **d = h->backing.realloc(h->dir, cap * sizeof(*d));
    if (!d) return false;
    h->dir = d;
    h->dir_cap = cap;
  }
  int at = dir_floor(h, s) + 1;
  memmove(&h->dir[at + 1], &h->dir[at], (h->dir_len - (uint32_t)at) * sizeof(*h->dir));
  h->dir[at] = s;
  h->dir_len++;
  return true;
}

static void dir_remove(small_heap_t *h, small_slab_t *s) {
  int at = dir_floor(h, s);
  if (at < 0 || h->dir[at] != s) return;
  h->dir_len--;
  memmove(&h->dir[at], &h->dir[at + 1], (h->dir_len - (uint32_t)at) * sizeof(*h->dir));
}

// ── Partial lists ──────────────────────────────────────────────────────────

static void partial_push(small_heap_t *h, small_slab_t *s) {
  small_slab_t **head = &h->partial[s->cls];
  s->prev = NULL;
  s->next = *head;
  if (*head) (*head)->prev = s;
  *head = s;
  s->on_partial = 1;
}

static void partial_unlink(small_heap_t *h, small_slab_t *s) {
  if (!s->on_partial) return;
  if (s->prev) s->prev->next = s->next;
  else h->partial[s->cls] = s->next;
  if (s->next) s->next->prev = s->prev;
  s->next = s->prev = NULL;
  s->on_partial = 0;
}

// ── Slabs ──────────────────────────────────────────────────────────────────

static void slab_format(small_slab_t *s, int cls) {
  uintptr_t first = ((uintptr_t)(s + 1) + SMALL_ALLOC_ALIGN - 1) &
                    ~(uintptr_t)(SMALL_ALLOC_ALIGN - 1);
  size_t size = small_class_size(cls);
  s->slots = (uint8_t *)first;
  s->cap = (uint16_t)(((uintptr_t)s + SMALL_SLAB_BYTES - first) / size);
  s->used = 0;
  s->fresh = 0;
  s->free_list = NULL;
  s->cls = (uint8_t)cls;
  s->on_partial = 0;
  s->next = s->prev = NULL;
  POISON(s->slots, (size_t)s->cap * size);
}

static void slab_release(small_heap_t *h, small_slab_t *s) {
  if (h->hint == s) h->hint = NULL;
  dir_remove(h, s);
  UNPOISON(s, SMALL_SLAB_BYTES);
  h->backing.free(s);
}

// A formatted, empty slab for `cls` on the partial list, or NULL.
static small_slab_t *slab_new(small_heap_t *h, int cls) {
  small_slab_t *s = h->spare;
  if (s) {
    h->spare = NULL;
    UNPOISON(s->slots, (uintptr_t)s + SMALL_SLAB_BYTES - (uintptr_t)s->slots);
  } else {
    s = h->backing.alloc(SMALL_SLAB_BYTES);
    if (!s) return NULL;
    if (!dir_insert(h, s)) {
      h->backing.free(s);
      return NULL;
    }
  }
  slab_format(s, cls);
  partial_push(h, s);
  return s;
}

static void *slot_take(small_heap_t *h, int cls, size_t size) {
  small_slab_t *s = h->partial[cls];
  if (!s) {
    s = slab_new(h, cls);
    if (!s) return NULL;
  }
  size_t csize = small_class_size(cls);
  void *p;
  if (s->free_list) {
    p = s->free_list;
    UNPOISON(p, sizeof(void *));
    memcpy(&s->free_list, p, sizeof(void *));
  } else {
    p = s->slots + (size_t)s->fresh * csize;
    s->fresh++;
  }
  POISON(p, csize);
  UNPOISON(p, size);
  s->used++;
  if (s->used == s->cap) partial_unlink(h, s);
  h->objects++;
  h->object_bytes += (uint32_t)csize;
  return p;
}

static void slot_give(small_heap_t *h, small_slab_t *s, void *p) {
  size_t csize = small_class_size(s->cls);
  bool was_full = s->used == s->cap;
  POISON(p, csize);
  UNPOISON(p, sizeof(void *));
  memcpy(p, &s->free_list, sizeof(void *));
  POISON(p, sizeof(void *));
  s->free_list = p;
  s->used--;
  h->objects--;
  h->object_bytes -= (uint32_t)csize;
  if (s->used == 0) {
    partial_unlink(h, s);
    if (!h->spare) {
      s->cls = NO_CLASS;
      h->spare = s;
    } else {
      slab_release(h, s);
    }
  } else if (was_full) {
    partial_push(h, s);
  }
}

// ── Public ─────────────────────────────────────────────────────────────────

small_heap_t *small_create(const small_backing_t *backing) {
  small_heap_t *h = backing->alloc(sizeof(*h));
  if (!h) return NULL;
  memset(h, 0, sizeof(*h));
  h->backing = *backing;
  return h;
}

void small_destroy(small_heap_t *h) {
  if (!h) return;
  while (h->dir_len) slab_release(h, h->dir[h->dir_len - 1]);
  h->backing.free(h->dir);
  void (*backing_free)(void *) = h->backing.free;
  backing_free(h);
}

bool small_owns(const small_heap_t *h, const void *ptr) {
  return slab_of(h, ptr) != NULL;
}

void *small_alloc(small_heap_t *h, size_t size) {
  if (size == 0) return NULL;  // Lua never wants a block for zero bytes
  int cls = small_class_of(size);
  if (cls >= 0) {
    void *p = slot_take(h, cls, size);
    if (p) return p;
    // No slab-sized block left: a block of its own is better than failing.
  }
  return h->backing.alloc(size);
}

void small_free(small_heap_t *h, void *ptr, size_t osize) {
  (void)osize;
  if (!ptr) return;
  small_slab_t *s = slab_of_hint(h, ptr);
  if (s) slot_give(h, s, ptr);
  else h->backing.free(ptr);
}

void *small_realloc(small_heap_t *h, void *ptr, size_t osize, size_t nsize) {
  // nsize 0 first: Lua frees an absent block as realloc(NULL, 0, 0), which
  // must not allocate anything.
  if (nsize == 0) {
    small_free(h, ptr, osize);
    return NULL;
  }
  if (!ptr) return small_alloc(h, nsize);
  small_slab_t *s = slab_of_hint(h, ptr);
  int ncls = small_class_of(nsize);

  if (!s) {
    // A backing block: stays there unless it shrinks into a pool class.
    if (ncls < 0) return h->backing.realloc(ptr, nsize);
    void *q = slot_take(h, ncls, nsize);
    if (!q) return h->backing.realloc(ptr, nsize);
    memcpy(q, ptr, osize < nsize ? osize : nsize);
    h->backing.free(ptr);
    return q;
  }

  size_t csize = small_class_size(s->cls);
  size_t keep = osize < nsize ? osize : nsize;
  if (keep > csize) keep = csize;
  if (ncls == s->cls) {
    // Same class: the slot already has room. Re-poison past the new size.
    POISON(ptr, csize);
    UNPOISON(ptr, nsize);
    return ptr;
  }
  void *q;
  if (nsize < csize) {
    // Down a class. Lua relies on shrinking never failing: if the smaller
    // class has no slab to give, the object stays in its roomier slot.
    q = slot_take(h, ncls, nsize);
    if (!q) {
      POISON(ptr, csize);
      UNPOISON(ptr, nsize);
      return ptr;
    }
  } else {
    // Up a class, or out of the pools.
    q = ncls >= 0 ? slot_take(h, ncls, nsize) : NULL;
    if (!q) q = h->backing.alloc(nsize);
    if (!q) return NULL;
  }
  memcpy(q, ptr, keep);
  slot_give(h, s, ptr);
  return q;
}

uint32_t small_live_objects(const small_heap_t *h) {
  return h ? h->objects : 0;
}

void small_stats(const small_heap_t *h, small_stats_t *out) {
  memset(out, 0, sizeof(*out));
  if (!h) return;
  out->slabs = h->dir_len;
  out->slab_bytes = (uint32_t)(h->dir_len * SMALL_SLAB_BYTES);
  out->objects = h->objects;
  out->object_bytes = h->object_bytes;
}
