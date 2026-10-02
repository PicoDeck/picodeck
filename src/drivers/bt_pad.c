// Bluetooth gamepads: BTstack's Classic HID host on the CYW43 (see bt_pad.h
// for the threads and the setting). Built only with PICODECK_BT (WiFi
// boards); bt_pad_stub.c answers the same calls elsewhere.
#include "bt_pad.h"
#include "hid_pad.h"
#include "pad_source.h"
#include "sdcard.h"
#include "../os/app_stack.h"
#include "../os/config.h"
#include "../os/sd_atomic.h"

#include "btstack.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "pico/btstack_cyw43.h"
#include "pico/btstack_hci_transport_cyw43.h"
#include "pico/btstack_run_loop_async_context.h"
#include "pico/cyw43_arch.h"
#include "umm_malloc.h"

#include <stdio.h>
#include <string.h>

#define BT_FILE "/system/bluetooth.dat"
#define BT_DESC_STORAGE 1024 // HID descriptors (DualShock 4: 364 B)
#define BT_INQUIRY_UNITS 8   // x 1.28 s

typedef struct {
  bt_pad_device_t d;
  uint8_t psrm;     // page scan repetition mode, for the name request
  uint16_t clock;   // clock offset, likewise
  bool named;       // the name is in (or the request failed)
} found_t;

// Everything BTstack points at, and our state: one umm_malloc block (PSRAM),
// made at the first enable and kept to the next boot.
typedef struct {
  btstack_packet_callback_registration_t hci_cb;
  bt_store_t store;
  bool store_loaded;
  bool initialised; // BTstack set up (once per boot)
  bt_pad_power_t power;
  bool inquiring;   // the inquiry itself
  bool naming;      // a remote name request is out
  bool scanning;    // inquiry, then names
  uint8_t n_found;
  found_t found[BT_PAD_FOUND_MAX];
  bt_pad_link_t link;
  bool pending;     // a connect waiting for the scan to stop
  uint16_t cid;
  bt_pad_device_t peer;
  bool have_layout;
  hid_pad_profile_t profile;
  hid_pad_layout_t layout;
  uint32_t reports;
  char note[48];
  uint8_t desc[BT_DESC_STORAGE];
} bt_t;

static async_context_t *s_ctx; // the CYW43 driver's (btstack_cyw43_init)
static bt_t *s_bt;
static bool s_enabled;

// ── Allocator for BTstack (HAVE_MALLOC; CMakeLists.txt renames hci.c's and
// btstack_memory.c's malloc/free to these) ───────────────────────────────────

void *bt_pad_malloc(size_t n) { return umm_malloc(n); }
void bt_pad_free(void *p) { umm_free(p); }

// ── The SDK's hooks. cyw43_arch_init (wifi_init) calls this with the driver's
// async context when CYW43_ENABLE_BLUETOOTH is set; the SDK's own version
// (pico_btstack_cyw43, not linked) would set BTstack up at once, flash TLV
// included. Nothing happens here until Bluetooth is turned on. ─────────────

bool btstack_cyw43_init(async_context_t *context) {
  s_ctx = context;
  return true;
}

void btstack_cyw43_deinit(async_context_t *context) {
  (void)context;
  s_ctx = NULL;
}

// ── TLV over the PSRAM store (BTstack's link key DB) ────────────────────────

static int tlv_get(void *ctx, uint32_t tag, uint8_t *buf, uint32_t size) {
  return bt_store_get((bt_store_t *)ctx, tag, buf, size);
}

static int tlv_store(void *ctx, uint32_t tag, const uint8_t *data,
                     uint32_t len) {
  return bt_store_put((bt_store_t *)ctx, tag, data, len) ? 0 : 1;
}

static void tlv_delete(void *ctx, uint32_t tag) {
  bt_store_del((bt_store_t *)ctx, tag);
}

static const btstack_tlv_t k_tlv = {tlv_get, tlv_store, tlv_delete};

// ── Under the context's lock ────────────────────────────────────────────────

typedef uint32_t (*locked_fn)(void *);

static uint32_t locked(locked_fn fn, void *arg) {
  return async_context_execute_sync(s_ctx, fn, arg);
}

static void note(const char *fmt, const char *arg) {
  snprintf(s_bt->note, sizeof(s_bt->note), fmt, arg ? arg : "");
  printf("[BT] %s\n", s_bt->note);
}

