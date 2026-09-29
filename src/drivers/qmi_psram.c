// QMI (CS1) PSRAM quad-mode initialisation.  See qmi_psram.h.
//
// Derived from SparkFun's sfe_psram.c (MIT, (c) 2024 SparkFun Electronics):
// https://github.com/sparkfun/sparkfun-pico — adapted for PicoDeck with a
// write/readback self-test, RXDELAY escalation, and a serial-mode fallback so
// a marginal chip degrades to the old (slow) configuration instead of
// boot-looping the device.

#include "qmi_psram.h"

#if defined(PICO_RP2350) && !defined(PICODECK_SIMULATOR)

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include <stdio.h>

// The M1 timing constants (QMI_PSRAM_SEC_TO_FS, QMI_PSRAM_MAX_SELECT_FS64,
// QMI_PSRAM_MIN_DESELECT_FS), the SCK divider (QMI_PSRAM_MAX_SCK_HZ,
// qmi_psram_clkdiv()) and the pre-scale helper (qmi_psram_timing()) all live
// in qmi_psram.h — pure and host-tested by tests/unit/test_qmi_psram_clkdiv.c.

#define PSRAM_CMD_QUAD_END    0xF5
#define PSRAM_CMD_QUAD_ENABLE 0x35
#define PSRAM_CMD_READ_ID     0x9F
#define PSRAM_CMD_RSTEN       0x66
#define PSRAM_CMD_RST         0x99
#define PSRAM_CMD_QUAD_READ   0xEB
#define PSRAM_CMD_QUAD_WRITE  0x38
#define PSRAM_CMD_NOOP        0xFF
#define PSRAM_KGD             0x5D

static bool s_quad_mode = false;
static uint32_t s_reset_timing, s_reset_rfmt, s_reset_rcmd;
static uint32_t s_reset_wfmt, s_reset_wcmd;

// PSRAM CS1 XIP window aliases
#define PSRAM_UNCACHED_BASE 0x15000000u

// ── Direct-mode helpers (must not execute from flash: direct mode stalls
//    XIP transfers, so flash fetches would deadlock) ────────────────────────

static void __no_inline_not_in_flash_func(direct_begin)(void) {
  qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
  while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
  }
}

static void __no_inline_not_in_flash_func(direct_end)(void) {
  qmi_hw->direct_csr &=
      ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
}

// Send a single-byte command with CS asserted, in quad width (for chips that
// are already in QPI mode, e.g. after a warm reboot).
static void __no_inline_not_in_flash_func(direct_cmd_quad)(uint8_t cmd) {
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                      QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
                      cmd;
  while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
  }
  (void)qmi_hw->direct_rx;
  qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  for (volatile int j = 0; j < 20; j++) {
  }
}

// Send a single-byte command with CS asserted, serial width.
static void __no_inline_not_in_flash_func(direct_cmd_serial)(uint8_t cmd) {
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  qmi_hw->direct_tx = cmd;
  while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
  }
  (void)qmi_hw->direct_rx;
  qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  for (volatile int j = 0; j < 20; j++) {
  }
}

// Reset the chip and enter quad (QPI) mode.  The ENTIRE direct-mode window
// must execute from RAM: while DIRECT_CSR.EN is set, memory-mapped XIP
// transfers (including flash instruction fetches!) stall, so any flash
// execution inside the window deadlocks the CPU.  This is why the sequence
// is one monolithic not_in_flash function rather than flash-resident glue
// calling RAM helpers.
static void __no_inline_not_in_flash_func(reset_to_quad)(void) {
  uint32_t intr_stash = save_and_disable_interrupts();
  direct_begin();
  direct_cmd_serial(PSRAM_CMD_RSTEN);
  direct_cmd_serial(PSRAM_CMD_RST);
  direct_cmd_serial(PSRAM_CMD_QUAD_ENABLE);
  direct_end();
  restore_interrupts(intr_stash);
}

// Leave QPI mode (fallback path).  Same RAM-residency rule as above.
static void __no_inline_not_in_flash_func(exit_quad)(void) {
  uint32_t intr_stash = save_and_disable_interrupts();
  direct_begin();
  direct_cmd_quad(PSRAM_CMD_QUAD_END);
  direct_end();
  restore_interrupts(intr_stash);
}

