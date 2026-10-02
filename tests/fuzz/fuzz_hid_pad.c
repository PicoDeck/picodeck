// libFuzzer target for src/drivers/hid_pad.c: a Bluetooth pad's HID report
// descriptor and an input report, both from the radio. The first two bytes
// (little-endian, mod the rest's size + 1) say how many of the remaining
// bytes are the descriptor; the rest is a report. Each is copied to a block
// of exactly its size, so ASan catches overreads; a decoded state may hold
// only PAD_* bits and Home.
#include "hid_pad.h"
#include "pad_source.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 2)
    return 0;
  size_t n = size - 2;
  size_t dlen = ((size_t)data[0] | (size_t)data[1] << 8) % (n + 1);
  size_t rlen = n - dlen;
  uint8_t *desc = malloc(dlen ? dlen : 1);
  uint8_t *rep = malloc(rlen ? rlen : 1);
  memcpy(desc, data + 2, dlen);
  memcpy(rep, data + 2 + dlen, rlen);
  hid_pad_layout_t l;
  if (hid_pad_parse(desc, dlen, &l)) {
    if (l.n == 0 || l.n > HID_PAD_MAX_FIELDS)
      abort();
    for (int p = 0; p < HID_PAD_PROFILE_COUNT; p++) {
      uint32_t st = 0;
      if (hid_pad_decode(&l, (hid_pad_profile_t)p, rep, rlen, &st) &&
          (st & ~(PAD_SOURCE_BUTTONS | PAD_SOURCE_HOME)))
        abort();
    }
  }
  free(desc);
  free(rep);
  return 0;
}
