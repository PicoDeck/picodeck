#pragma once
// The 256 colours shared by the terminal's ANSI codes (terminal_parser.c) and
// every new chargrid's palette (chargrid.c), so a colour number means the same
// colour in both. docs/API-Terminal.md and docs/API-Chargrid.md list 0-15.
#include <stdint.h>

// RGB565 in host order, in flash (512 B). Readers byte-swap at draw time, as
// for any colour: terminal_render.c and chargrid_render.c.
extern const uint16_t color256_palette[256];
