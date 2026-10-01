#pragma once

#include <stdint.h>

#include "os.h"

// buttons is a PAD_* mask (see input.c for the pre-gamepad firmware path).
typedef struct {
    uint32_t buttons;
    uint32_t prev_buttons;
} GBCInput;

void gbc_input_init(GBCInput *ctx);
void gbc_input_update(GBCInput *ctx, const PicoCalcAPI *api);
void gbc_input_get_joypad(GBCInput *ctx, uint8_t *up, uint8_t *down, uint8_t *left, uint8_t *right, uint8_t *a, uint8_t *b, uint8_t *select, uint8_t *start);
