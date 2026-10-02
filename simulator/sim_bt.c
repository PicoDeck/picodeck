// The simulator's Bluetooth gamepads: bt_pad.h over a scripted radio, so the
// system menu's Bluetooth page, the setting and the bonding file can be
// driven end to end (tests/e2e/test_bluetooth.py). There is no BTstack
// here: the devices "in range" are added with the `bt sim ...` dev command
// (dev_ops.c), the controller comes up, a search ends and a connection
// opens after short delays on the simulator's clock, and a connected pad
// publishes on PAD_SOURCE_BT like the firmware's. The bonds live in the
// same /system/bluetooth.dat (bt_pad_store.c), saved by bt_pad_service.
#include "../src/drivers/bt_pad.h"
#include "../src/drivers/hid_pad.h"
#include "../src/drivers/pad_source.h"
#include "../src/os/config.h"
#include "../src/os/sd_atomic.h"
#include "../src/os/sim_hooks.h"
#include "../src/drivers/sdcard.h"
#include "pico/time.h"
#include "umm_malloc.h"

#include <stdio.h>
#include <string.h>

#define BT_FILE "/system/bluetooth.dat"
#define SIM_RANGE_MAX 8
#define START_MS 300   // firmware load + HCI init
#define SEARCH_MS 1500 // inquiry + names
#define CONNECT_MS 500 // page, pairing, SDP, HID channels
#define STOP_MS 200    // a search stopped for a connect: the inquiry's end

static struct {
  bool init, enabled, store_loaded;
  bt_store_t store;
  bt_pad_power_t power;
  uint32_t power_at;
  bool scanning;
  uint32_t scan_at;
  uint8_t n_found;
  bt_pad_device_t found[BT_PAD_FOUND_MAX];
  bt_pad_link_t link;
  bool pairing; // as bt_pad.c: pairing a found, unpaired device (peer)
  bool pending; // as bt_pad.c: a connect waiting for the search to stop
  uint32_t pending_at;
  bt_pad_device_t peer;
  uint32_t link_at;
  bool link_ok;
  uint32_t reports;
  char note[48];
  // The world: devices in pairing mode in range.
  uint8_t n_range;
  bt_pad_device_t range[SIM_RANGE_MAX];
} s;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// The firmware prints these; here they also reach the test log
// (wait_for_log), as the Controls page's notices do.
static void note(const char *fmt, const char *arg) {
  snprintf(s.note, sizeof(s.note), fmt, arg ? arg : "");
  printf("[BT] %s\n", s.note);
  fflush(stdout);
  sim_log_os("[BT] %s", s.note);
}

static const bt_pad_device_t *in_range(const uint8_t addr[6]) {
  for (int i = 0; i < s.n_range; i++)
    if (memcmp(s.range[i].addr, addr, 6) == 0)
      return &s.range[i];
  return NULL;
}

static void load_store(void) {
  if (s.store_loaded)
    return;
  s.store_loaded = true;
  bt_store_init(&s.store);
  sd_atomic_recover(BT_FILE);
  int len = 0;
  char *buf = sdcard_read_file(BT_FILE, &len);
  if (!buf)
    return;
  if (!bt_store_parse(&s.store, (const uint8_t *)buf, (size_t)len))
    printf("[BT] %s is damaged: starting with no paired pads\n", BT_FILE);
  umm_free(buf);
}

// Moves the scripted radio on to now.
static void tick(void) {
  uint32_t t = now_ms();
  if (s.power == BT_PAD_STARTING && t - s.power_at >= START_MS) {
    s.power = BT_PAD_ON;
    note("On (%s)", "5E:1A:00:00:00:01");
  }
  if (s.power != BT_PAD_ON)
    return;
  if (s.scanning && t - s.scan_at >= SEARCH_MS) { // the names are in
    s.scanning = false;
    note("Search done: %s", s.n_found ? "pick a pad" : "nothing found");
  }
  if (s.pending && t - s.pending_at >= STOP_MS) { // the connect goes now
    s.pending = false;
    s.link_at = t;
  }
  if (s.link == BT_PAD_LINK_CONNECTING && !s.pending &&
      t - s.link_at >= CONNECT_MS) {
    bool pairing = s.pairing;
    s.pairing = false; // over, whatever happens (bt_pad.c's end_pairing)
    // An unpaired device must pair first: only the one being paired may.
    if (s.link_ok && !bt_store_find_pad(&s.store, s.peer.addr, NULL) &&
        !bt_pad_pairing_allowed(pairing, s.peer.addr, s.peer.addr))
      s.link_ok = false;
    if (!s.link_ok) {
      s.link = BT_PAD_LINK_NONE;
      note("Could not connect (%s)", "0x04");
      return;
    }
    s.link = BT_PAD_LINK_CONNECTED;
    bt_pad_record_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.addr, s.peer.addr, 6);
    memcpy(r.name, s.peer.name, sizeof(r.name));
    uint8_t evicted[6];
    bt_store_add_pad(&s.store, &r, evicted);
    pad_source_publish(PAD_SOURCE_BT, 0);
    note("Connected: %s", r.name[0] ? r.name : "pad");
  }
}

