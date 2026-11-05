/* diskio_spi.c — FatFS low-level disk I/O for SD card via SPI on RP2350
 *
 * Implements the FatFS diskio interface (disk_initialize, disk_status,
 * disk_read, disk_write, disk_ioctl) using Pico SDK hardware_spi.
 *
 * Hardware pins are taken from hardware.h (SD_SPI_PORT, SD_PIN_*).
 * SPI0 is initialised by sdcard_init() in sdcard.c before FatFS mounts —
 * this file only drives the SD card protocol on top of that bus.
 *
 * Optimizations:
 *   - Multi-block reads use single large SPI transfer (uf2loader approach)
 *   - CMD23 pre-erase for multi-block writes
 *   - Automatic fallback to per-block mode on errors
 *   - Config key "sd_optimized_read" to enable/disable (default: enabled)
 *
 * References:
 *   SD Association Physical Layer Simplified Specification v8.00
 *   FatFs R0.15 diskio.h
 */

#include "ff.h"
#include "diskio.h"
#include "hardware.h" /* SD_SPI_PORT, SD_PIN_CS, SD_SPI_BAUD */

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

/* usb_msc_is_active() — suppress printf when called from USB IRQ context.
 * Forward-declared here to avoid adding src/usb/ to diskio include paths. */
extern bool usb_msc_is_active(void);

/* ─── Timing / protocol constants ───────────────────────────────────────────
 */

#define SD_INIT_BAUD (400 * 1000) /* 400 kHz during card init      */
#define SD_CMD_TIMEOUT_MS 500     /* R1 / data token wait limit     */
#define SD_INIT_TIMEOUT_MS 2000   /* ACMD41 init loop limit         */
#define SD_MULTI_TIMEOUT_MS 5000  /* Total multi-block transfer limit */

/* R1 response flags */
#define SD_R1_IDLE 0x01
#define SD_R1_ERASE_RESET 0x02
#define SD_R1_ILLEGAL_CMD 0x04
#define SD_R1_CRC_ERR 0x08
#define SD_R1_ERASE_SEQ 0x10
#define SD_R1_ADDR_ERR 0x20
#define SD_R1_PARAM_ERR 0x40
#define SD_R1_VALID_MASK 0x80 /* MSB must be 0 for a valid R1  */

/* Data tokens */
#define SD_TOKEN_DATA_START 0xFE  /* Single/multi-read + write     */
#define SD_TOKEN_MULTI_WRITE 0xFC /* Multi-block write start       */
#define SD_TOKEN_STOP_TRAN 0xFD   /* Multi-block write stop        */

/* ─── Card state ────────────────────────────────────────────────────────────
 */

static volatile DSTATUS s_dstatus = STA_NOINIT;
static bool s_is_sdhc = false; /* true: block addressing        */

/* ─── SPI low-level helpers ─────────────────────────────────────────────────
 */

static inline void sd_cs_low(void) { gpio_put(SD_PIN_CS, 0); }
static inline void sd_cs_high(void) { gpio_put(SD_PIN_CS, 1); }

/* Transfer one byte; return received byte. */
static uint8_t spi_byte(uint8_t out) {
  uint8_t in = 0;
  spi_write_read_blocking(SD_SPI_PORT, &out, &in, 1);
  return in;
}

/* Receive len bytes into buf (sends 0xFF on MOSI). */
static void spi_recv_buf(uint8_t *buf, size_t len) {
  memset(buf, 0xFF, len);
  spi_write_read_blocking(SD_SPI_PORT, buf, buf, len);
}

/* Send len bytes from buf (discard MISO). */
static void spi_send_buf(const uint8_t *buf, size_t len) {
  spi_write_blocking(SD_SPI_PORT, buf, len);
}

/* Wait until MISO = 0xFF (card not busy). Returns false on timeout. */
static bool sd_wait_ready(uint32_t timeout_ms) {
  absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
  while (!time_reached(deadline)) {
    if (spi_byte(0xFF) == 0xFF)
      return true;
  }
  return false;
}

/* ─── SD command layer ──────────────────────────────────────────────────────
 */

