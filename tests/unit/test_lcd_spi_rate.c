// Host unit tests for src/drivers/lcd_spi_rate.h (issue #70): the LCD's PIO
// SPI program and divider at every validated clk_sys
// (app_manifest_clock_valid), and at every whole MHz from 48 to 400 the
// fastest rate at or below the ceiling any program and integer divider give.
#include "check.h"
#include "lcd_spi_rate.h"

#define MAX_HZ 100000000u  // LCD_SPI_BAUD

static void test_validated_clocks(void) {
  static const struct {
    uint32_t mhz, cycles, div, hz;
  } k_want[] = {
      {125, 2, 1, 62500000},  // 3 cycles would give 41.7
      {150, 2, 1, 75000000},
      {200, 2, 1, 100000000},
      {250, 3, 1, 83333333},  // was 2 cycles / 2: 62.5 MHz
      {300, 3, 1, 100000000}, // was 2 cycles / 2: 75 MHz
  };
  for (size_t i = 0; i < sizeof k_want / sizeof k_want[0]; i++) {
    const lcd_spi_rate_t r = lcd_spi_rate(k_want[i].mhz * 1000000u, MAX_HZ);
    CHECK_EQ_INT(r.cycles, k_want[i].cycles);
    CHECK_EQ_INT(r.div, k_want[i].div);
    CHECK_EQ_INT(r.hz, k_want[i].hz);
  }
}

static void test_fastest_at_or_below_max(void) {
  int bad = 0;
  for (uint32_t mhz = 48; mhz <= 400; mhz++) {
    const uint32_t clk = mhz * 1000000u;
    const lcd_spi_rate_t r = lcd_spi_rate(clk, MAX_HZ);
    if (r.hz > MAX_HZ || r.hz != clk / (r.cycles * r.div) || r.div < 1) bad++;
    for (uint32_t c = 2; c <= 3; c++)  // nothing faster is allowed
      for (uint32_t d = 1; d <= 8; d++)
        if (clk / (c * d) <= MAX_HZ && clk / (c * d) > r.hz) bad++;
  }
  CHECK_EQ_INT(bad, 0);
  // A clock under twice the ceiling runs 2 cycles at divider 1.
  const lcd_spi_rate_t slow = lcd_spi_rate(48000000u, MAX_HZ);
  CHECK(slow.cycles == 2 && slow.div == 1 && slow.hz == 24000000u);
  // A tie keeps the symmetric clock: at 600 MHz 2 cycles / 3 and 3 cycles
  // / 2 both give 100 MHz.
  const lcd_spi_rate_t tie = lcd_spi_rate(600000000u, MAX_HZ);
  CHECK(tie.cycles == 2 && tie.div == 3 && tie.hz == MAX_HZ);
}

int main(void) {
  test_validated_clocks();
  test_fastest_at_or_below_max();
  return check_report("test_lcd_spi_rate");
}
