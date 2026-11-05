// native_fidelity_probe — what a native app sees through the API and the
// simulator's emulated machine, one behaviour per mode. The mode is the app's
// name in app.json, so tests/e2e/test_native_fidelity.py stages this one ELF
// under a different name per case. Each mode logs what it saw; a mode that
// ends in a fault never logs SURVIVED.
//
// Built by tests/e2e/native/Makefile into tests/e2e/fixtures/native_fidelity/
// main.elf (committed, so the E2E job needs no ARM toolchain).
#include <stddef.h>

#include "app_abi.h"
#include "os.h"

static const PicoCalcAPI *A;
static const char *s_dir;

#define MB (1024u * 1024u)
#define W 320

static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// The raw framebuffer holds byte-swapped pixels.
static uint16_t raw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static void raw_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int r = y; r < y + h; r++)
        for (int i = x; i < x + w; i++)
            fb[r * W + i] = raw(c);
}

// Serve the OS until the test asks the app to leave.
static void hold(void) {
    for (;;) {
        A->sys->poll();
        if (A->sys->shouldExit() || (A->input->getButtonsPressed() & BTN_ESC))
            return;
    }
}

static void pause_ms(uint32_t ms) {
    uint32_t t0 = A->sys->getTimeMs();
    while (A->sys->getTimeMs() - t0 < ms)
        A->sys->poll();
}

// --- framebuffer ----------------------------------------------------------

static void fb_order(void) {
    uint16_t *fb = A->display->getBackBuffer();
    A->display->clear(PC_COLOR_RED);
    raw_rect(fb, 0, 0, 20, 20, PC_COLOR_BLUE);        // raw over a draw
    A->display->fillRect(5, 5, 5, 5, PC_COLOR_GREEN);  // a draw over raw
    raw_rect(fb, 6, 6, 1, 1, PC_COLOR_WHITE);         // raw over that draw
    A->sys->log("FB read %x", (unsigned)fb[100 * W + 100]);  // reads the clear
    A->display->flush();
    A->sys->log("FB READY");
    hold();
}

static void fb_refetch(void) {
    A->display->clear(PC_COLOR_RED);
    uint16_t *fb = A->display->getBackBuffer();
    raw_rect(fb, 0, 0, 10, 10, PC_COLOR_BLUE);
    A->display->flush();
    A->sys->log("FB READY");
    hold();
}

static void flush_region(void) {
    A->display->clear(PC_COLOR_RED);
    A->display->flushRegion(0, 319);
    A->sys->log("FB READY");
    hold();
}

static void flush_rows(void) {
    A->display->clear(PC_COLOR_RED);
    A->display->flushRows(0, 319);
    A->sys->log("FB READY");
    hold();
}

static void term_only(void) {
    terminal_t *t = A->terminal->create(30, 5, 0);
    A->terminal->write(t, "Hello terminal");
    A->terminal->render(t);
    A->display->flush();
    A->sys->log("FB READY");
    hold();
}

// --- qmiAlloc -------------------------------------------------------------

static void qmi_recycle(void) {
    for (int i = 0; i < 64; i++) {
        volatile uint8_t *p = A->psram->qmiAlloc(MB);
        if (!p) {
            A->sys->log("QMI failed at round %d", i);
            return;
        }
        p[0] = 1;
        p[MB - 1] = 2;
        A->psram->qmiFree((void *)p);
    }
    A->sys->log("QMI recycled");
}

static void qmi_merge(void) {
    void *a = A->psram->qmiAlloc(MB), *b = A->psram->qmiAlloc(MB);
    void *c = A->psram->qmiAlloc(MB);
    A->psram->qmiFree(a);
    A->psram->qmiFree(c);
    A->psram->qmiFree(b);
    void *all = A->psram->qmiAlloc(3 * MB);
    A->sys->log("QMI merged %d same %d", all != NULL, all == a);
}

static void qmi_budget(void) {
    void *big = A->psram->qmiAlloc(5 * MB);
    A->sys->log("QMI 5MB %d", big != NULL);
    A->psram->qmiFree(big);
    A->sys->log("QMI 7MB %d", A->psram->qmiAlloc(7 * MB) != NULL);
    A->sys->log("QMI 0 %d", A->psram->qmiAlloc(0) != NULL);
}

// --- faults ---------------------------------------------------------------

__attribute__((noinline)) static uint32_t burn(uint32_t bytes) {
    volatile uint8_t page[1024];
    page[0] = (uint8_t)bytes;
    if (bytes <= sizeof(page))
        return page[0];
    return burn(bytes - sizeof(page)) + page[0];
}

