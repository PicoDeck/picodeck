#pragma once

// =============================================================================
// The Bluetooth pads' bonding store (pure; host-tested in
// tests/unit/test_bt_pad_store.c). bt_pad.c keeps it in PSRAM and saves it to
// /system/bluetooth.dat on the SD card, not in flash. The SDK's flash TLV
// (pico_btstack_flash_bank: two sectors 12 KB below the end of the 16 MB
// chip) would not collide with the OTA image (written from offset 0, at most
// 2 MB), but each of its writes erases a sector from inside BTstack's
// callbacks with Core 1 locked out and XIP stopped; the SD card needs none
// of that, and a save happens only from the menu or the launcher.
//
// It is a small tag -> value table, the shape of BTstack's btstack_tlv_t, so
// it backs BTstack's link key DB directly ('BTL' tags, one per bonded
// device) and holds the paired-pad list beside it ('PDP' tags: address,
// name, and a sequence number that orders them newest first). The file is
// "PDBT", a version byte, the entry count, then per entry the tag (4 bytes,
// little-endian), the length and the value.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_STORE_ENTRIES 12
#define BT_STORE_VALUE_MAX 44
#define BT_STORE_FILE_MAX (6 + BT_STORE_ENTRIES * (5 + BT_STORE_VALUE_MAX))

#define BT_PAD_NAME_MAX 32 // with the NUL
#define BT_PAD_PAIRED_MAX 4

typedef struct {
  uint32_t tag;
  uint8_t len;
  uint8_t data[BT_STORE_VALUE_MAX];
} bt_store_entry_t;

typedef struct {
  bt_store_entry_t e[BT_STORE_ENTRIES];
  uint8_t n;
  bool dirty; // changed since the last save
} bt_store_t;

void bt_store_init(bt_store_t *s);

// BTstack's TLV calls. get: the value's length (0 when absent), at most
// `size` bytes copied. put: false when the value is too long or the table is
// full. Both put and del mark the store dirty when it changes.
int bt_store_get(const bt_store_t *s, uint32_t tag, uint8_t *buf,
                 uint32_t size);
bool bt_store_put(bt_store_t *s, uint32_t tag, const uint8_t *data,
                  uint32_t len);
void bt_store_del(bt_store_t *s, uint32_t tag);

// The file image. serialize: bytes written (0 when `cap` is too small).
// parse: false (and an empty store) for anything malformed.
size_t bt_store_serialize(const bt_store_t *s, uint8_t *out, size_t cap);
bool bt_store_parse(bt_store_t *s, const uint8_t *in, size_t len);

// ── Paired pads ─────────────────────────────────────────────────────────────

typedef struct {
  uint8_t addr[6];
  char name[BT_PAD_NAME_MAX];
} bt_pad_record_t;

// The paired pads, newest first; returns how many (at most `max`).
int bt_store_pads(const bt_store_t *s, bt_pad_record_t *out, int max);

// The record for `addr`, or false.
bool bt_store_find_pad(const bt_store_t *s, const uint8_t addr[6],
                       bt_pad_record_t *out);

// Add a pad, or refresh its name and make it the newest. With the list full
// the oldest is dropped: `evicted` gets its address and the return is true
// (the caller drops its link key too). False otherwise.
bool bt_store_add_pad(bt_store_t *s, const bt_pad_record_t *r,
                      uint8_t evicted[6]);

// Remove a pad's record; false when it had none.
bool bt_store_remove_pad(bt_store_t *s, const uint8_t addr[6]);

// "AA:BB:CC:DD:EE:FF" (18 bytes with the NUL) and back (false unless six
// hex pairs joined by ':').
void bt_pad_addr_str(const uint8_t addr[6], char out[18]);
bool bt_pad_addr_parse(const char *s, uint8_t addr[6]);
