// ota_update.c — SD-staged OTA firmware updater
//
// Flow:
//   1. Lua app downloads .bin to /system/update.bin, plus update.sha256 and
//      update.sig (ECDSA P-256 signature, see ota_verify.h)
//   2. Lua calls picocalc.sys.applyUpdate(path) → ota_prepare_update (size,
//      vector table, checksum, signature) → ui_confirm → ota_trigger_update()
//   3. ota_trigger_update() sets the one-shot token (scratch[1]=OTA_MAGIC), reboots
//   4. On next boot, main() calls ota_check_pending() + ota_apply_update()
//   5. ota_apply_update() pre-reads .bin into PSRAM, re-checks the checksum and
//      signature over the bytes it is about to write, then an SRAM-resident
//      function erases+programs all flash sectors and reboots.
//   A refused image is renamed to *.rejected (never retried) and logged.
//
// Safety:
//   - Flash writer runs BEFORE Core 1 launch (no Mongoose, no audio ISRs)
//   - Entire firmware pre-read into PSRAM before any flash writes begin
//   - Flash write loop is SRAM-resident and never calls flash-resident code
//     (the old firmware's code is overwritten sector-by-sector, so calling
//     back into flash would execute new firmware code at old addresses)
//   - Interrupts masked for the ENTIRE write, not per-sector: IRQ handlers
//     (timer alarm, stdio_usb, TinyUSB) are flash-resident and become
//     garbage once their sectors are rewritten with a cross-version image.
//     Per-sector re-enable windows bricked cross-version updates.
//   - Sector 0 (boot stage 2 + vector table) written last — if the write is
//     interrupted, the old firmware's boot code remains intact
//   - SRAM staging buffer used for flash_range_program (QMI bus may be
//     locked to CS0 during flash operations, making PSRAM on CS1 inaccessible)

#include "ota_update.h"
#include "app_stack.h"
#include "crashlog.h"
#include "../drivers/keyboard.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "../drivers/display.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
#include "ui.h"

// SHA-256 verification via mbedTLS (already linked for HTTPS)
#include "mbedtls/sha256.h"

// ── Progress display ────────────────────────────────────────────────────────

static void ota_show_progress(uint32_t bytes_done, uint32_t bytes_total) {
    int pct = (int)((uint64_t)bytes_done * 100 / bytes_total);

    // Progress bar dimensions
    int bar_x = 40, bar_y = 170, bar_w = 240, bar_h = 16;
    int fill_w = (int)((uint64_t)bar_w * bytes_done / bytes_total);

    display_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_DKGRAY);
    if (fill_w > 0)
        display_fill_rect(bar_x, bar_y, fill_w, bar_h, COLOR_GREEN);
    display_draw_rect(bar_x, bar_y, bar_w, bar_h, COLOR_WHITE);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%  (%luK / %luK)",
             pct, (unsigned long)(bytes_done / 1024),
             (unsigned long)(bytes_total / 1024));
    // Center text below progress bar
    int tw = display_text_width(buf);
    display_draw_text((320 - tw) / 2, bar_y + bar_h + 8, buf,
                      COLOR_WHITE, COLOR_BLACK);

    display_flush();
}

static void ota_show_status(const char *line1, const char *line2, uint16_t color) {
    display_clear(COLOR_BLACK);
    display_draw_text(8, 8, "PicoDeck Firmware Update", COLOR_CYAN, COLOR_BLACK);
    display_draw_text(8, 140, line1, color, COLOR_BLACK);
    if (line2)
        display_draw_text(8, 156, line2, COLOR_GRAY, COLOR_BLACK);
    display_flush();
}

// ── Image authentication ────────────────────────────────────────────────────
// Checksum + signature checks live in ota_verify.c (shared with the simulator
// and the host unit tests).
#include "ota_verify.h"

