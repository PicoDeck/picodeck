/*
 * PicoDeck C64 Emulator
 *
 * Commodore 64 emulator using floooh/chips (zlib license).
 * Cycle-accurate 6502 + VIC-II + SID + dual CIAs.
 */
#include "app_abi.h"
#include "os.h"
#include "pad_input.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

// chips emulator headers (CHIPS_IMPL creates the implementation)
#define CHIPS_IMPL
#define CHIPS_ASSERT(c) ((void)0)  // disable asserts for release builds
#define assert(c) ((void)0)        // some chips files use bare assert()
#include "chips/chips_common.h"
#include "chips/clk.h"
#include "chips/m6502.h"
#include "chips/m6522.h"
#include "chips/m6526.h"
#include "chips/m6569.h"
#include "chips/m6581.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/systems/c1530.h"
#include "chips/systems/c1541.h"
#include "chips/systems/c64.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define SCREEN_W         320
#define SCREEN_H         320
#define C64_VISIBLE_W    320
#define C64_VISIBLE_H    200
#define Y_OFFSET         60   // (320 - 200) / 2, centers vertically

// VIC-II framebuffer offsets to the 320x200 content area
// The visible area (392x272) is written at (0,0) in the FB (relative to vis_x0/vis_y0).
// Left border = (CSEL1_BORDER_LEFT - vis_x0) * 8 = (16-8)*8 = 64px
// Top border  = RSEL1_BORDER_TOP - vis_y0 = 51-24 = 27 lines
#define VIC_CONTENT_X    64
#define VIC_CONTENT_Y    27
#define VIC_FB_STRIDE    M6569_FRAMEBUFFER_WIDTH  // 504

// Custom key code for left shift (not defined by chips; shift is a matrix modifier)
#define C64_KEY_LSHIFT      0xFE

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_NUM_SAMPLES   256
#define AUDIO_BUF_SIZE      2048  // samples per frame batch

#define ROM_DIR          "/data/net.picodeck.c64/roms"
#define PROGS_DIR        "/data/net.picodeck.c64/progs"

#define MAX_PROGS        64
#define MAX_FILENAME     48

// PAL frame duration in microseconds (50 Hz)
#define FRAME_US         20000

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static const PicoCalcAPI *s_api;
static c64_t s_c64;

// Pre-byte-swapped (big-endian) RGB565 palette for direct framebuffer writes
static uint16_t s_palette_be[16];

// Audio buffer: stereo int16 pairs accumulated during c64_exec()
static int16_t s_audio_buf[AUDIO_BUF_SIZE * 2];
static volatile int s_audio_pos;

// File browser state
static char s_prog_names[MAX_PROGS][MAX_FILENAME];
static char s_prog_paths[MAX_PROGS][128];
static int s_prog_count;

// Input state
static bool s_joystick_mode;
static int s_held_key;        // currently held C64 key code
static int s_key_timer;       // frames until key release

// ---------------------------------------------------------------------------
// RGB565 helpers
// ---------------------------------------------------------------------------
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = rgb565(r, g, b);
    return (c >> 8) | (c << 8);  // byte-swap for display
}

// ---------------------------------------------------------------------------
// Palette initialization
// ---------------------------------------------------------------------------
// Pepto's VIC-II palette (https://www.pepto.de/projects/colorvic/)
// Colors stored in the VIC-II as RGBA8: 0xAABBGGRR
static void init_palette(void) {
    // Extract R,G,B from the chips RGBA8 palette
    chips_range_t pal = m6569_palette();
    const uint32_t *colors = (const uint32_t *)pal.ptr;
    for (int i = 0; i < 16; i++) {
        uint32_t c = colors[i];
        uint8_t r = c & 0xFF;
        uint8_t g = (c >> 8) & 0xFF;
        uint8_t b = (c >> 16) & 0xFF;
        s_palette_be[i] = rgb565_be(r, g, b);
    }
}

