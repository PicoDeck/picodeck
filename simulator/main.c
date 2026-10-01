// PicoDeck PC Simulator - Main Entry Point
// Cross-platform SDL2 implementation for desktop debugging

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <execinfo.h>
#endif
#include "hal/web_platform.h"  // sim_core1_service(); the web build's hooks
#include <fcntl.h>
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_pad.h"
#include "hal/hal_sdcard.h"
#include "hal/hal_psram.h"
#include "hal/hal_timing.h"
#include "hal/hal_audio.h"
#include "hal/hal_threading.h"
#include "sim_socket.h"

// PicoDeck includes (adapted for simulator)
#include "os.h"
#include "terminal.h"
#include "launcher.h"
#include "lua_psram_alloc.h"
#include "stubs/umm_malloc.h"  // sim_umm_use_real (--real-umm)
#include "splash_logo.h"
#include "drivers/display.h"
#include "drivers/audio.h"
#include "drivers/sound.h"
#include "drivers/fileplayer.h"
#include "drivers/mp3_player.h"
#include "drivers/http.h"
#include "drivers/mod_player.h"
#ifdef PICODECK_SIM_FIRMWARE_NET
#include "drivers/wifi.h"
#include "net/sim_net.h"
#else
#include "sim_wifi.h"
#endif
#include "drivers/keyboard.h"
#include "appconfig.h"
#include "gamepad.h"
#include "config.h"
#include "clock.h"
#include "idle_dim.h"
#include "system_menu.h"
#include "sim_test_control.h"
#include "core1_stats.h"
#include <stdatomic.h>
#include <time.h>

// Simulator configuration
#define SIM_WINDOW_TITLE "PicoDeck Simulator"
#ifdef PICODECK_PROJECT_ROOT
#ifdef __EMSCRIPTEN__
#define SIM_DEFAULT_SD_CARD "/sd"  // --preload-file mount point
#else
#define SIM_DEFAULT_SD_CARD PICODECK_PROJECT_ROOT
#endif
#else
#define SIM_DEFAULT_SD_CARD "."
#endif

// Global state — g_running is non-static so lua_bridge.c and sim_socket_handler.c
// can extern it for shutdown propagation during Lua app execution.
volatile int g_running = 1;
static char g_sd_card_path[512] = SIM_DEFAULT_SD_CARD;
static char g_launch_app[128] = "";  // App to auto-launch
static int g_auto_launch_done = 0;   // Flag to track if auto-launch was attempted
static int g_show_splash = 0;        // Show boot splash screen
static int g_virtual_time = 0;       // --virtual-time (needs --test-mode)
static int g_host_gamepad = -1;      // --gamepad 1 / --no-gamepad 0; -1: on
                                     // unless --test-mode
static int g_tcp_port = 7878;        // TCP port for RPC socket
static char g_instance_id[64] = "";  // Instance ID for unique socket paths
static char g_unix_socket[256] = ""; // --unix-socket PATH|none ("" = default)
// Per-process by default. A single shared path meant that under parallel test
// runs (pytest -n auto) one crashing instance was reported as a crash by every
// other instance's get_crash_log, and the file outlived the run so a crash in
// one run kept failing later ones. Overridable with --crash-log.
char g_crash_log_path[512] = "";

// External function from keyboard stub
extern void set_simulator_exit_flag(volatile int *flag);

// Function to check if we should auto-launch
const char* simulator_get_auto_launch_app(void) {
    if (!g_auto_launch_done && g_launch_app[0] != '\0') {
        g_auto_launch_done = 1;
        return g_launch_app;
    }
    return NULL;
}

// Signal handler for clean shutdown
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    extern void dev_commands_set_exit(void);
    dev_commands_set_exit();
}

