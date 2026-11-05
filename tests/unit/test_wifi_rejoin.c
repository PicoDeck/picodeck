// Host unit tests for src/drivers/wifi_rejoin.h: what the launcher owes back
// after the WiFi disconnect around an app's own clock (issue #71).
#include "check.h"
#include "wifi_rejoin.h"

static const wifi_status_t k_up[] = {WIFI_STATUS_CONNECTING, WIFI_STATUS_CONNECTED,
                                     WIFI_STATUS_ONLINE};

static wifi_rejoin_t leave(wifi_status_t st) {
  wifi_rejoin_t r = {0};
  wifi_rejoin_note_leave(&r, st, true, false, true);
  return r;
}

static void test_a_connection_up_or_on_its_way_is_owed(void) {
  for (unsigned i = 0; i < sizeof(k_up) / sizeof(k_up[0]); i++) {
    wifi_rejoin_t r = leave(k_up[i]);
    CHECK(r.owed);
    CHECK(wifi_rejoin_status_up(k_up[i]));
    CHECK(wifi_rejoin_take(&r));
  }
}

// The user's disconnect, the boot's own after its time sync, and a network
// that never answered: the launcher took nothing down.
static void test_nothing_up_is_nothing_owed(void) {
  wifi_status_t down[] = {WIFI_STATUS_DISCONNECTED, WIFI_STATUS_FAILED};
  for (unsigned i = 0; i < 2; i++) {
    wifi_rejoin_t r = leave(down[i]);
    CHECK(!wifi_rejoin_status_up(down[i]));
    CHECK(!r.owed);
    CHECK(!wifi_rejoin_take(&r));
  }
}

// Disconnect asked for a moment ago: Core 1 has not reached it, so the link
// still reads up, but the SSID has gone. It stays off.
static void test_a_disconnect_on_its_way_is_not_owed(void) {
  for (unsigned i = 0; i < sizeof(k_up) / sizeof(k_up[0]); i++) {
    wifi_rejoin_t r = {0};
    wifi_rejoin_note_leave(&r, k_up[i], false, false, true);
    CHECK(!r.owed);
    CHECK(!r.auto_sync);
  }
}

// The app's connect or disconnect, or the system menu's: whichever came last
// stands, so nothing is rejoined behind it.
static void test_any_request_since_cancels_it(void) {
  wifi_rejoin_t r = {0};
  wifi_rejoin_note_leave(&r, WIFI_STATUS_ONLINE, true, true, false);
  CHECK(r.owed && r.auto_sync);
  wifi_rejoin_cancel(&r);
  CHECK(!r.owed && !r.auto_sync);
  CHECK(!wifi_rejoin_take(&r));
}

static void test_it_is_owed_once(void) {
  wifi_rejoin_t r = leave(WIFI_STATUS_ONLINE);
  CHECK(wifi_rejoin_take(&r));
  CHECK(!wifi_rejoin_take(&r));
}

static void test_a_later_leave_starts_afresh(void) {
  wifi_rejoin_t r = leave(WIFI_STATUS_ONLINE);
  wifi_rejoin_note_leave(&r, WIFI_STATUS_DISCONNECTED, true, false, true);
  CHECK(!r.owed);
  wifi_rejoin_note_leave(&r, WIFI_STATUS_CONNECTING, true, false, true);
  CHECK(r.owed);
}

// The boot auto-connect's disconnect after its first time sync: kept only for
// a boot connection still short of that sync.
static void test_only_a_boot_connection_short_of_its_sync_stays_auto(void) {
  for (unsigned i = 0; i < sizeof(k_up) / sizeof(k_up[0]); i++) {
    wifi_rejoin_t r = {0};
    wifi_rejoin_note_leave(&r, k_up[i], true, true, false);
    CHECK(r.owed && r.auto_sync);
    // Synced already (an "http" app was running, or wifi_auto_disconnect is
    // off): the rejoin's own sync must not drop it.
    wifi_rejoin_note_leave(&r, k_up[i], true, true, true);
    CHECK(r.owed && !r.auto_sync);
    // Joined by hand or by an app: never.
    wifi_rejoin_note_leave(&r, k_up[i], true, false, false);
    CHECK(r.owed && !r.auto_sync);
    wifi_rejoin_note_leave(&r, k_up[i], true, false, true);
    CHECK(r.owed && !r.auto_sync);
  }
  // Nothing owed, nothing to keep.
  wifi_rejoin_t r = {0};
  wifi_rejoin_note_leave(&r, WIFI_STATUS_DISCONNECTED, true, true, false);
  CHECK(!r.owed && !r.auto_sync);
}

int main(void) {
  test_a_connection_up_or_on_its_way_is_owed();
  test_nothing_up_is_nothing_owed();
  test_a_disconnect_on_its_way_is_not_owed();
  test_any_request_since_cancels_it();
  test_it_is_owed_once();
  test_a_later_leave_starts_afresh();
  test_only_a_boot_connection_short_of_its_sync_stays_auto();
  return check_report("test_wifi_rejoin");
}
