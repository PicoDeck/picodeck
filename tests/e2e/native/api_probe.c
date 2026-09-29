// native_api_probe — logs the PicoCalcAPI layout it was compiled against
// (sdk/native/os.h) next to what the loader actually handed it.
//
// Built by tests/e2e/native/Makefile into tests/e2e/apps/native_api_probe/
// main.elf (committed, so the E2E job needs no ARM toolchain). Read by
// tests/e2e/test_native.py::test_api_layout_matches_os_h, which compares:
//   - api->version with g_api.version in src/main.c;
//   - the compiled sizeof()s with the struct layout in src/os/os.h (so a
//     stale probe, or an sdk/native/os.h that drifted, fails);
//   - the gap between consecutive sub-tables with their sizeof(): the
//     simulator lays its trampoline tables out back to back
//     (unicorn_build_api_struct), so gap != sizeof means its slot count
//     drifted from os.h, which shifts every later table and `version`;
//   - the gamepad table (API version 9, after `version`): read only when
//     api->version says it is there, as apps must; its buttons (none held)
//     and the labels of A's two slots (default: F4 and unbound), and that a
//     label kept from startup still reads the same after many more calls
//     (on the device the names are flash strings).
// It also logs one "PERF ..." line (not a PROBE line) for
// test_native.py::test_native_app_does_not_inherit_perf_pacing: the state
// perf->getFPS() and an unpaced perf->endFrame() see at app start.
#include "app_abi.h"
#include "os.h"

#include <stddef.h>
#include <stdint.h>

#define TABLES(X)                                                        \
    X(input, picocalc_input_t) X(display, picocalc_display_t)            \
    X(fs, picocalc_fs_t) X(sys, picocalc_sys_t)                          \
    X(audio, picocalc_audio_t) X(wifi, picocalc_wifi_t)                  \
    X(tcp, picocalc_tcp_t) X(ui, picocalc_ui_t)                          \
    X(psram, picocalc_psram_t) X(perf, picocalc_perf_t)                  \
    X(terminal, picocalc_terminal_t) X(http, picocalc_http_t)            \
    X(soundplayer, picocalc_soundplayer_t)                               \
    X(appconfig, picocalc_appconfig_t) X(crypto, picocalc_crypto_t)      \
    X(graphics, picocalc_graphics_t) X(video, picocalc_video_t)          \
    X(modplayer, picocalc_modplayer_t) X(zip, picocalc_zip_t)          \
    X(gamepad, picocalc_gamepad_t)

typedef struct {
    const char *name;
    uintptr_t addr;
    uint32_t size;
} table_t;

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                const char *app_id, const char *app_name) {
    (void)app_dir; (void)app_id; (void)app_name;
    void (*log)(const char *, ...) = api->sys->log;

#define ROW(field, type) {#field, (uintptr_t)api->field, (uint32_t)sizeof(type)},
    const table_t t[] = {TABLES(ROW)};
#undef ROW
    const unsigned n = sizeof(t) / sizeof(t[0]);

    // Perf state as this app finds it: an earlier app's setTargetFPS or frame
    // history must not leak in. Two endFrame calls, because the first only
    // starts the pacing schedule.
    int fps0 = api->perf->getFPS();
    uint32_t t0 = api->sys->getTimeMs();
    api->perf->endFrame();
    api->perf->endFrame();
    log("PERF fps=%d endframe_ms=%u", fps0, (unsigned)(api->sys->getTimeMs() - t0));

    log("PROBE version=%u", (unsigned)api->version);
    log("PROBE api size=%u version_off=%u tables=%u",
        (unsigned)sizeof(PicoCalcAPI),
        (unsigned)offsetof(PicoCalcAPI, version), n);
    for (unsigned i = 0; i < n; i++) {
        uint32_t gap = i + 1 < n ? (uint32_t)(t[i + 1].addr - t[i].addr) : 0;
        log("PROBE table %s size=%u gap=%u", t[i].name, (unsigned)t[i].size,
            (unsigned)gap);
    }
    if (api->version >= 9) {
        const char *a0 = api->gamepad->getLabel(PAD_A, 0);
        const char *a1 = api->gamepad->getLabel(PAD_A, 1);
        log("PROBE gamepad buttons=%u pressed=%u label_a=%s label_a_alt=%s",
            (unsigned)api->gamepad->getButtons(),
            (unsigned)api->gamepad->getButtonsPressed(), a0 ? a0 : "-",
            a1 ? a1 : "-");
        for (int i = 0; i < 10000; i++)
            (void)api->gamepad->getLabel(PAD_Y, 0);
        log("PROBE gamepad kept label_a=%s", a0 ? a0 : "-");
    }
    log("PROBE done");
}