// Forced shutdown after timeout (SIGALRM)
static void force_exit_handler(int sig) {
    (void)sig;
    const char msg[] = "\n[Simulator] Shutdown timeout — forcing exit\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

// ASan/TSan install their own fatal-signal handlers (PICODECK_SIM_SANITIZE).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SIM_HAS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SIM_HAS_SANITIZER 1
#endif
#endif
#ifndef SIM_HAS_SANITIZER
#define SIM_HAS_SANITIZER 0
#endif

// Crash handler — writes backtrace to crash log file using only async-signal-safe calls
static void crash_handler(int sig) {
    // Write crash info to log file
    int fd = open(g_crash_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *sig_name = "UNKNOWN";
        switch (sig) {
            case SIGSEGV: sig_name = "SIGSEGV"; break;
            case SIGABRT: sig_name = "SIGABRT"; break;
            case SIGBUS:  sig_name = "SIGBUS"; break;
            case SIGFPE:  sig_name = "SIGFPE"; break;
        }
        // Lengths from sizeof, never hand-counted: renaming the product
        // silently truncated a hard-coded count here once.
        static const char header[] = "PicoDeck Simulator Crash\nSignal: ";
        static const char bt_header[] = "\nBacktrace:\n";
        (void)!write(fd, header, sizeof header - 1);
        (void)!write(fd, sig_name, strlen(sig_name));
        (void)!write(fd, bt_header, sizeof bt_header - 1);

#ifndef __EMSCRIPTEN__
        void *frames[32];
        int n = backtrace(frames, 32);
        backtrace_symbols_fd(frames, n, fd);
#endif
        close(fd);
    }

    // Also print to stderr
    const char msg[] = "\n[Simulator] CRASH — see crash log\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(128 + sig);
}

// Print usage
static void print_usage(const char* program) {
    printf("Usage: %s [options]\n", program);
    printf("Options:\n");
    printf("  --sd-card PATH       Path to simulated SD card (default: %s)\n", SIM_DEFAULT_SD_CARD);
    printf("  --launch APP         Auto-launch app on startup\n");
    printf("  --port PORT          TCP port for RPC socket (default: 7878, 0=auto)\n");
    printf("  --instance-id ID     Unique instance ID (for parallel simulators)\n");
    printf("  --unix-socket PATH   UNIX control socket path, or 'none' to disable\n");
    printf("  --crash-log PATH     Crash log file path (default: /tmp/picodeck_sim_crash_<pid>.log)\n");
    printf("  --show-splash        Show boot splash screen with delays\n");
    printf("  --test-mode          Error screens return at once; idle dim off;\n"
           "                       math.random seeded and the clock pinned to\n"
           "                       2026-01-01T00:00:00Z at boot\n");
    printf("  --virtual-time       (with --test-mode) virtual clock: Core 0 sleeps\n"
           "                       advance it instead of waiting (step_time RPC)\n");
    printf("  --gamepad            Read the host's game controllers (the default,\n"
           "                       except under --test-mode)\n");
    printf("  --no-gamepad         Ignore the host's game controllers\n");
    printf("  --debug              Enable debug logging\n");
    printf("  --real-umm           Run umm_* on the firmware's umm_malloc (device heap\n"
           "                       size, 200 B blocks) instead of the counting allocator\n");
    printf("  --build-info         Print build facts (sanitizers) and exit\n");
    printf("  --help               Show this help\n");
}

// Parse command line arguments
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--build-info") == 0) {
            // Read by the E2E harness (conftest) to enable asan_only tests.
            printf("sanitize=%s\n", PICODECK_SIM_SANITIZE_STR);
#ifdef PICODECK_SIM_FIRMWARE_NET
            // test_network_firmware.py runs only against this build.
            printf("firmware_net=1\n");
#else
            printf("firmware_net=0\n");
#endif
            exit(0);
        } else if (strcmp(argv[i], "--sd-card") == 0 && i + 1 < argc) {
            strncpy(g_sd_card_path, argv[i + 1], sizeof(g_sd_card_path) - 1);
            g_sd_card_path[sizeof(g_sd_card_path) - 1] = '\0';
            i++;
        } else if (strcmp(argv[i], "--launch") == 0 && i + 1 < argc) {
            strncpy(g_launch_app, argv[i + 1], sizeof(g_launch_app) - 1);
            g_launch_app[sizeof(g_launch_app) - 1] = '\0';
            i++;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_tcp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--instance-id") == 0 && i + 1 < argc) {
            strncpy(g_instance_id, argv[i + 1], sizeof(g_instance_id) - 1);
            g_instance_id[sizeof(g_instance_id) - 1] = '\0';
            i++;
        } else if (strcmp(argv[i], "--unix-socket") == 0 && i + 1 < argc) {
            snprintf(g_unix_socket, sizeof(g_unix_socket), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--crash-log") == 0 && i + 1 < argc) {
            strncpy(g_crash_log_path, argv[i + 1], sizeof(g_crash_log_path) - 1);
            g_crash_log_path[sizeof(g_crash_log_path) - 1] = '\0';
            i++;
        } else if (strcmp(argv[i], "--show-splash") == 0) {
            g_show_splash = 1;
        } else if (strcmp(argv[i], "--test-mode") == 0) {
            sim_set_test_mode(true);
        } else if (strcmp(argv[i], "--virtual-time") == 0) {
            g_virtual_time = 1;
        } else if (strcmp(argv[i], "--gamepad") == 0) {
            g_host_gamepad = 1;
        } else if (strcmp(argv[i], "--no-gamepad") == 0) {
            g_host_gamepad = 0;
        } else if (strcmp(argv[i], "--debug") == 0) {
            hal_set_debug_mode(1);
        } else if (strcmp(argv[i], "--real-umm") == 0) {
            // Before anything allocates: every umm_* call from here on goes
            // to the real umm heap.
            if (!sim_umm_use_real()) {
                fprintf(stderr, "--real-umm: cannot allocate the umm arena\n");
                exit(1);
            }
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }

    if (g_virtual_time && !sim_test_mode()) {
        fprintf(stderr, "--virtual-time requires --test-mode\n");
        exit(1);
    }
    hal_timing_set_virtual(g_virtual_time != 0);

    // Default the crash log to a per-process path so parallel instances never
    // read each other's crashes, and a stale file cannot outlive the process.
    if (g_crash_log_path[0] == '\0') {
        snprintf(g_crash_log_path, sizeof(g_crash_log_path),
                 "/tmp/picodeck_sim_crash_%d.log", (int)getpid());
    }
    // A previous process may have died holding this PID's name.
    unlink(g_crash_log_path);
}