// ── SRAM-resident flash writer ──────────────────────────────────────────────
// This function runs entirely from SRAM and NEVER calls back into flash.
// Once flash writes begin, the old firmware's code is progressively
// overwritten — any call to a flash-resident function would execute the NEW
// firmware's code at the OLD addresses, causing undefined behaviour.
//
// CRITICAL: interrupts are masked ONCE at entry and never restored.  IRQ
// *handlers* and their callees (alarm_pool_irq_handler, stdio_usb's
// low_priority_worker_irq, TinyUSB's tu_fifo_write/memset) are
// flash-resident.  An earlier version of this function re-enabled
// interrupts between sector writes; once the sectors holding those
// handlers were overwritten with an address-shifted (cross-version) image,
// the next timer/USB interrupt executed garbage and the device crashed
// mid-flash (half-written image = brick).  Same-version re-flashes only
// survived because the overwritten bytes were identical.  Everything this
// function calls (flash_range_erase/program and their helpers, including
// the QMI CS1/PSRAM save-restore) is RAM- or ROM-resident — verified by
// disassembly of the linked ELF.  The watchdog and the final AIRCR
// SYSRESETREQ are unaffected by PRIMASK.
//
// The function writes sectors 1..N first (deferring sector 0 so the old
// boot code stays intact as long as possible), then writes sector 0 last,
// and reboots without returning to flash.
//
// fw_data:  firmware image in PSRAM (QMI CS1, accessible between flash ops)
// fw_size:  total firmware size in bytes
//
// This function does NOT return.

static void __no_inline_not_in_flash_func(ota_write_and_reboot)(
    const uint8_t *fw_data, uint32_t fw_size) {

    // SRAM staging buffer — flash_range_program reads from this during
    // erase/program when the QMI bus is locked to CS0 (flash), so the
    // source must be in SRAM, not PSRAM.
    static uint8_t staging[FLASH_SECTOR_SIZE];

    // Point of no return: mask all interrupts for the entire write (see
    // header comment).  Never restored — we hard-reset at the end.
    (void)save_and_disable_interrupts();

    // Write sectors 1..N (defer sector 0)
    for (uint32_t off = FLASH_SECTOR_SIZE; off < fw_size;
         off += FLASH_SECTOR_SIZE) {
        uint32_t chunk = fw_size - off;
        if (chunk > FLASH_SECTOR_SIZE) chunk = FLASH_SECTOR_SIZE;

        // Copy PSRAM → SRAM staging (byte loop — no flash-resident memcpy)
        for (uint32_t i = 0; i < chunk; i++)
            staging[i] = fw_data[off + i];
        for (uint32_t i = chunk; i < FLASH_SECTOR_SIZE; i++)
            staging[i] = 0xFF; // pad to full sector

        flash_range_erase(off, FLASH_SECTOR_SIZE);
        flash_range_program(off, staging, FLASH_SECTOR_SIZE);

        // Feed watchdog directly (watchdog_update is in flash)
        watchdog_hw->load = 10u * 1000u * 1000u; // ~10 s reload
    }

    // Commit sector 0 (boot stage 2 + vector table) — last write
    {
        uint32_t chunk = fw_size < FLASH_SECTOR_SIZE ? fw_size : FLASH_SECTOR_SIZE;
        for (uint32_t i = 0; i < chunk; i++)
            staging[i] = fw_data[i];
        for (uint32_t i = chunk; i < FLASH_SECTOR_SIZE; i++)
            staging[i] = 0xFF;

        flash_range_erase(0, FLASH_SECTOR_SIZE);
        flash_range_program(0, staging, FLASH_SECTOR_SIZE);
    }

    // Clear scratch registers and reboot into new firmware.
    // Cannot call watchdog_reboot() (flash-resident) — use direct reset.
    // No kbd_prepare_reset() here: interrupts are masked for this whole
    // routine (see the header comment), so the bus engine could not finish a
    // transaction anyway. The caller pauses it before the write begins.
    watchdog_hw->scratch[0] = 0; // clear boot counter
    watchdog_hw->scratch[OTA_SCRATCH_IDX] = 0; // clear OTA flag

    // ARM System Reset via AIRCR register (direct write — no flash code).
    // PPB_BASE + M33_AIRCR_OFFSET = 0xE000ED0C (Application Interrupt and
    // Reset Control Register).  VECTKEY=0x05FA, SYSRESETREQ=bit 2.
    volatile uint32_t *aircr = (volatile uint32_t *)(PPB_BASE + M33_AIRCR_OFFSET);
    __dmb(); // data memory barrier
    *aircr = (0x05FAu << 16) | (1u << 2);
    __dmb();
    while (1) { /* wait for reset */ }
}

// ── Boot-time image check (runs on the OS stack) ────────────────────────────

typedef struct {
    const uint8_t *data;
    uint32_t len;
    const char *err;
    bool ok;
} ota_image_check_t;