// ---------------------------------------------------------------------------
// Audio callback — called by SID during c64_exec()
// ---------------------------------------------------------------------------
static void audio_callback(const float *samples, int num_samples, void *user_data) {
    (void)user_data;
    for (int i = 0; i < num_samples && s_audio_pos < AUDIO_BUF_SIZE; i++) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)(s * 32767.0f);
        s_audio_buf[s_audio_pos * 2]     = v;  // left
        s_audio_buf[s_audio_pos * 2 + 1] = v;  // right (mono → stereo)
        s_audio_pos++;
    }
}

// ---------------------------------------------------------------------------
// ROM loading
// ---------------------------------------------------------------------------
static bool load_rom(const char *filename, uint8_t *buf, int expected_size) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", ROM_DIR, filename);

    pcfile_t f = s_api->fs->open(path, "rb");
    if (!f) return false;

    int n = s_api->fs->read(f, buf, expected_size);
    s_api->fs->close(f);
    return (n == expected_size);
}

static bool load_roms(uint8_t *basic, uint8_t *kernal, uint8_t *chargen) {
    if (!load_rom("basic.rom", basic, 8192)) {
        if (!load_rom("basic", basic, 8192)) return false;
    }
    if (!load_rom("kernal.rom", kernal, 8192)) {
        if (!load_rom("kernal", kernal, 8192)) return false;
    }
    if (!load_rom("chargen.rom", chargen, 4096)) {
        if (!load_rom("chargen", chargen, 4096)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Display rendering — DOOM-style direct framebuffer writes
// ---------------------------------------------------------------------------
static void render_frame(void) {
    uint16_t *fb = s_api->display->getBackBuffer();
    if (!fb) return;

    const uint8_t *vic_fb = s_c64.fb;

    for (int y = 0; y < C64_VISIBLE_H; y++) {
        const uint8_t *src = &vic_fb[(y + VIC_CONTENT_Y) * VIC_FB_STRIDE + VIC_CONTENT_X];
        uint16_t *dst = &fb[(y + Y_OFFSET) * SCREEN_W];
        for (int x = 0; x < C64_VISIBLE_W; x += 4) {
            dst[x]     = s_palette_be[src[x] & 0x0F];
            dst[x + 1] = s_palette_be[src[x + 1] & 0x0F];
            dst[x + 2] = s_palette_be[src[x + 2] & 0x0F];
            dst[x + 3] = s_palette_be[src[x + 3] & 0x0F];
        }
    }

    s_api->display->flushRegion(Y_OFFSET - 1, Y_OFFSET + C64_VISIBLE_H);
}

// ---------------------------------------------------------------------------
// Keyboard input
// ---------------------------------------------------------------------------
#define KEY_HOLD_FRAMES 3

static void input_update(void) {
    const picocalc_input_t *in = s_api->input;
    uint32_t pressed = in->getButtonsPressed();
    uint32_t released = in->getButtonsReleased();

    // Joystick mode toggle (F9)
    if (pressed & BTN_F9) {
        s_joystick_mode = !s_joystick_mode;
        // Flash a brief indicator
        if (s_joystick_mode) {
            c64_set_joystick_type(&s_c64, C64_JOYSTICKTYPE_DIGITAL_2);
        } else {
            c64_set_joystick_type(&s_c64, C64_JOYSTICKTYPE_NONE);
        }
    }

    if (s_joystick_mode) {
        // Joystick mode: gamepad D-pad + A (fire) -> joystick port 2
        uint32_t pad = c64_pad_held(s_api);
        uint8_t joy = 0;
        if (pad & PAD_UP)    joy |= C64_JOYSTICK_UP;
        if (pad & PAD_DOWN)  joy |= C64_JOYSTICK_DOWN;
        if (pad & PAD_LEFT)  joy |= C64_JOYSTICK_LEFT;
        if (pad & PAD_RIGHT) joy |= C64_JOYSTICK_RIGHT;
        if (pad & PAD_A)     joy |= C64_JOYSTICK_BTN;
        c64_joystick(&s_c64, 0, joy);
        return;
    }

    // Keyboard mode: clear joystick
    c64_joystick(&s_c64, 0, 0);

    // Handle modifier/special keys via held buttons (proper press/release)
    if (pressed & BTN_ENTER)     c64_key_down(&s_c64, C64_KEY_RETURN);
    if (released & BTN_ENTER)    c64_key_up(&s_c64, C64_KEY_RETURN);

    if (pressed & BTN_BACKSPACE) c64_key_down(&s_c64, C64_KEY_DEL);
    if (released & BTN_BACKSPACE) c64_key_up(&s_c64, C64_KEY_DEL);

    if (pressed & BTN_SHIFT)     c64_key_down(&s_c64, C64_KEY_LSHIFT);
    if (released & BTN_SHIFT)    c64_key_up(&s_c64, C64_KEY_LSHIFT);

    // Arrow keys — C64 uses cursor right/down + shift for left/up
    if (pressed & BTN_UP)        c64_key_down(&s_c64, C64_KEY_CSRUP);
    if (released & BTN_UP)       c64_key_up(&s_c64, C64_KEY_CSRUP);
    if (pressed & BTN_DOWN)      c64_key_down(&s_c64, C64_KEY_CSRDOWN);
    if (released & BTN_DOWN)     c64_key_up(&s_c64, C64_KEY_CSRDOWN);
    if (pressed & BTN_LEFT)      c64_key_down(&s_c64, C64_KEY_CSRLEFT);
    if (released & BTN_LEFT)     c64_key_up(&s_c64, C64_KEY_CSRLEFT);
    if (pressed & BTN_RIGHT)     c64_key_down(&s_c64, C64_KEY_CSRRIGHT);
    if (released & BTN_RIGHT)    c64_key_up(&s_c64, C64_KEY_CSRRIGHT);

    // F1-F7 keys
    if (pressed & BTN_F1)  c64_key_down(&s_c64, C64_KEY_F1);
    if (released & BTN_F1) c64_key_up(&s_c64, C64_KEY_F1);
    if (pressed & BTN_F2)  c64_key_down(&s_c64, C64_KEY_F3);
    if (released & BTN_F2) c64_key_up(&s_c64, C64_KEY_F3);
    if (pressed & BTN_F3)  c64_key_down(&s_c64, C64_KEY_F5);
    if (released & BTN_F3) c64_key_up(&s_c64, C64_KEY_F5);

    // RUN/STOP key (Ctrl on PicoCalc)
    if (pressed & BTN_CTRL)  c64_key_down(&s_c64, C64_KEY_STOP);
    if (released & BTN_CTRL) c64_key_up(&s_c64, C64_KEY_STOP);

    // Timer-based key release for character input
    if (s_key_timer > 0) {
        s_key_timer--;
        if (s_key_timer == 0 && s_held_key != 0) {
            c64_key_up(&s_c64, s_held_key);
            s_held_key = 0;
        }
    }

    // Character input via getChar() — ASCII keys
    char ch = in->getChar();
    if (ch && s_key_timer == 0) {
        int key = 0;
        if (ch >= 'a' && ch <= 'z') {
            key = ch - 'a' + 'A';  // C64 expects uppercase
        } else if (ch >= 'A' && ch <= 'Z') {
            key = ch;
        } else if (ch >= '0' && ch <= '9') {
            key = ch;
        } else if (ch == ' ') {
            key = C64_KEY_SPACE;
        } else {
            // Pass through common symbols
            switch (ch) {
                case '+': case '-': case '*': case '/':
                case '=': case ':': case ';': case ',':
                case '.': case '@': case '!': case '"':
                case '#': case '$': case '%': case '&':
                case '\'': case '(': case ')': case '<':
                case '>': case '?': case '[': case ']':
                case '^': case '_':
                    key = ch;
                    break;
                default:
                    break;
            }
        }
        if (key != 0) {
            c64_key_down(&s_c64, key);
            s_held_key = key;
            s_key_timer = KEY_HOLD_FRAMES;
        }
    }
}

// ---------------------------------------------------------------------------
// .prg file loading
// ---------------------------------------------------------------------------
static bool load_prg(const char *path) {
    pcfile_t f = s_api->fs->open(path, "rb");
    if (!f) return false;

    int size = s_api->fs->fsize(f);
    if (size < 3 || size > 65538) {
        s_api->fs->close(f);
        return false;
    }

    // Read the entire .prg into a temp buffer
    static uint8_t s_prg_buf[65538];
    int n = s_api->fs->read(f, s_prg_buf, size);
    s_api->fs->close(f);
    if (n != size) return false;

    // Use chips' built-in quickload (handles load address + BASIC pointers)
    chips_range_t data = { .ptr = s_prg_buf, .size = (size_t)n };
    return c64_quickload(&s_c64, data);
}

// ---------------------------------------------------------------------------
// File browser
// ---------------------------------------------------------------------------
static void prog_list_cb(const char *name, bool is_dir, uint32_t size, void *user) {
    (void)size; (void)user;
    if (is_dir || s_prog_count >= MAX_PROGS) return;

    // Check for .prg extension (case-insensitive)
    int len = strlen(name);
    if (len < 5) return;
    const char *ext = name + len - 4;
    if ((ext[0] != '.') ||
        (ext[1] != 'p' && ext[1] != 'P') ||
        (ext[2] != 'r' && ext[2] != 'R') ||
        (ext[3] != 'g' && ext[3] != 'G')) return;

    strncpy(s_prog_names[s_prog_count], name, MAX_FILENAME - 1);
    s_prog_names[s_prog_count][MAX_FILENAME - 1] = '\0';
    snprintf(s_prog_paths[s_prog_count], sizeof(s_prog_paths[0]),
             "%s/%s", PROGS_DIR, name);
    s_prog_count++;
}

static void scan_progs(void) {
    s_prog_count = 0;
    s_api->fs->listDir(PROGS_DIR, prog_list_cb, NULL);
}

// Returns index of selected program, or -1 for "Boot to BASIC"
static int file_browser(void) {
    const picocalc_display_t *d = s_api->display;
    const picocalc_input_t *in = s_api->input;
    const picocalc_sys_t *sys = s_api->sys;

    int sel = 0;  // 0 = "Boot to BASIC", 1..n = programs
    int total = s_prog_count + 1;
    int scroll = 0;
    int visible = 14;  // rows visible on screen

    uint16_t bg = 0x0000;
    uint16_t fg = 0xFFFF;
    uint16_t sel_bg = rgb565(0x50, 0x50, 0xFF);
    uint16_t c64_blue = rgb565(0x35, 0x4B, 0xCE);

    while (!sys->shouldExit()) {
        sys->poll();
        uint32_t pressed = in->getButtonsPressed();

        if (pressed & BTN_UP) {
            if (sel > 0) sel--;
            if (sel < scroll) scroll = sel;
        }
        if (pressed & BTN_DOWN) {
            if (sel < total - 1) sel++;
            if (sel >= scroll + visible) scroll = sel - visible + 1;
        }
        if (pressed & BTN_ENTER) {
            return sel - 1;  // -1 = BASIC, 0+ = program index
        }
        if (pressed & BTN_ESC) {
            return -2;  // exit app
        }

        d->clear(bg);

        // Header
        d->drawText(8, 8, "C64 Emulator", c64_blue, bg);
        d->drawText(8, 24, "Select program or boot to BASIC", fg, bg);

        // List
        for (int i = 0; i < visible && (scroll + i) < total; i++) {
            int idx = scroll + i;
            int y = 48 + i * 18;
            uint16_t row_bg = (idx == sel) ? sel_bg : bg;
            uint16_t row_fg = fg;

            // Draw selection highlight
            if (idx == sel) {
                d->fillRect(4, y - 2, SCREEN_W - 8, 16, sel_bg);
            }

            if (idx == 0) {
                d->drawText(12, y, "> Boot to BASIC", row_fg, row_bg);
            } else {
                char line[52];
                snprintf(line, sizeof(line), "  %s", s_prog_names[idx - 1]);
                d->drawText(12, y, line, row_fg, row_bg);
            }
        }

        // Footer
        char footer[48];
        snprintf(footer, sizeof(footer), "UP/DOWN: Navigate  ENTER: Select  ESC: Exit");
        d->drawText(8, SCREEN_H - 16, footer, c64_blue, bg);

        d->flush();
    }
    return -2;  // exit
}

// ---------------------------------------------------------------------------
// Error screen
// ---------------------------------------------------------------------------
static void show_error(const char *line1, const char *line2) {
    const picocalc_display_t *d = s_api->display;
    const picocalc_sys_t *sys = s_api->sys;
    uint16_t red = rgb565(0xFF, 0x40, 0x40);

    d->clear(0x0000);
    d->drawText(8, 100, line1, red, 0x0000);
    if (line2) d->drawText(8, 120, line2, 0xFFFF, 0x0000);
    d->drawText(8, 160, "Press any key to exit", 0xFFFF, 0x0000);
    d->flush();

    while (!sys->shouldExit()) {
        sys->poll();
        if (s_api->input->getButtonsPressed() || s_api->input->getChar()) break;
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
void picodeck_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    (void)app_dir; (void)app_id; (void)app_name;
    s_api = api;

    const picocalc_display_t *d = api->display;
    const picocalc_sys_t *sys = api->sys;

    // Splash screen
    d->clear(0x0000);
    d->drawText(100, 150, "C64 Emulator", 0xFFFF, 0x0000);
    d->drawText(80, 170, "Loading ROMs...", rgb565(0x80, 0x80, 0x80), 0x0000);
    d->flush();

    // Load ROMs
    static uint8_t rom_basic[8192];
    static uint8_t rom_kernal[8192];
    static uint8_t rom_chargen[4096];

    if (!load_roms(rom_basic, rom_kernal, rom_chargen)) {
        show_error("ROM files not found!", "Place basic.rom, kernal.rom, chargen.rom");
        return;
    }

    // Scan for .prg files
    scan_progs();

    // File browser (skip if no programs found, go straight to BASIC)
    int prog_sel = -1;
    if (s_prog_count > 0) {
        prog_sel = file_browser();
        if (prog_sel == -2) return;  // user chose to exit
    }

    // Initialize palette
    init_palette();

    // Initialize C64
    c64_desc_t desc = {
        .joystick_type = C64_JOYSTICKTYPE_NONE,
        .audio = {
            .callback = { .func = audio_callback, .user_data = NULL },
            .num_samples = AUDIO_NUM_SAMPLES,
            .sample_rate = AUDIO_SAMPLE_RATE,
            .volume = 1.0f,
        },
        .roms = {
            .chars  = { .ptr = rom_chargen, .size = 4096 },
            .basic  = { .ptr = rom_basic,   .size = 8192 },
            .kernal = { .ptr = rom_kernal,  .size = 8192 },
        },
    };
    c64_init(&s_c64, &desc);

    // Register left shift as a direct key at matrix position (col 7, line 1)
    kbd_register_key(&s_c64.kbd, C64_KEY_LSHIFT, 7, 1, 0);

    // Clear both framebuffers (double-buffered)
    d->clear(0x0000);
    d->flush();
    d->clear(0x0000);
    d->flush();

    // Start audio streaming
    api->audio->startStream(AUDIO_SAMPLE_RATE);

    // If a .prg was selected, boot the C64 and then load it
    if (prog_sel >= 0) {
        // Run the C64 for ~3 seconds to reach the READY prompt
        for (int i = 0; i < 150; i++) {
            sys->poll();
            c64_exec(&s_c64, FRAME_US);
            if (sys->shouldExit()) goto cleanup;
        }

        // Load the .prg file
        if (load_prg(s_prog_paths[prog_sel])) {
            // Auto-run: type RUN and press return
            c64_basic_run(&s_c64);
        }
    }

    // Main emulation loop
    s_joystick_mode = false;
    s_held_key = 0;
    s_key_timer = 0;

    while (!sys->shouldExit()) {
        sys->poll();

        // ESC exits
        if (s_api->input->getButtonsPressed() & BTN_ESC) break;

        // Handle input
        input_update();

        // Run one PAL frame
        s_audio_pos = 0;
        c64_exec(&s_c64, FRAME_US);

        // Push accumulated audio
        if (s_audio_pos > 0) {
            api->audio->pushSamples(s_audio_buf, s_audio_pos);
        }

        // Render VIC-II output
        render_frame();
    }

cleanup:
    api->audio->stopStream();
}
