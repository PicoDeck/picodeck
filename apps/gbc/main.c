#include "app_abi.h"
#include "os.h"
#include "display.h"
#include "input.h"
#include "fs.h"
#include "state.h"
#include <stddef.h>
#include "minigb_apu.h"
#include "peanut_gb.h"
#include <string.h>
#include <stdio.h>

// ROM_BANK_SIZE (0x4000) is defined by peanut_gb.h — do not redefine it here.
// ROM_MAX_SIZE: 1 MB covers all common DMG/CGB games (Kirby=256KB, Pokemon=1MB).
// Larger games (>1MB) will load bank 0 correctly but return 0xFF for missing banks.
#define ROM_MAX_SIZE  (1024 * 1024)
#define CART_RAM_SIZE 0x8000

static uint8_t s_rom[ROM_MAX_SIZE];
static int     s_rom_size = 0;
static uint8_t s_ram[CART_RAM_SIZE];
// AUDIO_SAMPLES (~738) involves float division and isn't a constant expression,
// so use a fixed-size buffer.  748 > 738 gives slight headroom per frame.
// 4 frames per display flush × stereo (2 channels) × int16_t.
#define GBC_AUDIO_BUF_FRAMES 748
static int16_t s_audio_buf[GBC_AUDIO_BUF_FRAMES * 2 * 4];

static struct gb_s s_gb;
static GBCDisplay s_display;
static GBCInput s_input;
static GBCFilesystem s_fs;
static const PicoCalcAPI *s_api;

// Save-state scratch: gbc_state_load validates into these; s_gb/s_ram are
// only overwritten after a fully successful read (~50KB + 32KB in PSRAM BSS).
static struct gb_s s_gb_scratch;
static uint8_t     s_ram_scratch[CART_RAM_SIZE];

static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    if (addr < (uint_fast32_t)s_rom_size) {
        return s_rom[addr];
    }
    return 0xFF;
}

static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    if (addr < CART_RAM_SIZE) {
        return s_ram[addr];
    }
    return 0xFF;
}

static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, uint8_t val) {
    (void)gb;
    if (addr < CART_RAM_SIZE) {
        s_ram[addr] = val;
    }
}

static void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    (void)gb;
    (void)addr;
    s_api->sys->log("GB Error: %d at 0x%04X\n", err, addr);
}

static void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
    gbc_display_draw_line(&s_display, pixels, line, gb);
}

#define ROMS_DIR "/data/net.picodeck.gbc/roms"
#define DATA_DIR "/data/net.picodeck.gbc"

#define RUN_EXIT       1
#define RUN_SWITCH_ROM 2

// System-menu requests: callbacks run inside sys->poll()'s menu overlay,
// so they only set flags; run_game() acts on them between emulated frames.
static volatile bool s_req_load_rom;
static volatile bool s_req_save_state;
static volatile bool s_req_load_state;

static void menu_cb_load_rom(void *user)   { (void)user; s_req_load_rom = true; }
static void menu_cb_save_state(void *user) { (void)user; s_req_save_state = true; }
static void menu_cb_load_state(void *user) { (void)user; s_req_load_state = true; }

// Transient status text drawn in the 16px top bar.
static char     s_notice[24];
static uint32_t s_notice_until;

static void set_notice(const char *msg) {
    int i = 0;
    while (msg[i] && i < (int)sizeof(s_notice) - 1) {
        s_notice[i] = msg[i];
        i++;
    }
    s_notice[i] = '\0';
    s_notice_until = s_api->sys->getTimeMs() + 1500;
}

// Clear + flush BOTH framebuffers (swap-flush double buffering) so a
// modal overlay (file browser / system menu action) leaves no remnants.
static void force_full_redraw(const picocalc_display_t *d) {
    d->clear(0x0000);
    d->flush();
    d->clear(0x0000);
    d->flush();
}