/*
 * Send one command, return the R1 response byte.
 * CS must be asserted by caller. Deassert is caller's responsibility too.
 *
 * CRC bytes are precomputed for CMD0 (reset) and CMD8 (voltage check).
 * All other commands use 0x01 as a dummy CRC — valid in SPI mode when
 * the card's CRC checking is disabled (default after CMD0).
 */
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg) {
  uint8_t crc = 0x01;
  if (cmd == 0)
    crc = 0x95; /* CMD0 precomputed CRC */
  if (cmd == 8)
    crc = 0x87; /* CMD8 precomputed CRC */

  /* Wait for any pending write to finish before sending.
   * 500ms is needed after multi-block writes (CMD25) where the card may
   * still be doing internal housekeeping. */
  sd_wait_ready(SD_CMD_TIMEOUT_MS);

  uint8_t pkt[6] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24),
                    (uint8_t)(arg >> 16),  (uint8_t)(arg >> 8),
                    (uint8_t)(arg),        crc};
  spi_send_buf(pkt, 6);

  /* R1 arrives within NCR = 8 clocks (1 byte). Poll up to 8 bytes. */
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 8; i++) {
    r1 = spi_byte(0xFF);
    if (!(r1 & SD_R1_VALID_MASK))
      break; /* MSB=0 → valid R1 */
  }
  return r1;
}

/* Send an application-specific command (CMD55 prefix + cmd). */
static uint8_t sd_send_acmd(uint8_t cmd, uint32_t arg) {
  sd_send_cmd(55, 0); /* APP_CMD */
  return sd_send_cmd(cmd, arg);
}

/* ─── Card initialisation ───────────────────────────────────────────────────
 */

static bool sd_init_card(void) {
  /* Drop to slow init clock */
  spi_set_baudrate(SD_SPI_PORT, SD_INIT_BAUD);
  sleep_ms(1);

  /* ≥74 dummy clocks with CS high to transition card to SPI mode */
  sd_cs_high();
  for (int i = 0; i < 10; i++)
    spi_byte(0xFF); /* 80 clocks */

  /* ── Recovery: abort any interrupted multi-block write from a crash ── */
  /* If the previous session crashed during CMD25 (WRITE_MULTIPLE_BLOCK),
   * the card may be stuck in a busy state holding MISO low.  CMD12
   * (STOP_TRANSMISSION) aborts the interrupted write; extra dummy clocks
   * let the card finish internal housekeeping.  Harmless if no write was
   * active — the card just returns an error we ignore. */
  sd_cs_low();
  sd_send_cmd(12, 0);        /* STOP_TRANSMISSION */
  spi_byte(0xFF);            /* stuff byte after CMD12 */
  sd_wait_ready(500);        /* wait for card to finish internal ops */
  sd_cs_high();
  for (int i = 0; i < 20; i++)
    spi_byte(0xFF);          /* 160 more dummy clocks with CS high */

  /* ── CMD0: Software reset ───────────────────────────────────────────── */
  /* Some cards need several attempts to enter SPI idle mode. */
  sd_cs_low();
  uint8_t r1 = 0xFF;
  for (int retry = 0; retry < 10; retry++) {
    r1 = sd_send_cmd(0, 0);
    if (r1 == SD_R1_IDLE)
      break;
    spi_byte(0xFF); /* release bus between retries */
  }
  if (r1 != SD_R1_IDLE) {
    sd_cs_high();
    return false;
  }

  /* ── CMD8: Interface condition (v2 detection) ───────────────────────── */
  /* Arg: VHS=1 (2.7–3.6 V), check pattern=0xAA */
  bool is_v2 = false;
  r1 = sd_send_cmd(8, 0x000001AA);
  if (r1 == SD_R1_IDLE) {
    uint8_t r7[4];
    spi_recv_buf(r7, 4);
    /* Validate voltage range echo and check pattern */
    is_v2 = ((r7[2] & 0x0F) == 0x01) && (r7[3] == 0xAA);
  }
  /* If CMD8 returns 0x05 (illegal command) the card is v1 — that's fine,
   * we just don't set is_v2 and skip the HCS bit in ACMD41. */

  /* ── ACMD41: Card init (activate internal initialisation) ───────────── */
  /* Set HCS bit (bit 30) for v2 cards to signal SDHC support. */
  absolute_time_t deadline = make_timeout_time_ms(SD_INIT_TIMEOUT_MS);
  do {
    r1 = sd_send_acmd(41, is_v2 ? 0x40000000 : 0);
    if (r1 == 0x00)
      break;
  } while (!time_reached(deadline));

  if (r1 != 0x00) {
    sd_cs_high();
    return false;
  }

  /* ── CMD58: Read OCR — check CCS bit to distinguish SDHC vs SDSC ────── */
  s_is_sdhc = false;
  if (is_v2) {
    r1 = sd_send_cmd(58, 0);
    if (r1 == 0x00) {
      uint8_t ocr[4];
      spi_recv_buf(ocr, 4);
      s_is_sdhc = (ocr[0] & 0x40) != 0; /* CCS bit */
    }
  }

  /* ── CMD16: Set block length = 512 (SDSC cards only) ─────────────────── */
  if (!s_is_sdhc) {
    r1 = sd_send_cmd(16, 512);
    if (r1 != 0x00) {
      sd_cs_high();
      return false;
    }
  }

  sd_cs_high();
  spi_byte(0xFF); /* Release the bus */

  /* Switch to full operating speed */
  spi_set_baudrate(SD_SPI_PORT, SD_SPI_BAUD);
  return true;
}

