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

// True when the PSRAM is running in quad mode (init + self-test succeeded).
bool qmi_psram_is_quad(void);