// Draw splash screen with status message
static void draw_splash_screen(const char* status, const char* subtext) {
    // Clear to black
    uint16_t* fb = hal_display_get_framebuffer();
    for (int i = 0; i < 320 * 320; i++) {
        fb[i] = 0x0000;  // Black in RGB565
    }

#if LOGO_W > 0 && LOGO_H > 0
    // Draw logo centered
    int lx = (320 - LOGO_W) / 2;
    int ly = (320 - LOGO_H) / 2 - 16;

    // Draw the logo pixel by pixel
    for (int dy = 0; dy < LOGO_H && ly + dy < 320; dy++) {
        for (int dx = 0; dx < LOGO_W && lx + dx < 320; dx++) {
            if (lx + dx >= 0 && ly + dy >= 0) {
                fb[(ly + dy) * 320 + (lx + dx)] = logo_data[dy * LOGO_W + dx];
            }
        }
    }

    // Draw status text (using simple font approximation)
    if (status && status[0]) {
        // Draw status at bottom of screen
        // For now, we'll skip text drawing since we don't have a simple text renderer here
        // The visual logo is sufficient for the splash screen
        (void)status;
        (void)subtext;
    }
#endif

    // Present to screen
    hal_display_present();
}

// Show boot splash sequence with artificial delays
static void show_boot_splash(void) {
    printf("[Core0] Showing boot splash screen...\n");
    fflush(stdout);

    // Initial splash - "PicoDeck Simulator"
    draw_splash_screen("PicoDeck Simulator", "Starting up...");
    hal_sleep_ms(800);

    // Show initialization steps like hardware does
    draw_splash_screen("Initialising keyboard...", NULL);
    hal_sleep_ms(600);

    draw_splash_screen("Mounting SD card...", NULL);
    hal_sleep_ms(800);

    draw_splash_screen("Initialising WiFi...", NULL);
    hal_sleep_ms(600);

    draw_splash_screen("Loading...", NULL);
    hal_sleep_ms(500);

    printf("[Core0] Splash sequence complete\n");
    fflush(stdout);
}

// One Core 1 tick, with no sleep. core1_thread repeats it every 5 ms; the web
// build has no threads, so its web_core1_tick() paces it instead (see
// hal/web_platform.h). A new Core 1 duty goes here, so it runs in both.
void sim_core1_service(void) {
    // Update audio
    hal_audio_update();

    // MOD player: render PCM and push to audio stream
    mod_player_update();

    // Network polling — drain IPC queue, run curl_multi, poll TCP sockets
    wifi_poll();
    http_fire_c_pending();
}