static void ensure_data_dirs(void) {
    const picocalc_fs_t *fs = s_api->fs;
    // Legacy battery-save location used by fs.c — must exist or the
    // "wb" open inside gbc_fs_save_ram() fails silently.
    fs->mkdir("/data/gbc");
    fs->mkdir("/data/gbc/saves");
    fs->mkdir(DATA_DIR);
    fs->mkdir(ROMS_DIR);
    fs->mkdir(DATA_DIR "/states");
}

// Show the OS file browser rooted at the app data dir, starting in roms/.
static bool pick_rom(char *out, int out_len) {
    return s_api->fs->browse(ROMS_DIR, DATA_DIR, out, out_len);
}

// Write the current game's battery RAM to its .sav (no-op for ROMs
// without battery). Call while s_gb/current_rom_name still refer to the
// outgoing ROM.
static void flush_battery_save(void) {
    uint32_t save_size = gb_get_save_size(&s_gb);
    if (save_size > 0 && save_size <= CART_RAM_SIZE) {
        bool ok = gbc_fs_save_ram(&s_fs, s_ram, (int)save_size);
        s_api->sys->log("[GBC] battery save (%lu bytes): %s\n",
                        (unsigned long)save_size, ok ? "ok" : "FAILED");
    }
}

// Load a ROM file and (re)initialize the emulator for it.
// Returns false on load/init failure (caller re-opens the browser).
static bool start_rom(const char *path) {
    const picocalc_display_t *d = s_api->display;
    const picocalc_sys_t *sys = s_api->sys;

    d->clear(0x0000);
    d->drawText(80, 150, "Loading ROM...", 0x07E0, 0x0000);
    d->flush();

    sys->log("[GBC] loading ROM: %s\n", path);
    sys->poll(); // feed watchdog before potentially long SD read
    s_rom_size = gbc_fs_load_rom(&s_fs, path, s_rom, ROM_MAX_SIZE);
    sys->poll(); // feed watchdog after ROM load
    sys->log("[GBC] ROM load complete: size=%d\n", s_rom_size);
    if (s_rom_size <= 0)
        return false;

    enum gb_init_error_e ret = gb_init(&s_gb, &gb_rom_read, &gb_cart_ram_read,
                                       &gb_cart_ram_write, &gb_error, NULL);
    sys->log("[GBC] gb_init returned: %d\n", (int)ret);
    if (ret != GB_INIT_NO_ERROR)
        return false;

    // Wire direct ROM/RAM pointers for inlined access in __gb_read/__gb_write
    s_gb.rom = s_rom;
    s_gb.rom_size = (uint32_t)s_rom_size;
    s_gb.cart_ram_data = s_ram;
    s_gb.cart_ram_data_size = CART_RAM_SIZE;

    gb_init_lcd(&s_gb, &lcd_draw_line);
    s_gb.direct.frame_skip = 1;
    s_display.cgb_mode = s_gb.cgb.cgbMode;
    s_display.cgb_palette = s_gb.cgb.fixPalette;
    gbc_display_update_cgb_lut(&s_display);
    sys->log("[GBC] lcd callback set (cgb=%d)\n", s_display.cgb_mode);

    audio_init();
    static bool s_audio_started = false;
    if (!s_audio_started) {
        s_api->audio->startStream(AUDIO_SAMPLE_RATE);
        s_audio_started = true;
        sys->log("[GBC] audio started at %u Hz\n", AUDIO_SAMPLE_RATE);
    }

    // Fresh cart RAM for the incoming ROM — real hardware powers up with
    // undefined SRAM, and without this a ROM with no .sav would inherit
    // (and later persist) the previous game's cart RAM.
    memset(s_ram, 0xFF, CART_RAM_SIZE);

    uint32_t save_size = gb_get_save_size(&s_gb);
    sys->log("[GBC] save_size=%lu\n", (unsigned long)save_size);
    if (save_size > 0 && save_size <= CART_RAM_SIZE)
        gbc_fs_load_ram(&s_fs, s_ram, (int)save_size);

    force_full_redraw(d);
    return true;
}

