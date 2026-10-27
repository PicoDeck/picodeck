// native_pad_probe — runs the real gamepad adapters of the native games and
// logs what they produce, so the E2E test can drive them through bound keys.
//
// Compiled from the games' own code: apps/gbc/input.c (the Game Boy joypad)
// and apps/c64/pad_input.h (the C64 joystick source). The games themselves
// need a ROM / the chip core and cannot be observed from a test; these are
// the smallest pieces that decide which key does what.
//
// Each time either result changes it logs one line:
//   PAD gbc=<up down left right a b select start, '1' = pressed>
//       legacy=<same, from a copy of the API with version 8 and no gamepad>
//       c64=<up down left right fire>
// A pressed Esc ends the app.
//
// Built by tests/e2e/native/Makefile into tests/e2e/fixtures/native_pad_probe/
// main.elf (committed, so the E2E job needs no ARM toolchain).
#include "app_abi.h"
#include "os.h"
#include "input.h"
#include "pad_input.h"


// active-low joypad bits -> "10000000"-style text of pressed buttons
static void joypad_text(GBCInput *in, char out[9]) {
    uint8_t v[8];
    gbc_input_get_joypad(in, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    for (int i = 0; i < 8; i++) out[i] = v[i] ? '0' : '1';
    out[8] = 0;
}

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                   const char *app_id, const char *app_name) {
    (void)app_dir; (void)app_id; (void)app_name;
    // Firmware before the gamepad: version 8, no gamepad table (the copy is
    // field-wise; a struct copy would need memcpy, which apps do not link).
    static PicoCalcAPI old;
    old.input = api->input;
    old.version = 8;

    GBCInput gbc, gbc_old;
    gbc_input_init(&gbc);
    gbc_input_init(&gbc_old);
    static char last[24];
    api->sys->log("PADREADY version=%d\n", (int)api->version);
    for (;;) {
        api->sys->poll();
        if (api->sys->shouldExit() ||
            (api->input->getButtonsPressed() & BTN_ESC))
            return;
        gbc_input_update(&gbc, api);
        gbc_input_update(&gbc_old, &old);
        char a[9], b[9], c[6];
        joypad_text(&gbc, a);
        joypad_text(&gbc_old, b);
        uint32_t pad = c64_pad_held(api);
        c[0] = (pad & PAD_UP) ? '1' : '0';
        c[1] = (pad & PAD_DOWN) ? '1' : '0';
        c[2] = (pad & PAD_LEFT) ? '1' : '0';
        c[3] = (pad & PAD_RIGHT) ? '1' : '0';
        c[4] = (pad & PAD_A) ? '1' : '0';
        c[5] = 0;
        char line[24];
        for (int i = 0; i < 8; i++) { line[i] = a[i]; line[9 + i] = b[i]; }
        for (int i = 0; i < 5; i++) line[18 + i] = c[i];
        line[8] = line[17] = ' ';
        line[23] = 0;
        int same = 1;
        for (int i = 0; i < 24; i++) if (line[i] != last[i]) same = 0;
        if (!same) {
            for (int i = 0; i < 24; i++) last[i] = line[i];
            api->sys->log("PAD gbc=%s legacy=%s c64=%s\n", a, b, c);
        }
    }
}
