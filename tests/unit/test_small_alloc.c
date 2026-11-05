// Host tests for src/os/small_alloc.c: size classes, pooling, realloc across
// classes and to/from the backing heap, free routing (a pooled pointer never
// reaches the backing free, a backing block always does), slab release, the
// out-of-memory fallbacks, and ASan poisoning of free slots.
#include "check.h"
#include "small_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#define HAVE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HAVE_ASAN 1
#endif
#endif
#ifdef HAVE_ASAN
#include <sanitizer/asan_interface.h>
#endif

// ── Fake backing heap: tracks every block it hands out ─────────────────────
#define FAKE_MAX 4096
static void  *s_blocks[FAKE_MAX];
static size_t s_sizes[FAKE_MAX];
static int    s_nblocks;
static int    s_bad_frees;     // free/realloc of a pointer we never handed out
static int    s_fail_after = -1;  // allocations left before refusing (-1: never)
static int    s_allocs, s_frees;

static int fake_find(void *p) {
  for (int i = 0; i < s_nblocks; i++)
    if (s_blocks[i] == p) return i;
  return -1;
}
static unsigned s_fail_rng;        // non-zero: refuse ~1 in 16 at random
static bool fake_refuse(void) {
  if (s_fail_rng) {
    s_fail_rng = s_fail_rng * 1103515245u + 12345u;
    if (((s_fail_rng >> 16) & 15) == 0) return true;
  }
  if (s_fail_after < 0) return false;
  if (s_fail_after == 0) return true;
  s_fail_after--;
  return false;
}
static void *fake_alloc(size_t n) {
  if (fake_refuse() || s_nblocks >= FAKE_MAX) return NULL;
  void *p = malloc(n);
  s_blocks[s_nblocks] = p;
  s_sizes[s_nblocks++] = n;
  s_allocs++;
  return p;
}
static void fake_free(void *p) {
  if (!p) return;
  int i = fake_find(p);
  if (i < 0) { s_bad_frees++; return; }
  free(p);
  s_blocks[i] = s_blocks[--s_nblocks];
  s_sizes[i] = s_sizes[s_nblocks];
  s_frees++;
}
static void *fake_realloc(void *p, size_t n) {
  if (!p) return fake_alloc(n);
  int i = fake_find(p);
  if (i < 0) { s_bad_frees++; return NULL; }
  if (n > s_sizes[i] && fake_refuse()) return NULL;
  void *q = realloc(p, n);
  if (!q) return NULL;
  s_blocks[i] = q;
  s_sizes[i] = n;
  return q;
}
static const small_backing_t k_backing = {fake_alloc, fake_realloc, fake_free};

static void fake_reset(void) {
  s_nblocks = 0;
  s_bad_frees = 0;
  s_fail_after = -1;
  s_fail_rng = 0;
  s_allocs = s_frees = 0;
}

static small_heap_t *new_heap(void) {
  fake_reset();
  small_heap_t *h = small_create(&k_backing);
  CHECK(h != NULL);
  return h;
}

static void end_heap(small_heap_t *h) {
  small_destroy(h);
  CHECK_EQ_INT(s_nblocks, 0);       // every slab and the bookkeeping returned
  CHECK_EQ_INT(s_bad_frees, 0);     // and nothing foreign reached the backing
}

static void fill(void *p, size_t n, unsigned seed) {
  for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)(seed + i * 7);
}
static bool holds(const void *p, size_t n, unsigned seed) {
  for (size_t i = 0; i < n; i++)
    if (((const uint8_t *)p)[i] != (uint8_t)(seed + i * 7)) return false;
  return true;
}

// ── Tests ──────────────────────────────────────────────────────────────────