static int pad_count(void) {
  bt_pad_record_t r[BT_PAD_PAIRED_MAX];
  return bt_store_pads(&s_bt->store, r, BT_PAD_PAIRED_MAX);
}

// Page scan (a bonded pad can reconnect) only while there is one to wait
// for: no radio time spent on it otherwise.
static void update_connectable(void) {
  if (s_bt->power == BT_PAD_ON)
    gap_connectable_control(pad_count() > 0 && s_bt->link == BT_PAD_LINK_NONE);
}

static bool is_pad_cod(uint32_t cod) {
  uint32_t major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x0F;
  return major == 0x05 && (minor == 0x01 || minor == 0x02); // joystick, pad
}

static void add_found(const uint8_t *packet) {
  bd_addr_t addr;
  gap_event_inquiry_result_get_bd_addr(packet, addr);
  uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
  found_t *f = NULL;
  for (int i = 0; i < s_bt->n_found; i++)
    if (memcmp(s_bt->found[i].d.addr, addr, 6) == 0)
      f = &s_bt->found[i];
  if (!f) {
    bool pad = is_pad_cod(cod);
    if (s_bt->n_found < BT_PAD_FOUND_MAX) {
      f = &s_bt->found[s_bt->n_found++];
    } else if (pad) { // full: a pad takes the last non-pad's place
      for (int i = BT_PAD_FOUND_MAX - 1; i >= 0 && !f; i--)
        if (!is_pad_cod(s_bt->found[i].d.cod))
          f = &s_bt->found[i];
    }
    if (!f)
      return;
    memset(f, 0, sizeof(*f));
    memcpy(f->d.addr, addr, 6);
  }
  f->d.cod = cod;
  f->psrm = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
  f->clock = gap_event_inquiry_result_get_clock_offset(packet);
  if (gap_event_inquiry_result_get_name_available(packet)) {
    int n = gap_event_inquiry_result_get_name_len(packet);
    if (n > BT_PAD_NAME_MAX - 1)
      n = BT_PAD_NAME_MAX - 1;
    memcpy(f->d.name, gap_event_inquiry_result_get_name(packet), (size_t)n);
    f->d.name[n] = '\0';
    f->named = true;
  }
  char a[18];
  bt_pad_addr_str(addr, a);
  printf("[BT] found %s cod=%06lx %s\n", a, (unsigned long)cod,
         f->named ? f->d.name : "(no name yet)");
}

static void start_connect(void);

// After the inquiry: ask each unnamed device for its name, one at a time;
// then the scan is over (and a connect waiting for it goes).
static void next_name(void) {
  for (int i = 0; s_bt->scanning && i < s_bt->n_found; i++) {
    found_t *f = &s_bt->found[i];
    if (f->named)
      continue;
    f->named = true; // asked once, whatever the answer
    if (gap_remote_name_request(f->d.addr, f->psrm, f->clock | 0x8000) ==
        ERROR_CODE_SUCCESS) {
      s_bt->naming = true;
      return;
    }
  }
  if (s_bt->scanning) {
    s_bt->scanning = false;
    note("Search done: %s", s_bt->n_found ? "pick a pad" : "nothing found");
  }
  if (s_bt->pending)
    start_connect();
}

static void start_connect(void) {
  s_bt->pending = false;
  if (s_bt->inquiring || s_bt->naming) { // after the scan has stopped
    s_bt->pending = true;
    return;
  }
  s_bt->scanning = false;
  uint8_t st = hid_host_connect(s_bt->peer.addr, HID_PROTOCOL_MODE_REPORT,
                                &s_bt->cid);
  if (st != ERROR_CODE_SUCCESS) {
    s_bt->link = BT_PAD_LINK_NONE;
    char code[8];
    snprintf(code, sizeof(code), "0x%02x", st);
    note("Could not connect (%s)", code);
    update_connectable();
    return;
  }
  gap_connectable_control(0);
  note("Connecting to %s...", s_bt->peer.name[0] ? s_bt->peer.name : "pad");
}

