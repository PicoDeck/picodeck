// The boot splash (specs/2026-10-10-boot-splash-design.md): the frames of a
// CRT powering on to the "PicoDeck" logo (splash_render.c). Outside the
// boot animation the splash is its final frame, drawn once.
#pragma once

// Draws the still frame (the animation's last) with `status` and `subtext`
// under the logo (NULL or "" for none) on the calling core, and flushes.
void splash_draw_static(const char *status, const char *subtext);