// Copy a validated state blob into the live emulator and re-patch every
// pointer field: the saved values may come from a previous run of this
// app loaded at a different PSRAM address.
static void apply_loaded_state(void) {
    memcpy(&s_gb, &s_gb_scratch, sizeof(s_gb));
    memcpy(s_ram, s_ram_scratch, CART_RAM_SIZE);

    s_gb.gb_rom_read        = gb_rom_read;
    s_gb.gb_cart_ram_read   = gb_cart_ram_read;
    s_gb.gb_cart_ram_write  = gb_cart_ram_write;
    s_gb.gb_error           = gb_error;
    s_gb.gb_serial_tx       = NULL;
    s_gb.gb_serial_rx       = NULL;
    s_gb.gb_bootrom_read    = NULL;
    s_gb.display.lcd_draw_line = lcd_draw_line;
    s_gb.direct.priv        = NULL;
    s_gb.rom                = s_rom;
    s_gb.rom_size           = (uint32_t)s_rom_size;
    s_gb.cart_ram_data      = s_ram;
    s_gb.cart_ram_data_size = CART_RAM_SIZE;

    // Same ROM guaranteed by the header checks, so cgb_mode is unchanged;
    // palette contents may differ — rebuild the LUT.
    s_display.cgb_palette = s_gb.cgb.fixPalette;
    if (s_display.cgb_mode)
        gbc_display_update_cgb_lut(&s_display);
}

// Loop structure: GB_FRAMES_PER_FLUSH emulated frames per display flush,
// paced to native GB speed (59.73 Hz).  With peanut_gb frame_skip=1 the
// emulator renders LCD lines every other frame, so 2 frames per flush
// shows every rendered frame at ~30 Hz while game logic runs at 60 Hz.
// The flush DMA overlaps the next frame's emulation.  perf is ticked per
// GAME frame, so the OS FPS counter (system menu -> Settings -> Show FPS)
// reports game frames/second: 60 means the game runs at native speed.
#define GB_FRAMES_PER_FLUSH 2
#define GB_FRAME_US 16742u  // 59.73 Hz
// Set GBC_PROFILE to 1 to log per-phase timing over serial.
#define GBC_PROFILE 0
#if GBC_PROFILE
    #define PROF_MARK(v) uint64_t v = sys->getTimeUs()
    #define PROF_ADD(acc, v) acc += sys->getTimeUs() - (v)
#else
    #define PROF_MARK(v) do {} while (0)
    #define PROF_ADD(acc, v) do {} while (0)
#endif

