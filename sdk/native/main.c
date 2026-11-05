// Hello C — minimal PicoDeck native app demo
//
// Build:   make
// Deploy:  copy main.elf + app.json to /apps/hello_c/ on the SD card
//          (apps/hello_c in this repository is this program)

#include "app_abi.h"
#include "os.h"

void picodeck_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    (void)app_dir; (void)app_id;

    const picocalc_display_t *d = api->display;
    const picocalc_sys_t     *s = api->sys;

    // ── Precompute layout ─────────────────────────────────────────────────────
    int cx = d->getWidth() / 2;
    int cy = d->getHeight() / 2;

    // Build "Running: <name>" without sprintf
    char line2[64];
    const char *prefix = "Running: ";
    char *p = line2;
    while (*prefix) *p++ = *prefix++;
    const char *n = app_name;
    while (*n) *p++ = *n++;
    *p = '\0';

    // ── Draw + flush in a loop ────────────────────────────────────────────────
    // display_flush() is non-blocking: DMA starts and the function returns
    // before the LCD has received the frame (a full 320x320 frame is 204,800 bytes,
    // about 16.4 ms at the 100 MHz PIO SPI clock; the next flush waits for it).
    // Redrawing every iteration (same pattern as Lua apps) keeps the display
    // simple: the whole frame is rebuilt each pass.
    //
    // getButtonsPressed() covers BTN_* keys (arrows, Enter, Esc, Fn-keys).
    // getChar() covers alphanumeric and punctuation keys.
    // Checking both means *any* key exits.
    while (true) {
        d->clear(PC_COLOR_BLACK);
        d->drawText(cx - 48, cy - 16, "Hello from C!", PC_COLOR_WHITE, PC_COLOR_BLACK);
        d->drawText(cx - 60, cy,       line2,           PC_RGB565(96, 96, 160), PC_COLOR_BLACK);
        d->drawText(cx - 52, cy + 16,  "Press any key...", PC_COLOR_GRAY, PC_COLOR_BLACK);
        if (api->version >= 6) {
            // API v6: font selection, measurement, and transparent text.
            d->setFont(PC_FONT_8X12);
            const char *msg = "API v6 fonts";
            int w = d->textWidth(msg);
            d->fillRect(cx - w / 2 - 4, cy + 34, w + 8, d->getFontHeight() + 4, PC_RGB565(32, 32, 128));
            d->drawTextTransparent(cx - w / 2, cy + 36, msg, PC_COLOR_YELLOW);
            d->setFont(PC_FONT_6X8);
        }
        d->flush();

        s->poll();
        if (s->shouldExit())  // "Exit App" chosen from the system menu
            break;
        if (api->input->getButtonsPressed() || api->input->getChar())
            break;
    }

    // Return to launcher
}
