#pragma once

// =============================================================================
// Bluetooth gamepads (issue #26): a Classic HID host on the CYW43, through
// BTstack, feeding PAD_SOURCE_BT (pad_source.h).
//
// Off by default: the "bt_enabled" system config key ("1"), set from the
// system menu's Settings -> Bluetooth page. Without it the BT firmware is
// never loaded and BTstack never initialised: nothing at boot, nothing at
// runtime. The firmware is built with it (PICODECK_BT, WiFi boards); other
// boards and the simulator build bt_pad_stub.c / simulator/sim_bt.c behind
// the same API.
//
// Threads. BTstack runs on the CYW43 driver's async context
// (threadsafe_background, set up by cyw43_arch_init on Core 0 in
// wifi_init): its work, and every callback below, runs in that context's
// low-priority interrupt on Core 0 or when Core 0 releases the context's
// lock, never on Core 1. The calls below are for Core 0's thread (the menu,
// the launcher, dev commands); they take the lock (async_context_execute_sync)
// for each BTstack call. HID reports go straight from the callback to
// pad_source_publish (any context).
//
// One pad at a time. Pads bond: the link key and the pad's name are kept in
// /system/bluetooth.dat (bt_pad_store.h), written by bt_pad_service() from
// the menu and the launcher, never from a callback. A bonded pad reconnects
// on its own (it pages the PicoDeck when woken: the host stays connectable
// while on); an unknown device that pages is refused.
// =============================================================================

#include "bt_pad_store.h"

#include <stdbool.h>
#include <stdint.h>

#define BT_PAD_FOUND_MAX 8

typedef enum {
  BT_PAD_OFF = 0,
  BT_PAD_STARTING, // the BT firmware and the HCI init sequence
  BT_PAD_ON,
  BT_PAD_FAILED,   // the controller did not come up (see note)
} bt_pad_power_t;

typedef enum {
  BT_PAD_LINK_NONE = 0,
  BT_PAD_LINK_CONNECTING, // paging the pad, pairing, opening HID
  BT_PAD_LINK_CONNECTED,  // HID open (reports flow once the descriptor is in)
} bt_pad_link_t;

typedef struct {
  uint8_t addr[6];
  char name[BT_PAD_NAME_MAX]; // "" until the name is known
  uint32_t cod;               // class of device (0 when unknown)
} bt_pad_device_t;

// A Core 0 copy of the state, for the menu and the `bt` dev command (~650 B:
// never on the 4 KB main stack).
typedef struct {
  bool available; // built with Bluetooth, and the CYW43 is up
  bool enabled;   // the setting
  bt_pad_power_t power;
  bool scanning;
  uint8_t n_found;
  bt_pad_device_t found[BT_PAD_FOUND_MAX]; // gamepads first
  uint8_t n_paired;
  bt_pad_record_t paired[BT_PAD_PAIRED_MAX]; // newest first
  bt_pad_link_t link;
  bt_pad_device_t peer;  // the pad of `link`
  bool pad_ready;        // its descriptor is in: reports reach the gamepad
  const char *profile;   // its button layout ("PlayStation", "generic", ...)
  uint32_t reports;      // input reports since power on
  bool radio_in_use;     // bt_pad_radio_in_use(), in the same snapshot
  char note[48];         // the last thing that happened, for the menu
} bt_pad_status_t;

// Boot, after wifi_init and before Core 1 starts: powers the controller up
// when the setting is on (bonds are read from the SD card first).
void bt_pad_init(void);

// Turn Bluetooth on or off and save the setting. False when unavailable.
bool bt_pad_set_enabled(bool on);
bool bt_pad_enabled(void);
bool bt_pad_available(void);

void bt_pad_get_status(bt_pad_status_t *out);

// Search for pads (an inquiry of ~10 s, then their names). on=false stops it.
bool bt_pad_scan(bool on);

// Pair with, or reconnect to, the device at `addr` (a search result or a
// paired pad). `name` is kept with the bond ("" when unknown).
bool bt_pad_connect(const uint8_t addr[6], const char *name);

// Drop the connection (the pad stays paired).
void bt_pad_disconnect(void);

// Unpair: drop the link key and the record (disconnects first).
bool bt_pad_forget(const uint8_t addr[6]);

// Core 0 thread, with an SD card to write to: saves the bonds when they
// changed. The menu calls it on every pass, the launcher on its loop.
void bt_pad_service(void);

// True while Bluetooth needs the CYW43 driver running: wifi_pause_radio()
// then leaves the radio alone (a paused driver reads as powered off, and the
// next BT transfer would power-cycle the chip).
bool bt_pad_radio_in_use(void);