static void ota_check_image_buf(void *arg) {
    ota_image_check_t *chk = (ota_image_check_t *)arg;
    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    for (uint32_t off = 0; off < chk->len; off += 64u * 1024u) {
        uint32_t n = chk->len - off < 64u * 1024u ? chk->len - off : 64u * 1024u;
        mbedtls_sha256_update(&sha, chk->data + off, n);
        watchdog_update();
    }
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    chk->ok = ota_check_digest(digest, OTA_HASH_PATH, OTA_SIG_PATH, &chk->err);
}

// ── Staged-file bookkeeping ─────────────────────────────────────────────────

// Rename the staged image and its .sha256/.sig to <name><suffix>, replacing
// older files of that name; a file that cannot be renamed is deleted so it is
// never picked up again.
static void ota_rename_one(const char *path, const char *suffix) {
    if (sdcard_fsize(path) < 0) return;
    char dst[48];
    snprintf(dst, sizeof(dst), "%s%s", path, suffix);
    sdcard_delete(dst);
    if (!sdcard_rename(path, dst)) {
        printf("[OTA] Rename failed, deleting %s\n", path);
        sdcard_delete(path);
    }
}

static void ota_rename_set(const char *suffix) {
    ota_rename_one(OTA_BIN_PATH, suffix);
    ota_rename_one(OTA_HASH_PATH, suffix);
    ota_rename_one(OTA_SIG_PATH, suffix);
}

// ── Public API ──────────────────────────────────────────────────────────────

bool ota_check_pending(void) {
    return watchdog_hw->scratch[OTA_SCRATCH_IDX] == OTA_MAGIC;
}

bool ota_apply_update(void) {
    printf("[OTA] Applying firmware update from %s\n", OTA_BIN_PATH);

    const char *reason = NULL;

    // Open the firmware file
    int file_size = sdcard_fsize(OTA_BIN_PATH);
    if (file_size <= 0 || (uint32_t)file_size < OTA_MIN_SIZE) {
        reason = "Firmware file too small";
        printf("[OTA] File too small: %d bytes\n", file_size);
        goto fail;
    }
    if ((uint32_t)file_size > OTA_MAX_SIZE) {
        reason = "Firmware file too large";
        printf("[OTA] File too large: %d bytes\n", file_size);
        goto fail;
    }

    // ── Pre-read entire firmware into PSRAM ─────────────────────────────────
    // The flash write loop is SRAM-resident and cannot call flash-resident
    // code (SD card, FatFS, display, etc.) because those flash regions get
    // overwritten during the update.  Pre-reading into PSRAM means the write
    // loop only needs PSRAM reads and SRAM staging — no SD card access.
    ota_show_status("Loading firmware...", "Reading from SD card", COLOR_WHITE);
    watchdog_update();

    uint8_t *fw_buf = (uint8_t *)umm_malloc((uint32_t)file_size);
    if (!fw_buf) {
        reason = "Not enough memory";
        printf("[OTA] umm_malloc(%d) failed\n", file_size);
        goto fail;
    }

    sdfile_t f = sdcard_fopen(OTA_BIN_PATH, "r");
    if (!f) {
        reason = "Cannot open firmware file";
        printf("[OTA] Cannot open %s\n", OTA_BIN_PATH);
        umm_free(fw_buf);
        goto fail;
    }

    // Read in chunks with progress
    uint32_t total = (uint32_t)file_size;
    uint32_t bytes_read = 0;
    while (bytes_read < total) {
        int n = sdcard_fread(f, fw_buf + bytes_read, FLASH_SECTOR_SIZE);
        if (n <= 0) break;
        bytes_read += (uint32_t)n;
        ota_show_progress(bytes_read, total);
        watchdog_update();
    }
    sdcard_fclose(f);

    if (bytes_read < total) {
        reason = "SD card read error";
        printf("[OTA] Read only %lu of %lu bytes\n",
               (unsigned long)bytes_read, (unsigned long)total);
        umm_free(fw_buf);
        goto fail;
    }

    // Validate vector table from the pre-read buffer
    if (!ota_image_header_ok(fw_buf, total)) {
        reason = "Invalid firmware header";
        umm_free(fw_buf);
        goto fail;
    }

    // Authenticate the exact bytes that will be written: checksum pre-check,
    // then the ECDSA signature against the key embedded in this firmware.
    // On the 32 KB OS stack (PSRAM): PEM + ECDSA are too deep for the 4 KB
    // MSP.  Only the check runs there — the flash writer below must stay on
    // the MSP (SRAM), since PSRAM is unreachable while flash is written.
    ota_show_status("Verifying firmware...", "Checking signature", COLOR_WHITE);
    {
        ota_image_check_t chk = {fw_buf, total, NULL, false};
        if (!app_stack_run_os(ota_check_image_buf, &chk))
            chk.err = "Not enough memory to verify";
        if (!chk.ok) {
            reason = chk.err;
            printf("[OTA] Refusing image: %s\n", reason);
            umm_free(fw_buf);
            goto fail;
        }
        printf("[OTA] SHA-256 and signature verified\n");
    }

    printf("[OTA] Firmware loaded into PSRAM: %lu bytes\n", (unsigned long)total);

    // ── Rename update files BEFORE flash writes ─────────────────────────────
    // After flash writes begin, SD card functions (flash-resident) can't be
    // called.  Rename now so the file doesn't re-trigger on next boot.
    ota_rename_set(".flashed");

    // ── Write firmware to flash ─────────────────────────────────────────────
    // Point of no return — this function does not return.  It writes all
    // flash sectors from PSRAM (SRAM-resident, no flash code called),
    // clears scratch registers, and reboots into the new firmware.
    ota_show_status("Writing firmware...", "DO NOT POWER OFF!", COLOR_YELLOW);
    sleep_ms(500);
    watchdog_update();
    stdio_flush();

    printf("[OTA] Starting flash write (%lu bytes)...\n", (unsigned long)total);
    stdio_flush();

    // Pause the keyboard bus here, in task context, before the write
    // sequence masks interrupts for good (see ota_write_and_reboot's header
    // comment) — it cannot be paused from inside that routine.
    kbd_prepare_reset();

    // This call never returns
    ota_write_and_reboot(fw_buf, total);

    // Unreachable
    while (1) tight_loop_contents();

fail:
    // Clear the OTA flag so we don't loop on failed updates, and move the
    // image aside so it is neither retried nor mistaken for an unrequested
    // one at the next boot.  tools/ota_flash.py looks for update.bin.rejected.
    watchdog_hw->scratch[OTA_SCRATCH_IDX] = 0;
    ota_show_status("Update failed!", reason ? reason : "unknown error",
                    COLOR_RED);
    crashlog_write("OTA REJECTED", "system", reason ? reason : "unknown error",
                   OTA_BIN_PATH " renamed to update.bin.rejected");
    ota_rename_set(".rejected");
    return false;
}