static size_t __no_inline_not_in_flash_func(read_psram_id)(void) {
  size_t psram_size = 0;
  uint32_t intr_stash = save_and_disable_interrupts();

  direct_begin();

  // Exit QPI mode in case the chip kept it across a warm reboot.
  direct_cmd_quad(PSRAM_CMD_QUAD_END);

  // Read the 8-byte ID response: MF ID, KGD, EID...
  qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
  uint8_t kgd = 0, eid = 0;
  for (size_t i = 0; i < 7; i++) {
    qmi_hw->direct_tx = (i == 0 ? PSRAM_CMD_READ_ID : PSRAM_CMD_NOOP);
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
    }
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }
    if (i == 5)
      kgd = qmi_hw->direct_rx;
    else if (i == 6)
      eid = qmi_hw->direct_rx;
    else
      (void)qmi_hw->direct_rx;
  }
  direct_end();

  if (kgd == PSRAM_KGD) {
    psram_size = 1024 * 1024;
    uint8_t size_id = eid >> 5;
    if (eid == 0x26 || size_id == 2)
      psram_size *= 8;
    else if (size_id == 0)
      psram_size *= 2;
    else if (size_id == 1)
      psram_size *= 4;
  }
  restore_interrupts(intr_stash);
  return psram_size;
}

// lo_hz == hi_hz applies the steady-state timing for that one clk_sys;
// lo_hz != hi_hz applies the pre-scale timing safe at both (see
// qmi_psram_timing() in qmi_psram.h). Stays __no_inline_not_in_flash_func:
// callers use this while clk_sys is about to change (or has just changed)
// with the Lua VM's stack living in this same PSRAM, so it must not touch
// PSRAM or flash-resident data. qmi_psram_timing() has its only call site
// here, so it inlines rather than leaving RAM (verified with objdump).
static void __no_inline_not_in_flash_func(apply_timing)(uint32_t rxdelay,
                                                         uint32_t lo_hz,
                                                         uint32_t hi_hz) {
  qmi_psram_timing_t t = qmi_psram_timing(lo_hz, hi_hz);

  uint32_t intr_stash = save_and_disable_interrupts();
  qmi_hw->m[1].timing =
      QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
      3u << QMI_M1_TIMING_SELECT_HOLD_LSB |
      1u << QMI_M1_TIMING_COOLDOWN_LSB |
      rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
      t.max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
      t.min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
      t.clkdiv << QMI_M1_TIMING_CLKDIV_LSB;
  __asm volatile("dsb sy" ::: "memory");
  restore_interrupts(intr_stash);

  printf("[QMI_PSRAM] timing: div=%lu (%lu kHz SCK) rxdelay=%lu maxSel=%lu "
         "minDesel=%lu at %lu/%lu kHz sysclk\n",
         (unsigned long)t.clkdiv, (unsigned long)(hi_hz / t.clkdiv / 1000),
         (unsigned long)rxdelay, (unsigned long)t.max_select,
         (unsigned long)t.min_deselect, (unsigned long)(lo_hz / 1000),
         (unsigned long)(hi_hz / 1000));
}

static void __no_inline_not_in_flash_func(apply_quad_formats)(void) {
  uint32_t intr_stash = save_and_disable_interrupts();
  qmi_hw->m[1].rfmt =
      (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
       QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
       QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
       QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
       QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB |
       QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
       QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB |
       QMI_M1_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_RFMT_SUFFIX_LEN_LSB);
  qmi_hw->m[1].rcmd = PSRAM_CMD_QUAD_READ << QMI_M1_RCMD_PREFIX_LSB |
                      0 << QMI_M1_RCMD_SUFFIX_LSB;
  qmi_hw->m[1].wfmt =
      (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
       QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
       QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
       QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
       QMI_M1_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M1_WFMT_DUMMY_LEN_LSB |
       QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
       QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB |
       QMI_M1_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_WFMT_SUFFIX_LEN_LSB);
  qmi_hw->m[1].wcmd = PSRAM_CMD_QUAD_WRITE << QMI_M1_WCMD_PREFIX_LSB |
                      0 << QMI_M1_WCMD_SUFFIX_LSB;
  __asm volatile("dsb sy" ::: "memory");
  restore_interrupts(intr_stash);
}

