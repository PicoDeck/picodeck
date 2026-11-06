#pragma once

#include <stdbool.h>

#include "../os/os.h"  // wifi_status_t

// The connection the launcher takes down around an app's own clock and owes
// back when the app exits (wifi_leave_for_clock / wifi_rejoin_after_clock,
// wifi.h). Pure, so the firmware's wifi.c and the simulator's sim_wifi.c decide
// alike and tests/unit/test_wifi_rejoin.c runs it. Core 0 only.

typedef struct {
  bool owed;       // taken down, and nobody has asked for anything since
  bool auto_sync;  // the boot auto-connect, still short of its time sync
} wifi_rejoin_t;

// Up, or on its way up: what a disconnect stops.
static inline bool wifi_rejoin_status_up(wifi_status_t st) {
  return st == WIFI_STATUS_CONNECTED || st == WIFI_STATUS_CONNECTING ||
         st == WIFI_STATUS_ONLINE;
}

// The launcher is about to disconnect. DISCONNECTED (the user's choice, or the
// boot's own disconnect after its time sync) and FAILED are not touched, so
// nothing is owed. Neither is a link still up with no SSID: someone has already
// asked for the disconnect (wifi_disconnect clears the SSID at once, the link
// goes down when Core 1 gets to it).
//
// A boot auto-connect disconnects after its first time sync. One short of it
// carries on to that sync and that disconnect. Past it (an "http" app was
// running then, or wifi_auto_disconnect is off) it is an ordinary connection:
// with the boot's flag, the rejoin's own time sync would drop it.
static inline void wifi_rejoin_note_leave(wifi_rejoin_t *r, wifi_status_t st,
                                          bool has_ssid, bool auto_connected,
                                          bool clock_set) {
  r->owed = wifi_rejoin_status_up(st) && has_ssid;
  r->auto_sync = r->owed && auto_connected && !clock_set;
}

// Anyone's connect or disconnect after that (the app's, the system menu's)
// decides the state from then on, as it would in any other app.
static inline void wifi_rejoin_cancel(wifi_rejoin_t *r) {
  r->owed = false;
  r->auto_sync = false;
}

// The app has exited: is a connection owed? True once. r->auto_sync still says
// how to start it.
static inline bool wifi_rejoin_take(wifi_rejoin_t *r) {
  bool owed = r->owed;
  r->owed = false;
  return owed;
}