/* Forward declarations for recovery functions (defined below disk_write) */
static void sd_bus_recovery(void);
static bool sd_try_reinit(uint8_t r1);

/* ─── FatFS disk interface ──────────────────────────────────────────────────
 */

DSTATUS disk_initialize(BYTE pdrv) {
  if (pdrv != 0)
    return STA_NOINIT;
  if (sd_init_card()) {
    s_dstatus = 0;
    return s_dstatus;
  }
  /* Basic init failed — try bus recovery for stuck cards */
  sd_bus_recovery();
  sleep_ms(100);
  s_dstatus = sd_init_card() ? 0 : STA_NOINIT;
  return s_dstatus;
}

DSTATUS disk_status(BYTE pdrv) {
  if (pdrv != 0)
    return STA_NOINIT;
  return s_dstatus;
}

/* Multi-block read using CMD18 with per-block token polling.
 * Polls for each 0xFE data token individually — compatible with all SD cards.
 */
static bool sd_read_multi(BYTE *buff, uint32_t addr, UINT count) {
  UINT count0 = count;
  /* Send CMD18: READ_MULTIPLE_BLOCK */
  if (sd_send_cmd(18, addr) != 0x00) {
    return false;
  }

  absolute_time_t outer_deadline = make_timeout_time_ms(SD_MULTI_TIMEOUT_MS);

  while (count--) {
    if (time_reached(outer_deadline)) {
      printf("[SD] multi-block read total timeout (%dms)\n",
             SD_MULTI_TIMEOUT_MS);
      sd_send_cmd(12, 0);
      spi_byte(0xFF);
      return false;
    }
    absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
    uint8_t tok;
    do {
      tok = spi_byte(0xFF);
    } while (tok != SD_TOKEN_DATA_START && !time_reached(deadline));
    if (tok != SD_TOKEN_DATA_START) {
      printf("[SD] multi-block token timeout blk %u/%u tok=0x%02x\n",
             (unsigned)(count0 - count - 1), (unsigned)count0, tok);
      sd_send_cmd(12, 0); /* Try to stop anyway */
      spi_byte(0xFF);
      return false;
    }

    spi_recv_buf(buff, 512);
    spi_byte(0xFF); /* CRC high */
    spi_byte(0xFF); /* CRC low  */
    buff += 512;
  }

  /* CMD12: STOP_TRANSMISSION */
  sd_send_cmd(12, 0);
  spi_byte(0xFF); /* Discard stuff byte */
  sd_wait_ready(SD_CMD_TIMEOUT_MS);
  
  return true;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0 || (s_dstatus & STA_NOINIT))
    return RES_NOTRDY;

  /* SDSC uses byte address; SDHC uses block address */
  uint32_t addr = s_is_sdhc ? (uint32_t)sector : (uint32_t)sector * 512;

  sd_cs_low();

  if (count == 1) {
    /* CMD17: READ_SINGLE_BLOCK */
    uint8_t r1 = sd_send_cmd(17, addr);
    if (r1 != 0x00) {
      /* Card may have spontaneously reset.  Try reinit and retry once. */
      sd_cs_high();
      if (sd_try_reinit(r1)) {
        sd_cs_low();
        r1 = sd_send_cmd(17, addr);
      }
      if (r1 != 0x00) {
        if (!usb_msc_is_active())
          printf("[SD_RD] CMD17 fail: R1=0x%02x sec=%lu\n", r1, (unsigned long)sector);
        sd_cs_high();
        return RES_ERROR;
      }
    }

    absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
    uint8_t tok;
    do {
      tok = spi_byte(0xFF);
    } while (tok != SD_TOKEN_DATA_START && !time_reached(deadline));
    if (tok != SD_TOKEN_DATA_START) {
      sd_cs_high();
      return RES_ERROR;
    }

    spi_recv_buf(buff, 512);
    spi_byte(0xFF); /* CRC high */
    spi_byte(0xFF); /* CRC low  */
  } else {
    /* Multi-block read via CMD18 with per-block token polling */
    if (!sd_read_multi(buff, addr, count)) {
      /* Reinit and retry the entire multi-block read once */
      sd_cs_high();
      if (sd_try_reinit(0x01 /* assume idle-state reset */)) {
        sd_cs_low();
        if (!sd_read_multi(buff, addr, count)) {
          sd_cs_high();
          return RES_ERROR;
        }
      } else {
        return RES_ERROR;
      }
    }
  }

  sd_cs_high();
  spi_byte(0xFF); /* Release */
  return RES_OK;
}

