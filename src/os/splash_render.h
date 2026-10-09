// The boot splash's frames (specs/2026-10-10-boot-splash-design.md): a CRT
// powering on to the "PicoDeck" logo. A pure function of time, with no
// state, allocation, cores or timers, so the host tests, the simulator and
// the device draw the same pixels.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SPLASH_T_SETTLED 1.9f  // the picture is stable from here
#define SPLASH_T_TEXT    2.2f  // the status text appears
#define SPLASH_T_END     2.8f  // the glint is over: every later frame is this one
#define SPLASH_END_MS    2800  // SPLASH_T_END in milliseconds
#define SPLASH_STATUS_Y  196
#define SPLASH_SUBTEXT_Y 208

// Draws the frame at t seconds (NaN and t < 0 draw t = 0; t > SPLASH_T_END
// draws SPLASH_T_END) into a 320x320 RGB565 buffer of row pitch `stride`
// pixels, byte-swapped when `swap` (the device's panel order). From
// SPLASH_T_TEXT on, `status` and `subtext` (NULL or "" for none) are drawn
// centred under the logo in the 6x8 font, clipped to the screen.
void splash_render(uint16_t *fb, int stride, bool swap, float t,
                   const char *status, const char *subtext);
