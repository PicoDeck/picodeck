#include "leaf_stack.h"

#include <stddef.h>

#include "app_stack.h"

// The memory is ota_update.c's sector staging buffer (ota_write_and_reboot):
// that runs once with interrupts off and never returns, and never from inside
// a leaf call, so the two cannot overlap, and it fills the buffer before it
// reads it. So this costs no SRAM.
extern uint8_t g_ota_staging[LEAF_STACK_BYTES];

static bool s_active;
static bool s_painted;

// Order matters: PSPLIM must never be above SP, or an interrupt's stacking
// faults. SRAM is above PSRAM, so going in SP moves first and coming out
// PSPLIM does. Two registers' worth of padding keeps the old stack aligned.
__attribute__((naked, noinline))
static void leaf_switch(void (*fn)(void *), void *arg, uint32_t top, uint32_t limit) {
  __asm volatile(
      "push   {r4, r5, r6, lr}  \n\t"
      "mov    r4, sp            \n\t"
      "mrs    r5, psplim        \n\t"
      "mov    r6, r0            \n\t"
      "mov    r0, r1            \n\t"
      "mov    sp, r2            \n\t"
      "msr    psplim, r3        \n\t"
      "blx    r6                \n\t"
      "msr    psplim, r5        \n\t"
      "mov    sp, r4            \n\t"
      "pop    {r4, r5, r6, pc}  \n\t");
}

void leaf_stack_call(void (*fn)(void *), void *arg) {
  if (s_active || !app_stack_active()) {
    fn(arg);
    return;
  }
  const uint32_t top = (uint32_t)(uintptr_t)(g_ota_staging + LEAF_STACK_BYTES) & ~7u;
  uint32_t *lo = (uint32_t *)(((uintptr_t)g_ota_staging + 3u) & ~(uintptr_t)3u);
  const uint32_t limit = ((uint32_t)(uintptr_t)lo + APP_STACK_GUARD_BYTES + 7u) & ~7u;
  if (!s_painted) {
    for (uint32_t *p = lo; (uint32_t)(uintptr_t)p < top; p++) *p = APP_STACK_PAINT;
    s_painted = true;
  }
  s_active = true;
  leaf_switch(fn, arg, top, limit);
  s_active = false;
}

bool leaf_stack_contains(uint32_t psp, uint32_t bytes) {
  const uint32_t lo = (uint32_t)(uintptr_t)g_ota_staging;
  return psp >= lo && psp + bytes <= lo + LEAF_STACK_BYTES;
}

uint32_t leaf_stack_peak(void) {
  if (!s_painted) return 0;
  return app_stack_high_water(g_ota_staging, LEAF_STACK_BYTES);
}
