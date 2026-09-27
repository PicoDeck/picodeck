// Host unit tests for src/drivers/qmi_psram.h's qmi_psram_clkdiv(): the pure
// M1 SCK-divider computation apply_timing() uses. Every system clock
// app_manifest_clock_valid() accepts (src/os/app_manifest.c) must land the
// PSRAM SCK at or below QMI_PSRAM_MAX_SCK_HZ — 250 MHz is the case that
// broke: with the old 84 MHz cap it lands on div=3 (83.33 MHz SCK), the only
// operating point above 75 MHz, which corrupts the Lua heap because the
// boot-calibrated RXDELAY only passes the self-test up to 75 MHz SCK on this
// board (see specs/2026-09-28-qmi250-experiment-report.md). The fix caps at
// 75 MHz so every clock keeps a divider already proven to work.
#include "check.h"
#include "qmi_psram.h"

// app_manifest_clock_valid() accepts 0 (meaning "use the boot default") and
// these five real clocks (kHz). 0 is not a sys_hz an app ever runs at, so it
// is not exercised here.
static const uint32_t k_app_clocks_khz[] = {125000, 150000, 200000, 250000,
                                            300000};

// The fixed target the fix must hit: 75 MHz, the fastest SCK proven with the
// boot-calibrated RXDELAY (see the file header). Checked against this literal
// rather than QMI_PSRAM_MAX_SCK_HZ itself, which would be tautological (any
// cap value makes qmi_psram_clkdiv's own ceiling division satisfy it).
#define PROVEN_SAFE_SCK_HZ 75000000u

static void test_every_supported_clock_keeps_sck_at_or_below_75mhz(void) {
  for (size_t i = 0; i < sizeof(k_app_clocks_khz) / sizeof(k_app_clocks_khz[0]);
       i++) {
    uint32_t sys_hz = k_app_clocks_khz[i] * 1000u;
    uint32_t divider = qmi_psram_clkdiv(sys_hz);
    uint32_t sck_hz = sys_hz / divider;
    CHECK(divider >= 1);
    if (sck_hz > PROVEN_SAFE_SCK_HZ)
      printf("FAIL %s:%d: %lu kHz sysclk -> div=%lu -> %lu Hz SCK exceeds "
             "the %lu Hz proven-safe target\n",
             __FILE__, __LINE__, (unsigned long)k_app_clocks_khz[i],
             (unsigned long)divider, (unsigned long)sck_hz,
             (unsigned long)PROVEN_SAFE_SCK_HZ);
    CHECK(sck_hz <= PROVEN_SAFE_SCK_HZ);
  }
}

// The existing operating points below the historical 84 MHz cap must keep
// their dividers exactly: this fix changes ONLY the 250 MHz case.
static void test_existing_operating_points_keep_their_dividers(void) {
  CHECK_EQ_U32(qmi_psram_clkdiv(125000000u), 2);
  CHECK_EQ_U32(qmi_psram_clkdiv(150000000u), 2);
  CHECK_EQ_U32(qmi_psram_clkdiv(200000000u), 3);
  CHECK_EQ_U32(qmi_psram_clkdiv(300000000u), 4);
}

// The whole point of the fix: 250 MHz must land on div=4 (62.5 MHz SCK), not
// div=3 (83.33 MHz SCK, which fails the boot-calibrated RXDELAY on hardware).
static void test_250mhz_gets_a_higher_divider_than_before(void) {
  CHECK_EQ_U32(qmi_psram_clkdiv(250000000u), 4);
  CHECK_EQ_U32(250000000u / qmi_psram_clkdiv(250000000u), 62500000u);
}

static void test_tiny_sys_hz_gives_the_minimum_divider(void) {
  CHECK_EQ_U32(qmi_psram_clkdiv(1u), 1);
  CHECK_EQ_U32(qmi_psram_clkdiv(0u), 1);
}

int main(void) {
  test_every_supported_clock_keeps_sck_at_or_below_75mhz();
  test_existing_operating_points_keep_their_dividers();
  test_250mhz_gets_a_higher_divider_than_before();
  test_tiny_sys_hz_gives_the_minimum_divider();
  return check_report("test_qmi_psram_clkdiv");
}