#if FF_FS_READONLY == 0

/* SRAM staging buffer — eliminates XIP cache / QMI contention when writing
 * data that lives in PSRAM (e.g. FIL->buf allocated via umm_malloc).
 * CPU reads from PSRAM cached alias can stall on QMI bus contention with
 * Core 1 (WiFi/TLS), causing irregular SPI clock gaps. */
static uint8_t s_wr_staging[512];

/* ─── Core 1 pause during SD writes ─────────────────────────────────────────
 * CYW43 WiFi draws significant current on Core 1.  Combined with SD card
 * flash programming current, the 3.3 V rail droops below the card's minimum
 * operating voltage → spontaneous card reset (R1 idle bit).  Pausing Core 1
 * during writes eliminates the concurrent power draw.  The TCP stack handles
 * the brief (~5-20 ms per sector) interruption via retransmission. */
extern volatile bool g_core1_pause;
extern volatile bool g_core1_paused;

static inline void sd_pause_core1(void) {
  if (usb_msc_is_active()) return; /* already in IRQ — can't wait */
  g_core1_pause = true;
  /* Spin until Core 1 acknowledges the pause.  Core 1 checks this flag
   * between wifi_poll() iterations — during active TLS, mg_mgr_poll()
   * can block for 50-100 ms (record decryption), so 200 ms is needed
   * to guarantee at least one check opportunity.  Without a confirmed
   * pause, the combined CYW43 + SD flash-programming current droops
   * the 3.3 V rail below the card's minimum → spontaneous card reset. */
  for (int i = 0; i < 200 && !g_core1_paused; i++)
    sleep_ms(1);
  if (!g_core1_paused && !usb_msc_is_active())
    printf("[SD_WR] Core 1 pause timeout (200 ms) — write may fail\n");
}

static inline void sd_resume_core1(void) {
  g_core1_pause = false;
}

/* ─── Aggressive bus recovery ──────────────────────────────────────────────
 * When the card enters an unresponsive state (R1=0xFF), the SPI interface
 * may be stuck mid-transaction.  A card that brownout-reset during WiFi
 * activity (without any SPI communication) reverts to native SD mode and
 * needs a full re-entry sequence.
 *
 * Recovery steps (per SD Physical Layer Simplified Spec):
 *   1. CMD12 (STOP_TRANSMISSION) to abort any in-progress operation
 *   2. ≥800 dummy clocks with CS high to flush stuck state machines
 *   3. Delay for internal card housekeeping
 */