// core1_stats.h, for `audiostat` (src/dev_ops.c): the desktop Core 1
// thread's service passes, timed on the host's monotonic clock (Core 1 is
// never on virtual time). Nothing merges ticks here, so `missed` stays 0,
// and a tick is 5 ms, not the device's 1 ms. The web build's cooperative
// tick (web_core1_tick) is not timed: its counters stay 0.
static _Atomic uint32_t s_c1_ticks, s_c1_over, s_c1_max_us;
static _Atomic uint64_t s_c1_window_t0_us;
static atomic_bool s_c1_reset_req;

static uint64_t core1_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void core1_get_tick_stats(core1_tick_stats_t *out) {
    // A reset the Core 1 thread has not carried out yet reads as the empty
    // window it asked for.
    bool reset = atomic_load(&s_c1_reset_req);
    out->window_ms = reset ? 0
        : (uint32_t)((core1_now_us() - atomic_load(&s_c1_window_t0_us)) / 1000);
    out->ticks = reset ? 0 : atomic_load(&s_c1_ticks);
    out->over = reset ? 0 : atomic_load(&s_c1_over);
    out->missed = 0;
    out->max_us = reset ? 0 : atomic_load(&s_c1_max_us);
}

void core1_reset_tick_stats(void) {
    atomic_store(&s_c1_reset_req, true);
}

// One timed service pass (the Core 1 thread only).
static void core1_timed_service(void) {
    if (atomic_exchange(&s_c1_reset_req, false)) {
        atomic_store(&s_c1_ticks, 0);
        atomic_store(&s_c1_over, 0);
        atomic_store(&s_c1_max_us, 0);
        atomic_store(&s_c1_window_t0_us, core1_now_us());
    }
    uint64_t t0 = core1_now_us();
    sim_core1_service();
    uint32_t dt = (uint32_t)(core1_now_us() - t0);
    atomic_fetch_add(&s_c1_ticks, 1);
    if (dt > 1000)
        atomic_fetch_add(&s_c1_over, 1);
    if (dt > atomic_load(&s_c1_max_us))
        atomic_store(&s_c1_max_us, dt);
}

// Core 1 entry point (simulates the second core)
static void* core1_thread(void* arg) {
    (void)arg;
#ifdef PICODECK_SIM_FIRMWARE_NET
    // Firmware wifi_poll() only runs on Core 1 (get_core_num() == 1).
    sim_net_core1_init();
#endif
    printf("[Core1] Started (network/audio thread)\n");
    
    // Initialize audio
    hal_audio_init();
    
    // Core 1 main loop
    atomic_store(&s_c1_window_t0_us, core1_now_us());
    while (g_running) {
        core1_timed_service();

        // 5ms delay (same as hardware)
        hal_sleep_ms(5);
    }
    
    printf("[Core1] Shutting down\n");
    hal_audio_shutdown();
    return NULL;
}

// ── g_api wiring (required by lua_bridge_sound, lua_bridge_network, etc.) ─────

// g_api is DEFINED once in stubs/driver_stubs.c and declared extern in os.h.
// A second definition here was an ODR violation: two translation units each
// claiming the storage. It happened to be harmless only because both saw the
// same 160-byte layout — if the os.h copies ever drift, the two definitions
// disagree on the size of the same object and writes run past it.

// -- Sound player wrappers (same pattern as src/main.c) --

static pcsound_sample_t sp_sampleLoad(const char *path) {
    sound_sample_t *s = sound_sample_create();
    if (!s) return NULL;
    if (!sound_sample_load(s, path)) { sound_sample_destroy(s); return NULL; }
    return (pcsound_sample_t)s;
}
static void sp_sampleFree(pcsound_sample_t s) { sound_sample_destroy((sound_sample_t *)s); }
static pcsound_player_t sp_playerNew(void) { return (pcsound_player_t)sound_player_create(); }
static void sp_playerSetSample(pcsound_player_t p, pcsound_sample_t s) { sound_player_set_sample((sound_player_t *)p, (sound_sample_t *)s); }
static void sp_playerPlay(pcsound_player_t p, uint8_t repeat) { sound_player_play((sound_player_t *)p, repeat); }
static void sp_playerStop(pcsound_player_t p) { sound_player_stop((sound_player_t *)p); }
static bool sp_playerIsPlaying(pcsound_player_t p) { return sound_player_is_playing((const sound_player_t *)p); }
static uint8_t sp_playerGetVolume(pcsound_player_t p) { return sound_player_get_volume((const sound_player_t *)p); }
static void sp_playerSetVolume(pcsound_player_t p, uint8_t vol) { sound_player_set_volume((sound_player_t *)p, vol); }
static void sp_playerSetLoop(pcsound_player_t p, bool loop) { ((sound_player_t *)p)->repeat_count = loop ? 255 : 0; }
static void sp_playerFree(pcsound_player_t p) { sound_player_destroy((sound_player_t *)p); }