static int run_game(char *rom_path, int rom_path_len) {
    const picocalc_display_t *d = s_api->display;
    const picocalc_sys_t *sys = s_api->sys;
    const picocalc_input_t *in = s_api->input;
    const PicoCalcAPI *api = s_api;
    int exit_reason = RUN_EXIT;

    sys->log("[GBC] entering main loop\n");

    bool running = true;
#if GBC_PROFILE
    uint64_t t_poll = 0, t_emu = 0, t_audio = 0, t_render = 0;
    int t_iters = 0;
#endif
    uint64_t next_frame_us = sys->getTimeUs();
    while (running) {
        // Poll the OS (keyboard I2C, watchdog, dev commands) once per display
        // frame, not per GB frame — polls are rate-limited firmware-side but
        // still cost a few ms when the I2C window is due.
        {
            PROF_MARK(t0);
            sys->poll();
            gbc_input_update(&s_input, in->getButtons);
            // --- system-menu request handling (Load ROM / states) ---
            if (s_req_load_rom) {
                s_req_load_rom = false;
                // Save the outgoing game's battery RAM before anything else so
                // it survives even if the user picks a new ROM.
                flush_battery_save();
                if (pick_rom(rom_path, rom_path_len)) {
                    exit_reason = RUN_SWITCH_ROM;
                    running = false;
                } else {
                    // Cancelled — resume. The Esc that closed the browser may
                    // still be latched in the poll-edge state. One fresh
                    // kbd_poll() clears the stale edge (prev=curr runs even
                    // when the I2C read is rate-limited); the bounded loop is
                    // belt-and-braces for injected/simulated input paths, so
                    // the game's own Esc-exit check below doesn't see a stale
                    // edge and quit. Bounded to 300ms (6x the 50ms rate limit)
                    // so a stuck bus can't hang the resume.
                    uint32_t drain_start = sys->getTimeMs();
                    while ((in->getButtonsPressed() & BTN_ESC) &&
                           (sys->getTimeMs() - drain_start) < 300) {
                        sys->poll();
                    }
                    force_full_redraw(d);
                }
            }
            if (s_req_save_state) {
                s_req_save_state = false;
                bool ok = gbc_state_save(api, s_fs.current_rom_name,
                                         &s_gb, (uint32_t)sizeof(s_gb),
                                         s_ram, CART_RAM_SIZE,
                                         s_rom, (uint32_t)s_rom_size);
                sys->log("[GBC] save state: %s\n", ok ? "ok" : "FAILED");
                set_notice(ok ? "State saved" : "Save failed");
                force_full_redraw(d);
            }
            if (s_req_load_state) {
                s_req_load_state = false;
                if (gbc_state_load(api, s_fs.current_rom_name,
                                   &s_gb_scratch, (uint32_t)sizeof(s_gb_scratch),
                                   s_ram_scratch, CART_RAM_SIZE,
                                   s_rom, (uint32_t)s_rom_size)) {
                    apply_loaded_state();
                    sys->log("[GBC] load state: ok\n");
                    set_notice("State loaded");
                } else {
                    sys->log("[GBC] load state: missing or mismatched\n");
                    set_notice("No state found");
                }
                force_full_redraw(d);
            }
            PROF_ADD(t_poll, t0);
        }
        for (int f = 0; f < GB_FRAMES_PER_FLUSH && running; f++) {
            api->perf->beginFrame();
            s_gb.direct.joypad_bits.a      = (s_input.buttons & BTN_F4)    ? 0 : 1;
            s_gb.direct.joypad_bits.b      = (s_input.buttons & BTN_F5)    ? 0 : 1;
            s_gb.direct.joypad_bits.select = (s_input.buttons & BTN_F1)    ? 0 : 1;
            s_gb.direct.joypad_bits.start  = (s_input.buttons & BTN_F2)    ? 0 : 1;
            s_gb.direct.joypad_bits.right  = (s_input.buttons & BTN_RIGHT) ? 0 : 1;
            s_gb.direct.joypad_bits.left   = (s_input.buttons & BTN_LEFT)  ? 0 : 1;
            s_gb.direct.joypad_bits.up     = (s_input.buttons & BTN_UP)    ? 0 : 1;
            s_gb.direct.joypad_bits.down   = (s_input.buttons & BTN_DOWN)  ? 0 : 1;

            if (in->getButtonsPressed() & BTN_ESC) {
                running = false;
                break;
            }
            if (sys->shouldExit()) {
                running = false;
                break;
            }

            PROF_MARK(t1);
            s_gb.gb_frame = 0;
            while (s_gb.gb_frame == 0) {
                __gb_step_cpu(&s_gb);
            }
            PROF_ADD(t_emu, t1);

            PROF_MARK(t2);
            audio_callback(NULL,
                           s_audio_buf + f * AUDIO_SAMPLES * 2,
                           AUDIO_BUFFER_SIZE_BYTES);
            PROF_ADD(t_audio, t2);

            // Real-time pacing: don't run the game faster than native speed.
            // When emulation is behind schedule this never waits.
            next_frame_us += GB_FRAME_US;
            uint64_t now_us = sys->getTimeUs();
            if (now_us > next_frame_us + GB_FRAME_US * 2) {
                // Schedule fell far behind wall-clock (e.g. a modal was open) — resync
                // instead of fast-forwarding through the accumulated deficit.
                next_frame_us = now_us;
            }
            while (sys->getTimeUs() < next_frame_us) { /* spin */ }
            api->perf->endFrame();  // no target FPS: paced above
        }

        if (running) {
            api->audio->pushSamples(s_audio_buf,
                                    AUDIO_SAMPLES * GB_FRAMES_PER_FLUSH);
        }

#if GBC_PROFILE
        if (++t_iters == 64) {
            sys->log("[GBC] prof(64 it): poll=%lums emu=%lums audio=%lums render=%lums\n",
                     (unsigned long)((uint32_t)t_poll / 1000u),
                     (unsigned long)((uint32_t)t_emu / 1000u),
                     (unsigned long)((uint32_t)t_audio / 1000u),
                     (unsigned long)((uint32_t)t_render / 1000u));
            t_poll = t_emu = t_audio = t_render = 0;
            t_iters = 0;
        }
#endif

        if (running) {
            // The 16px black bar above the GBC image, cleared every frame
            // (the file browser leaves its header in one swap buffer), and
            // the transient notice in it.
            d->fillRect(0, 0, 320, 16, 0x0000);
            if (s_notice[0]) {
                if (sys->getTimeMs() < s_notice_until)
                    d->drawText(120, 4, s_notice, 0xFFE0, 0x0000);
                else
                    s_notice[0] = '\0';
            }

            // Refresh CGB palette LUT (palettes can change mid-game)
            if (s_display.cgb_mode)
                gbc_display_update_cgb_lut(&s_display);

            PROF_MARK(t3);
            gbc_display_render(&s_display,
                d->drawImageNN,
                d->flush,
                d->flushRows);
            PROF_ADD(t_render, t3);
        }
    }

    return exit_reason;
}

