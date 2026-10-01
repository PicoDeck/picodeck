#ifndef XIP_STATS_H
#define XIP_STATS_H

// The XIP cache's and the bus fabric's contention counters, summed over a
// window: the `xipstat` dev command (src/dev_commands.c) and
// sys.getStats().xip_cache_hit_rate. Firmware only; Core 0 only.
//
// Both cores, the DMA and every XIP alias share one 16 KB, 2-way cache of
// 8-byte lines in front of the QMI (flash on CS0, QMI PSRAM on CS1). None of
// these counters tells the cores apart: compare windows instead (an idle
// app, the game with no music, the game with MP3).
//
// The hardware counters saturate (XIP: 32 bits, ~40 s of a busy game; bus
// fabric: 24 bits, 84 ms of stall cycles at 200 MHz) and are cleared by a
// write. Every reader harvests them (read, clear, add to 64-bit totals);
// while a window runs, a 10 ms timer on Core 0's default alarm pool
// harvests too, so nothing saturates. The few events between a read and its
// clear are lost. The window's state takes one umm block (PSRAM), allocated
// by the first xip_stats_start and kept until reboot.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t window_ms;     // the window's span: to now, or to the app's exit
  uint64_t accesses;      // XIP accesses, cached or not, from any manager
  uint64_t hits;          // of which the cache served
  uint64_t stall[2];      // cycles each XIP port (even / odd lines) stalled
                          // downstream: a miss or an uncached access waiting
                          // on the QMI
  uint64_t contested[2];  // accesses on each XIP port that first waited for
                          // another manager's access to finish
  bool running;           // a window has been started
  bool frozen;            // it ended when the app it measured exited
  bool saturated;         // a counter saturated between harvests: figures low
} xip_stats_t;

// Starts (or restarts) a window now: the bus fabric's four counters are set
// to the XIP ports' downstream stalls and contested accesses, and the
// harvest timer runs. A window started while an app runs, or that sees one
// start, ends (freezes) when that app exits, so `xipstat` read at the
// launcher afterwards reports the app's time alone.
void xip_stats_start(void);

// Stops the harvest timer and the bus counters; the last window is kept.
void xip_stats_stop(void);

// The current (or frozen) window.
void xip_stats_get(xip_stats_t *out);

// The cache hit rate in percent since the previous call (0-100), or -1 when
// nothing was counted or a counter saturated meanwhile (no harvest for
// ~40 s).
int xip_stats_hit_rate_since_last(void);

// Bus priority experiment (BUSCTRL BUS_PRIORITY): Core 0 high priority wins
// every arbitration against Core 1 and the DMA (all ports, not only XIP).
// Off at boot.
void xip_stats_set_core0_priority(bool high);
bool xip_stats_core0_priority(void);

#endif  // XIP_STATS_H
