// libFuzzer target for src/drivers/qoa.c: header probing over arbitrary
// bytes, then the streaming decode of every frame the input holds, as the
// file player runs it (256-frame chunks; each frame begins where the last
// one's size field says).  ASan catches any overread; a parsed header must
// be self-consistent and no frame may yield more than it announced.
#include "qoa.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  qoa_info_t info;
  if (qoa_parse(data, size, &info) != QOA_OK)
    return 0;
  if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0 ||
      info.sample_rate > 192000 || info.samples == 0 ||
      info.frame_samples == 0 || info.frame_samples > 5120 ||
      info.frame_size > QOA_MAX_FRAME_BYTES || info.first_frame_offset != 8)
    abort();

  int16_t out[256 * 2];
  size_t off = info.first_frame_offset;
  for (int frames = 0; frames < 64 && off < size; frames++) {
    qoa_dec_t d;
    uint32_t n = qoa_dec_begin(&d, &info, data + off, size - off);
    if (n == 0)
      break;
    uint32_t got = 0, k;
    while ((k = qoa_dec_run(&d, data + off, out, 256)) > 0) {
      if (k > 256)
        abort();
      got += k;
    }
    if (got != n)
      abort();
    uint32_t step = qoa_frame_bytes(data + off, size - off);
    if (step == 0)
      break;
    off += step;
  }
  return 0;
}
