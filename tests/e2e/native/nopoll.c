#include "app_abi.h"
#include "os.h"
#include <stdint.h>

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                   const char *app_id, const char *app_name) {
    (void)app_dir; (void)app_id; (void)app_name;
    api->sys->log("NP:START\n");
    uint32_t t0 = api->sys->getTimeMs();
    while (api->sys->getTimeMs() - t0 < 4000) { }
    api->sys->log("NP:BUSYDONE\n");
    for (;;) {
        api->sys->poll();
        if (api->sys->shouldExit()) break;
    }
    api->sys->log("NP:EXIT\n");
}