static void drop_link(const char *why) {
  if (s.link == BT_PAD_LINK_NONE)
    return;
  pad_source_disconnect(PAD_SOURCE_BT);
  s.link = BT_PAD_LINK_NONE;
  note(why, s.peer.name[0] ? s.peer.name : "Pad");
}

void bt_pad_init(void) {
  const char *v = config_get("bt_enabled");
  s.enabled = v && strcmp(v, "1") == 0;
  s.init = true;
  if (s.enabled) {
    load_store();
    s.power = BT_PAD_STARTING;
    s.power_at = now_ms();
  }
}

// Not in the browser demo (as host controllers, hal_pad.c): no Settings
// item for a radio nobody can script there.
bool bt_pad_available(void) {
#ifdef __EMSCRIPTEN__
  return false;
#else
  return true;
#endif
}
bool bt_pad_enabled(void) { return s.enabled; }

bool bt_pad_set_enabled(bool on) {
  tick();
  if (on != s.enabled) {
    s.enabled = on;
    config_set("bt_enabled", on ? "1" : NULL);
    config_save();
  }
  if (on) {
    load_store();
    if (s.power == BT_PAD_OFF || s.power == BT_PAD_FAILED) {
      s.power = BT_PAD_STARTING;
      s.power_at = now_ms();
      note("Starting%s", "...");
    }
  } else if (s.power != BT_PAD_OFF) {
    drop_link("%s disconnected");
    s.scanning = false;
    s.power = BT_PAD_OFF;
    note("Off%s", "");
  }
  return true;
}

bool bt_pad_radio_in_use(void) { return s.power != BT_PAD_OFF; }

void bt_pad_get_status(bt_pad_status_t *o) {
  tick();
  memset(o, 0, sizeof(*o));
  o->available = bt_pad_available();
  o->enabled = s.enabled;
  o->power = s.power;
  o->scanning = s.scanning;
  // Pads first, in the order found (as bt_pad.c).
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < s.n_found; i++) {
      uint32_t cod = s.found[i].cod;
      bool pad = ((cod >> 8) & 0x1F) == 0x05 &&
                 (((cod >> 2) & 0x0F) == 1 || ((cod >> 2) & 0x0F) == 2);
      if (pad == (pass == 0))
        o->found[o->n_found++] = s.found[i];
    }
  o->n_paired = (uint8_t)bt_store_pads(&s.store, o->paired, BT_PAD_PAIRED_MAX);
  o->link = s.link;
  o->peer = s.peer;
  o->pad_ready = s.link == BT_PAD_LINK_CONNECTED;
  o->profile = s.link == BT_PAD_LINK_CONNECTED
                   ? hid_pad_profile_label(hid_pad_profile_for_name(s.peer.name))
                   : NULL;
  o->reports = s.reports;
  o->radio_in_use = s.power != BT_PAD_OFF;
  memcpy(o->note, s.note, sizeof(o->note));
}

bool bt_pad_scan(bool on) {
  tick();
  if (s.power != BT_PAD_ON)
    return false;
  if (on && !s.scanning && s.link == BT_PAD_LINK_NONE) {
    s.scanning = true;
    s.scan_at = now_ms();
    // Devices in pairing mode answer the inquiry at once (the firmware lists
    // them as they come); the search then runs on for their names.
    s.n_found = 0;
    for (int i = 0; i < s.n_range && s.n_found < BT_PAD_FOUND_MAX; i++)
      s.found[s.n_found++] = s.range[i];
    note("Searching%s", "...");
  } else if (!on && s.scanning) {
    s.scanning = false;
    note("Search stopped%s", "");
  }
  return true;
}

bool bt_pad_connect(const uint8_t addr[6], const char *name) {
  tick();
  if (s.power != BT_PAD_ON || s.link != BT_PAD_LINK_NONE)
    return false;
  // As bt_pad.c: a connect asked for during a search stops it and waits
  // for the inquiry in flight to end.
  s.pending = s.scanning;
  s.pending_at = now_ms();
  s.scanning = false;
  memset(&s.peer, 0, sizeof(s.peer));
  memcpy(s.peer.addr, addr, 6);
  if (name)
    strncpy(s.peer.name, name, BT_PAD_NAME_MAX - 1);
  // A pad answers when it is in range (pairing mode) or already bonded.
  s.link_ok = in_range(addr) || bt_store_find_pad(&s.store, addr, NULL);
  // Pairing only for a device the search found that is not paired yet.
  bool found = false;
  for (int i = 0; i < s.n_found; i++)
    found |= memcmp(s.found[i].addr, addr, 6) == 0;
  s.pairing = found && !bt_store_find_pad(&s.store, addr, NULL);
  s.link = BT_PAD_LINK_CONNECTING;
  s.link_at = now_ms();
  note("Connecting to %s...", s.peer.name[0] ? s.peer.name : "pad");
  return true;
}

