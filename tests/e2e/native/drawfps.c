#include "app_abi.h"
#include "os.h"
#include <stdint.h>

// The SDK Showcase's frame from a native app (issue #66): a blue full
// redraw paced at 60, its own counter at (290, 15) (cut off by the right
// edge), one flush and endFrame. With Show FPS on, the OS counter must be
// the only one on screen; with it off, drawFPS draws the firmware's
// "FPS: n" in the colour code (test_show_fps.py).
void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                   const char *app_id, const char *app_name) {
    (void)app_dir; (void)app_id; (void)app_name;
    api->perf->setTargetFPS(60);
    for (int n = 1;; n++) {
        api->sys->poll();
        if (api->sys->shouldExit()) break;
        api->display->clear(0x001F);
        api->perf->drawFPS(290, 15);
        api->display->flush();
        api->perf->endFrame();
        if (n == 3) api->sys->log("DF:READY\n");
    }
}
