// PC-sampling profiler for the running app (the `prof` dev command).
//
// A repeating timer on Core 0's default alarm pool reads the exception frame
// the timer interrupt pushed on the app's PSP stack: the interrupted PC and
// LR (the caller, while the interrupted function is a leaf or has not saved
// LR yet). Only samples whose PSP lies inside the running app's stack are
// kept, so the launcher, dev commands and OS stacks never contribute. A
// sample taken in a nested interrupt reads the frame the outer interrupt
// pushed: still a PC the app was at, charged the nested handler's time.
//
// Samples go to one umm block (PSRAM), allocated by `prof start` and kept
// until `prof free` or a reboot. Firmware only, Core 0 only. Symbolise the
// dump on the host against the build's ELF (tools: see the dump header).
#include "pc_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_stack.h"
#include "hardware/watchdog.h"
#include "pico/time.h"
#include "umm_malloc.h"

#define PROF_MAX_SAMPLES 262144  // 2 MB of pairs

static uint32_t *s_buf;  // pairs: pc, lr
static uint32_t s_cap;   // pairs
static volatile uint32_t s_n;
static volatile uint32_t s_skipped;  // ticks with no app frame to read
static repeating_timer_t s_timer;
static bool s_on;

static bool prof_cb(repeating_timer_t *rt) {
  (void)rt;
  uint8_t owner = g_app_stack_owner;
  uint8_t *base = g_app_stack_base;
  uint32_t size = g_app_stack_size;
  if (s_n >= s_cap) return true;
  if (!base || (owner != APP_STACK_LUA && owner != APP_STACK_NATIVE)) {
    s_skipped++;
    return true;
  }
  uint32_t psp;
  __asm volatile("mrs %0, psp" : "=r"(psp));
  if (psp < (uint32_t)base || psp + 32u > (uint32_t)base + size) {
    s_skipped++;
    return true;
  }
  const uint32_t *frame = (const uint32_t *)psp;  // r0-r3, r12, lr, pc, xpsr
  uint32_t i = s_n;
  s_buf[2 * i] = frame[6];
  s_buf[2 * i + 1] = frame[5];
  s_n = i + 1;
  return true;
}

void pc_prof_command(const char *args) {
  while (*args == ' ') args++;
  if (strncmp(args, "start", 5) == 0) {
    char *end;
    long period = strtol(args + 5, &end, 10);
    long cap = strtol(end, NULL, 10);
    if (period <= 0) period = 200;
    if (period < 50) period = 50;
    if (cap <= 0) cap = 32768;
    if (cap > PROF_MAX_SAMPLES) cap = PROF_MAX_SAMPLES;  // and cap * 8 fits
    if (s_on) {
      cancel_repeating_timer(&s_timer);
      s_on = false;
    }
    if (!s_buf || s_cap != (uint32_t)cap) {
      umm_free(s_buf);
      s_buf = umm_malloc((size_t)cap * 8u);
      s_cap = s_buf ? (uint32_t)cap : 0;
    }
    if (!s_buf) {
      printf("[DEV] Error: prof: no memory for %ld samples\n", cap);
      return;
    }
    s_n = 0;
    s_skipped = 0;
    s_on = add_repeating_timer_us(-period, prof_cb, NULL, &s_timer);
    printf("[DEV] PROF started period_us=%ld cap=%ld ok=%d\n", period, cap,
           (int)s_on);
  } else if (strcmp(args, "stop") == 0) {
    if (s_on) cancel_repeating_timer(&s_timer);
    s_on = false;
    printf("[DEV] PROF stopped samples=%lu skipped=%lu\n",
           (unsigned long)s_n, (unsigned long)s_skipped);
  } else if (strcmp(args, "dump") == 0) {
    if (s_on) cancel_repeating_timer(&s_timer);
    s_on = false;
    uint32_t n = s_n;
    printf("[DEV] PROF dump samples=%lu skipped=%lu\n", (unsigned long)n,
           (unsigned long)s_skipped);
    // Eight pc:lr pairs per line, '%' prefixed so a reader can pick them out
    // of interleaved log lines.
    for (uint32_t i = 0; i < n; i += 8) {
      printf("%%");
      for (uint32_t j = i; j < n && j < i + 8; j++)
        printf(" %08lx:%08lx", (unsigned long)s_buf[2 * j],
               (unsigned long)s_buf[2 * j + 1]);
      printf("\n");
      if ((i & 255) == 0) watchdog_update();
    }
    printf("[DEV] PROF end\n");
  } else if (strcmp(args, "free") == 0) {
    if (s_on) cancel_repeating_timer(&s_timer);
    s_on = false;
    umm_free(s_buf);
    s_buf = NULL;
    s_cap = 0;
    s_n = 0;
    printf("[DEV] PROF freed\n");
  } else {
    printf("[DEV] PROF on=%d samples=%lu skipped=%lu cap=%lu "
           "(prof start [period_us] [max_samples] | stop | dump | free)\n",
           (int)s_on, (unsigned long)s_n, (unsigned long)s_skipped,
           (unsigned long)s_cap);
  }
}