static void hid_event(uint8_t *packet) {
  bd_addr_t addr;
  switch (hci_event_hid_meta_get_subevent_code(packet)) {
  case HID_SUBEVENT_INCOMING_CONNECTION: {
    uint16_t cid = hid_subevent_incoming_connection_get_hid_cid(packet);
    hid_subevent_incoming_connection_get_address(packet, addr);
    bt_pad_record_t r;
    if (s_bt->link == BT_PAD_LINK_NONE &&
        bt_store_find_pad(&s_bt->store, addr, &r)) {
      s_bt->link = BT_PAD_LINK_CONNECTING;
      s_bt->cid = cid;
      memset(&s_bt->peer, 0, sizeof(s_bt->peer));
      memcpy(s_bt->peer.addr, addr, 6);
      memcpy(s_bt->peer.name, r.name, sizeof(r.name));
      hid_host_accept_connection(cid, HID_PROTOCOL_MODE_REPORT);
      note("%s is reconnecting", r.name[0] ? r.name : "A pad");
    } else {
      char a[18];
      bt_pad_addr_str(addr, a);
      hid_host_decline_connection(cid);
      note("Refused %s (not paired)", a);
    }
    break;
  }
  case HID_SUBEVENT_CONNECTION_OPENED: {
    uint8_t st = hid_subevent_connection_opened_get_status(packet);
    if (st != ERROR_CODE_SUCCESS) {
      s_bt->link = BT_PAD_LINK_NONE;
      s_bt->cid = 0;
      char code[8];
      snprintf(code, sizeof(code), "0x%02x", st);
      note("Could not connect (%s)", code);
      update_connectable();
      break;
    }
    s_bt->cid = hid_subevent_connection_opened_get_hid_cid(packet);
    s_bt->link = BT_PAD_LINK_CONNECTED;
    s_bt->have_layout = false;
    s_bt->profile = hid_pad_profile_for_name(s_bt->peer.name);
    bt_pad_record_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.addr, s_bt->peer.addr, 6);
    memcpy(r.name, s_bt->peer.name, sizeof(r.name));
    uint8_t evicted[6];
    if (bt_store_add_pad(&s_bt->store, &r, evicted))
      gap_drop_link_key_for_bd_addr(evicted); // the oldest pad's bond goes
    // A pad whose name the search never got: ask now, over the link (its
    // button layout depends on it; the reply updates profile and record).
    if (!r.name[0])
      gap_remote_name_request(s_bt->peer.addr, 0, 0);
    note("Connected: %s", r.name[0] ? r.name : "pad");
    break;
  }
  case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
    if (hid_subevent_descriptor_available_get_status(packet) ==
            ERROR_CODE_SUCCESS &&
        hid_pad_parse(hid_descriptor_storage_get_descriptor_data(s_bt->cid),
                      hid_descriptor_storage_get_descriptor_len(s_bt->cid),
                      &s_bt->layout)) {
      s_bt->have_layout = true;
      pad_source_publish(PAD_SOURCE_BT, 0); // connected: the header's icon
      printf("[BT] %s: %u fields, %s layout\n", s_bt->peer.name,
             (unsigned)s_bt->layout.n, hid_pad_profile_label(s_bt->profile));
    } else {
      note("%s is not a gamepad", s_bt->peer.name);
    }
    break;
  case HID_SUBEVENT_REPORT: {
    if (!s_bt->have_layout)
      break;
    const uint8_t *r = hid_subevent_report_get_report(packet);
    uint16_t n = hid_subevent_report_get_report_len(packet);
    uint32_t st;
    if (n >= 2 && r[0] == 0xA1 && // DATA | Input
        hid_pad_decode(&s_bt->layout, s_bt->profile, r + 1, n - 1u, &st)) {
      pad_source_publish(PAD_SOURCE_BT, st);
      s_bt->reports++;
    }
    break;
  }
  case HID_SUBEVENT_CONNECTION_CLOSED:
    pad_source_disconnect(PAD_SOURCE_BT); // nothing stays held
    s_bt->link = BT_PAD_LINK_NONE;
    s_bt->cid = 0;
    s_bt->have_layout = false;
    note("%s disconnected", s_bt->peer.name[0] ? s_bt->peer.name : "Pad");
    update_connectable();
    break;
  default:
    break;
  }
}