static pcfileplayer_t sp_filePlayerNew(void) { return (pcfileplayer_t)fileplayer_create(); }
static void sp_filePlayerLoad(pcfileplayer_t fp, const char *path) { fileplayer_load((fileplayer_t *)fp, path); }
static void sp_filePlayerPlay(pcfileplayer_t fp, uint8_t repeat) { fileplayer_play((fileplayer_t *)fp, repeat); }
static void sp_filePlayerStop(pcfileplayer_t fp) { fileplayer_stop((fileplayer_t *)fp); }
static void sp_filePlayerPause(pcfileplayer_t fp) { fileplayer_pause((fileplayer_t *)fp); }
static void sp_filePlayerResume(pcfileplayer_t fp) { fileplayer_resume((fileplayer_t *)fp); }
static bool sp_filePlayerIsPlaying(pcfileplayer_t fp) { return fileplayer_is_playing((const fileplayer_t *)fp); }
static void sp_filePlayerSetVolume(pcfileplayer_t fp, uint8_t vol) { fileplayer_set_volume((fileplayer_t *)fp, vol, vol); }
static uint8_t sp_filePlayerGetVolume(pcfileplayer_t fp) { uint8_t l=0,r=0; fileplayer_get_volume((const fileplayer_t *)fp,&l,&r); return l; }
static uint32_t sp_filePlayerGetOffset(pcfileplayer_t fp) { return fileplayer_get_offset((const fileplayer_t *)fp); }
static void sp_filePlayerSetOffset(pcfileplayer_t fp, uint32_t pos) { fileplayer_set_offset((fileplayer_t *)fp, pos); }
static bool sp_filePlayerDidUnderrun(pcfileplayer_t fp) { return fileplayer_did_underrun((fileplayer_t *)fp); }
static void sp_filePlayerFree(pcfileplayer_t fp) { fileplayer_destroy((fileplayer_t *)fp); }

static pcmp3player_t sp_mp3PlayerNew(void) { return (pcmp3player_t)mp3_player_create(); }
static void sp_mp3PlayerLoad(pcmp3player_t mp, const char *path) { mp3_player_load((mp3_player_t *)mp, path); }
static void sp_mp3PlayerPlay(pcmp3player_t mp, uint8_t repeat) {
    // 0 = loop until stopped, any other count plays once (as Lua's play()).
    mp3_player_set_loop((mp3_player_t *)mp, repeat == 0);
    mp3_player_play((mp3_player_t *)mp, repeat);
}
static void sp_mp3PlayerStop(pcmp3player_t mp) { mp3_player_stop((mp3_player_t *)mp); }
static void sp_mp3PlayerPause(pcmp3player_t mp) { mp3_player_pause((mp3_player_t *)mp); }
static void sp_mp3PlayerResume(pcmp3player_t mp) { mp3_player_resume((mp3_player_t *)mp); }
static bool sp_mp3PlayerIsPlaying(pcmp3player_t mp) { return mp3_player_is_playing((const mp3_player_t *)mp); }
static void sp_mp3PlayerSetVolume(pcmp3player_t mp, uint8_t vol) { mp3_player_set_volume((mp3_player_t *)mp, vol); }
static uint8_t sp_mp3PlayerGetVolume(pcmp3player_t mp) { return mp3_player_get_volume((const mp3_player_t *)mp); }
static void sp_mp3PlayerSetLoop(pcmp3player_t mp, bool loop) { mp3_player_set_loop((mp3_player_t *)mp, loop); }
static void sp_mp3PlayerFree(pcmp3player_t mp) { mp3_player_destroy((mp3_player_t *)mp); }

