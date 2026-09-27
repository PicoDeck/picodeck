#include "usb_msc.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../drivers/wifi.h"
#include "../os/config.h"
#include "../os/os.h"
#include "../os/ui.h"

#include <stdatomic.h>

// Define BYTE/LBA_t and other FatFs types manually before diskio.h
// just in case they are missing from diskio.h inclusion order
#include <stdint.h>
#include <stdio.h>
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint32_t DWORD;
typedef DWORD LBA_t;

#include "diskio.h"
#include "ff.h"

#include <string.h>

// Block devices and filesystems
static uint16_t msc_block_size = 512;
static bool s_msc_active = false;
static volatile bool s_msc_ejected = false;  // Set by tud_msc_start_stop_cb when host ejects
static volatile bool s_media_changed = false; // Signals UNIT ATTENTION on next TUR
static uint32_t s_cached_sector_count = 0;   // Cached at MSC entry, avoids SPI per query

bool usb_msc_is_active(void) { return s_msc_active; }

// --------------------------------------------------------------------
// USB MSC Entry point
// --------------------------------------------------------------------

// Declared in main.c - pauses Core 1's background tasks (WiFi, HTTP, audio)
extern _Atomic bool g_core1_pause;
extern _Atomic bool g_core1_paused;
// Declared in main.c - feeds the watchdog and stamps Core 0's heartbeat.
extern void core0_heartbeat(void);