void bt_pad_disconnect(void) {
  tick();
  if (s.link == BT_PAD_LINK_CONNECTING && s.pending) { // never started
    s.pending = false;
    s.pairing = false;
    s.link = BT_PAD_LINK_NONE;
    note("Cancelled%s", "");
    return;
  }
  s.pairing = false;
  drop_link("%s disconnected");
}

bool bt_pad_forget(const uint8_t addr[6]) {
  tick();
  if (s.link != BT_PAD_LINK_NONE && memcmp(s.peer.addr, addr, 6) == 0)
    drop_link("%s disconnected");
  return bt_store_remove_pad(&s.store, addr);
}

void bt_pad_service(void) {
  tick();
  if (!s.store.dirty)
    return;
  uint8_t buf[BT_STORE_FILE_MAX];
  size_t len = bt_store_serialize(&s.store, buf, sizeof(buf));
  s.store.dirty = false;
  if (len && sd_atomic_write(BT_FILE, (const char *)buf, (int)len)) {
    printf("[BT] bonds saved (%u bytes)\n", (unsigned)len);
    sim_log_os("[BT] bonds saved (%u bytes)", (unsigned)len);
  } else {
    printf("[BT] could not save %s\n", BT_FILE);
    sim_log_os("[BT] could not save %s", BT_FILE);
    s.store.dirty = true;
  }
  fflush(stdout);
}

// ── `bt sim ...` (dev_ops.c, simulator only) ────────────────────────────────

bool bt_pad_sim_add(const uint8_t addr[6], uint32_t cod, const char *name) {
  bt_pad_device_t *d = NULL;
  for (int i = 0; i < s.n_range; i++)
    if (memcmp(s.range[i].addr, addr, 6) == 0)
      d = &s.range[i];
  if (!d) {
    if (s.n_range >= SIM_RANGE_MAX)
      return false;
    d = &s.range[s.n_range++];
  }
  memset(d, 0, sizeof(*d));
  memcpy(d->addr, addr, 6);
  d->cod = cod;
  strncpy(d->name, name ? name : "", BT_PAD_NAME_MAX - 1);
  return true;
}

void bt_pad_sim_clear(void) { s.n_range = 0; }

// The connected pad's state (PAD_* | PAD_SOURCE_HOME), as one HID report.
bool bt_pad_sim_report(uint32_t state) {
  tick();
  if (s.link != BT_PAD_LINK_CONNECTED)
    return false;
  pad_source_publish(PAD_SOURCE_BT, state);
  s.reports++;
  return true;
}

// The link drops (the pad switched off, out of range).
bool bt_pad_sim_drop(void) {
  tick();
  if (s.link == BT_PAD_LINK_NONE)
    return false;
  drop_link("%s disconnected");
  return true;
}

// A device pages the PicoDeck with `addr` and no link key, and asks to pair
// (a device spoofing a paired pad's address). The connection filter lets a
// paired address in; the pairing request is refused unless the user is
// pairing that very device (bt_pad_pairing_allowed). True if it bonded.
bool bt_pad_sim_spoof(const uint8_t addr[6]) {
  tick();
  char a[18];
  bt_pad_addr_str(addr, a);
  if (s.power != BT_PAD_ON ||
      !bt_pad_connection_allowed(&s.store, s.pairing, s.peer.addr, addr)) {
    note("Refused %s (not paired)", a);
    return false;
  }
  if (!bt_pad_pairing_allowed(s.pairing, s.peer.addr, addr)) {
    note("Refused pairing from %s", a);
    return false;
  }
  return true;
}

// A bonded pad pages the PicoDeck (switched on): it reconnects on its own.
bool bt_pad_sim_wake(const uint8_t addr[6]) {
  tick();
  bt_pad_record_t r;
  if (s.power != BT_PAD_ON || s.link != BT_PAD_LINK_NONE ||
      !bt_store_find_pad(&s.store, addr, &r)) {
    char a[18];
    bt_pad_addr_str(addr, a);
    note("Refused %s (not paired)", a);
    return false;
  }
  memset(&s.peer, 0, sizeof(s.peer));
  memcpy(s.peer.addr, addr, 6);
  memcpy(s.peer.name, r.name, sizeof(r.name));
  s.link_ok = true;
  s.link = BT_PAD_LINK_CONNECTING;
  s.link_at = now_ms();
  note("%s is reconnecting", r.name[0] ? r.name : "A pad");
  return true;
}