// Write/readback self-test through the uncached alias (bypasses the XIP
// cache so it exercises the real QMI transactions).  Patterns cover both
// bit polarities and address-dependent data; the span crosses a 1KB page
// boundary since page-end behaviour is where this chip has shown margin.
static bool selftest(void) {
  volatile uint32_t *u = (volatile uint32_t *)(PSRAM_UNCACHED_BASE + 0x3E0);
  static const uint32_t pats[4] = {0xA5A5A5A5u, 0x5A5A5A5Au, 0x00000000u,
                                   0xFFFFFFFFu};
  for (int p = 0; p < 4; p++) {
    for (int i = 0; i < 64; i++)
      u[i] = pats[p] ^ (uint32_t)(i * 0x01010101);
    __asm volatile("dsb sy" ::: "memory");
    for (int i = 0; i < 64; i++) {
      if (u[i] != (pats[p] ^ (uint32_t)(i * 0x01010101)))
        return false;
    }
  }
  return true;
}

size_t qmi_psram_init(uint32_t cs_pin) {
  gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

  // Snapshot the reset-default M1 registers for the fallback path.
  s_reset_timing = qmi_hw->m[1].timing;
  s_reset_rfmt = qmi_hw->m[1].rfmt;
  s_reset_rcmd = qmi_hw->m[1].rcmd;
  s_reset_wfmt = qmi_hw->m[1].wfmt;
  s_reset_wcmd = qmi_hw->m[1].wcmd;

  size_t psram_size = read_psram_id();
  if (psram_size == 0) {
    printf("[QMI_PSRAM] no PSRAM detected on CS1\n");
    return 0;
  }

  // Reset the chip, then enter quad (QPI) mode.
  reset_to_quad();

  apply_quad_formats();
  xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

  // Self-test, escalating RXDELAY until the readback is clean.
  uint32_t sys_hz = clock_get_hz(clk_sys);
  for (uint32_t rxdelay = 1; rxdelay <= 4; rxdelay++) {
    apply_timing(rxdelay, sys_hz, sys_hz);
    if (selftest()) {
      s_quad_mode = true;
      printf("[QMI_PSRAM] quad mode OK: %u MB, rxdelay=%lu\n",
             (unsigned)(psram_size / (1024 * 1024)), (unsigned long)rxdelay);
      return psram_size;
    }
    printf("[QMI_PSRAM] selftest FAILED at rxdelay=%lu\n",
           (unsigned long)rxdelay);
  }

  // Quad mode unusable on this unit — return the chip to serial SPI mode and
  // restore the reset-default (slow but known-working) M1 configuration.
  exit_quad();
  uint32_t intr_stash2 = save_and_disable_interrupts();
  qmi_hw->m[1].timing = s_reset_timing;
  qmi_hw->m[1].rfmt = s_reset_rfmt;
  qmi_hw->m[1].rcmd = s_reset_rcmd;
  qmi_hw->m[1].wfmt = s_reset_wfmt;
  qmi_hw->m[1].wcmd = s_reset_wcmd;
  __asm volatile("dsb sy" ::: "memory");
  restore_interrupts(intr_stash2);
  xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;
  s_quad_mode = false;
  printf("[QMI_PSRAM] quad mode unusable — fell back to serial SPI mode\n");
  return selftest() ? psram_size : 0;
}

void qmi_psram_update_timing(void) {
  if (!s_quad_mode)
    return;
  // Keep the RXDELAY that passed the boot self-test; timing scales the rest.
  uint32_t rxdelay = (qmi_hw->m[1].timing & QMI_M1_TIMING_RXDELAY_BITS) >>
                     QMI_M1_TIMING_RXDELAY_LSB;
  uint32_t sys_hz = clock_get_hz(clk_sys);
  apply_timing(rxdelay, sys_hz, sys_hz);
}

void qmi_psram_prescale_timing(uint32_t a_hz, uint32_t b_hz) {
  if (!s_quad_mode)
    return;
  uint32_t rxdelay = (qmi_hw->m[1].timing & QMI_M1_TIMING_RXDELAY_BITS) >>
                     QMI_M1_TIMING_RXDELAY_LSB;
  apply_timing(rxdelay, a_hz < b_hz ? a_hz : b_hz, a_hz < b_hz ? b_hz : a_hz);
}

bool qmi_psram_is_quad(void) { return s_quad_mode; }

#else // !RP2350 or simulator

size_t qmi_psram_init(uint32_t cs_pin) {
  (void)cs_pin;
  return 0;
}
void qmi_psram_update_timing(void) {}
void qmi_psram_prescale_timing(uint32_t a_hz, uint32_t b_hz) {
  (void)a_hz;
  (void)b_hz;
}
bool qmi_psram_is_quad(void) { return false; }

#endif
