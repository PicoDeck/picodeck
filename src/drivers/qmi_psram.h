// QMI (CS1) PSRAM initialisation for the Pimoroni Pico Plus 2 W's onboard
// APS6404L.  Switches the chip into quad (QPI) mode with 0xEB fast reads and
// 0x38 quad writes — roughly 6x the bandwidth of the reset-default 1-bit
// serial 0x03 reads the system previously ran on.
//
// Derived from SparkFun's sfe_psram.c (MIT, (c) 2024 SparkFun Electronics),
// itself based on the CircuitPython RP2350 PSRAM bring-up.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// APS6404L-3SQR is rated 109 MHz at 3.3 V, but this cap is NOT about the
// chip's own rating — it is the fastest SCK proven to work with the RXDELAY
// (read-sample delay) the boot self-test calibrates once and every later
// clock change reuses unmodified (qmi_psram_update_timing()). Measured on
// hardware (specs/2026-09-28-qmi250-experiment-report.md): at 66.7-75 MHz
// SCK, RXDELAY 1-4 all pass the self-test; at 83.3 MHz SCK (what the old 84
// MHz cap gave at 250 MHz sysclk, the only supported clock above 75 MHz),
// the boot-calibrated RXDELAY=1 FAILS the self-test and corrupts the Lua
// heap, while RXDELAY 2-4 pass. Re-validating RXDELAY at each clock change
// was rejected: it would mean trying failing settings while live Lua
// stack/heap data already sit in that same PSRAM (picocalc.video's 300 MHz
// boost retimes QMI while running ON the Lua VM's PSRAM stack — see
// src/os/lua_runner.c) — the exact class of hazard that once bricked a
// device. So this cap must not be "optimised" back up above 75 MHz without
// also solving that live-retiming problem.
#define QMI_PSRAM_MAX_SCK_HZ 75000000u

// M1 SCK divider for a given sys_hz: ceil(sys_hz / QMI_PSRAM_MAX_SCK_HZ),
// minimum 1. Pure and host-testable (tests/unit/test_qmi_psram_clkdiv.c);
// keep it free of pico/hardware headers.
static inline uint32_t qmi_psram_clkdiv(uint32_t sys_hz) {
  uint32_t divider =
      (sys_hz + QMI_PSRAM_MAX_SCK_HZ - 1) / QMI_PSRAM_MAX_SCK_HZ;
  return divider < 1 ? 1 : divider;
}

// M1 timing constants apply_timing()/qmi_psram_timing() derive CLKDIV,
// MAX_SELECT and MIN_DESELECT from. #defines, not `static const`: the RAM
// function that applies them (apply_timing(), qmi_psram.c) must not touch
// flash-resident data while PSRAM may be mid-retime, and a #define bakes the
// value in as an immediate rather than a load.
#define QMI_PSRAM_SEC_TO_FS          1000000000000000ull
// tCEM: max CS-low 8 us; the MAX_SELECT field is in units of 64 sys clocks:
// 8000 ns * 1e6 fs/ns / 64 = 125e6 fs per unit.
#define QMI_PSRAM_MAX_SELECT_FS64    125000000u  // tCEM 8 us / 64
// tCPH: min CS-high 50 ns (datasheet 18 ns; 50 ns keeps SparkFun's margin).
#define QMI_PSRAM_MIN_DESELECT_FS    50000000u   // tCPH 50 ns

// M1 timing fields that are safe at every clk_sys in [lo_hz, hi_hz], for a
// clock change in progress: the divider from the faster clock (SCK within
// QMI_PSRAM_MAX_SCK_HZ), MAX_SELECT from the slower (CS low at most 8 us,
// in units of 64 sys clocks) and MIN_DESELECT from the faster (at least
// 50 ns). lo_hz == hi_hz gives the steady-state timing.
typedef struct {
  uint32_t clkdiv;
  uint32_t max_select;
  uint32_t min_deselect;
} qmi_psram_timing_t;

// Pure and host-testable (tests/unit/test_qmi_psram_clkdiv.c); keep it free
// of pico/hardware headers. Called from apply_timing() (qmi_psram.c), which
// must stay __no_inline_not_in_flash_func — this has exactly one call site
// in that file, so it inlines into it at -Os (verified with objdump; see the
// header comment on apply_timing).
static inline qmi_psram_timing_t qmi_psram_timing(uint32_t lo_hz,
                                                   uint32_t hi_hz) {
  qmi_psram_timing_t t;
  t.clkdiv = qmi_psram_clkdiv(hi_hz);

  // Keep today's integer steps: fs_per_cycle, then divide.
  uint32_t lo_fs_per_cycle = (uint32_t)(QMI_PSRAM_SEC_TO_FS / lo_hz);
  uint32_t hi_fs_per_cycle = (uint32_t)(QMI_PSRAM_SEC_TO_FS / hi_hz);
  t.max_select = QMI_PSRAM_MAX_SELECT_FS64 / lo_fs_per_cycle;
  t.min_deselect =
      (QMI_PSRAM_MIN_DESELECT_FS + hi_fs_per_cycle - 1) / hi_fs_per_cycle;
  return t;
}

// Detect and configure the QMI CS1 PSRAM in quad mode.  Runs a write/readback
// self-test through the uncached alias; on failure it steps RXDELAY up, and
// if no setting passes it returns the chip to serial SPI mode and restores
// the reset-default M1 registers (slow but safe).
// Returns the detected PSRAM size in bytes, 0 if no PSRAM responded.
size_t qmi_psram_init(uint32_t cs_pin);

// Recompute M1 timing (CLKDIV / MAX_SELECT / MIN_DESELECT) for the current
// clk_sys.  Call after every set_sys_clock_khz change.  Safe no-op when the
// quad-mode init failed or never ran.
void qmi_psram_update_timing(void);

// Retime M1 for a clock change from one clk_sys to another, before the
// switch: timing safe at both (qmi_psram_timing). The boot RXDELAY stays.
// Call before set_sys_clock_khz, with the current and target clk_sys (either
// order); safe no-op when the quad-mode init failed or never ran.
void qmi_psram_prescale_timing(uint32_t a_hz, uint32_t b_hz);

// True when the PSRAM is running in quad mode (init + self-test succeeded).
bool qmi_psram_is_quad(void);