static const picocalc_soundplayer_t s_soundplayer_impl = {
    .sampleLoad = sp_sampleLoad, .sampleFree = sp_sampleFree,
    .playerNew = sp_playerNew, .playerSetSample = sp_playerSetSample,
    .playerPlay = sp_playerPlay, .playerStop = sp_playerStop,
    .playerIsPlaying = sp_playerIsPlaying, .playerGetVolume = sp_playerGetVolume,
    .playerSetVolume = sp_playerSetVolume, .playerSetLoop = sp_playerSetLoop,
    .playerFree = sp_playerFree,
    .filePlayerNew = sp_filePlayerNew, .filePlayerLoad = sp_filePlayerLoad,
    .filePlayerPlay = sp_filePlayerPlay, .filePlayerStop = sp_filePlayerStop,
    .filePlayerPause = sp_filePlayerPause, .filePlayerResume = sp_filePlayerResume,
    .filePlayerIsPlaying = sp_filePlayerIsPlaying, .filePlayerSetVolume = sp_filePlayerSetVolume,
    .filePlayerGetVolume = sp_filePlayerGetVolume, .filePlayerGetOffset = sp_filePlayerGetOffset,
    .filePlayerSetOffset = sp_filePlayerSetOffset, .filePlayerDidUnderrun = sp_filePlayerDidUnderrun,
    .filePlayerFree = sp_filePlayerFree,
    .mp3PlayerNew = sp_mp3PlayerNew, .mp3PlayerLoad = sp_mp3PlayerLoad,
    .mp3PlayerPlay = sp_mp3PlayerPlay, .mp3PlayerStop = sp_mp3PlayerStop,
    .mp3PlayerPause = sp_mp3PlayerPause, .mp3PlayerResume = sp_mp3PlayerResume,
    .mp3PlayerIsPlaying = sp_mp3PlayerIsPlaying, .mp3PlayerSetVolume = sp_mp3PlayerSetVolume,
    .mp3PlayerGetVolume = sp_mp3PlayerGetVolume, .mp3PlayerSetLoop = sp_mp3PlayerSetLoop,
    .mp3PlayerFree = sp_mp3PlayerFree,
};

// -- HTTP wrappers --

static pchttp_t http_newConn_w(const char *server, uint16_t port, bool use_ssl) {
    http_conn_t *c = http_alloc();
    if (!c) return NULL;
    strncpy(c->server, server, HTTP_SERVER_MAX - 1);
    c->server[HTTP_SERVER_MAX - 1] = '\0';
    c->port = port ? port : (use_ssl ? 443 : 80);  // 0 = scheme default
    c->use_ssl = use_ssl;
    return (pchttp_t)c;
}
static void http_get_w(pchttp_t c, const char *path, const char *extra_hdrs) { http_get((http_conn_t *)c, path, extra_hdrs); }
static void http_post_w(pchttp_t c, const char *path, const char *extra_hdrs, const char *body, uint32_t body_len) { http_post((http_conn_t *)c, path, extra_hdrs, body, (size_t)body_len); }
static int http_read_w(pchttp_t c, uint8_t *buf, uint32_t len) { return (int)http_read((http_conn_t *)c, buf, len); }
static uint32_t http_available_w(pchttp_t c) { return http_bytes_available((http_conn_t *)c); }
static void http_close_w(pchttp_t c) { http_free((http_conn_t *)c); }
static int http_getStatus_w(pchttp_t c) { return http_get_status((http_conn_t *)c); }
static const char *http_getError_w(pchttp_t c) { return http_get_error((http_conn_t *)c); }
static int http_getProgress_w(pchttp_t c, int *received, int *total) { int r, t; http_get_progress((http_conn_t *)c, &r, &t); if (received) *received = r; if (total) *total = t; return t; }
static void http_setKeepAlive_w(pchttp_t c, bool ka) { ((http_conn_t *)c)->keep_alive = ka; }
static void http_setByteRange_w(pchttp_t c, int from, int to) { ((http_conn_t *)c)->range_from = from; ((http_conn_t *)c)->range_to = to; }
static void http_setConnectTimeout_w(pchttp_t c, int s) { ((http_conn_t *)c)->connect_timeout_ms = (uint32_t)(s * 1000); }
static void http_setReadTimeout_w(pchttp_t c, int s) { ((http_conn_t *)c)->read_timeout_ms = (uint32_t)(s * 1000); }
static bool http_setReadBufferSize_w(pchttp_t c, int bytes) { return http_set_recv_buf((http_conn_t *)c, (uint32_t)bytes); }
static bool http_isComplete_w(pchttp_t c) { return http_is_complete((http_conn_t *)c); }
// The simulator's libcurl transport keeps its own TLS policy; the flag is
// stored so the API behaves the same (see src/drivers/wifi.c for firmware).
static void http_setInsecure_w(pchttp_t c, bool insecure) { ((http_conn_t *)c)->insecure = insecure; }

