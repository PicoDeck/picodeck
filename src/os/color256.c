// The 256-colour table (color256.h). tests/unit/test_color256.c pins it.
#include "color256.h"

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define GREY(v) RGB565(v, v, v)
// One row of the cube, then one red level's six rows.
#define CUBE_ROW(r, g)                                                                \
  RGB565(r, g, 0), RGB565(r, g, 95), RGB565(r, g, 135), RGB565(r, g, 175),            \
      RGB565(r, g, 215), RGB565(r, g, 255)
#define CUBE_RED(r)                                                                   \
  CUBE_ROW(r, 0), CUBE_ROW(r, 95), CUBE_ROW(r, 135), CUBE_ROW(r, 175), CUBE_ROW(r, 215), \
      CUBE_ROW(r, 255)

const uint16_t color256_palette[256] = {
    // 0-15: the ANSI colours (30-37/40-47, then 90-97/100-107).
    RGB565(0, 0, 0),        // 0 black
    RGB565(128, 0, 0),      // 1 red
    RGB565(0, 128, 0),      // 2 green
    RGB565(128, 128, 0),    // 3 yellow
    RGB565(0, 0, 128),      // 4 blue
    RGB565(128, 0, 128),    // 5 magenta
    RGB565(0, 128, 128),    // 6 cyan
    RGB565(192, 192, 192),  // 7 white
    RGB565(128, 128, 128),  // 8 grey
    RGB565(255, 0, 0),      // 9 bright red
    RGB565(0, 255, 0),      // 10 bright green
    RGB565(255, 255, 0),    // 11 bright yellow
    RGB565(0, 0, 255),      // 12 bright blue
    RGB565(255, 0, 255),    // 13 bright magenta
    RGB565(0, 255, 255),    // 14 bright cyan
    RGB565(255, 255, 255),  // 15 bright white
    // 16-231: the 6x6x6 cube, 16 + 36r + 6g + b.
    CUBE_RED(0), CUBE_RED(95), CUBE_RED(135), CUBE_RED(175), CUBE_RED(215), CUBE_RED(255),
    // 232-255: the grey ramp.
    GREY(8), GREY(18), GREY(28), GREY(38), GREY(48), GREY(58), GREY(68), GREY(78),
    GREY(88), GREY(98), GREY(108), GREY(118), GREY(128), GREY(138), GREY(148), GREY(158),
    GREY(168), GREY(178), GREY(188), GREY(198), GREY(208), GREY(218), GREY(228), GREY(238),
};
