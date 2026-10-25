#pragma once
// The LCD's PIO SPI rate at a given clk_sys (issue #70). The PIO programs
// (lcd_spi.pio) send a bit in 2 PIO cycles (lcd_spi) or 3 (lcd_spi3), at an
// integer divider of clk_sys: a fractional divider jitters the clock. So
// the rate is clk_sys / (cycles * div), and with 2 cycles only, 250 and
// 300 MHz had to round down to 62.5 and 75 MHz. lcd_spi_rate picks the
// pair with the highest rate at or below max_hz; on a tie, 2 cycles (the
// symmetric clock). Pure, for the host tests (tests/unit).
#include <stdint.h>

typedef struct {
  uint8_t cycles;  // PIO cycles a bit: 2 or 3
  uint32_t div;    // the state machine's integer clock divider, >= 1
  uint32_t hz;     // the resulting SPI clock, rounded down
} lcd_spi_rate_t;

static inline lcd_spi_rate_t lcd_spi_rate(uint32_t clk_hz, uint32_t max_hz) {
  lcd_spi_rate_t best = {2, 1, 0};
  for (uint32_t cycles = 2; cycles <= 3; cycles++) {
    const uint64_t per = (uint64_t)cycles * max_hz;
    uint32_t div = (uint32_t)(((uint64_t)clk_hz + per - 1) / per);  // rounded up
    if (div < 1) div = 1;
    const uint32_t hz = clk_hz / (cycles * div);
    if (hz > best.hz) best = (lcd_spi_rate_t){(uint8_t)cycles, div, hz};
  }
  return best;
}