static void sd_bus_recovery(void) {
  spi_set_baudrate(SD_SPI_PORT, SD_INIT_BAUD);

  /* Abort any in-progress multi-block operation */
  sd_cs_low();
  sd_send_cmd(12, 0);
  spi_byte(0xFF);
  sd_wait_ready(SD_CMD_TIMEOUT_MS);
  sd_cs_high();

  /* 800 dummy clocks (100 bytes × 8 bits) with CS high.  A card stuck
   * mid-transaction needs clocks to complete its internal operation.
   * The spec requires ≥74 for init; deeply stuck cards need more. */
  for (int i = 0; i < 100; i++)
    spi_byte(0xFF);
}

/* Try to recover a card that has become unresponsive.  Handles both
 * transient resets (R1 idle bit, from brief power droop) and deep
 * lockups (R1=0xFF, from prolonged brownout during WiFi activity).
 *
 * Three escalating attempts:
 *   1. Quick reinit (10 ms delay) — handles transient card resets
 *   2. Bus recovery + reinit (100 ms) — handles stuck SPI transactions
 *   3. Extended delay + bus recovery (500 ms) — handles deep lockup
 *
 * Returns true if the card was successfully recovered. */
static bool sd_try_reinit(uint8_t r1) {
  if (r1 == 0x00)
    return false; /* No error — nothing to reinit */

  if (!usb_msc_is_active())
    printf("[SD] Card error (R1=0x%02x), recovering...\n", r1);

  /* Attempt 1: Quick reinit — sufficient for transient resets */
  sd_cs_high();
  sleep_ms(10);
  if (sd_init_card()) {
    if (!usb_msc_is_active())
      printf("[SD] Recovered (quick reinit)\n");
    return true;
  }

  /* Attempt 2: Bus recovery + reinit — clears stuck SPI state */
  if (!usb_msc_is_active())
    printf("[SD] Quick reinit failed, trying bus recovery...\n");
  sd_bus_recovery();
  sleep_ms(100);
  if (sd_init_card()) {
    if (!usb_msc_is_active())
      printf("[SD] Recovered (bus recovery)\n");
    return true;
  }

  /* Attempt 3: Extended delay — card may need time to finish internal
   * flash operations (erase/program) before it can accept commands. */
  if (!usb_msc_is_active())
    printf("[SD] Bus recovery failed, trying extended delay...\n");
  sleep_ms(500);
  sd_bus_recovery();
  sleep_ms(100);
  if (sd_init_card()) {
    if (!usb_msc_is_active())
      printf("[SD] Recovered (extended delay)\n");
    return true;
  }

  if (!usb_msc_is_active())
    printf("[SD] Recovery failed after 3 attempts\n");
  return false;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  static uint32_t s_wr_seq = 0;
  if (pdrv != 0 || (s_dstatus & STA_NOINIT))
    return RES_NOTRDY;
  if (s_dstatus & STA_PROTECT)
    return RES_WRPRT;

  s_wr_seq++;
  /* NOTE: disk_write is called from USB IRQ context during MSC mode.
   * Printf writes to CDC serial which shares the USB peripheral; when
   * the CDC TX buffer fills, printf spins waiting for USB events that
   * can't fire (we're already in USBCTRL_IRQ) → deadlock → host timeout
   * → filesystem corruption.  All printf below guarded by
   * usb_msc_is_active() check.  Same applies to disk_read(). */

  uint32_t addr = s_is_sdhc ? (uint32_t)sector : (uint32_t)sector * 512;

  /* Per-sector Core 1 pause is counterproductive — the rapid pause/resume
   * cycling gives Core 1 brief CYW43 activity windows between every 512B
   * write, making the problem worse.  Callers that need clean SD writes
   * (OTA updater, USB MSC) should pause Core 1 once for the entire batch
   * using g_core1_pause directly, matching the USB MSC pattern. */
  // sd_pause_core1();

  DRESULT result = RES_OK;

  sd_cs_low();

  if (count == 1) {
    /* CMD24: WRITE_BLOCK */
    uint8_t r1 = sd_send_cmd(24, addr);
    if (r1 != 0x00) {
      /* Card may have spontaneously reset (power droop, noise).
       * If R1 has idle bit, re-init card and retry once. */
      if (sd_try_reinit(r1)) {
        sd_cs_low();
        r1 = sd_send_cmd(24, addr);
      }
      if (r1 != 0x00) {
        if (!usb_msc_is_active())
          printf("[SD_WR] CMD24 fail: R1=0x%02x sec=%lu buf=%p\n", r1, (unsigned long)sector, buff);
        sd_cs_high();
        result = RES_ERROR;
        goto out;
      }
    }

    /* Stage data in SRAM to avoid PSRAM/XIP stalls during SPI transfer */
    memcpy(s_wr_staging, buff, 512);

    spi_byte(0xFF);                    /* One idle byte before token   */
    spi_byte(SD_TOKEN_DATA_START);     /* Data start token             */
    spi_send_buf(s_wr_staging, 512);   /* 512 bytes of payload         */
    spi_byte(0xFF);                    /* Dummy CRC (2 bytes)          */
    spi_byte(0xFF);

    /* Poll for data response token — card may need several clocks */
    {
      absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
      uint8_t resp;
      do {
        resp = spi_byte(0xFF);
      } while (resp == 0xFF && !time_reached(deadline));
      if ((resp & 0x1F) != 0x05) {
        if (!usb_msc_is_active())
          printf("[SD_WR] CMD24 data resp=0x%02x sec=%lu\n", resp, (unsigned long)sector);
        sd_cs_high();
        result = RES_ERROR;
        goto out;
      }
    }

    if (!sd_wait_ready(SD_CMD_TIMEOUT_MS)) {
      if (!usb_msc_is_active())
        printf("[SD_WR] CMD24 busy timeout sec=%lu\n", (unsigned long)sector);
      sd_cs_high();
      result = RES_ERROR;
      goto out;
    }
  } else {
    /* ACMD23 (SET_WR_BLK_ERASE_COUNT): optional pre-erase hint before a
     * multi-block write.  This was previously sent as bare CMD23, which is
     * an MMC-only command that SD cards reject in SPI mode — and the
     * failure path ran a full card re-init before EVERY multi-block write.
     * Back-to-back reinit storms (FatFS dir_clear zeroing a new directory
     * cluster in 64-block bursts) wedged the card into an R1=0x3f state
     * that survives soft reboots (only a power cycle clears it) and
     * starved the watchdog.  The hint is best-effort: if the card rejects
     * it, just proceed — CMD25 terminates with the Stop Tran token
     * regardless. */
    sd_send_acmd(23, count);

    uint8_t r1 = sd_send_cmd(25, addr);
    if (r1 != 0x00) {
      if (sd_try_reinit(r1)) {
        sd_cs_low();
        sd_send_acmd(23, count); /* best-effort pre-erase hint */
        r1 = sd_send_cmd(25, addr);
      }
      if (r1 != 0x00) {
        if (!usb_msc_is_active())
          printf("[SD_WR] CMD25 fail: R1=0x%02x sec=%lu cnt=%u buf=%p\n", r1, (unsigned long)sector, count, buff);
        sd_cs_high();
        result = RES_ERROR;
        goto out;
      }
    }

    UINT blk = 0;
    absolute_time_t outer_deadline = make_timeout_time_ms(SD_MULTI_TIMEOUT_MS);
    while (count--) {
      if (time_reached(outer_deadline)) {
        if (!usb_msc_is_active())
          printf("[SD] multi-block write total timeout (%dms)\n",
                 SD_MULTI_TIMEOUT_MS);
        spi_byte(SD_TOKEN_STOP_TRAN);
        spi_byte(0xFF);
        sd_wait_ready(SD_CMD_TIMEOUT_MS);
        sd_cs_high();
        result = RES_ERROR;
        goto out;
      }
      /* Stage data in SRAM */
      memcpy(s_wr_staging, buff, 512);

      spi_byte(0xFF);                 /* Idle byte                */
      spi_byte(SD_TOKEN_MULTI_WRITE); /* Multi-block start token  */
      spi_send_buf(s_wr_staging, 512);
      spi_byte(0xFF); /* Dummy CRC               */
      spi_byte(0xFF);

      /* Poll for data response token — card may need several clocks */
      {
        absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
        uint8_t resp;
        do {
          resp = spi_byte(0xFF);
        } while (resp == 0xFF && !time_reached(deadline));
        if ((resp & 0x1F) != 0x05) {
          if (!usb_msc_is_active())
            printf("[SD_WR] CMD25 blk %u resp=0x%02x sec=%lu\n", blk, resp, (unsigned long)sector);
          spi_byte(SD_TOKEN_STOP_TRAN);
          sd_cs_high();
          result = RES_ERROR;
          goto out;
        }
      }

      if (!sd_wait_ready(SD_CMD_TIMEOUT_MS)) {
        if (!usb_msc_is_active())
          printf("[SD_WR] CMD25 blk %u busy timeout sec=%lu\n", blk, (unsigned long)sector);
        spi_byte(SD_TOKEN_STOP_TRAN);
        sd_cs_high();
        result = RES_ERROR;
        goto out;
      }

      buff += 512;
      blk++;
    }

    /* Stop the multi-block write.  SD spec requires at least one idle
     * byte before the Stop Tran token so the card can synchronise to a
     * byte boundary.  After the card signals not-busy we send extra idle
     * clocks — some cards need recovery time before accepting the next
     * command (CMD24 for a single-sector flush from FatFs). */
    spi_byte(0xFF);                 /* Idle byte before stop token  */
    spi_byte(SD_TOKEN_STOP_TRAN);   /* 0xFD — end multi-block write */
    spi_byte(0xFF);                 /* Stuff byte after stop token  */
    sd_wait_ready(SD_CMD_TIMEOUT_MS);
    spi_byte(0xFF);                 /* Extra idle clocks            */
    spi_byte(0xFF);
  }

  sd_cs_high();
  spi_byte(0xFF);

out:
  // sd_resume_core1();  /* see sd_pause_core1 comment above */
  return result;
}