void picodeck_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    (void)app_dir;
    (void)app_id;
    (void)app_name;

    s_api = api;

    const picocalc_display_t *d = api->display;
    const picocalc_sys_t *sys = api->sys;
    const picocalc_input_t *in = api->input;
    sys->log("[GBC] picodeck_main entered (api version %lu)\n",
             (unsigned long)api->version);
    d->clear(0x0000);
    d->drawText(100, 150, "GBC...", 0xFFFF, 0x0000);
    d->flush();

    if (api->version < 3) {
        d->clear(0x0000);
        d->drawText(30, 150, "Firmware too old (need v3+)", 0xF800, 0x0000);
        d->drawText(60, 170, "Update PicoDeck firmware", 0xFFFF, 0x0000);
        d->flush();
        while (1) {
            sys->poll();
            if ((in->getButtonsPressed() & BTN_ESC) || sys->shouldExit())
                return;
        }
    }

    gbc_display_init(&s_display);
    s_display.draw_image_nn_fn = d->drawImageNN;
    gbc_input_init(&s_input);
    gbc_fs_init(&s_fs);
    gbc_fs_set_api(api);
    ensure_data_dirs();

    sys->addMenuItem("Load ROM...", menu_cb_load_rom, NULL);
    sys->addMenuItem("Save State", menu_cb_save_state, NULL);
    sys->addMenuItem("Load State", menu_cb_load_state, NULL);

    char rom_path[192];
    if (!pick_rom(rom_path, sizeof(rom_path))) {
        sys->log("[GBC] picker cancelled; put .gb/.gbc files in %s\n", ROMS_DIR);
        return;
    }

    for (;;) {
        if (!start_rom(rom_path)) {
            d->clear(0x0000);
            d->drawText(60, 150, "Failed to load ROM", 0xF800, 0x0000);
            d->flush();
            sys->log("[GBC] load failed: %s\n", rom_path);
            if (!pick_rom(rom_path, sizeof(rom_path)))
                break; // cancelled — exit to launcher
            continue;
        }

        int reason = run_game(rom_path, sizeof(rom_path));

        // On exit, s_gb / current_rom_name still refer to the outgoing ROM
        // here, so flush now. On switch, the s_req_load_rom handler inside
        // run_game already flushed the outgoing ROM's save before the
        // browser opened — flushing again here would be redundant.
        if (reason == RUN_EXIT)
            flush_battery_save();

        if (reason == RUN_EXIT)
            break;
        // RUN_SWITCH_ROM: rom_path was updated inside run_game
    }

    api->audio->stopStream();
}
