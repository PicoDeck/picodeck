#pragma once

// =============================================================================
// Gamepad runtime (picocalc.gamepad, g_api.gamepad)
//
// The keyboard decode keeps the gamepad's masks (kbd_get_pad*, keyboard.h);
// gamepad_map.h holds the key names and the bindings files. This is the glue:
// the effective map installed at app launch, and getLabel.
// =============================================================================

#include <stdint.h>

// getLabel: the name of the key in `slot` (0 primary, 1 alternate) of the
// PAD_* button `pad_button` in the installed map, NULL if unbound (or not a
// single PAD_* bit / slot).
const char *gamepad_get_label(uint32_t pad_button, int slot);

// Build the effective map for the running app (app_identity_current(); the
// launcher's, global only, when none) from the bindings files and install it
// (kbd_set_pad_map, which drops the gamepad state). The SD work runs on an OS
// stack (app_stack_run_os: inline on an app stack, a 32 KB PSRAM stack from
// the launcher's 4 KB MSP). Called by both runners right after the app's
// identity is installed; the Settings page calls it after saving.
void gamepad_apply(void);
