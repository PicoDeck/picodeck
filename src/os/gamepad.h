#pragma once

// =============================================================================
// Gamepad runtime (picocalc.gamepad, g_api.gamepad)
//
// The keyboard decode keeps the gamepad's masks (kbd_get_pad*, keyboard.h);
// gamepad_map.h holds the key names. This is the glue the API tables use.
// =============================================================================

#include <stdint.h>

// getLabel: the name of the key in `slot` (0 primary, 1 alternate) of the
// PAD_* button `pad_button` in the installed map, NULL if unbound (or not a
// single PAD_* bit / slot).
const char *gamepad_get_label(uint32_t pad_button, int slot);
