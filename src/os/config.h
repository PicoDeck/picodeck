#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// =============================================================================
// Shared Config — /system/config.json
//
// A minimal flat key/value store backed by a JSON file on the SD card.
// Supports string values only. Maximum CONFIG_MAX_ENTRIES entries.
//
// JSON format: {"key1":"value1","key2":"value2"}
//
// Well-known keys:
//   "wifi_ssid"            — WiFi network name
//   "wifi_pass"            — WiFi password
//   "brightness"           — Backlight brightness (0-255, decimal string)
//   "dim_timeout_s"        — Idle screen-dim timeout in seconds; "0" disables
//   "tz_offset"            — Timezone offset for the clock
//   "dev_mode"             — "1" enables developer mode
//   "wifi_auto_disconnect" — "0" keeps WiFi up after initial time sync
//   "battery_pct"          — "1" shows the battery percentage in the header
// =============================================================================

// Every entry is static SRAM (CONFIG_KEY_MAX + CONFIG_VAL_MAX bytes), and the
// SRAM heap is only ~3 KB: grow this with care.
#define CONFIG_MAX_ENTRIES  10
#define CONFIG_KEY_MAX      32
#define CONFIG_VAL_MAX      128

// Load /system/config.json from SD card into memory.
// Returns true if the file was read; false if missing (empty config is valid).
// Safe to call before the file exists.
bool        config_load(void);

// Write current in-memory config back to /system/config.json.
// Returns true on success.
bool        config_save(void);

// Return the value for key, or NULL if the key is not present.
const char *config_get(const char *key);

// Set or overwrite a string value.  A NULL or empty value removes the key.
// Silently does nothing if the store is full (CONFIG_MAX_ENTRIES reached).
void        config_set(const char *key, const char *value);

// Parse a persisted "brightness" value for restore. Missing/empty → 128.
// Present values are clamped to [16, 255]: the menu can set 0 live, but a
// restored 0 (or atoi garbage) would boot with the backlight off and look
// bricked, so restores never go below a dim-but-visible 16.
static inline uint8_t config_parse_brightness(const char *v) {
  if (!v || !v[0])
    return 128;
  int b = atoi(v);
  if (b < 16) b = 16;
  if (b > 255) b = 255;
  return (uint8_t)b;
}