static void fault(const char *mode) {
    A->sys->log("FAULT READY");
    if (is(mode, "null_read")) {
        volatile uint32_t v = *(volatile uint32_t *)0;
        (void)v;
    } else if (is(mode, "null_write")) {
        *(volatile uint32_t *)0 = 1234;
    } else if (is(mode, "wild_read")) {
        volatile uint32_t v = *(volatile uint32_t *)0x60000000u;
        (void)v;
    } else if (is(mode, "wild_write")) {
        *(volatile uint32_t *)0x60000000u = 1;
    } else if (is(mode, "wild_call")) {
        ((void (*)(void))0x70000001u)();
    } else if (is(mode, "stack_48k")) {
        burn(48 * 1024);
    } else if (is(mode, "stack_128k")) {
        burn(128 * 1024);
    } else if (is(mode, "stack_600k")) {
        burn(600 * 1024);
    }
    A->sys->log("SURVIVED");
}

// --- file modes -------------------------------------------------------------

#define MODE_FILE "/data/modes.txt"

static char first_byte(void) {
    char c = 0;
    pcfile_t f = A->fs->open(MODE_FILE, "r");
    if (f) {
        A->fs->read(f, &c, 1);
        A->fs->close(f);
    }
    return c;
}

static void put_at_start(const char *mode, char c) {
    pcfile_t f = A->fs->open(MODE_FILE, mode);
    if (f) {
        A->fs->write(f, &c, 1);
        A->fs->close(f);
    }
}

static void file_modes(void) {
    A->fs->mkdir("/data");
    A->fs->deleteFile(MODE_FILE);
    put_at_start("rw", 'A');  // creates the file
    A->sys->log("MODE rw creates %c", first_byte());
    put_at_start("r+", 'B');  // read-only, so it writes nothing
    A->sys->log("MODE r+ leaves %c", first_byte());
    put_at_start("rw", 'C');  // writes
    A->sys->log("MODE rw writes %c", first_byte());
    A->fs->deleteFile(MODE_FILE);
}

// --- brightness, sample player, HTTP ---------------------------------------

static void brightness(void) {
    A->display->setBrightness(40);
    A->sys->log("BRIGHT READY");
    hold();
}

static void player(const char *mode) {
    char path[96];
    char *p = path;
    for (const char *s = s_dir; *s; s++) *p++ = *s;
    for (const char *s = "/beep.wav"; *s; s++) *p++ = *s;
    *p = '\0';
    pcsound_sample_t s = A->soundplayer->sampleLoad(path);
    pcsound_player_t pl = A->soundplayer->playerNew();
    if (!s || !pl) {
        A->sys->log("LOOP no sample");
        return;
    }
    A->soundplayer->playerSetSample(pl, s);
    if (is(mode, "loop_before_play")) {
        A->soundplayer->playerSetLoop(pl, true);  // outlives play's count
        A->soundplayer->playerPlay(pl, 1);
    } else {  // loop_off_while_looping: a play(0) that setLoop(false) ends
        A->soundplayer->playerPlay(pl, 0);
    }
    pause_ms(600);  // the sample is 50 ms long
    A->sys->log("LOOP looping %d", A->soundplayer->playerIsPlaying(pl));
    A->soundplayer->playerSetLoop(pl, false);
    pause_ms(600);
    A->sys->log("LOOP stopped %d", !A->soundplayer->playerIsPlaying(pl));
}

static void http(const char *mode) {
    pchttp_t c = A->http->newConn("127.0.0.1", 1, false);
    if (is(mode, "http_post"))
        A->http->post(c, "/", NULL, "x", 1);
    else
        A->http->get(c, "/", NULL);
    uint32_t t0 = A->sys->getTimeMs();
    bool done = false;
    while (!done && A->sys->getTimeMs() - t0 < 3000) {
        A->sys->poll();
        done = A->http->isComplete(c);
    }
    const char *err = A->http->getError(c);
    A->sys->log("HTTP complete %d err %s", done, err ? err : "(none)");
    A->http->close(c);
}

// The modes: not inlined, so picodeck_main keeps a small frame (see below).
__attribute__((noinline)) static void run_mode(const char *m) {
    if (is(m, "fb_order")) fb_order();
    else if (is(m, "fb_refetch")) fb_refetch();
    else if (is(m, "flush_region")) flush_region();
    else if (is(m, "flush_rows")) flush_rows();
    else if (is(m, "term_only")) term_only();
    else if (is(m, "qmi_recycle")) qmi_recycle();
    else if (is(m, "qmi_merge")) qmi_merge();
    else if (is(m, "qmi_budget")) qmi_budget();
    else if (is(m, "brightness")) brightness();
    else if (is(m, "loop_before_play") || is(m, "loop_off_while_looping")) player(m);
    else if (is(m, "http_get") || is(m, "http_post")) http(m);
    else if (is(m, "file_modes")) file_modes();
    else fault(m);
}

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                   const char *app_id, const char *app_name) {
    (void)app_id;
    A = api;
    s_dir = app_dir;
    // A local of the first frame on the stack, within 64 bytes of its top.
    volatile char at_top[4] = {'o', 'k', 0, 0};
    if (is(app_name, "string_at_stack_top"))
        A->sys->log("TOP [%s]", (const char *)at_top);
    else
        run_mode(app_name);
}