#endif /* FF_FS_READONLY */

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  if (pdrv != 0)
    return RES_PARERR;
  if (s_dstatus & STA_NOINIT)
    return RES_NOTRDY;

  switch (cmd) {
  case CTRL_SYNC:
    sd_cs_low();
    sd_wait_ready(500);
    sd_cs_high();
    return RES_OK;

  case GET_SECTOR_SIZE:
    *(WORD *)buff = 512;
    return RES_OK;

  case GET_BLOCK_SIZE:
    *(DWORD *)buff = 1; /* Erase block size unknown; report 1 */
    return RES_OK;

  case GET_SECTOR_COUNT: {
    /* CMD9: READ_CSD — parse to determine card capacity */
    sd_cs_low();
    if (sd_send_cmd(9, 0) != 0x00) {
      sd_cs_high();
      return RES_ERROR;
    }

    absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
    uint8_t tok;
    do {
      tok = spi_byte(0xFF);
    } while (tok != SD_TOKEN_DATA_START && !time_reached(deadline));
    if (tok != SD_TOKEN_DATA_START) {
      sd_cs_high();
      return RES_ERROR;
    }

    uint8_t csd[16];
    spi_recv_buf(csd, 16);
    spi_byte(0xFF);
    spi_byte(0xFF); /* CRC */
    sd_cs_high();

    DWORD sectors = 0;
    if ((csd[0] >> 6) == 1) {
      /* CSD v2 (SDHC/SDXC): C_SIZE in bits [69:48] */
      uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                        ((uint32_t)csd[8] << 8) | (uint32_t)csd[9];
      sectors = (c_size + 1) * 1024;
    } else {
      /* CSD v1 (SDSC) */
      uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) |
                        ((uint32_t)csd[7] << 2) | (uint32_t)(csd[8] >> 6);
      uint32_t c_mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
      uint32_t read_bl_len = csd[5] & 0x0F;
      sectors = (c_size + 1) << (c_mult + 2);
      if (read_bl_len > 9)
        sectors <<= (read_bl_len - 9);
    }
    *(DWORD *)buff = sectors;
    return RES_OK;
  }

  default:
    return RES_PARERR;
  }
}
