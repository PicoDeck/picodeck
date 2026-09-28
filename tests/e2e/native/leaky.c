// native_leaky — a native app that leaks every kind of handle it can get
// from the API, then returns without freeing any of them.
//
// Built by tests/e2e/native/Makefile into tests/e2e/apps/native_leaky/
// main.elf (committed). Driven by tests/e2e/test_native_resources.py, which
// checks that the loader's exit sweep gives it all back: the files (FatFS
// allows 16 open at once), the PSRAM behind images/terminals/players, the
// audio slot a still-playing sample player holds, and the master volume it
// turned down.
//
// It also exercises the well-behaved paths through the same wrappers (a
// file closed twice, an image freed twice: the second call must be a no-op)
// and the native gates: appconfig->load of another app's id is refused, and
// crypto->randomBytes returns whether it produced random bytes.
#include "app_abi.h"
#include "os.h"

#include <stddef.h>
#include <stdint.h>

#define LEAK_FILES  12   // of FatFS's 16 (FF_FS_LOCK): the next app still loads
#define LEAK_IMAGES 8    // 8 x 160x160 RGB565 = 400 KB of PSRAM

static void cat(char *dst, unsigned cap, const char *a, const char *b) {
    unsigned n = 0;
    for (; a && *a && n + 1 < cap; a++) dst[n++] = *a;
    for (; b && *b && n + 1 < cap; b++) dst[n++] = *b;
    dst[n] = '\0';
}

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                const char *app_id, const char *app_name) {
    (void)app_id; (void)app_name;
    void (*log)(const char *, ...) = api->sys->log;
    char path[160];

    // Well-behaved first: open + close twice, newBlank + free twice.
    cat(path, sizeof(path), app_dir, "/app.json");
    pcfile_t f = api->fs->open(path, "r");
    api->fs->close(f);
    api->fs->close(f);
    pcimage_t tidy = api->graphics->newBlank(16, 16);
    api->graphics->free(tidy);
    api->graphics->free(tidy);

    int files = 0;
    for (int i = 0; i < LEAK_FILES; i++)
        if (api->fs->open(path, "r"))
            files++;

    int images = 0;
    for (int i = 0; i < LEAK_IMAGES; i++)
        if (api->graphics->newBlank(160, 160))
            images++;

    // A sample player left playing (beep.wav is staged by the test), and a
    // sampleless player: both hold a mixer slot.
    cat(path, sizeof(path), app_dir, "/beep.wav");
    pcsound_sample_t s = api->soundplayer->sampleLoad(path);
    pcsound_player_t p = api->soundplayer->playerNew();
    if (s && p) {
        api->soundplayer->playerSetSample(p, s);
        api->soundplayer->playerPlay(p, 255);
    }
    api->audio->setVolume(20);  // left turned down: the exit resets it
    pcsound_player_t idle = api->soundplayer->playerNew();
    pcfileplayer_t fp = api->soundplayer->filePlayerNew();
    terminal_t *term = api->terminal->create(40, 20, 200);
    pcmodplayer_t mod = api->modplayer->create();
    void *blk = api->psram->qmiAlloc(64 * 1024);

    // Native gate: the per-app store binds only to this app's own id.
    bool other = api->appconfig->load("com.test.someone_else");
    const char *bound = api->appconfig->getAppId();

    // (The simulator's log formats at most 8 arguments.)
    log("LEAKY files=%d images=%d sample=%d player=%d idle=%d",
        files, images, s != NULL, p != NULL, idle != NULL);
    log("LEAKY fp=%d term=%d mod=%d qmi=%d", fp != NULL, term != NULL,
        mod != NULL, blk != NULL);
    log("LEAKY appconfig other=%d bound=%s", (int)other,
        bound ? bound : "(none)");

    // randomBytes reports success (false + zeroed buffer without an RNG).
    uint8_t rnd[32];
    bool rnd_ok = api->crypto->randomBytes(rnd, sizeof(rnd));
    int nonzero = 0;
    for (unsigned i = 0; i < sizeof(rnd); i++)
        nonzero += rnd[i] != 0;
    log("LEAKY random ok=%d nonzero=%d", (int)rnd_ok, nonzero > 0);
    log("LEAKY done");
}