static void test_classes(void) {
  size_t prev = 0;
  for (int c = 0; c < SMALL_CLASS_COUNT; c++) {
    size_t sz = small_class_size(c);
    CHECK(sz > prev);
    CHECK_EQ_INT(sz % SMALL_ALLOC_ALIGN, 0);
    prev = sz;
  }
  CHECK_EQ_INT(small_class_size(SMALL_CLASS_COUNT - 1), SMALL_ALLOC_MAX);
  // class_of is the smallest class that fits, for every pooled size
  for (size_t s = 1; s <= SMALL_ALLOC_MAX; s++) {
    int c = small_class_of(s);
    CHECK(c >= 0 && c < SMALL_CLASS_COUNT);
    if (c < 0 || c >= SMALL_CLASS_COUNT) continue;
    CHECK(small_class_size(c) >= s);
    if (c > 0) CHECK(small_class_size(c - 1) < s);
  }
  CHECK_EQ_INT(small_class_of(SMALL_ALLOC_MAX + 1), -1);
  CHECK_EQ_INT(small_class_of(0), -1);
}

static void test_small_is_pooled(void) {
  small_heap_t *h = new_heap();
  enum { N = 64 };
  void *p[N];
  for (int i = 0; i < N; i++) {
    size_t n = 1 + (size_t)i * (SMALL_ALLOC_MAX - 1) / (N - 1);
    p[i] = small_alloc(h, n);
    CHECK(p[i] != NULL);
    CHECK(small_owns(h, p[i]));
    CHECK(fake_find(p[i]) < 0);  // not a backing block of its own
    CHECK_EQ_INT((uintptr_t)p[i] % SMALL_ALLOC_ALIGN, 0);
    fill(p[i], n, (unsigned)i);
  }
  for (int i = 0; i < N; i++) {
    size_t n = 1 + (size_t)i * (SMALL_ALLOC_MAX - 1) / (N - 1);
    CHECK(holds(p[i], n, (unsigned)i));  // no two objects overlap
  }
  small_stats_t st;
  small_stats(h, &st);
  CHECK_EQ_INT(st.objects, N);
  CHECK_EQ_INT(small_live_objects(h), N);
  CHECK(st.slabs >= 1 && st.slabs <= SMALL_CLASS_COUNT);
  CHECK_EQ_INT(st.slab_bytes, st.slabs * SMALL_SLAB_BYTES);
  for (int i = 0; i < N; i++) {
    size_t n = 1 + (size_t)i * (SMALL_ALLOC_MAX - 1) / (N - 1);
    small_free(h, p[i], n);
  }
  small_stats(h, &st);
  CHECK_EQ_INT(st.objects, 0);
  CHECK_EQ_INT(small_live_objects(h), 0);
  CHECK_EQ_INT(small_live_objects(NULL), 0);
  CHECK_EQ_INT(st.object_bytes, 0);
  CHECK(st.slabs <= 1);  // at most the one spare survives
  end_heap(h);
}

static void test_large_passes_through(void) {
  small_heap_t *h = new_heap();
  void *p = small_alloc(h, SMALL_ALLOC_MAX + 1);
  CHECK(p != NULL);
  CHECK(!small_owns(h, p));
  CHECK(fake_find(p) >= 0);
  int frees = s_frees;
  small_free(h, p, SMALL_ALLOC_MAX + 1);
  CHECK_EQ_INT(s_frees, frees + 1);  // the backing got exactly this block
  CHECK(fake_find(p) < 0);
  end_heap(h);
}

static void test_free_routing(void) {
  small_heap_t *h = new_heap();
  void *a = small_alloc(h, 24);
  void *b = small_alloc(h, 24);
  int frees = s_frees;
  small_free(h, a, 24);       // pooled: the backing must not see it
  CHECK_EQ_INT(s_frees, frees);
  CHECK_EQ_INT(s_bad_frees, 0);
  // Ownership is by address, not by the caller's size: a wrong osize still
  // routes a pooled pointer to its slab.
  small_free(h, b, SMALL_ALLOC_MAX * 4);
  CHECK_EQ_INT(s_bad_frees, 0);
  small_free(h, NULL, 0);     // free(NULL) is a no-op
  CHECK_EQ_INT(s_bad_frees, 0);
  // Lua frees an absent array part as realloc(NULL, 0, 0): nothing is
  // allocated (a zero-byte backing block would leak - the counting
  // simulator heap lost ~2 MB to this in a table-heavy run).
  int allocs = s_allocs;
  CHECK(small_realloc(h, NULL, 0, 0) == NULL);
  CHECK(small_alloc(h, 0) == NULL);
  CHECK_EQ_INT(s_allocs, allocs);
  end_heap(h);
}

