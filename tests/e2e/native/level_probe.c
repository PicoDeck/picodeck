// native_level_probe — reads the keyboard and the gamepad as a game does (the
// held masks, compared by the app from one frame to the next) and logs every
// change, so the E2E test (tests/e2e/test_menu_key_leak.py) can see whether the
// key that closed the system menu reached a native app.
//
// Logs LEVELREADY once running, then, each time anything changes:
//   LEVEL btn=<held> press=<pressed> rel=<released> pad=<held> padp=<pressed>
//         padr=<released> char=<getChar>
// and LEVELEXIT when shouldExit() turns true.
//
// Built by tests/e2e/native/Makefile into tests/e2e/fixtures/native_level_probe/
// main.elf (committed, so the E2E job needs no ARM toolchain).
#include "app_abi.h"
#include "os.h"

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                   const char *app_id, const char *app_name) {
    (void)app_dir; (void)app_id; (void)app_name;
    static uint32_t last[7];
    api->sys->log("LEVELREADY");
    for (;;) {
        api->sys->poll();
        if (api->sys->shouldExit()) {
            api->sys->log("LEVELEXIT");
            return;
        }
        uint32_t now[7] = {
            api->input->getButtons(),
            api->input->getButtonsPressed(),
            api->input->getButtonsReleased(),
            api->gamepad->getButtons(),
            api->gamepad->getButtonsPressed(),
            api->gamepad->getButtonsReleased(),
            (uint32_t)(unsigned char)api->input->getChar(),
        };
        int same = 1;
        for (int i = 0; i < 7; i++) {
            if (now[i] != last[i]) same = 0;
            last[i] = now[i];
        }
        if (!same)
            api->sys->log("LEVEL btn=%d press=%d rel=%d pad=%d padp=%d padr=%d char=%d",
                          (int)now[0], (int)now[1], (int)now[2], (int)now[3],
                          (int)now[4], (int)now[5], (int)now[6]);
    }
}
