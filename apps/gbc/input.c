#include "input.h"
#include "os.h"

// The joypad is read from the logical gamepad (api->version >= 9), so the
// player's bindings (Settings -> Controls) apply. Older firmware has no
// gamepad table: translate the keys the game has always used (arrows, F4 = A,
// F5 = B, F1 = Select, F2 = Start) into the same PAD_* mask, so the joypad
// code below is written once. The E2E pad probe is built from this file:
// after a change, rebuild its committed ELF (make -C tests/e2e/native).
static uint32_t legacy_to_pad(uint32_t keys) {
    uint32_t pad = 0;
    if (keys & BTN_UP)    pad |= PAD_UP;
    if (keys & BTN_DOWN)  pad |= PAD_DOWN;
    if (keys & BTN_LEFT)  pad |= PAD_LEFT;
    if (keys & BTN_RIGHT) pad |= PAD_RIGHT;
    if (keys & BTN_F4)    pad |= PAD_A;
    if (keys & BTN_F5)    pad |= PAD_B;
    if (keys & BTN_F1)    pad |= PAD_SELECT;
    if (keys & BTN_F2)    pad |= PAD_START;
    return pad;
}

void gbc_input_init(GBCInput *ctx) {
    ctx->buttons = 0;
    ctx->prev_buttons = 0;
}

void gbc_input_update(GBCInput *ctx, const PicoCalcAPI *api) {
    ctx->prev_buttons = ctx->buttons;
    if (api->version >= 9 && api->gamepad)
        ctx->buttons = api->gamepad->getButtons();
    else
        ctx->buttons = legacy_to_pad(api->input->getButtons());
}

void gbc_input_get_joypad(GBCInput *ctx, uint8_t *up, uint8_t *down, uint8_t *left, uint8_t *right, uint8_t *a, uint8_t *b, uint8_t *select, uint8_t *start) {
    // Game Boy joypad bits are active low.
    *up     = (ctx->buttons & PAD_UP)     ? 0 : 1;
    *down   = (ctx->buttons & PAD_DOWN)   ? 0 : 1;
    *left   = (ctx->buttons & PAD_LEFT)   ? 0 : 1;
    *right  = (ctx->buttons & PAD_RIGHT)  ? 0 : 1;
    *a      = (ctx->buttons & PAD_A)      ? 0 : 1;
    *b      = (ctx->buttons & PAD_B)      ? 0 : 1;
    *select = (ctx->buttons & PAD_SELECT) ? 0 : 1;
    *start  = (ctx->buttons & PAD_START)  ? 0 : 1;
}