// A backing heap that hands a released slab's memory straight out again, as umm
// does, with the new block starting inside what was the slab.
static uint8_t s_rb_arena[1 << 18] __attribute__((aligned(16)));
static size_t s_rb_used;
static uint8_t *s_rb_last_slab;
static int s_rb_frees;
static void *rb_alloc(size_t n) {
  if (n != SMALL_SLAB_BYTES && s_rb_last_slab) {
    void *p = s_rb_last_slab + 64;
    s_rb_last_slab = NULL;
    return p;
  }
  void *p = s_rb_arena + s_rb_used;
  s_rb_used += (n + 15) & ~(size_t)15;
  CHECK(s_rb_used <= sizeof(s_rb_arena));
  return p;
}
static void *rb_realloc(void *p, size_t n) {
  void *q = s_rb_arena + s_rb_used;
  s_rb_used += (n + 15) & ~(size_t)15;
  if (p) memcpy(q, p, n);
  return q;
}
static void rb_free(void *p) {
  s_rb_last_slab = (uint8_t *)p;
  s_rb_frees++;
}

// The routing's last-slab hint must not outlive its slab: a plain block now at
// those addresses has to reach the backing free, not the slab's free list.
static void test_hint_forgets_a_released_slab(void) {
  enum { N = 200 };  // 128-byte objects: over 6 slabs
  static const small_backing_t rb = {rb_alloc, rb_realloc, rb_free};
  small_heap_t *h = small_create(&rb);
  void *p[N];
  for (int i = 0; i < N; i++) p[i] = small_alloc(h, 128);
  for (int i = 0; i < N; i++) small_free(h, p[i], 128);  // the last slab goes back
  CHECK(small_live_objects(h) == 0);
  void *plain = small_alloc(h, 1000);
  CHECK(!small_owns(h, plain));
  int frees = s_rb_frees;
  small_free(h, plain, 1000);
  CHECK(s_rb_frees == frees + 1);
  CHECK(small_live_objects(h) == 0);
  small_destroy(h);
}

static void test_realloc_same_class(void) {
  small_heap_t *h = new_heap();
  size_t c1 = small_class_size(1);
  void *p = small_alloc(h, c1 - 3);
  fill(p, c1 - 3, 5);
  void *q = small_realloc(h, p, c1 - 3, c1);  // grows within its class
  CHECK(q == p);
  CHECK(holds(q, c1 - 3, 5));
  void *r = small_realloc(h, q, c1, small_class_size(0) + 1);  // shrinks within
  CHECK(r == p);
  small_free(h, r, small_class_size(0) + 1);
  end_heap(h);
}

static void test_realloc_across_classes(void) {
  small_heap_t *h = new_heap();
  size_t s0 = small_class_size(0), s5 = small_class_size(5);
  void *p = small_alloc(h, s0);
  fill(p, s0, 9);
  void *q = small_realloc(h, p, s0, s5);  // up a class: moves, keeps content
  CHECK(q != NULL && q != p);
  CHECK(small_owns(h, q));
  CHECK(holds(q, s0, 9));
  fill(q, s5, 11);
  void *r = small_realloc(h, q, s5, s0);  // down a class: moves, keeps prefix
  CHECK(r != NULL && r != q);
  CHECK(holds(r, s0, 11));
  small_stats_t st;
  small_stats(h, &st);
  CHECK_EQ_INT(st.objects, 1);
  CHECK_EQ_INT(st.object_bytes, s0);
  small_free(h, r, s0);
  end_heap(h);
}

static void test_realloc_to_and_from_backing(void) {
  small_heap_t *h = new_heap();
  size_t big = SMALL_ALLOC_MAX * 3;
  void *p = small_alloc(h, 40);
  fill(p, 40, 3);
  void *q = small_realloc(h, p, 40, big);  // pool -> backing
  CHECK(q != NULL && !small_owns(h, q));
  CHECK(fake_find(q) >= 0);
  CHECK(holds(q, 40, 3));
  fill(q, big, 4);
  void *r = small_realloc(h, q, big, big * 2);  // backing -> backing
  CHECK(r != NULL && !small_owns(h, r) && fake_find(r) >= 0);
  CHECK(holds(r, big, 4));
  int frees = s_frees;
  void *s = small_realloc(h, r, big * 2, 30);  // backing -> pool
  CHECK(s != NULL && small_owns(h, s));
  CHECK(holds(s, 30, 4));
  CHECK_EQ_INT(s_frees, frees + 1);  // the backing block was released
  CHECK(fake_find(r) < 0);
  small_free(h, s, 30);
  end_heap(h);
}

