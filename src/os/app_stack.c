#include "app_stack.h"

#include <stddef.h>

#include "umm_malloc.h"

#ifndef PICODECK_SIMULATOR
#include "hardware/sync.h"
#endif

uint8_t *volatile g_app_stack_base = NULL;
volatile uint32_t g_app_stack_size = 0;
volatile uint8_t g_app_stack_owner = APP_STACK_NONE;

#define GUARD_WORDS (APP_STACK_GUARD_BYTES / 4u)

static void paint_words(uint32_t *lo, uint32_t *hi) {
  while (lo < hi) *lo++ = APP_STACK_PAINT;
}

static uint32_t *align_up4(const uint8_t *p) {
  return (uint32_t *)(((uintptr_t)p + 3u) & ~(uintptr_t)3u);
}

bool app_stack_guard_intact(const uint8_t *base) {
  const uint32_t *g = align_up4(base);
  for (uint32_t i = 0; i < GUARD_WORDS; i++)
    if (g[i] != APP_STACK_PAINT) return false;
  return true;
}

#ifndef PICODECK_SIMULATOR

// Bytes between the first non-paint word at or above lo and hi.
static uint32_t used_above(const uint32_t *lo, const uint32_t *hi) {
  while (lo < hi && *lo == APP_STACK_PAINT) lo++;
  return (uint32_t)((uintptr_t)hi - (uintptr_t)lo);
}

// Naked trampoline: psp_call(psp_top, fn, arg)
//   1. saves r4 and LR on the current (main) stack; r4 keeps CONTROL
//   2. PSP = psp_top, CONTROL.SPSEL = 1: Thread mode now uses the PSP
//   3. fn(arg) runs entirely on the PSP
//   4. SPSEL = 0 again (read-modify-write, so a CONTROL.FPCA set by fn's
//      floating-point use is kept), return on the main stack.
// Two registers pushed keeps the main stack 8-byte aligned (AAPCS).
__attribute__((naked, noinline))
static void psp_call(uint32_t psp_top, void (*fn)(void *), void *arg) {
  __asm__ volatile(
      "push   {r4, lr}        \n\t"
      "mrs    r4, control     \n\t"
      "msr    psp, r0         \n\t"
      "orr    r0, r4, #2      \n\t"
      "msr    control, r0     \n\t"
      "isb                    \n\t"
      "mov    r0, r2          \n\t"
      "blx    r1              \n\t"
      "mrs    r0, control     \n\t"
      "bic    r0, r0, #2      \n\t"
      "msr    control, r0     \n\t"
      "isb                    \n\t"
      "pop    {r4, pc}        \n\t");
}

void app_stack_run(uint8_t *base, uint32_t size, app_stack_owner_t owner,
                   void (*fn)(void *), void *arg) {
  uint32_t *lo = align_up4(base);
  // AAPCS: SP 8-byte aligned at every public interface. umm_malloc only
  // guarantees 4-byte alignment, so round the top down.
  uint32_t top = ((uint32_t)(uintptr_t)(base + size)) & ~7u;
  paint_words(lo, (uint32_t *)(uintptr_t)top);

  // PSPLIM (8-byte granule) just above the guard words: a push past it
  // faults with CFSR.STKOF instead of running through the guard into
  // whatever lies below the buffer.
  uint32_t limit = ((uint32_t)(uintptr_t)(lo + GUARD_WORDS) + 7u) & ~7u;

  g_app_stack_base = base;
  g_app_stack_size = size;
  g_app_stack_owner = (uint8_t)owner;
  __dmb();
  __asm volatile("msr psplim, %0" : : "r"(limit));

  psp_call(top, fn, arg);

  __asm volatile("msr psplim, %0" : : "r"(0u));
  g_app_stack_owner = APP_STACK_NONE;
  g_app_stack_size = 0;
  g_app_stack_base = NULL;
  __dmb();
}