void ota_discard_unrequested(void) {
    printf("[OTA] %s present without an update request: not flashing, "
           "renaming to .stale\n", OTA_BIN_PATH);
    crashlog_write("OTA IGNORED", "system",
                   "firmware staged without an update request",
                   OTA_BIN_PATH " renamed to update.bin.stale");
    ota_rename_set(".stale");
}

bool ota_prepare_update(const char *bin_path, const char **out_err) {
    return ota_prepare_check(bin_path, OTA_HASH_PATH, OTA_SIG_PATH, out_err);
}

bool ota_trigger_update(const char *bin_path, const char **out_err) {
    if (!ota_prepare_update(bin_path, out_err))
        return false;
    int size = sdcard_fsize(bin_path);

    // If the file is not already at the standard path, it needs to be there
    // for the boot-time updater to find it.
    if (strcmp(bin_path, OTA_BIN_PATH) != 0) {
        // Copy to standard location
        if (!sdcard_copy(bin_path, OTA_BIN_PATH, NULL, NULL)) {
            *out_err = "Failed to copy firmware to /system/update.bin";
            return false;
        }
    }

    printf("[OTA] Triggering update: %d bytes, rebooting...\n", size);
    stdio_flush();

    // Set OTA magic and reboot. Single helper for both triggers (the
    // `reboot-ota` dev command and sys.applyUpdate's confirm), so
    // kbd_prepare_reset() belongs here once rather than at each caller.
    watchdog_hw->scratch[OTA_SCRATCH_IDX] = OTA_MAGIC;
    sleep_ms(100);
    kbd_prepare_reset();
    watchdog_reboot(0, 0, 0);

    // Unreachable
    while (1) tight_loop_contents();
    return true;
}
