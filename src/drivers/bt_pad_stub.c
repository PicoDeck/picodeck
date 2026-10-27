// Bluetooth gamepads on a build without them (no CYW43, or PICODECK_BT
// off): the calls of bt_pad.h, all answering "not available".
#include "bt_pad.h"

#include <string.h>

void bt_pad_init(void) {}
bool bt_pad_set_enabled(bool on) {
  (void)on;
  return false;
}
bool bt_pad_enabled(void) { return false; }
bool bt_pad_available(void) { return false; }
void bt_pad_get_status(bt_pad_status_t *out) { memset(out, 0, sizeof(*out)); }
bool bt_pad_scan(bool on) {
  (void)on;
  return false;
}
bool bt_pad_connect(const uint8_t addr[6], const char *name) {
  (void)addr;
  (void)name;
  return false;
}
void bt_pad_disconnect(void) {}
bool bt_pad_forget(const uint8_t addr[6]) {
  (void)addr;
  return false;
}
void bt_pad_service(void) {}
bool bt_pad_radio_in_use(void) { return false; }