static void test_shrink_never_fails(void) {
  small_heap_t *h = new_heap();
  size_t s6 = small_class_size(6);
  void *p = small_alloc(h, s6);
  fill(p, s6, 1);
  s_fail_after = 0;  // the backing refuses everything from now on
  // No slab can be had for the smaller class: the block stays where it is.
  void *q = small_realloc(h, p, s6, small_class_size(0));
  CHECK(q == p);
  CHECK(holds(q, small_class_size(0), 1));
  // Growing into a class with no slab and no backing memory fails cleanly
  // and leaves the old block intact.
  void *r = small_realloc(h, q, small_class_size(0), SMALL_ALLOC_MAX * 2);
  CHECK(r == NULL);
  CHECK(holds(q, small_class_size(0), 1));
  s_fail_after = -1;
  small_free(h, q, small_class_size(0));
  end_heap(h);
}

static void test_out_of_memory(void) {
  small_heap_t *h = new_heap();
  s_fail_after = 0;  // no slab and no backing block either
  CHECK(small_alloc(h, 16) == NULL);
  CHECK(small_alloc(h, SMALL_ALLOC_MAX * 2) == NULL);
  s_fail_after = -1;
  end_heap(h);
}

// Backing that refuses slab-sized requests but serves small ones: the pool
// falls back to per-object backing blocks, still routed correctly on free.
static void *picky_alloc(size_t n) {
  return n >= SMALL_SLAB_BYTES ? NULL : fake_alloc(n);
}
static void test_fallback_small_block(void) {
  fake_reset();
  const small_backing_t picky = {picky_alloc, fake_realloc, fake_free};
  small_heap_t *h = small_create(&picky);
  CHECK(h != NULL);
  void *p = small_alloc(h, 20);
  CHECK(p != NULL);
  CHECK(!small_owns(h, p));
  CHECK(fake_find(p) >= 0);
  int frees = s_frees;
  small_free(h, p, 20);
  CHECK_EQ_INT(s_frees, frees + 1);
  end_heap(h);
}

static void test_slabs_released(void) {
  small_heap_t *h = new_heap();
  int base = s_nblocks;  // heap bookkeeping
  size_t sz = small_class_size(3);
  int per_slab = (int)(SMALL_SLAB_BYTES / sz);
  int n = per_slab * 3;
  void **p = malloc(sizeof(void *) * (size_t)n);
  for (int i = 0; i < n; i++) p[i] = small_alloc(h, sz);
  small_stats_t st;
  small_stats(h, &st);
  CHECK(st.slabs >= 3);
  for (int i = 0; i < n; i++) small_free(h, p[i], sz);
  small_stats(h, &st);
  CHECK_EQ_INT(st.slabs, 1);         // one spare, the rest went back
  CHECK(s_nblocks <= base + 2);      // + the spare, + a directory resize
  // The spare serves any class.
  void *q = small_alloc(h, small_class_size(8));
  small_stats(h, &st);
  CHECK_EQ_INT(st.slabs, 1);
  small_free(h, q, small_class_size(8));
  free(p);
  end_heap(h);
}

