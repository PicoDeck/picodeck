// libFuzzer target for the chargrid text paths (src/os/chargrid_text.c):
// arbitrary bytes through print, print_wrapped and stamp on a small grid, so
// ASan catches a UTF-8 overread or a write outside a plane, and every decode
// step must consume 1..remaining bytes.
#include "chargrid.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static chargrid_t *g;
  if (!g) g = chargrid_create(13, 7, 6, 8);
  const char *s = (const char *)data;
  const int x = size ? (int)(data[0] % 20) - 5 : 0;
  const int y = size > 1 ? (int)(data[1] % 12) - 3 : 0;
  (void)chargrid_print(g, x, y, s, size, 3, CG_KEEP);
  (void)chargrid_print_wrapped(g, x, y, size > 2 ? data[2] % 16 : 1, 0, s, size, CG_KEEP, 4);
  chargrid_stamp(g, x, y, s, size, 1, 2, ' ');
  for (size_t i = 0; i < size;) {
    uint8_t c;
    const int k = chargrid_utf8_code(s + i, size - i, &c);
    if (k < 1 || (size_t)k > size - i) abort();
    i += (size_t)k;
  }
  return 0;
}
