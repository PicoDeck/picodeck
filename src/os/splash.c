// The boot splash (splash.h).
#include "splash.h"

#include "../drivers/display.h"
#include "../drivers/display_raster.h"
#include "splash_render.h"

void splash_draw_static(const char *status, const char *subtext) {
  display_raster_target_t r;
  display_get_raster_target(&r);
  splash_render(r.fb, r.stride, r.swap, SPLASH_T_END, status, subtext);
  display_flush();
}
