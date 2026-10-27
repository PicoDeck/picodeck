// native_exit_probe — ends only when sys->shouldExit() says so: it polls and
// ignores every key, so an exit request (the system menu's Exit App, the
// exit_app RPC, the dev `exit` command) is the one way out. The E2E test
// (tests/e2e/test_native_exit.py) checks the request reaches the app and does
// not outlive it.
//
// Logs EXITREADY once running and EXITSEEN when shouldExit() turns true.
// Built by tests/e2e/native/Makefile into tests/e2e/fixtures/native_exit_probe/
// main.elf (committed, so the E2E job needs no ARM toolchain).
#include "app_abi.h"
#include "os.h"

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                   const char *app_id, const char *app_name) {
    (void)app_dir; (void)app_id; (void)app_name;
    api->sys->log("EXITREADY");
    for (;;) {
        api->sys->poll();
        (void)api->input->getButtonsPressed();  // keys are read, and ignored
        if (api->sys->shouldExit()) {
            api->sys->log("EXITSEEN");
            return;
        }
    }
}
