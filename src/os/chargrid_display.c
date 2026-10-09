// chargrid_draw: the chargrid renderer on the display back buffer. The
// firmware and the simulator each answer display_get_raster_target with
// their own buffer, clip rect and byte order.
#include "chargrid.h"

#include "../drivers/display_raster.h"

int chargrid_draw(chargrid_t *g, int px, int py, int sx, int sy, int w, int h) {
  display_raster_target_t rt;
  display_get_raster_target(&rt);
  const chargrid_target_t t = {rt.fb, rt.stride, rt.clip_x0, rt.clip_y0, rt.clip_x1, rt.clip_y1,
                               rt.swap};
  return chargrid_render(g, &t, px, py, sx, sy, w, h);
}