// Random alloc / realloc / free against a shadow table, checking contents.
// With `failing`, the backing refuses about one request in 16: a refused
// alloc/realloc must leave the old block intact and leak nothing.
static void random_stress(bool failing) {
  small_heap_t *h = new_heap();
  if (failing) s_fail_rng = 777;
  enum { SLOTS = 3000, OPS = 100000 };
  static void *ptr[SLOTS];
  static size_t len[SLOTS];
  static unsigned seed[SLOTS];
  memset(ptr, 0, sizeof(ptr));
  unsigned rng = 12345;
  int bad = 0;
  for (int op = 0; op < OPS; op++) {
    rng = rng * 1103515245u + 12345u;
    int i = (int)((rng >> 8) % SLOTS);
    rng = rng * 1103515245u + 12345u;
    // Mostly small sizes, some large ones.
    size_t n = (rng >> 20) % 8 == 0 ? SMALL_ALLOC_MAX + (rng >> 9) % 600
                                    : 1 + (rng >> 9) % SMALL_ALLOC_MAX;
    if (!ptr[i]) {
      ptr[i] = small_alloc(h, n);
      if (!ptr[i]) continue;  // refused
      len[i] = n;
      seed[i] = rng;
      fill(ptr[i], n, seed[i]);
    } else if ((rng >> 4) % 3 == 0) {
      if (!holds(ptr[i], len[i], seed[i])) bad++;
      small_free(h, ptr[i], len[i]);
      ptr[i] = NULL;
    } else {
      if (!holds(ptr[i], len[i], seed[i])) bad++;
      void *q = small_realloc(h, ptr[i], len[i], n);
      if (!q) {  // refused: the old block is untouched
        if (!holds(ptr[i], len[i], seed[i])) bad++;
        continue;
      }
      size_t keep = len[i] < n ? len[i] : n;
      if (!holds(q, keep, seed[i])) bad++;
      ptr[i] = q;
      len[i] = n;
      seed[i] = rng;
      fill(q, n, seed[i]);
    }
  }
  CHECK_EQ_INT(bad, 0);
  for (int i = 0; i < SLOTS; i++)
    if (ptr[i]) {
      if (!holds(ptr[i], len[i], seed[i])) bad++;
      small_free(h, ptr[i], len[i]);
    }
  CHECK_EQ_INT(bad, 0);
  small_stats_t st;
  small_stats(h, &st);
  CHECK_EQ_INT(st.objects, 0);
  CHECK(st.slabs <= 1);
  // Nothing but the heap, its directory and the spare is left in the backing.
  CHECK(s_nblocks <= 3);
  s_fail_rng = 0;
  end_heap(h);
}

static void test_random_stress(void) { random_stress(false); }
static void test_random_stress_failing(void) { random_stress(true); }

// Many slabs: the directory grows and every object stays owned.
static void test_many_slabs(void) {
  small_heap_t *h = new_heap();
  enum { N = 20000 };
  void **p = malloc(sizeof(void *) * N);
  for (int i = 0; i < N; i++) {
    p[i] = small_alloc(h, SMALL_ALLOC_MAX - (size_t)(i % 7));
    CHECK(p[i] != NULL);
  }
  small_stats_t st;
  small_stats(h, &st);
  CHECK(st.slabs >= (uint32_t)(N / (SMALL_SLAB_BYTES / SMALL_ALLOC_MAX)));
  int owned = 0;
  for (int i = 0; i < N; i++) owned += small_owns(h, p[i]);
  CHECK_EQ_INT(owned, N);
  for (int i = 0; i < N; i += 2) small_free(h, p[i], 1);
  for (int i = 1; i < N; i += 2) small_free(h, p[i], 1);
  small_stats(h, &st);
  CHECK_EQ_INT(st.objects, 0);
  free(p);
  end_heap(h);
}

static void test_asan_poisoning(void) {
#ifdef HAVE_ASAN
  small_heap_t *h = new_heap();
  size_t sz = small_class_size(4);
  uint8_t *p = small_alloc(h, sz - 5);
  CHECK(!__asan_address_is_poisoned(p));
  CHECK(!__asan_address_is_poisoned(p + sz - 6));
  CHECK(__asan_address_is_poisoned(p + sz - 5));  // slack past the request
  small_free(h, p, sz - 5);
  CHECK(__asan_address_is_poisoned(p));           // a free slot
  end_heap(h);
#endif
}

int main(void) {
  test_classes();
  test_small_is_pooled();
  test_large_passes_through();
  test_free_routing();
  test_hint_forgets_a_released_slab();
  test_realloc_same_class();
  test_realloc_across_classes();
  test_realloc_to_and_from_backing();
  test_shrink_never_fails();
  test_out_of_memory();
  test_fallback_small_block();
  test_slabs_released();
  test_random_stress();
  test_random_stress_failing();
  test_many_slabs();
  test_asan_poisoning();
  return check_report("test_small_alloc");
}