void usb_msc_enter_mode(void) {
  printf("[USB MSC] Entering USB Mass Storage mode\n");

  // 1. Disconnect WiFi and pause Core 1 to prevent SPI/I2C contention
  //    Core 1 runs WiFi/HTTP/audio tasks every 5ms which can interfere
  //    with USB MSC operations and cause keyboard I2C timeouts.
  wifi_status_t wst = wifi_get_status();
  bool was_connected = (wst == WIFI_STATUS_CONNECTED || wst == WIFI_STATUS_ONLINE);
  if (was_connected) {
    printf("[USB MSC] Disconnecting WiFi...\n");
    wifi_disconnect();
  }
  
  // Pause Core 1 completely - wait for acknowledgment to ensure it's stopped
  g_core1_pause = true;
  for (int i = 0; i < 500 && !g_core1_paused; i++)
    sleep_ms(1);
  if (!g_core1_paused)
    printf("[USB_MSC] Core 1 pause timeout (500ms)\n");
  printf("[USB MSC] Core 1 paused (WiFi/HTTP/audio halted)\n");
  // Keep STM32 traffic to one keyboard read per 500 ms while USB is active
  // (see the ESC loop below); the bus engine keeps running from interrupts.
  kbd_set_poll_interval_ms(500);
  // f_getfree below scans the FAT (~3 s on a 256 GB card, longer on slow
  // cards) with no watchdog kick; the heartbeat lets paused Core 1 relay
  // the watchdog through it.
  core0_heartbeat();

  // 2. Ensure FS Info has valid free cluster count, then unmount FatFS.
  //    Without this, the host scans the entire FAT (~16MB for a 256GB card)
  //    over Full-Speed USB to compute free space — takes ~23 seconds.
  //    f_getfree() computes it over SPI at 40MHz (~3s) and marks fsi_flag
  //    so f_unmount() writes the FS Info sector.  Host then skips the scan.
  printf("[USB MSC] Updating FS Info and unmounting FatFS...\n");
  {
    FATFS *fs;
    DWORD free_clust;
    f_getfree("", &free_clust, &fs);
  }
  f_unmount("");

  // Cache sector count before entering MSC mode — avoids SPI CMD9 on every
  // host capacity query.  Must be done after f_unmount but before tud_disconnect.
  {
    LBA_t count = 0;
    DRESULT res = disk_ioctl(0, GET_SECTOR_COUNT, &count);
    s_cached_sector_count = (res == RES_OK) ? (uint32_t)count : 0;
    printf("[USB MSC] Cached sector count: %lu\n", (unsigned long)s_cached_sector_count);
  }

  s_msc_active = true;
  s_msc_ejected = false;  // Reset eject flag on entry
  s_media_changed = true;  // Signal UNIT ATTENTION on next TUR

  // Force a USB re-enumeration so the host re-probes the medium.  The MSC
  // interface enumerates at boot with no media (TUR: NOT READY), and Linux
  // then stops polling ("[sda] Media removed, stopped polling") — without
  // this, the UNIT ATTENTION armed above is never delivered and the host
  // needs a manual rescan or replug to see the disk.  Cost: CDC serial
  // drops and /dev/ttyACM* may renumber; hosts must reopen the port.
  printf("[USB MSC] Re-enumerating USB so the host re-probes media...\n");
  stdio_flush();
  tud_disconnect();
  sleep_ms(300);
  tud_connect();

  // 2. Draw the splash screen
  ui_draw_splash("USB Mode", "Hold escape to exit");

  // 3. Poll loop — USB events are processed exclusively by the SDK's
  //    USBCTRL_IRQ background task (PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
  //    installs low_priority_worker_irq which calls tud_task() under
  //    stdio_usb_mutex).  We must NOT call tud_task() here: the IRQ and a
  //    direct call share no mutual exclusion around the processing callbacks,
  //    so both could call disk_read() simultaneously, corrupting SPI0 state
  //    and hanging the device (which manifests as the keyboard locking up).
  // CRITICAL: the keyboard bus engine is limited to one STM32 read per
  // 500ms during USB MSC (kbd_set_poll_interval_ms above). uf2loader-main
  // reference shows that frequent I2C access during active USB causes
  // STM32 lockup due to electrical interference/noise. The "Hold escape"
  // message cues users to keep the key pressed longer.
  // See: reference/uf2loader-main/ui/text_directory_ui.c (no I2C during USB)
  printf("[USB MSC] Waiting for host or ESC key (hold to exit)...\n");

  uint32_t last_kbd_poll_ms = 0;
  uint32_t loop_start_ms = to_ms_since_boot(get_absolute_time());
  const uint32_t KBD_POLL_INTERVAL_MS = 50;  // kbd_poll() only drains what the engine read: cheap
  const uint32_t HOST_TIMEOUT_MS = 10000;     // 10s timeout (accounts for re-enumeration)

  while (true) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Check ESC every 50 ms. kbd_poll() does not touch the bus; the engine's
    // 500 ms idle interval (above) is what limits STM32 traffic here.
    if (now - last_kbd_poll_ms >= KBD_POLL_INTERVAL_MS) {
      kbd_poll();
      last_kbd_poll_ms = now;
      if (kbd_get_buttons_pressed() & BTN_ESC) {
        printf("[USB MSC] ESC key pressed, exiting\n");
        break;
      }
    }

    // Check for ESC via CDC serial (for automated workflows)
    int cdc_ch = getchar_timeout_us(0);
    if (cdc_ch != PICO_ERROR_TIMEOUT) {
      if (cdc_ch == 'x' || cdc_ch == 'X' || cdc_ch == 0x03) {
        printf("[USB MSC] ESC via CDC, exiting\n");
        break;
      }
    }

    // Exit if host has ejected the device
    if (s_msc_ejected) {
      printf("[USB MSC] Device ejected by host, exiting\n");
      break;
    }

    // Exit if no host has mounted the device within HOST_TIMEOUT_MS.
    // Use wall time so the timeout is accurate regardless of loop body duration.
    if (!tud_mounted() && (now - loop_start_ms) > HOST_TIMEOUT_MS) {
      printf("[USB MSC] No host connected for >%ums, exiting\n", HOST_TIMEOUT_MS);
      break;
    }

    sleep_us(100); // 100µs base interval — lets USB IRQs fire between iterations
    watchdog_update(); // keep watchdog alive while in USB mode
  }

  // 4. Deactivate MSC and remount
  //    Do NOT call tud_disconnect() — that would kill CDC serial too.
  //    Just set s_msc_active=false so callbacks return "not ready" again.
  printf("[USB MSC] Exiting USB Mass Storage mode\n");
  s_msc_active = false;

  // Disable USB IRQ to prevent MSC callbacks from touching SPI during card reinit.
  // CDC serial won't work briefly but printf output is buffered.
  irq_set_enabled(USBCTRL_IRQ, false);

  // Flush SD card's internal write buffer before reinitializing.
  // Without this, the last host writes may not be committed when CMD0
  // (software reset) fires inside disk_initialize().
  disk_ioctl(0, CTRL_SYNC, NULL);
  sleep_ms(50);

  // Remount FatFS
  printf("[USB MSC] Remounting FatFS...\n");
  if (!sdcard_remount()) {
    printf("[USB MSC] WARNING: FatFS remount failed, retrying...\n");
    sleep_ms(200);
    if (!sdcard_remount()) {
      printf("[USB MSC] ERROR: FatFS remount failed after retry\n");
    }
  }

  irq_set_enabled(USBCTRL_IRQ, true);

  // Reconnect WiFi if we were connected before
  if (was_connected) {
    const char *ssid = config_get("wifi_ssid");
    const char *pass = config_get("wifi_pass");
    if (ssid && ssid[0]) {
      printf("[USB MSC] Reconnecting WiFi...\n");
      wifi_connect(ssid, pass ? pass : "");
    }
  }

  // Recover I2C bus - STM32 may need re-initialization after USB activity
  printf("[USB MSC] Recovering I2C bus...\n");
  kbd_set_poll_interval_ms(0);  // back to the normal cadence
  kbd_recover_i2c_bus();
  kbd_apply_clock();
  kbd_clear_state();

  printf("[USB MSC] Done\n");

  // Resume Core 1 background tasks
  g_core1_pause = false;
}

