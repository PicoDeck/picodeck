#pragma once
#include <stdbool.h>
#include <stdint.h>

// The display back buffer and clip rect as a rasteriser target (gfx3d).
// src/drivers/display.c stores pixels byte-swapped (swap = true);
// simulator/stubs/driver_stubs.c stores host order (swap = false).
typedef struct {
  uint16_t *fb;
  int stride;
  int clip_x0, clip_y0, clip_x1, clip_y1;  // inclusive
  bool swap;
} display_raster_target_t;

void display_get_raster_target(display_raster_target_t *t);
