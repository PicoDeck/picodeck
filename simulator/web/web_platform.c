// Web (Emscripten) platform glue.
//
// The desktop simulator runs Core 1 on a pthread and lets the OS block freely
// (launcher_run() never returns; apps spin in their own loops). A browser tab
// has one thread and freezes unless code returns to the event loop, so here:
//
//   * Core 1's 5 ms service loop becomes web_core1_tick(), run cooperatively.
//   * Every place the OS already waits (hal_sleep_*, display present, the Lua
//     watchdog hook, kbd_poll) calls web_yield*(), which uses ASYNCIFY's
//     emscripten_sleep() to unwind to the browser and resume afterwards.
//
// No core OS file needs to know about any of this.

#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/web_platform.h"
#include "sim_socket.h"  // sim_socket_notify* stubs below

extern void hal_audio_update(void);
extern void mod_player_update(void);
extern void wifi_poll(void);

// Longest the OS may run without handing the tab back to the browser. Longer
// than a frame so apps that flush every frame are paced by present, not this.
#define WEB_MAX_BUSY_MS 20
#define WEB_CORE1_PERIOD_MS 5

static double s_last_yield_ms;
static double s_last_core1_ms;
static bool s_in_core1;

void web_core1_tick(void) {
    double now = emscripten_get_now();
    if (s_in_core1 || now - s_last_core1_ms < WEB_CORE1_PERIOD_MS) return;
    s_in_core1 = true;
    s_last_core1_ms = now;
    hal_audio_update();
    mod_player_update();
    wifi_poll();
    s_in_core1 = false;
}

void web_yield(uint32_t ms) {
    web_core1_tick();
    emscripten_sleep(ms);
    s_last_yield_ms = emscripten_get_now();
    web_core1_tick();
}

void web_yield_if_due(void) {
    web_core1_tick();
    if (emscripten_get_now() - s_last_yield_ms >= WEB_MAX_BUSY_MS) web_yield(0);
}

// ── Persistent saves ─────────────────────────────────────────────────────────
// /sd/data (per-app config + game saves) lives in IndexedDB via IDBFS; the rest
// of the SD image is the read-only preload. Loaded before the SD card mounts,
// written back every 2 s and whenever the page is hidden. If IndexedDB is
// unavailable (some private windows) saves just stay in memory for the session.
EM_ASYNC_JS(void, web_fs_init_js, (const char *dir_c), {
  var dir = UTF8ToString(dir_c);
  var syncing = false;
  function sync(populate) {
    return new Promise(function (resolve) {
      if (syncing) return resolve();
      syncing = true;
      FS.syncfs(populate, function (err) {
        syncing = false;
        if (err) console.warn('[WEB] save sync failed:', err);
        resolve();
      });
    });
  }
  try {
    FS.mkdirTree(dir);
    FS.mount(IDBFS, {}, dir);
  } catch (e) {
    console.warn('[WEB] persistent saves unavailable:', e);
    return;
  }
  await sync(true);
  setInterval(function () { sync(false); }, 2000);
  document.addEventListener('visibilitychange', function () {
    if (document.visibilityState === 'hidden') sync(false);
  });
  window.addEventListener('pagehide', function () { sync(false); });
});

void web_fs_init(const char *sd_root) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/data", sd_root);
    web_fs_init_js(dir);
    printf("[WEB] Saves persisted in browser storage: %s\n", dir);
}

// ── Soft-keyboard text ───────────────────────────────────────────────────────
// SDL derives characters from the unshifted US key (Shift+= arrives as '='), so
// the page's phone-keyboard path types exact ASCII straight into the char queue.
extern void hal_input_inject_char(char c);

EMSCRIPTEN_KEEPALIVE void web_type_char(int c) {
    if (c >= 32 && c < 127) hal_input_inject_char((char)c);
}

// ── sim_socket_handler.c hooks (no RPC socket in the browser) ────────────────
// sim_handler_check_launch and sim_log_append come from sim_test_control.c,
// which the web build compiles (it has no socket code of its own).

// sim_socket.c is not built: sim_test_control.c's notifications go nowhere.
void sim_socket_notify(const char *method, const char *params_json) {
    (void)method; (void)params_json;
}
void sim_socket_notify_log_subscribers(const char *json_line, size_t len) {
    (void)json_line; (void)len;
}

static void *s_active_terminal;
void sim_set_active_terminal(void *term) { s_active_terminal = term; }
void *sim_get_active_terminal(void) { return s_active_terminal; }

// Native ELF apps need Unicorn (a JIT), which cannot run inside WASM.
bool unicorn_run_app(const char *elf_path, const char *app_dir,
                     const char *app_id, const char *app_name) {
    (void)elf_path; (void)app_dir; (void)app_id;
    printf("[WEB] Native app '%s' is not supported in the web demo\n", app_name);
    return false;
}