static const picocalc_http_t s_http_impl = {
    .newConn = http_newConn_w, .get = http_get_w, .post = http_post_w,
    .read = http_read_w, .available = http_available_w, .close = http_close_w,
    .getStatus = http_getStatus_w, .getError = http_getError_w,
    .getProgress = http_getProgress_w, .setKeepAlive = http_setKeepAlive_w,
    .setByteRange = http_setByteRange_w, .setConnectTimeout = http_setConnectTimeout_w,
    .setReadTimeout = http_setReadTimeout_w, .setReadBufferSize = http_setReadBufferSize_w,
    .isComplete = http_isComplete_w, .setInsecure = http_setInsecure_w,
};

// -- App config wrappers --

static const picocalc_appconfig_t s_appconfig_impl = {
    .load = appconfig_load, .save = appconfig_save,
    .get = appconfig_get, .set = appconfig_set,
    .clear = appconfig_clear, .reset = appconfig_reset,
    .getAppId = appconfig_get_app_id,
};

// -- Gamepad (the keyboard stub's masks, the shared label lookup) --

static const picocalc_gamepad_t s_gamepad_impl = {
    .getButtons = kbd_get_pad, .getButtonsPressed = kbd_get_pad_pressed,
    .getButtonsReleased = kbd_get_pad_released, .getLabel = gamepad_get_label,
};

static void sim_wire_g_api(void) {
    memset(&g_api, 0, sizeof(g_api));
    g_api.soundplayer = &s_soundplayer_impl;
    g_api.http        = &s_http_impl;
    g_api.appconfig   = &s_appconfig_impl;
    g_api.version     = 9;  // 9 = gamepad; 8 = http->setInsecure, tcp->connectEx; 7 = video seek/OSD; 6 = fonts (matches src/main.c)
    g_api.gamepad     = &s_gamepad_impl;
}

int main(int argc, char** argv) {
    printf("PicoDeck Simulator v%s\n", PICODECK_VERSION);
    printf("=====================\n\n");
    
    // Parse arguments
    parse_args(argc, argv);
    
    printf("Configuration:\n");
    printf("  SD Card: %s\n", g_sd_card_path);
    printf("  Display: 960x960 (3x scale)\n");
    printf("  Audio: SDL2\n");
    printf("  Threading: Dual-core simulation\n\n");
    
#ifndef __EMSCRIPTEN__
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Crash handlers — write backtrace to crash log on fatal signals. A
    // sanitizer build leaves them to the sanitizer runtime, whose own handler
    // prints the symbolised report the E2E harness scans stderr for.
#if !SIM_HAS_SANITIZER
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGFPE, crash_handler);
#else
    (void)crash_handler;
#endif  // !SIM_HAS_SANITIZER
#endif
    
    // Initialize SDL
#ifdef __EMSCRIPTEN__
    // Read keys from the canvas, not the whole window: keys reach the simulator
    // while it has focus, and the page around it (picodeck.net) still scrolls
    // and takes typing. simulator/web/shell.js sends the touch pad's keys there.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    
    // Initialize subsystems
    if (!hal_display_init(SIM_WINDOW_TITLE)) {
        fprintf(stderr, "Display initialization failed\n");
        SDL_Quit();
        return 1;
    }

    // Show boot splash if requested (before other init for visibility)
    if (g_show_splash) {
        show_boot_splash();
    }

    if (!hal_input_init()) {
        fprintf(stderr, "Input initialization failed\n");
        hal_display_shutdown();
        SDL_Quit();
        return 1;
    }
    // The host's game controllers as a gamepad source (hal_pad.c). Off in
    // --test-mode unless asked for, so a test never sees whatever controller
    // the host has; the web build has none.
#ifdef __EMSCRIPTEN__
    hal_pad_init(false);
#else
    hal_pad_init(g_host_gamepad < 0 ? !sim_test_mode() : g_host_gamepad != 0);
#endif
    
#ifdef __EMSCRIPTEN__
    web_fs_init(g_sd_card_path);