uint32_t app_stack_high_water(const uint8_t *base, uint32_t size) {
  const uint32_t *lo = align_up4(base) + GUARD_WORDS;
  const uint32_t *hi =
      (const uint32_t *)(((uintptr_t)(base + size)) & ~(uintptr_t)7u);
  return used_above(lo, hi);
}

// Linker symbols for Core 0's main stack (SCRATCH_Y).
extern uint32_t __StackTop;
extern uint32_t __StackBottom;

// Stay clear of the MSPLIM margin (main.c STACK_LIMIT_MARGIN) and of the
// words just below the caller's frame.
#define MSP_PAINT_SKIP_BOTTOM 64u
#define MSP_PAINT_SKIP_BELOW_SP 64u

void app_stack_paint_msp(void) {
  // Interrupts off: an IRQ taken while painting would push its frame below
  // our SP, into the region being painted.
  uint32_t irq = save_and_disable_interrupts();
  uint32_t sp;
  __asm volatile("mov %0, sp" : "=r"(sp));
  uint32_t *lo =
      (uint32_t *)((uintptr_t)&__StackBottom + MSP_PAINT_SKIP_BOTTOM);
  uint32_t *hi = (uint32_t *)((sp - MSP_PAINT_SKIP_BELOW_SP) & ~3u);
  paint_words(lo, hi);
  restore_interrupts(irq);
}

uint32_t app_stack_msp_high_water(void) {
  const uint32_t *lo =
      (const uint32_t *)((uintptr_t)&__StackBottom + MSP_PAINT_SKIP_BOTTOM);
  return used_above(lo, (const uint32_t *)&__StackTop);
}

// Core 1's stack (SCRATCH_X), used by multicore_launch_core1.
extern uint32_t __StackOneTop;
extern uint32_t __StackOneBottom;

void app_stack_paint_core1(void) {
  uint32_t irq = save_and_disable_interrupts();
  uint32_t sp;
  __asm volatile("mov %0, sp" : "=r"(sp));
  uint32_t *lo =
      (uint32_t *)((uintptr_t)&__StackOneBottom + MSP_PAINT_SKIP_BOTTOM);
  uint32_t *hi = (uint32_t *)((sp - MSP_PAINT_SKIP_BELOW_SP) & ~3u);
  paint_words(lo, hi);
  restore_interrupts(irq);
}

uint32_t app_stack_core1_high_water(void) {
  const uint32_t *lo =
      (const uint32_t *)((uintptr_t)&__StackOneBottom + MSP_PAINT_SKIP_BOTTOM);
  return used_above(lo, (const uint32_t *)&__StackOneTop);
}

uint32_t app_stack_core1_size(void) {
  return (uint32_t)((uintptr_t)&__StackOneTop - (uintptr_t)&__StackOneBottom);
}

bool app_stack_active(void) {
  // CONTROL.SPSEL: Thread mode uses the PSP. Only the app runners and
  // app_stack_run_os() set it, so this is the ground truth for "on an app
  // stack", independent of the g_app_stack_* bookkeeping.
  uint32_t control;
  __asm volatile("mrs %0, control" : "=r"(control));
  return (control & 2u) != 0;
}

static uint32_t s_lent_peak, s_lent_size;

// Naked trampoline: sp_call(top, limit, fn, arg), in Thread mode on the PSP.
//   1. saves r4, r5 and LR on the app stack; r4 keeps its SP, r5 its PSPLIM
//   2. SP = top first, while the old limit (lower: PSRAM) is in force, then
//      PSPLIM = limit, below the new SP: neither step leaves SP under the limit
//      in force, so an interrupt between them stacks its frame safely
//   3. fn(arg) runs on the new stack
//   4. the old limit first (below both SPs), then the old SP; return.
// Four registers pushed keeps the app stack 8-byte aligned (AAPCS).
__attribute__((naked, noinline))
static void sp_call(uint32_t top, uint32_t limit, void (*fn)(void *), void *arg) {
  __asm__ volatile(
      "push   {r4, r5, r6, lr} \n\t"
      "mov    r4, sp           \n\t"
      "mrs    r5, psplim       \n\t"
      "mov    sp, r0           \n\t"
      "msr    psplim, r1       \n\t"
      "mov    r0, r3           \n\t"
      "blx    r2               \n\t"
      "msr    psplim, r5       \n\t"
      "mov    sp, r4           \n\t"
      "pop    {r4, r5, r6, pc} \n\t");
}