static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet,
                           uint16_t size) {
  (void)channel;
  (void)size;
  if (type != HCI_EVENT_PACKET || !s_bt)
    return;
  bd_addr_t addr;
  switch (hci_event_packet_get_type(packet)) {
  case BTSTACK_EVENT_STATE:
    switch (btstack_event_state_get_state(packet)) {
    case HCI_STATE_WORKING: {
      s_bt->power = BT_PAD_ON;
      bd_addr_t local;
      gap_local_bd_addr(local);
      char a[18];
      bt_pad_addr_str(local, a);
      note("On (%s)", a);
      update_connectable();
      break;
    }
    case HCI_STATE_OFF:
      s_bt->power = BT_PAD_OFF;
      s_bt->inquiring = s_bt->naming = s_bt->scanning = s_bt->pending = false;
      if (s_bt->link != BT_PAD_LINK_NONE)
        pad_source_disconnect(PAD_SOURCE_BT);
      s_bt->link = BT_PAD_LINK_NONE;
      s_bt->have_layout = false;
      note("Off%s", "");
      break;
    default:
      break;
    }
    break;
  case GAP_EVENT_INQUIRY_RESULT:
    if (s_bt->inquiring)
      add_found(packet);
    break;
  case GAP_EVENT_INQUIRY_COMPLETE:
    s_bt->inquiring = false;
    next_name();
    break;
  case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
    hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
    if (hci_event_remote_name_request_complete_get_status(packet) == 0) {
      const char *name =
          hci_event_remote_name_request_complete_get_remote_name(packet);
      for (int i = 0; i < s_bt->n_found; i++)
        if (memcmp(s_bt->found[i].d.addr, addr, 6) == 0) {
          strncpy(s_bt->found[i].d.name, name, BT_PAD_NAME_MAX - 1);
          printf("[BT] name: %s\n", s_bt->found[i].d.name);
        }
      if (s_bt->link == BT_PAD_LINK_CONNECTED && !s_bt->peer.name[0] &&
          memcmp(s_bt->peer.addr, addr, 6) == 0) {
        strncpy(s_bt->peer.name, name, BT_PAD_NAME_MAX - 1);
        s_bt->profile = hid_pad_profile_for_name(s_bt->peer.name);
        bt_pad_record_t r;
        memset(&r, 0, sizeof(r));
        memcpy(r.addr, s_bt->peer.addr, 6);
        memcpy(r.name, s_bt->peer.name, sizeof(r.name));
        uint8_t evicted[6];
        bt_store_add_pad(&s_bt->store, &r, evicted); // already there: renamed
        note("Connected: %s", r.name);
      }
    }
    if (s_bt->naming) {
      s_bt->naming = false;
      next_name();
    }
    break;
  case HCI_EVENT_PIN_CODE_REQUEST: // legacy pairing: the common default
    hci_event_pin_code_request_get_bd_addr(packet, addr);
    gap_pin_code_response(addr, "0000");
    break;
  case HCI_EVENT_HID_META:
    hid_event(packet);
    break;
  default:
    break;
  }
}

// ── Power ───────────────────────────────────────────────────────────────────

// BTstack's static state, in QMI PSRAM (btstack/bt_psram.ld): crt0 never
// zeroed it.
extern uint8_t __bt_psram_bss_start__[], __bt_psram_bss_end__[];

