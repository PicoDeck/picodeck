// Gamepad -> C64 joystick adapter, kept free of the chip headers so the E2E
// pad probe (tests/e2e/native/pad_probe.c) can build the same code.
#pragma once

#include <stdint.h>

#include "os.h"

// Held gamepad buttons as a PAD_* mask. Firmware that has the gamepad
// (api->version >= 9) gives the player's bindings (Settings -> Controls);
// older firmware has no gamepad table, so the keys the game has always used
// (arrows, F4 = fire) are translated into the same mask.
static inline uint32_t c64_pad_held(const PicoCalcAPI *api) {
    if (api->version >= 9 && api->gamepad)
        return api->gamepad->getButtons();
    uint32_t keys = api->input->getButtons();
    uint32_t pad = 0;
    if (keys & BTN_UP)    pad |= PAD_UP;
    if (keys & BTN_DOWN)  pad |= PAD_DOWN;
    if (keys & BTN_LEFT)  pad |= PAD_LEFT;
    if (keys & BTN_RIGHT) pad |= PAD_RIGHT;
    if (keys & BTN_F4)    pad |= PAD_A;
    return pad;
}