bool app_stack_call_lent(uint8_t *lo, uint32_t size, void (*fn)(void *), void *arg) {
  if (!app_stack_active()) return false;
  uint32_t *const bottom = align_up4(lo);
  const uint32_t top = ((uint32_t)(uintptr_t)(lo + size)) & ~7u;
  const uint32_t limit = ((uint32_t)(uintptr_t)(bottom + GUARD_WORDS) + 7u) & ~7u;
  if (top <= limit) return false;
  paint_words(bottom, (uint32_t *)(uintptr_t)top);
  uint8_t *const base = g_app_stack_base;
  const uint32_t base_size = g_app_stack_size;
  g_app_stack_base = lo;
  g_app_stack_size = size;
  sp_call(top, limit, fn, arg);
  g_app_stack_base = base;
  g_app_stack_size = base_size;
  const uint32_t used = used_above(bottom + GUARD_WORDS, (const uint32_t *)(uintptr_t)top);
  if (used > s_lent_peak) s_lent_peak = used;
  s_lent_size = size;
  return true;
}

uint32_t app_stack_lent_peak(void) { return s_lent_peak; }
uint32_t app_stack_lent_size(void) { return s_lent_size; }

#else  // PICODECK_SIMULATOR: no PSP; run on the host stack.

bool app_stack_call_lent(uint8_t *lo, uint32_t size, void (*fn)(void *), void *arg) {
  (void)lo;
  (void)size;
  (void)fn;
  (void)arg;
  return false;
}

uint32_t app_stack_lent_peak(void) { return 0; }
uint32_t app_stack_lent_size(void) { return 0; }

void app_stack_run(uint8_t *base, uint32_t size, app_stack_owner_t owner,
                   void (*fn)(void *), void *arg) {
  uint32_t *lo = align_up4(base);
  paint_words(lo, lo + GUARD_WORDS);  // keep app_stack_guard_intact() true
  g_app_stack_base = base;
  g_app_stack_size = size;
  g_app_stack_owner = (uint8_t)owner;
  fn(arg);
  g_app_stack_owner = APP_STACK_NONE;
  g_app_stack_size = 0;
  g_app_stack_base = NULL;
}

uint32_t app_stack_high_water(const uint8_t *base, uint32_t size) {
  (void)base;
  (void)size;
  return 0;
}

void app_stack_paint_msp(void) {}
uint32_t app_stack_msp_high_water(void) { return 0; }
void app_stack_paint_core1(void) {}
uint32_t app_stack_core1_high_water(void) { return 0; }
uint32_t app_stack_core1_size(void) { return 0; }

bool app_stack_active(void) { return g_app_stack_base != NULL; }

#endif

static uint32_t s_os_last_peak = 0;

bool app_stack_run_os(void (*fn)(void *), void *arg) {
  if (app_stack_active()) {
    fn(arg);  // nested inside an app: its stack already has the room
    return true;
  }
  uint8_t *stack = (uint8_t *)umm_malloc(APP_STACK_OS_SIZE);
  if (!stack) return false;
  app_stack_run(stack, APP_STACK_OS_SIZE, APP_STACK_OS, fn, arg);
  s_os_last_peak = app_stack_high_water(stack, APP_STACK_OS_SIZE);
  umm_free(stack);
  return true;
}

uint32_t app_stack_os_last_peak(void) { return s_os_last_peak; }