static void setup_btstack(void) {
  memset(__bt_psram_bss_start__, 0,
         (size_t)(__bt_psram_bss_end__ - __bt_psram_bss_start__));
  btstack_memory_init();
  btstack_run_loop_init(btstack_run_loop_async_context_get_instance(s_ctx));
  hci_init(hci_transport_cyw43_instance(), NULL);
  btstack_tlv_set_instance(&k_tlv, &s_bt->store);
  hci_set_link_key_db(
      btstack_link_key_db_tlv_get_instance(&k_tlv, &s_bt->store));
  l2cap_init();
  hid_host_init(s_bt->desc, sizeof(s_bt->desc));
  hid_host_register_packet_handler(packet_handler);
  s_bt->hci_cb.callback = packet_handler;
  hci_add_event_handler(&s_bt->hci_cb);

  gap_set_local_name("PicoDeck 00:00:00:00:00:00"); // BTstack fills the address
  gap_set_class_of_device(0x000114); // computer: handheld PC/PDA
  gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE |
                                       LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
  hci_set_master_slave_policy(HCI_ROLE_MASTER);
  gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  gap_ssp_set_authentication_requirement(
      SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
  gap_set_bondable_mode(1);
  gap_discoverable_control(0); // pads are found by us, not the other way
  s_bt->initialised = true;
}

static uint32_t power_on_locked(void *arg) {
  (void)arg;
  if (!s_bt->initialised)
    setup_btstack();
  if (s_bt->power == BT_PAD_ON || s_bt->power == BT_PAD_STARTING)
    return 0;
  s_bt->power = BT_PAD_STARTING;
  note("Starting%s", "...");
  // Opens the transport: loads the BT firmware over the CYW43's bus (with
  // the context's lock held), then runs the HCI init sequence.
  if (hci_power_control(HCI_POWER_ON) != 0) {
    s_bt->power = BT_PAD_FAILED;
    note("The controller did not start%s", "");
  }
  return 0;
}

static uint32_t power_off_locked(void *arg) {
  (void)arg;
  if (!s_bt->initialised || s_bt->power == BT_PAD_OFF)
    return 0;
  // Halting disconnects the pad and stops every scan before the transport
  // closes (BTSTACK_EVENT_STATE -> HCI_STATE_OFF).
  hci_power_control(HCI_POWER_OFF);
  if (s_bt->link != BT_PAD_LINK_NONE)
    pad_source_disconnect(PAD_SOURCE_BT);
  return 0;
}

static void load_store_job(void *arg) {
  (void)arg;
  sd_atomic_recover(BT_FILE);
  int len = 0;
  char *buf = sdcard_read_file(BT_FILE, &len);
  if (!buf)
    return; // none yet: an empty store
  if (!bt_store_parse(&s_bt->store, (const uint8_t *)buf, (size_t)len))
    printf("[BT] %s is damaged: starting with no paired pads\n", BT_FILE);
  umm_free(buf);
}

static bool power_on(void) {
  if (!s_ctx)
    return false;
  if (!s_bt) {
    s_bt = (bt_t *)umm_malloc(sizeof(bt_t));
    if (!s_bt) {
      printf("[BT] no memory for the Bluetooth state\n");
      return false;
    }
    memset(s_bt, 0, sizeof(*s_bt));
  }
  if (!s_bt->store_loaded) {
    // Bonds come from the SD card, on an OS stack (FatFS frames do not
    // fit the main stack the launcher and boot run on).
    if (!app_stack_run_os(load_store_job, NULL))
      printf("[BT] could not read %s\n", BT_FILE);
    s_bt->store.dirty = false;
    s_bt->store_loaded = true;
  }
  locked(power_on_locked, NULL);
  return true;
}

// ── API ─────────────────────────────────────────────────────────────────────

bool bt_pad_available(void) { return s_ctx != NULL; }
bool bt_pad_enabled(void) { return s_enabled; }

void bt_pad_init(void) {
  const char *v = config_get("bt_enabled");
  s_enabled = v && strcmp(v, "1") == 0;
  if (!s_enabled)
    return;
  if (!power_on())
    printf("[BT] enabled, but the radio is not available\n");
}

bool bt_pad_set_enabled(bool on) {
  if (!s_ctx)
    return false;
  if (on != s_enabled) {
    s_enabled = on;
    config_set("bt_enabled", on ? "1" : NULL);
    config_save();
  }
  if (on)
    return power_on();
  if (s_bt)
    locked(power_off_locked, NULL);
  return true;
}

bool bt_pad_radio_in_use(void) {
  return s_bt && s_bt->power != BT_PAD_OFF;
}

typedef struct {
  bt_pad_status_t *out;
} status_job_t;

static uint32_t status_locked(void *arg) {
  bt_pad_status_t *o = (bt_pad_status_t *)arg;
  o->power = s_bt->power;
  o->scanning = s_bt->scanning;
  // Pads first, in the order found.
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < s_bt->n_found; i++)
      if (is_pad_cod(s_bt->found[i].d.cod) == (pass == 0))
        o->found[o->n_found++] = s_bt->found[i].d;
  o->n_paired =
      (uint8_t)bt_store_pads(&s_bt->store, o->paired, BT_PAD_PAIRED_MAX);
  o->link = s_bt->link;
  o->peer = s_bt->peer;
  o->pad_ready = s_bt->have_layout;
  o->profile = s_bt->link == BT_PAD_LINK_CONNECTED
                   ? hid_pad_profile_label(s_bt->profile)
                   : NULL;
  o->reports = s_bt->reports;
  memcpy(o->note, s_bt->note, sizeof(o->note));
  return 0;
}

void bt_pad_get_status(bt_pad_status_t *out) {
  memset(out, 0, sizeof(*out));
  out->available = s_ctx != NULL;
  out->enabled = s_enabled;
  if (s_bt && s_ctx)
    locked(status_locked, out);
}

