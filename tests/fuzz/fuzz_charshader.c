// libFuzzer target for the chargrid shader (src/os/charshader_*.c): arbitrary
// source through the compiler, and anything that compiles runs over a small
// grid with a smaller layer, a missing one and the grid itself as layers,
// extreme uniforms and an array. ASan and UBSan (float-cast-overflow
// included) catch a lexer overread, a bad register or scalar index, or an
// out-of-range float-to-index conversion; the error message must always be
// terminated. The grids are new for each input, so a crash reproduces from
// its input alone.
#include "charshader_internal.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char err[96];
  memset(err, 'x', sizeof err);
  charshader_t *s = charshader_compile((const char *)data, size, err, sizeof err);
  if (!memchr(err, '\0', sizeof err)) abort();
  if (!s) return 0;
  static const uint8_t ramp[] = {0, 1, 2, 3, 4};
  chargrid_t *g = chargrid_create(19, 5, 4, 4), *layer = chargrid_create(7, 3, 4, 4);
  if (!g || !layer) abort();
  chargrid_set_ramp(g, ramp, 5, true);
  chargrid_set_color_ramp(g, ramp, 5, false);
  static const char *const names[] = {"t", "a", "k", "edge"};
  static const float vals[] = {1e30f, -0.0f, 3.5f, -7.25f};
  for (int i = 0; i < 4; i++) {
    charshader_set_uniform(s, names[i], vals[i]);
    charshader_set_array(s, names[i], vals, 4);
  }
  chargrid_t *layers[CS_MAX_LAYERS] = {layer, NULL, g, layer};
  charshader_run(s, g, -3, -1, 40, 9, layers, CS_MAX_LAYERS);
  float out[19 * 5];
  for (int o = 0; o < CS_OUT_COUNT; o++)
    if (charshader_run_values(s, g, 0, 0, 19, 5, layers, CS_MAX_LAYERS, o, out) > 19 * 5) abort();
  charshader_free(s);
  chargrid_free(layer);
  chargrid_free(g);
  return 0;
}
