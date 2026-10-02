// Host test for the default alarm pool fix (cmake/picodeck_pico_time.cmake,
// issue #58). tests/unit/CMakeLists.txt cuts alarm_pool_irq_handler's
// cancellation pass out of the patched pico_time time.c (generated as the
// firmware build generates it) into pass_patched.inc, and out of the
// unpatched text into pass_orig.inc. Both run here over every ordered list of
// 1 to 9 entries and every set of cancelled ones (on scattered pool indices).
//
// The patched pass must keep every entry in the list, the cancelled ones at
// the front with target -1 (the handler then frees them one by one) and the
// rest in order. SDK 2.2.0's own pass, run the same way, must lose exactly
// the leak #58 found: all but the last of the cancelled entries the list
// starts with. That shows the check sees a leak and the cut found the pass.
#include "check.h"

#include <stdbool.h>
#include <stdint.h>

#define __compiler_memory_barrier() __asm__ volatile("" ::: "memory")

// The fields the pass touches, as pico_time's time.c declares them.
typedef struct {
  int16_t next;
  volatile uint16_t sequence;  // bit 15: cancellation pending
  int64_t target;
} alarm_pool_entry_t;

typedef struct {
  volatile bool has_pending_cancellations;
  int16_t ordered_head;
  alarm_pool_entry_t *entries;
} alarm_pool_t;

static void pass_patched(alarm_pool_t *pool) {
#include "pass_patched.inc"
}

static void pass_sdk(alarm_pool_t *pool) {
#include "pass_orig.inc"
}

#define POOL_LEN 16
#define MAX_LIST 9

typedef struct {
  int lost;     // entries no longer reachable from ordered_head
  bool valid;   // no cycle, cancelled first (target -1), live ones in order
} outcome_t;

// A list of n entries due in order (targets 1000..), entry i cancelled when
// bit i of cancel is set, then one pass.
static outcome_t run(void (*pass)(alarm_pool_t *), int n, uint32_t cancel) {
  alarm_pool_entry_t e[POOL_LEN];
  memset(e, 0, sizeof(e));
  alarm_pool_t p = {.entries = e};
  int16_t idx[MAX_LIST];
  for (int i = 0; i < n; i++)
    idx[i] = (int16_t)((i * 5 + 3) % POOL_LEN);  // never index == position
  p.ordered_head = idx[0];
  for (int i = 0; i < n; i++) {
    e[idx[i]].next = i + 1 < n ? idx[i + 1] : -1;
    e[idx[i]].target = 1000 + i;
    e[idx[i]].sequence = (uint16_t)((1 + i) | ((cancel >> i) & 1u ? 0x8000 : 0));
  }
  p.has_pending_cancellations = true;
  pass(&p);

  outcome_t o = {.lost = n, .valid = !p.has_pending_cancellations};
  bool seen[POOL_LEN] = {false};
  bool in_prefix = true;
  int64_t last = -1;
  int reached = 0;
  for (int16_t i = p.ordered_head; i != -1; i = e[i].next) {
    if (i < 0 || i >= POOL_LEN || seen[i]) {
      o.valid = false;
      break;
    }
    seen[i] = true;
    reached++;
    if ((int16_t)e[i].sequence < 0) {
      if (!in_prefix || e[i].target != -1)
        o.valid = false;
    } else {
      in_prefix = false;
      if (e[i].target < last)
        o.valid = false;
      last = e[i].target;
    }
  }
  o.lost = n - reached;
  return o;
}

static int leading_cancelled(int n, uint32_t cancel) {
  int k = 0;
  while (k < n && ((cancel >> k) & 1u))
    k++;
  return k;
}

static void test_patched_pass_keeps_every_entry(void) {
  int cases = 0, bad = 0;
  for (int n = 1; n <= MAX_LIST; n++) {
    for (uint32_t cancel = 0; cancel < (1u << n); cancel++) {
      outcome_t o = run(pass_patched, n, cancel);
      cases++;
      if (o.lost != 0 || !o.valid) {
        if (bad++ < 5)
          printf("patched: n=%d cancel=0x%x lost %d valid %d\n", n,
                 (unsigned)cancel, o.lost, o.valid);
      }
    }
  }
  CHECK_EQ_INT(cases, 1022);
  CHECK_EQ_INT(bad, 0);
}

static void test_sdk_2_2_0_pass_leaks_a_leading_run(void) {
  int bad = 0, leaky = 0;
  for (int n = 1; n <= MAX_LIST; n++) {
    for (uint32_t cancel = 0; cancel < (1u << n); cancel++) {
      outcome_t o = run(pass_sdk, n, cancel);
      int k = leading_cancelled(n, cancel);
      int want = k > 1 ? k - 1 : 0;
      if (o.lost != want)
        bad++;
      if (o.lost)
        leaky++;
    }
  }
  CHECK_EQ_INT(bad, 0);
  CHECK(leaky > 0);  // #58: the head and the next entry, both cancelled
  CHECK_EQ_INT(run(pass_sdk, 2, 0x3).lost, 1);
  CHECK_EQ_INT(run(pass_patched, 2, 0x3).lost, 0);
}

int main(void) {
  test_patched_pass_keeps_every_entry();
  test_sdk_2_2_0_pass_leaks_a_leading_run();
  return check_report("test_pico_time_pass");
}