static uint32_t scan_locked(void *arg) {
  bool on = arg != NULL;
  if (s_bt->power != BT_PAD_ON)
    return 0;
  if (on) {
    if (s_bt->scanning || s_bt->link != BT_PAD_LINK_NONE)
      return 0;
    s_bt->n_found = 0;
    if (gap_inquiry_start(BT_INQUIRY_UNITS) != 0) {
      note("Search failed%s", "");
      return 0;
    }
    s_bt->inquiring = s_bt->scanning = true;
    note("Searching%s", "...");
  } else if (s_bt->scanning) {
    s_bt->scanning = false; // next_name asks for no more
    if (s_bt->inquiring)
      gap_inquiry_stop();
    note("Search stopped%s", "");
  }
  return 0;
}

bool bt_pad_scan(bool on) {
  if (!s_bt || !s_ctx || s_bt->power != BT_PAD_ON)
    return false;
  locked(scan_locked, on ? (void *)1 : NULL);
  return true;
}

static uint32_t connect_locked(void *arg) {
  const bt_pad_device_t *d = (const bt_pad_device_t *)arg;
  if (s_bt->power != BT_PAD_ON || s_bt->link != BT_PAD_LINK_NONE)
    return 1;
  s_bt->peer = *d;
  s_bt->link = BT_PAD_LINK_CONNECTING;
  if (s_bt->scanning) {
    s_bt->scanning = false;
    if (s_bt->inquiring)
      gap_inquiry_stop();
  }
  start_connect();
  return 0;
}

bool bt_pad_connect(const uint8_t addr[6], const char *name) {
  if (!s_bt || !s_ctx)
    return false;
  bt_pad_device_t d;
  memset(&d, 0, sizeof(d));
  memcpy(d.addr, addr, 6);
  if (name)
    strncpy(d.name, name, BT_PAD_NAME_MAX - 1);
  return locked(connect_locked, &d) == 0;
}

static uint32_t disconnect_locked(void *arg) {
  (void)arg;
  if (s_bt->link != BT_PAD_LINK_NONE && s_bt->cid)
    hid_host_disconnect(s_bt->cid);
  else if (s_bt->link == BT_PAD_LINK_CONNECTING && s_bt->pending) {
    s_bt->pending = false; // never started
    s_bt->link = BT_PAD_LINK_NONE;
    update_connectable();
  }
  return 0;
}

void bt_pad_disconnect(void) {
  if (s_bt && s_ctx)
    locked(disconnect_locked, NULL);
}

static uint32_t forget_locked(void *arg) {
  const uint8_t *addr = (const uint8_t *)arg;
  if (s_bt->link != BT_PAD_LINK_NONE &&
      memcmp(s_bt->peer.addr, addr, 6) == 0)
    disconnect_locked(NULL);
  bool had = bt_store_remove_pad(&s_bt->store, addr);
  if (s_bt->initialised) {
    bd_addr_t a;
    memcpy(a, addr, 6);
    gap_drop_link_key_for_bd_addr(a);
  }
  update_connectable();
  return had ? 0 : 1;
}

bool bt_pad_forget(const uint8_t addr[6]) {
  if (!s_bt || !s_ctx)
    return false;
  uint8_t a[6];
  memcpy(a, addr, 6);
  return locked(forget_locked, a) == 0;
}

// ── Saving the bonds (Core 0 thread) ────────────────────────────────────────

typedef struct {
  uint8_t *buf;
  size_t len;
  bool ok;
} save_job_t;

static uint32_t snapshot_locked(void *arg) {
  save_job_t *j = (save_job_t *)arg;
  j->len = bt_store_serialize(&s_bt->store, j->buf, BT_STORE_FILE_MAX);
  s_bt->store.dirty = false; // set again by any change from here on
  return 0;
}

static void save_job(void *arg) {
  save_job_t *j = (save_job_t *)arg;
  j->ok = sd_atomic_write(BT_FILE, (const char *)j->buf, (int)j->len);
}

void bt_pad_service(void) {
  if (!s_bt || !s_ctx || !s_bt->store.dirty)
    return;
  save_job_t j = {(uint8_t *)umm_malloc(BT_STORE_FILE_MAX), 0, false};
  if (!j.buf)
    return; // next time
  locked(snapshot_locked, &j);
  if (j.len && app_stack_run_os(save_job, &j) && j.ok) {
    printf("[BT] bonds saved (%u bytes)\n", (unsigned)j.len);
  } else {
    printf("[BT] could not save %s\n", BT_FILE);
    s_bt->store.dirty = true;
  }
  umm_free(j.buf);
}
