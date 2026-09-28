// libFuzzer target for src/drivers/qoa.c: header probing over arbitrary
// bytes, then a decode of the first frame when the input holds it (the
// geometry checks in qoa_parse bound the frame, so the decode is in-range;
// ASan catches any overread).  A parsed header must be self-consistent.
#include "qoa.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  qoa_info_t info;
  if (qoa_parse(data, size, &info) != QOA_OK)
    return 0;
  if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0 ||
      info.samples == 0 || info.frame_samples == 0 ||
      info.frame_samples > 5120 || info.frame_size > QOA_MAX_FRAME_BYTES ||
      info.first_frame_offset != 8)
    abort();

  size_t off = info.first_frame_offset;
  if (size < off + info.frame_size)
    return 0;
  int16_t *out = malloc(QOA_MAX_FRAME_PCM_BYTES);
  uint32_t n = qoa_frame_decode(&info, data + off, size - off, out);
  if (n > info.frame_samples)
    abort();
  free(out);
  return 0;
}
