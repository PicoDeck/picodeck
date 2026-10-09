// The boot splash's frames on the Cortex-M33 against the host's (ctest
// test_splash_m33; tests/unit/CMakeLists.txt). splash_render() uses the
// chargrid shader's maths (charshader_math.h, noise.h), whose floors take
// ARMv8 instructions on the device and portable C on the host and in the
// simulator. Built for both, the M33 builds (-O2, the renderer's flag, and
// -Os) run under qemu-arm; each prints a CRC per frame, and the outputs
// must be equal. The device's byte-swapped still frame, text included, must
// also be the byte swap of the unswapped one, or the run fails on its own.
#include "splash_render.h"

#include <stdint.h>
#include <stdio.h>

#define N (320 * 320)

static uint16_t fb[N], swapped[N];

static uint32_t crc32(const uint16_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    for (int b = 0; b < 2; b++) {
      c ^= (uint32_t)((p[i] >> (8 * b)) & 0xFF);
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

int main(void) {
  static const float ts[] = {0.0f, 0.2f, 0.33f, 0.4f, 0.7f, 0.9f, 1.2f, 1.6f, 2.4f, 2.8f};
  for (unsigned i = 0; i < sizeof ts / sizeof ts[0]; i++) {
    splash_render(fb, 320, false, ts[i], "Loading...", "Please wait");
    printf("t=%.2f crc=%08lx\n", (double)ts[i], (unsigned long)crc32(fb, N));
  }
  // The still frame with its text, in the device's byte order.
  splash_render(fb, 320, false, SPLASH_T_END, "Loading...", "Please wait");
  splash_render(swapped, 320, true, SPLASH_T_END, "Loading...", "Please wait");
  printf("swapped t=%.2f crc=%08lx\n", (double)SPLASH_T_END, (unsigned long)crc32(swapped, N));
  int bad = 0;
  for (int i = 0; i < N; i++) bad += swapped[i] != (uint16_t)((fb[i] >> 8) | (fb[i] << 8));
  if (bad) {
    printf("swapped t=%.2f: %d pixels are not the byte swap of the unswapped frame\n",
           (double)SPLASH_T_END, bad);
    return 1;
  }
  return 0;
}