#endif
    if (!hal_sdcard_init(g_sd_card_path)) {
        fprintf(stderr, "SD card initialization failed\n");
        hal_input_shutdown();
        hal_display_shutdown();
        SDL_Quit();
        return 1;
    }
    
    if (!hal_psram_init()) {
        fprintf(stderr, "PSRAM initialization failed\n");
        hal_sdcard_shutdown();
        hal_input_shutdown();
        hal_display_shutdown();
        SDL_Quit();
        return 1;
    }
    
    hal_timing_init();

    // --test-mode pins the wall clock to a fixed date (getClock, the header
    // clock); it then runs with the sim clock. The SIM_FIRMWARE_NET build's
    // SNTP stand-in may still re-sync it once the network is up.
    if (sim_test_mode())
        clock_sntp_set(SIM_TEST_EPOCH);

    // Wire global API struct (required by Lua bridge modules)
    sim_wire_g_api();

    // Initialize Lua heap (required for Lua apps)
    lua_psram_alloc_init();
    printf("[Core0] Lua heap initialized\n");
    fflush(stdout);

    // ── Boot order below mirrors src/main.c: config, idle dim, network,
    //    Core 1, system menu, launcher. ──

    // Load persisted settings from /system/config.json
    config_load();

    // Idle screen dimming. The keyboard stub never polls it, so it stays
    // inert in the simulator; --test-mode disables it outright.
    {
        uint32_t dim_timeout_s = 60;
        const char *dt = config_get("dim_timeout_s");
        if (dt)
            dim_timeout_s = (uint32_t)atoi(dt);
        if (sim_test_mode())
            dim_timeout_s = 0;
        uint8_t brightness = config_parse_brightness(config_get("brightness"));
        kbd_set_backlight(brightness);
        idle_dim_init(brightness, dim_timeout_s);
    }

    // Sample players and the mixer's own sources (audio_mix.c), mirroring
    // src/main.c: before Core 1 starts pulling hal_audio_update(). The MP3
    // player self-initializes lazily on first use, as on the device.
    sound_init();
    audio_init();

    // Initialize toast system and networking (before Core 1 starts)
    extern void toast_init(void);
    extern void http_init(void);
    extern void tcp_init(void);
    extern void wifi_init(void);
    toast_init();
    wifi_init();
    http_init();
    tcp_init();
#ifdef PICODECK_SIM_FIRMWARE_NET
    // Firmware network stack (simulator/net): join the stand-in network now,
    // as sim_wifi.c's mock is "always online", unless config.json's
    // wifi_ssid already started the firmware's own boot auto-connect.
    if (wifi_get_status() == WIFI_STATUS_DISCONNECTED)
        wifi_connect("SimulatorWiFi", "");
#endif

#ifdef __EMSCRIPTEN__
    // No threads in the browser: the web sources' web_core1_tick() runs
    // sim_core1_service() cooperatively from every yield point.
    hal_audio_init();
#else
    // Start Core 1 thread (simulates second core)
    thread_t core1;
    if (!hal_thread_create(&core1, core1_thread, NULL)) {
        fprintf(stderr, "Failed to create Core 1 thread\n");
        hal_psram_shutdown();
        hal_sdcard_shutdown();
        hal_input_shutdown();
        hal_display_shutdown();
        SDL_Quit();
        return 1;
    }
#endif  // __EMSCRIPTEN__

    printf("[Core0] Starting main loop...\n");
    fflush(stdout);

    // Set up exit flag for keyboard stub
    set_simulator_exit_flag(&g_running);

    system_menu_init();

    printf("[Core0] Starting launcher...\n");
    fflush(stdout);

    extern void dev_commands_init(void);
    dev_commands_init();

#ifndef __EMSCRIPTEN__
    sim_socket_init(g_tcp_port, g_instance_id[0] ? g_instance_id : NULL,
                    g_unix_socket[0] ? g_unix_socket : NULL);
#endif

    launcher_run();
    printf("[Core0] Launcher exited, setting g_running=0\n");
    fflush(stdout);
    g_running = 0;

#ifdef __EMSCRIPTEN__
    // Nothing to join or close; leave the last frame on the canvas.
    return 0;
#else
    // Arm a forced-exit timeout so cleanup can't hang forever
    signal(SIGALRM, force_exit_handler);
    alarm(3);

    printf("\n[Core0] Shutting down...\n");

    // Wait for Core 1 to finish
    hal_thread_join(&core1);

    // Cleanup — stop socket thread first, then close sockets
    sim_socket_shutdown();
    sim_socket_close();
    hal_psram_shutdown();
    hal_sdcard_shutdown();
    hal_pad_shutdown();
    hal_input_shutdown();
    hal_display_shutdown();
    SDL_Quit();

    printf("Simulator exited cleanly.\n");
    return 0;
#endif
}