// --------------------------------------------------------------------
// USB MSC callbacks
// --------------------------------------------------------------------

// Invoked when device is mounted by the host
// No printf — runs in USBCTRL_IRQ; CDC serial deadlocks here.
void tud_mount_cb(void) { }

// Invoked when device is unmounted by the host
void tud_umount_cb(void) { }

// Invoked to determine max LUN
uint8_t tud_msc_get_maxlun_cb(void) { return 0; }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t p_vendor_id[8],
                        uint8_t p_product_id[16], uint8_t p_product_rev[4]) {
  (void)lun;
  static const char vendor[8] = "PICO";
  static const char product[16] = "PicoDeck_MSC";
  static const char revision[4] = "1.0 ";

  memcpy(p_vendor_id, vendor, sizeof(vendor));
  memcpy(p_product_id, product, sizeof(product));
  memcpy(p_product_rev, revision, sizeof(revision));
  // No printf here — runs in USB IRQ context, CDC output stalls MSC transfers
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
  (void)lun;
  // Use cached sector count — no SPI, no mutex, no printf in USB IRQ context
  if (s_msc_active && s_cached_sector_count > 0) {
    *block_size = msc_block_size;
    *block_count = s_cached_sector_count;
  } else {
    *block_size = 0;
    *block_count = 0;
  }
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
  (void)lun;
  (void)power_condition;
  (void)start;
  if (load_eject) {
    // No printf — runs in USBCTRL_IRQ; CDC serial deadlocks here.
    s_msc_ejected = true;
  }
  return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
  (void)lun;
  (void)offset;

  if (!s_msc_active)
    return -1;

  // Defense-in-depth: Core 1 is paused during MSC mode so the mutex is
  // uncontended, but acquire it anyway to guard against future changes.
  recursive_mutex_enter_blocking(&g_sdcard_mutex);
  DRESULT res = disk_read(0, (BYTE *)buffer, lba, bufsize / msc_block_size);
  recursive_mutex_exit(&g_sdcard_mutex);
  if (res != RES_OK)
    return -1;

  return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return s_msc_active;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
  (void)lun;
  (void)offset;

  if (!s_msc_active)
    return -1;

  // Defense-in-depth: Core 1 is paused during MSC mode so the mutex is
  // uncontended, but acquire it anyway to guard against future changes.
  recursive_mutex_enter_blocking(&g_sdcard_mutex);
  DRESULT res = disk_write(0, (const BYTE *)buffer, lba, bufsize / msc_block_size);
  recursive_mutex_exit(&g_sdcard_mutex);
  if (res != RES_OK)
    return -1;

  return (int32_t)bufsize;
}

void tud_msc_write10_flush_cb(uint8_t lun) {
  (void)lun;
  // No mutex — Core 1 is paused during MSC mode
  disk_ioctl(0, CTRL_SYNC, NULL);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  // Only check s_msc_active — sdcard_is_mounted() tracks FatFS mount
  // state, which is intentionally false during MSC mode.
  // Note: No mutex needed here as we don't access SPI0, just check a flag.
  if (!s_msc_active) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
  }
  // Signal media change on the first TUR after activation so the host
  // re-reads capacity/partition table (belt-and-suspenders with re-enum).
  if (s_media_changed) {
    s_media_changed = false;
    tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
  }
  return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {
  (void)buffer;
  (void)bufsize;

  switch (scsi_cmd[0]) {
  case 0x35: /* SYNCHRONIZE CACHE (10) — host sends before eject to flush
              * its write-back cache.  Returning ILLEGAL REQUEST here caused
              * the host to skip the final sync → unflushed FAT/bitmap sectors
              * → filesystem corruption (clusters marked as free). */
    disk_ioctl(0, CTRL_SYNC, NULL);
    return 0;

  default:
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
  }
}
