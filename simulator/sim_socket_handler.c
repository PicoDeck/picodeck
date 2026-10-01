#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "sim_socket.h"
#include "sim_socket_handler.h"
#include "sim_test_control.h"
#include "hal/hal_sdcard.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_audio.h"
#include "hal/hal_psram.h"
#include "hal/hal_timing.h"
#include "drivers/keyboard.h"
#include "drivers/audio_mix.h"
#include "drivers/sound.h"
#include "drivers/mp3_player.h"
#include "os/core0_idle.h"
#include "os/launcher.h"
#include "os/lua_psram_alloc.h"
#include "os/screenshot.h"
#include "os/os.h"
#include "sim_wifi.h"
#include "os/terminal.h"
#include "dev_ops.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <pthread.h>

// Software PNG encoder fallback for headless mode where SDL_image may not work
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

// The log ring, launch slot and app outcome live in sim_test_control.c.

// ── Active terminal tracking ─────────────────────────────────────────────────

static void *s_active_terminal = NULL;

void sim_set_active_terminal(void *term) { s_active_terminal = term; }
void *sim_get_active_terminal(void) { return s_active_terminal; }

// ── WiFi error injection ──────────────────────────────────────────────────────

typedef enum {
    WIFI_NORMAL,
    WIFI_DISCONNECTED,
    WIFI_NOT_AVAILABLE,
    WIFI_ERROR,
} wifi_error_mode_t;

static struct {
    bool enabled;
    wifi_error_mode_t mode;
    int error_code;
    const char *error_str;
} s_wifi_error = {0};

// set_wifi_state "connecting" / "failed" pin the status sim_wifi.c reports
// (-1 = none), so tests can see those states; network access is unchanged.
static int s_wifi_forced_status = -1;

int sim_wifi_forced_status(void) {
    return __atomic_load_n(&s_wifi_forced_status, __ATOMIC_RELAXED);
}

bool sim_wifi_is_available(void) {
    if (!s_wifi_error.enabled) return true;
    return s_wifi_error.mode != WIFI_DISCONNECTED &&
           s_wifi_error.mode != WIFI_NOT_AVAILABLE;
}

bool sim_network_blocked(void) {
    if (!s_wifi_error.enabled) return false;
    return s_wifi_error.mode == WIFI_DISCONNECTED ||
           s_wifi_error.mode == WIFI_NOT_AVAILABLE ||
           s_wifi_error.mode == WIFI_ERROR;
}

const char *sim_wifi_get_error(void) {
    return s_wifi_error.error_str ? s_wifi_error.error_str : "network error";
}

// ── Path sandboxing ───────────────────────────────────────────────────────────

// NOTE ON BUFFER SIZES: every realpath() destination below must be at least
// PATH_MAX bytes. glibc's _FORTIFY_SOURCE wrapper __realpath_chk() aborts the
// process when it can see at compile time that the destination is smaller —
// unconditionally, regardless of how short the actual path is. These buffers
// were 1024 bytes, which aborted every sandboxed request on distros that
// fortify by default (Ubuntu/CI) while working fine on those that do not
// (Fedora). Never realpath() into a buffer of unknown or sub-PATH_MAX size.
static bool sandbox_path(const char *path, char *out_resolved, size_t max) {
    extern char g_base_path[512];
    // Resolve base path to absolute for consistent comparison with realpath output
    char abs_base[PATH_MAX];
    if (!realpath(g_base_path, abs_base)) return false;
    size_t base_len = strlen(abs_base);

    char full[PATH_MAX];
    if (path[0] == '/') {
        snprintf(full, sizeof(full), "%s%s", abs_base, path);
    } else {
        snprintf(full, sizeof(full), "%s/%s", abs_base, path);
    }
    // Try realpath first (works for existing files). Resolve into a local
    // PATH_MAX buffer rather than the caller's, whose size we only know as
    // `max` — which may legitimately be smaller than PATH_MAX.
    char tmp[PATH_MAX];
    if (realpath(full, tmp)) {
        if (strncmp(tmp, abs_base, base_len) != 0) return false;
        if (strlen(tmp) >= max) return false;  // refuse to truncate a sandboxed path
        memcpy(out_resolved, tmp, strlen(tmp) + 1);
        return true;
    }
    // For new files: resolve the parent directory and append the filename.
    // `full` is no longer needed, so reuse it as the parent scratch buffer.
    char *slash = strrchr(full, '/');
    if (!slash) return false;
    char filename[256];
    snprintf(filename, sizeof(filename), "%s", slash + 1);
    *slash = '\0';
    char resolved_parent[PATH_MAX];
    if (!realpath(full, resolved_parent)) return false;
    if (strncmp(resolved_parent, abs_base, base_len) != 0) return false;
    int n = snprintf(out_resolved, max, "%s/%s", resolved_parent, filename);
    if (n < 0 || (size_t)n >= max) return false;  // truncated — reject
    return true;
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

static bool json_get_str(const char *obj, const char *key, char *out, size_t max) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static bool json_get_int(const char *obj, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    *out = atoi(p);
    return true;
}

static bool json_get_uint(const char *obj, const char *key, unsigned int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    *out = (unsigned int)atoi(p);
    return true;
}

static bool json_get_float(const char *obj, const char *key, float *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    *out = (float)atof(p);
    return true;
}

static bool json_get_bool(const char *obj, const char *key, bool *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static void json_escape(const char *src, char *out, size_t max) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 5 < max; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else { out[j++] = c; }
    }
    out[j] = '\0';
}

// ── Base64 encoding (minimal) ─────────────────────────────────────────────────

static const char s_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *src, size_t len, char *out) {
    size_t i = 0, j = 0;
    while (i < len) {
        size_t remain = len - i;
        uint32_t b = (uint32_t)src[i++] << 16;
        if (remain > 1) b |= (uint32_t)src[i++] << 8;
        if (remain > 2) b |= (uint32_t)src[i++] << 0;
        out[j++] = s_b64[(b >> 18) & 0x3F];
        out[j++] = s_b64[(b >> 12) & 0x3F];
        out[j++] = remain > 1 ? s_b64[(b >>  6) & 0x3F] : '=';
        out[j++] = remain > 2 ? s_b64[(b >>  0) & 0x3F] : '=';
    }
    out[j] = '\0';
    return (int)j;
}

static int b64_enc_size(size_t len) {
    return (int)(((len + 2) / 3) * 4 + 1);
}

// ── Screenshot to PNG base64 ──────────────────────────────────────────────────

// stb_image_write callback: appends to a growable buffer
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} stb_png_buf_t;

static void stb_png_write_func(void *context, void *data, int size) {
    stb_png_buf_t *buf = (stb_png_buf_t *)context;
    if (buf->size + (size_t)size > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap < buf->size + (size_t)size) new_cap = buf->size + (size_t)size;
        uint8_t *tmp = realloc(buf->data, new_cap);
        if (!tmp) return;
        buf->data = tmp;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, data, (size_t)size);
    buf->size += (size_t)size;
}

// Convert RGB565 framebuffer to RGB888
static uint8_t *rgb565_to_rgb888(const uint16_t *fb, int w, int h) {
    uint8_t *rgb = malloc((size_t)(w * h * 3));
    if (!rgb) return NULL;
    for (int i = 0; i < w * h; i++) {
        uint16_t p = fb[i];
        rgb[i * 3 + 0] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        rgb[i * 3 + 1] = (uint8_t)(((p >> 5)  & 0x3F) * 255 / 63);
        rgb[i * 3 + 2] = (uint8_t)((p & 0x1F) * 255 / 31);
    }
    return rgb;
}

static char *take_screenshot_png(int *out_len) {
    uint16_t *fb = hal_display_get_framebuffer();
    if (!fb) { *out_len = 0; return NULL; }

    // Always use stb_image_write — the SDL_image path produces corrupt PNGs
    // when the simulator runs headless or with the dummy video driver.
    uint8_t *rgb = rgb565_to_rgb888(fb, 320, 320);
    if (!rgb) { *out_len = 0; return NULL; }

    stb_png_buf_t png = { .data = malloc(256 * 1024), .size = 0, .capacity = 256 * 1024 };
    if (!png.data) { free(rgb); *out_len = 0; return NULL; }

    int ok = stbi_write_png_to_func(stb_png_write_func, &png, 320, 320, 3, rgb, 320 * 3);
    free(rgb);
    if (!ok || png.size == 0) { free(png.data); *out_len = 0; return NULL; }

    char *b64 = malloc(b64_enc_size(png.size) + 1);
    if (!b64) { free(png.data); *out_len = 0; return NULL; }
    b64_encode(png.data, png.size, b64);
    free(png.data);
    *out_len = (int)strlen(b64);
    return b64;
}

// ── RGB565 framebuffer to base64 ───────────────────────────────────────────────

static char *framebuffer_base64(int *out_len) {
    uint16_t *fb = hal_display_get_framebuffer();
    if (!fb) { *out_len = 0; return NULL; }
    char *b64 = malloc(b64_enc_size(320 * 320 * 2) + 1);
    if (!b64) { *out_len = 0; return NULL; }
    b64_encode((const uint8_t *)fb, 320 * 320 * 2, b64);
    *out_len = (int)strlen(b64);
    return b64;
}

// ── Method handlers ────────────────────────────────────────────────────────────

static char *h_ping(const char *params) {
    (void)params;
    static char buf[256];
    uint32_t ms = hal_get_time_ms();
    snprintf(buf, sizeof(buf),
             "{\"result\":{\"version\":\"1.0.0\",\"uptime_ms\":%u}}", ms);
    return strdup(buf);
}

// Hands the name to the main thread (sim_handler_check_launch). The launch
// happens when the launcher loop next runs, i.e. after any running app exits;
// the outcome arrives as the app.exited notification.
static char *h_launch_app(const char *params) {
    char name[128] = {0};
    json_get_str(params, "name", name, sizeof(name));
    if (!name[0]) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"name required\"}}");
    }
    const char *running = launcher_get_running_app_name();
    bool busy = running && running[0];
    uint32_t launch_id = sim_launch_request(name, NULL);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"queued\":true,\"busy\":%s,"
             "\"launch_id\":%u}}",
             busy ? "true" : "false", launch_id);
    return strdup(buf);
}

// The app.exited params of the last finished launch ({"launch_id":0} if
// none). Backfill for clients whose app.exited notification was dropped.
static char *h_get_last_outcome(const char *params) {
    (void)params;
    char *outcome = sim_last_outcome_json();
    if (!outcome) return NULL;
    size_t n = strlen(outcome) + 32;
    char *buf = malloc(n);
    if (buf) snprintf(buf, n, "{\"jsonrpc\":\"2.0\",\"result\":%s}", outcome);
    free(outcome);
    return buf;
}

static char *h_rescan_apps(const char *params) {
    (void)params;
    sim_rescan_request();
    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"queued\":true}}");
}

static char *h_exit_app(const char *params) {
    (void)params;
    // Same semantics as the dev `exit` command (dev_ops.c): with no app
    // running it is an error reply, and the launcher drops the flag.
    char reply[DEV_OP_REPLY_MAX];
    bool ok = dev_op_exit(reply, sizeof(reply));
    // Also inject ESC key so native apps with input-based exit loops
    // (checking getButtonsPressed/getChar instead of shouldExit) will exit.
    if (ok) kbd_inject_buttons(BTN_ESC);
    char esc[2 * DEV_OP_REPLY_MAX];
    json_escape(reply, esc, sizeof(esc));
    size_t n = strlen(esc) + 96;
    char *buf = malloc(n);
    if (buf)
        snprintf(buf, n,
                 "{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":%s,\"message\":\"%s\"}}",
                 ok ? "true" : "false", esc);
    return buf;
}

// Run one dev-command line on Core 0 (see dev_commands_stub.c): the
// launcher loop or a running app's Lua pump executes it, like a line on the
// firmware's serial console. params: {"cmd": "...", "timeout_ms": N}.
static char *h_dev_command(const char *params) {
    char cmd[300] = {0};
    if (!json_get_str(params, "cmd", cmd, sizeof(cmd)) || !cmd[0])
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"cmd required\"}}");
    int timeout_ms = 15000;
    json_get_int(params, "timeout_ms", &timeout_ms);
    extern bool dev_commands_sim_run(const char *cmd, int timeout_ms,
                                     char *reply, size_t reply_len, bool *ok);
    char reply[DEV_OP_REPLY_MAX];
    bool ok = false;
    dev_commands_sim_run(cmd, timeout_ms, reply, sizeof(reply), &ok);
    char esc[2 * DEV_OP_REPLY_MAX];
    json_escape(reply, esc, sizeof(esc));
    size_t n = strlen(esc) + 96;
    char *buf = malloc(n);
    if (buf)
        snprintf(buf, n,
                 "{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":%s,\"output\":\"%s\"}}",
                 ok ? "true" : "false", esc);
    return buf;
}

static char *h_get_running_app(const char *params) {
    (void)params;
    // Always an object, so clients can .get("name") without a None check:
    // {"running": bool, "name": str | null}.
    const char *name = launcher_get_running_app_name();
    sim_strbuf_t sb = {0};
    if (name && name[0]) {
        sim_sb_appendf(&sb, "{\"jsonrpc\":\"2.0\",\"result\":{\"running\":true,\"name\":");
        sim_sb_append_json_str(&sb, name);
        sim_sb_appendf(&sb, "}}");
    } else {
        sim_sb_appendf(&sb, "{\"jsonrpc\":\"2.0\",\"result\":{\"running\":false,\"name\":null}}");
    }
    return sim_sb_finish(&sb);
}

static uint32_t button_name_to_mask(const char *button) {
    if (strcmp(button, "up") == 0) return BTN_UP;
    if (strcmp(button, "down") == 0) return BTN_DOWN;
    if (strcmp(button, "left") == 0) return BTN_LEFT;
    if (strcmp(button, "right") == 0) return BTN_RIGHT;
    if (strcmp(button, "enter") == 0) return BTN_ENTER;
    if (strcmp(button, "esc") == 0 || strcmp(button, "escape") == 0) return BTN_ESC;
    if (strcmp(button, "menu") == 0) return BTN_MENU;
    if (strcmp(button, "tab") == 0) return BTN_TAB;
    if (strcmp(button, "backspace") == 0 || strcmp(button, "bkspc") == 0) return BTN_BACKSPACE;
    if (strcmp(button, "del") == 0 || strcmp(button, "delete") == 0) return BTN_DEL;
    if (strcmp(button, "shift") == 0) return BTN_SHIFT;
    if (strcmp(button, "ctrl") == 0) return BTN_CTRL;
    if (strcmp(button, "sym") == 0 || strcmp(button, "fn") == 0) return BTN_FN;
    // F10 is the hardware Menu key; there are no BTN_F10+ constants.
    if (strcmp(button, "f10") == 0) return BTN_MENU;
    if (strncmp(button, "f", 1) == 0 && strlen(button) <= 3) {
        int f = atoi(button + 1);
        if (f >= 1 && f <= 9) return (uint32_t)(BTN_F1 << (f - 1));
    }
    return 0;
}

// Injections answer with the seq assigned to them; get_input_state reports
// when the OS has read it. RPCs run on one thread, so "last issued" is ours.
static char *input_seq_response(void) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true,\"input_seq\":%u}}",
             hal_input_last_issued_seq());
    return strdup(buf);
}

static char *h_inject_button(const char *params) {
    char button[32] = {0}, action[16] = {0};
    json_get_str(params, "button", button, sizeof(button));
    json_get_str(params, "action", action, sizeof(action));

    uint32_t btn_mask = button_name_to_mask(button);
    if (!btn_mask) {
        static char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"unknown button: %s\"}}", button);
        return strdup(buf);
    }

    if (strcmp(action, "release") == 0) {
        kbd_release_buttons(btn_mask);
    } else if (strcmp(action, "press") == 0) {
        // Hold until an explicit "release" — for modifier chords (ctrl+s etc.)
        kbd_hold_buttons(btn_mask);
        SDL_Delay(16);
    } else {
        // "click" / default: one-shot press, held ~HAL_INJECT_HOLD_MS ms before auto-release
        kbd_inject_buttons(btn_mask);
        SDL_Delay(16);
    }
    return input_seq_response();
}

static char *h_inject_char(const char *params) {
    char ch[2] = {0};
    json_get_str(params, "char", ch, 2);
    if (!ch[0]) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"char required\"}}");
    }
    kbd_inject_char(ch[0]);
    return input_seq_response();
}

// Highest input seq the OS has read (every seq <= consumed_seq was seen by
// kbd_poll / the char reader), and the last seq issued.
static char *h_get_input_state(const char *params) {
    (void)params;
    uint32_t issued = 0, consumed = 0;
    hal_input_get_seq_state(&issued, &consumed);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"issued_seq\":%u,\"consumed_seq\":%u}}",
             issued, consumed);
    return strdup(buf);
}

static char *h_screenshot(const char *params) {
    char format[16] = "png";
    json_get_str(params, "format", format, sizeof(format));

    char *data;
    int len;
    if (strcmp(format, "raw") == 0) {
        data = framebuffer_base64(&len);
    } else {
        data = take_screenshot_png(&len);
    }
    if (!data) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"screenshot failed\"}}");
    }
    char *resp = malloc(1024 + (size_t)len + 1);
    int n = snprintf(resp, 1024,
                    "{\"result\":{\"format\":\"%s\",\"width\":320,\"height\":320,\"data\":\"",
                    format);
    memcpy(resp + n, data, (size_t)len);
    n += len;
    snprintf(resp + n, 256, "\"}}");
    free(data);
    return resp;
}

static char *h_read_file(const char *params) {
    char path[512] = {0};
    json_get_str(params, "path", path, sizeof(path));
    if (!path[0]) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"path required\"}}");
    }

    char resolved[1024];
    if (!sandbox_path(path, resolved, sizeof(resolved))) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32003,\"message\":\"path outside sandbox\"}}");
    }

    FILE *f = fopen(resolved, "rb");
    if (!f) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32002,\"message\":\"file not found\"}}");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize ? (size_t)fsize : 1);
    if (!buf) { fclose(f); return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"out of memory\"}}"); }
    if (fsize > 0) fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    char *b64 = malloc(b64_enc_size((size_t)fsize) + 1);
    if (!b64) { free(buf); return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"out of memory\"}}"); }
    b64_encode(buf, (size_t)fsize, b64);
    free(buf);

    char escaped_path[1024];
    json_escape(path, escaped_path, sizeof(escaped_path));
    size_t b64len = strlen(b64);
    char *resp = malloc(1024 + b64len + 1);
    int n = snprintf(resp, 1024,
                    "{\"jsonrpc\":\"2.0\",\"result\":{\"path\":\"%s\",\"size\":%ld,\"data\":\"", escaped_path, fsize);
    memcpy(resp + n, b64, b64len);
    n += (int)b64len;
    snprintf(resp + n, 256, "\"}}");
    free(b64);
    return resp;
}

static size_t b64_decode_inplace(char *b64, size_t len) {
    static const unsigned char LUT[256] = {
        ['A']=0+1,['B']=1+1,['C']=2+1,['D']=3+1,['E']=4+1,['F']=5+1,['G']=6+1,['H']=7+1,
        ['I']=8+1,['J']=9+1,['K']=10+1,['L']=11+1,['M']=12+1,['N']=13+1,['O']=14+1,['P']=15+1,
        ['Q']=16+1,['R']=17+1,['S']=18+1,['T']=19+1,['U']=20+1,['V']=21+1,['W']=22+1,['X']=23+1,
        ['Y']=24+1,['Z']=25+1,['a']=26+1,['b']=27+1,['c']=28+1,['d']=29+1,['e']=30+1,['f']=31+1,
        ['g']=32+1,['h']=33+1,['i']=34+1,['j']=35+1,['k']=36+1,['l']=37+1,['m']=38+1,['n']=39+1,
        ['o']=40+1,['p']=41+1,['q']=42+1,['r']=43+1,['s']=44+1,['t']=45+1,['u']=46+1,['v']=47+1,
        ['w']=48+1,['x']=49+1,['y']=50+1,['z']=51+1,['0']=52+1,['1']=53+1,['2']=54+1,['3']=55+1,
        ['4']=56+1,['5']=57+1,['6']=58+1,['7']=59+1,['8']=60+1,['9']=61+1,['+']=62+1,['/']=63+1,
    };
    // Standard base64: process 4 chars at a time into 3 bytes
    size_t j = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t sextet[4] = {0};
        int valid = 0;
        for (int k = 0; k < 4 && i < len; ) {
            unsigned char c = (unsigned char)b64[i++];
            if (c == '=' || c == '\0') { i = len; break; }  // padding = end
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            unsigned char v = LUT[c];
            if (v == 0) continue;  // invalid char
            sextet[k] = v - 1;
            valid++;
            k++;
        }
        if (valid >= 2) {
            uint32_t triple = (sextet[0] << 18) | (sextet[1] << 12) |
                              (sextet[2] << 6)  |  sextet[3];
            b64[j++] = (char)((triple >> 16) & 0xFF);
            if (valid >= 3) b64[j++] = (char)((triple >> 8) & 0xFF);
            if (valid >= 4) b64[j++] = (char)(triple & 0xFF);
        }
    }
    b64[j] = '\0';
    return j;
}

static char *h_write_file(const char *params) {
    char path[512] = {0};
    json_get_str(params, "path", path, sizeof(path));
    if (!path[0]) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"path required\"}}");
    }

    char resolved[1024];
    if (!sandbox_path(path, resolved, sizeof(resolved))) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32003,\"message\":\"path outside sandbox\"}}");
    }

    const char *data_start = strstr(params, "\"data\"");
    if (!data_start) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"data required\"}}");
    }
    const char *colon = strchr(data_start, ':');
    if (!colon) return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"data format error\"}}");
    const char *p = colon + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"data must be base64 string\"}}");
    p++;

    static char b64_buf[256 * 1024];
    size_t di = 0;
    while (*p && *p != '"' && di < sizeof(b64_buf) - 1) b64_buf[di++] = *p++;
    b64_buf[di] = '\0';

    size_t dec_len = b64_decode_inplace(b64_buf, di);

    FILE *f = fopen(resolved, "wb");
    if (!f) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32003,\"message\":\"write failed\"}}");
    }
    fwrite(b64_buf, 1, dec_len, f);
    fclose(f);

    char *resp = malloc(128);
    snprintf(resp, 128, "{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true,\"bytes_written\":%zu}}", dec_len);
    return resp;
}

static char *h_delete_file(const char *params) {
    char path[512] = {0};
    json_get_str(params, "path", path, sizeof(path));
    if (!path[0]) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"path required\"}}");
    }
    char resolved[1024];
    if (!sandbox_path(path, resolved, sizeof(resolved))) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32003,\"message\":\"path outside sandbox\"}}");
    }
    if (unlink(resolved) != 0) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32002,\"message\":\"delete failed\"}}");
    }
    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true}}");
}

static char *h_list_dir(const char *params) {
    char path[512] = "/";
    json_get_str(params, "path", path, sizeof(path));

    char resolved[1024];
    if (!sandbox_path(path, resolved, sizeof(resolved))) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32003,\"message\":\"path outside sandbox\"}}");
    }

    DIR *dir = opendir(resolved);
    if (!dir) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32002,\"message\":\"directory not found\"}}");
    }

    static char buf[65536];
    char escaped_path[1024];
    json_escape(path, escaped_path, sizeof(escaped_path));
    int off = snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"result\":{\"path\":\"%s\",\"entries\":[", escaped_path);
    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && (size_t)off < sizeof(buf) - 200) {
        if (entry->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", resolved, entry->d_name);
        struct stat st;
        bool is_dir = false;
        uint32_t fsize = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            fsize = (uint32_t)st.st_size;
        }
        char name_esc[256];
        json_escape(entry->d_name, name_esc, sizeof(name_esc));
        off += snprintf(buf + off, (size_t)(sizeof(buf) - (size_t)off),
                        "%s{\"name\":\"%s\",\"is_dir\":%s,\"size\":%u}",
                        first ? "" : ",", name_esc, is_dir ? "true" : "false", fsize);
        first = false;
    }
    closedir(dir);
    off += snprintf(buf + off, (size_t)(sizeof(buf) - (size_t)off), "]}}");
    return strdup(buf);
}

static char *h_disk_info(const char *params) {
    (void)params;
    extern char g_base_path[512];
    uint32_t free_kb = 0, total_kb = 0;
    struct statvfs st;
    if (statvfs(g_base_path, &st) == 0) {
        total_kb = (uint32_t)((uint64_t)st.f_blocks * (uint64_t)st.f_frsize / 1024);
        free_kb = (uint32_t)((uint64_t)st.f_bavail * (uint64_t)st.f_frsize / 1024);
    }
    static char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"free_kb\":%u,\"total_kb\":%u}}", free_kb, total_kb);
    return strdup(buf);
}

static char *h_play_tone(const char *params) {
    int freq = 440, duration = 500;
    json_get_int(params, "frequency", &freq);
    json_get_int(params, "duration_ms", &duration);
    if (freq < 20) freq = 20;
    if (freq > 20000) freq = 20000;
    extern void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
    audio_play_tone((uint32_t)freq, (uint32_t)duration);
    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true}}");
}

static char *h_stop_audio(const char *params) {
    (void)params;
    extern void audio_stop_tone(void);
    extern void audio_stop_stream(void);
    audio_stop_tone();
    audio_stop_stream();
    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true}}");
}

static char *h_get_display_buffer(const char *params) {
    (void)params;
    int len;
    char *data = framebuffer_base64(&len);
    if (!data) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"framebuffer unavailable\"}}");
    }
    char *resp = malloc(1024 + (size_t)len + 1);
    int n = snprintf(resp, 1024,
                    "{\"jsonrpc\":\"2.0\",\"result\":{\"width\":320,\"height\":320,\"format\":\"rgb565\",\"data\":\"");
    memcpy(resp + n, data, (size_t)len);
    n += len;
    snprintf(resp + n, 256, "\"}}");
    free(data);
    return resp;
}

static char *h_get_button_state(const char *params) {
    (void)params;
    uint32_t btns = kbd_get_buttons();
    uint32_t pressed = kbd_get_buttons_pressed();
    uint32_t released = kbd_get_buttons_released();
    static char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"buttons\":%u,\"buttons_pressed\":%u,\"buttons_released\":%u}}",
             btns, pressed, released);
    return strdup(buf);
}

static char *h_get_heap_info(const char *params) {
    (void)params;
    size_t free_psram = hal_psram_free_size();
    size_t used_psram = hal_psram_total_size() - free_psram;
    size_t lua_heap_free = lua_psram_alloc_free_size();
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"lua_heap_used_kb\":%zu,\"lua_heap_free_kb\":%zu,\"psram_total_kb\":%zu}}",
             used_psram / 1024, lua_heap_free / 1024, hal_psram_total_size() / 1024);
    return strdup(buf);
}

static char *h_get_audio_state(const char *params) {
    (void)params;
    bool tone = audio_tone_playing();
    bool stream = audio_stream_active();
    int players = sound_get_playing_source_count();
    static char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"tone_playing\":%s,\"stream_active\":%s,\"sound_players_active\":%d,\"master_volume\":%d}}",
             tone ? "true" : "false", stream ? "true" : "false", players,
             (int)audio_get_volume());
    return strdup(buf);
}

static char *h_get_wifi_state(const char *params) {
    (void)params;
    wifi_status_t st = wifi_get_status();
    const char *ssid = wifi_get_ssid();
    const char *ip = wifi_get_ip();
    const char *status_str = "disconnected";
    int forced = sim_wifi_forced_status();
    if (forced >= 0) st = (wifi_status_t)forced;
    if (st == WIFI_STATUS_ONLINE) status_str = "online";
    else if (st == WIFI_STATUS_CONNECTED) status_str = "connected";
    else if (st == WIFI_STATUS_CONNECTING) status_str = "connecting";
    else if (st == WIFI_STATUS_FAILED) status_str = "failed";

    // Injected state (set_wifi_state) overrides the real status. Without this
    // the injection was write-only: sim_wifi_is_available()/sim_network_blocked()
    // honoured it, but this getter reported the underlying status, so a
    // set→get roundtrip never observed what was injected.
    if (s_wifi_error.enabled) {
        switch (s_wifi_error.mode) {
            case WIFI_DISCONNECTED:   status_str = "disconnected";   break;
            case WIFI_NOT_AVAILABLE:  status_str = "not_available";  break;
            case WIFI_ERROR:          status_str = "error";          break;
            case WIFI_NORMAL:         break;  // report the real status
        }
    }
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"status\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\"}}",
             status_str, ssid ? ssid : "", ip ? ip : "");
    return strdup(buf);
}

static char *h_set_wifi_state(const char *params) {
    char status[32] = {0};
    char error_str[128] = {0};
    int error_code = 0;
    json_get_str(params, "status", status, sizeof(status));
    json_get_str(params, "error_str", error_str, sizeof(error_str));
    json_get_int(params, "error_code", &error_code);

    int forced = -1;
    if (strcmp(status, "connecting") == 0) forced = WIFI_STATUS_CONNECTING;
    else if (strcmp(status, "failed") == 0) forced = WIFI_STATUS_FAILED;
    __atomic_store_n(&s_wifi_forced_status, forced, __ATOMIC_RELAXED);

    s_wifi_error.enabled = true;
    if (forced >= 0) {
        s_wifi_error.mode = WIFI_NORMAL;
        s_wifi_error.enabled = false;
    } else if (strcmp(status, "connected") == 0 || strcmp(status, "online") == 0) {
        s_wifi_error.mode = WIFI_NORMAL;
        s_wifi_error.enabled = false;
    } else if (strcmp(status, "disconnected") == 0) {
        s_wifi_error.mode = WIFI_DISCONNECTED;
    } else if (strcmp(status, "not_available") == 0) {
        s_wifi_error.mode = WIFI_NOT_AVAILABLE;
    } else if (strcmp(status, "error") == 0) {
        s_wifi_error.mode = WIFI_ERROR;
        s_wifi_error.error_code = error_code;
        static char errbuf[128];
        if (error_str[0]) {
            snprintf(errbuf, sizeof(errbuf), "%s", error_str);
        } else {
            snprintf(errbuf, sizeof(errbuf), "Network error %d", error_code);
        }
        s_wifi_error.error_str = errbuf;
    }

    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true}}");
}

// {percent, charging}: what kbd_get_battery_percent / kbd_is_charging report.
extern void sim_kbd_set_battery(int percent, bool charging);

static char *h_set_battery(const char *params) {
    int percent = 100;
    bool charging = false;
    json_get_int(params, "percent", &percent);
    json_get_bool(params, "charging", &charging);
    sim_kbd_set_battery(percent, charging);
    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true}}");
}

// {since_seq, tail}: entries with seq >= since_seq (0 = all held), or only
// the newest `tail` of them. Paged at ~256 KB ("more":true).
static char *h_get_log_buffer(const char *params) {
    unsigned int since = 0, tail = 0;
    json_get_uint(params, "since_seq", &since);
    json_get_uint(params, "tail", &tail);
    char *body = sim_log_build_json(since, tail);
    if (!body) return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"out of memory\"}}");
    size_t n = strlen(body) + 40;
    char *resp = malloc(n);
    if (resp) snprintf(resp, n, "{\"jsonrpc\":\"2.0\",\"result\":%s}", body);
    free(body);
    return resp;
}

// {"logs":bool}: push each new log entry to this client as a `log`
// notification {seq,src,text}.
static char *h_subscribe(const char *params) {
    bool logs = false;
    json_get_bool(params, "logs", &logs);
    sim_socket_set_log_subscription(logs);
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"result\":{\"logs\":%s}}",
             logs ? "true" : "false");
    return strdup(buf);
}

static char *h_set_time_multiplier(const char *params) {
    float mult = 1.0f;
    json_get_float(params, "multiplier", &mult);
    if (mult < 0.0f) mult = 0.0f;
    hal_set_time_multiplier(mult);
    static char buf[64];
    snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true,\"multiplier\":%g}}", mult);
    return strdup(buf);
}

// step_time {ms}: advance the --virtual-time clock (a paused one included)
// by ms. Returns {"now_ms"}; step_time {ms:0} reads the clock.
static char *h_step_time(const char *params) {
    int ms = 0;
    json_get_int(params, "ms", &ms);
    if (ms < 0)
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,"
                      "\"message\":\"ms must be >= 0\"}}");
    uint64_t now_ms = 0;
    if (!hal_time_step((uint32_t)ms, &now_ms))
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32002,"
                      "\"message\":\"step_time needs --virtual-time\"}}");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"result\":{\"now_ms\":%llu}}",
             (unsigned long long)now_ms);
    return strdup(buf);
}

// ── Terminal buffer dump ─────────────────────────────────────────────────────

static char *h_get_terminal_buffer(const char *params) {
    (void)params;
    terminal_t *term = (terminal_t *)s_active_terminal;
    if (!term) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"no active terminal\"}}");
    }

    int cols = terminal_getCols(term);
    int rows = terminal_getRows(term);
    int cx, cy;
    terminal_getCursor(term, &cx, &cy);

    // Estimate: ~(cols+10) per row for JSON string + overhead
    size_t buf_size = (size_t)(cols + 20) * (size_t)rows + 512;
    char *buf = malloc(buf_size);
    if (!buf) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"out of memory\"}}");
    }

    int off = snprintf(buf, buf_size,
        "{\"jsonrpc\":\"2.0\",\"result\":{\"cols\":%d,\"rows\":%d,\"cursor_x\":%d,\"cursor_y\":%d,\"lines\":[",
        cols, rows, cx, cy);

    for (int y = 0; y < rows && (size_t)off < buf_size - 128; y++) {
        // Extract chars from cells, trim trailing spaces
        char line[256];
        int len = 0;
        for (int x = 0; x < cols && x < 255; x++) {
            uint16_t cell = terminal_getCell(term, x, y);
            char ch = (char)(cell & 0xFF);
            if (ch < 0x20 || ch > 0x7E) ch = ' ';
            line[len++] = ch;
        }
        // Trim trailing spaces
        while (len > 0 && line[len - 1] == ' ') len--;
        line[len] = '\0';

        // JSON-escape the line
        char escaped[512];
        json_escape(line, escaped, sizeof(escaped));

        off += snprintf(buf + off, buf_size - (size_t)off,
            "%s\"%s\"", y > 0 ? "," : "", escaped);
    }

    off += snprintf(buf + off, buf_size - (size_t)off, "]}}");
    return buf;
}

// ── Shutdown ─────────────────────────────────────────────────────────────────

static char *h_shutdown(const char *params) {
    (void)params;
    extern volatile int g_running;
    g_running = 0;
    extern void dev_commands_set_exit(void);
    dev_commands_set_exit();
    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true}}");
}

// ── Sanitizer self-test ──────────────────────────────────────────────────────
// ASan builds only, and only with --test-mode: reads one byte past a heap
// block so the E2E suite can prove, end to end, that a sanitizer report on
// stderr fails the test (test_harness_health.py). A release build refuses.

#if defined(__SANITIZE_ADDRESS__)
#define SIM_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SIM_ASAN_BUILD 1
#endif
#endif

// The MP3 decoder's schedule and output since boot (issue #28): output
// frames the mixer found no MP3 data for, frames decoded because the PCM
// ring was low or ahead in Core 0's idle windows (perf pacing), and those
// windows. tests/e2e/test_mp3_pacing.py.
static char *h_get_mp3_stats(const char *params) {
    (void)params;
    mp3_sched_stats_t m;
    mp3_player_get_sched_stats(&m);
    uint32_t windows;
    uint64_t idle_us;
    core0_idle_stats(&windows, &idle_us);
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"result\":{\"underruns\":%u,"
             "\"low_frames\":%u,\"idle_frames\":%u,\"overran\":%u,"
             "\"frame_us\":%u,\"decode_ahead\":%s,\"idle_windows\":%u,"
             "\"idle_ms\":%llu}}",
             (unsigned)mp3_player_staging_underruns(), (unsigned)m.low_frames,
             (unsigned)m.idle_frames, (unsigned)m.overran,
             (unsigned)m.frame_us, m.decode_ahead ? "true" : "false",
             (unsigned)windows, (unsigned long long)(idle_us / 1000u));
    return strdup(buf);
}

static char *h_sanitizer_selftest(const char *params) {
    (void)params;
#ifdef SIM_ASAN_BUILD
    if (sim_test_mode()) {
        volatile char *p = malloc(16);
        volatile char c = p[16];  // heap-buffer-overflow: ASan aborts here
        (void)c;
        free((void *)p);
    }
#endif
    return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32601,"
                  "\"message\":\"needs an ASan build and --test-mode\"}}");
}

// ── Clear log buffer ─────────────────────────────────────────────────────────

static char *h_clear_log_buffer(const char *params) {
    (void)params;
    sim_log_clear();
    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true}}");
}

// ── Crash log ────────────────────────────────────────────────────────────────

static char *h_get_crash_log(const char *params) {
    (void)params;
    extern char g_crash_log_path[512];
    FILE *f = fopen(g_crash_log_path, "r");
    if (!f) {
        return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"crash_log\":null}}");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 65536) {
        fclose(f);
        return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"crash_log\":null}}");
    }
    char *content = malloc((size_t)fsize + 1);
    if (!content) { fclose(f); return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"oom\"}}"); }
    fread(content, 1, (size_t)fsize, f);
    content[fsize] = '\0';
    fclose(f);

    // JSON-escape the content
    char *escaped = malloc((size_t)fsize * 2 + 128);
    if (!escaped) { free(content); return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"oom\"}}"); }
    json_escape(content, escaped, (size_t)fsize * 2 + 64);
    free(content);

    size_t resp_size = strlen(escaped) + 128;
    char *resp = malloc(resp_size);
    if (!resp) { free(escaped); return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"oom\"}}"); }
    snprintf(resp, resp_size, "{\"jsonrpc\":\"2.0\",\"result\":{\"crash_log\":\"%s\"}}", escaped);
    free(escaped);
    return resp;
}

// ── Wait for idle (no app running) ──────────────────────────────────────────

static char *h_wait_for_idle(const char *params) {
    (void)params;
    const char *name = launcher_get_running_app_name();
    if (!name || !name[0]) {
        return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"idle\":true}}");
    }
    return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"idle\":false}}");
}

// ── Display diagnostics ─────────────────────────────────────────────────────

static uint16_t s_diff_reference[320 * 320];
static bool s_diff_has_reference = false;

static char *h_display_stats(const char *params) {
    uint16_t *fb = hal_display_get_framebuffer();
    if (!fb) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"no framebuffer\"}}");
    }

    const int total = 320 * 320;
    int nonzero = 0;
    int first_nz_x = -1, first_nz_y = -1;
    uint16_t first_nz_val = 0;

    // 8KB bitset for counting unique RGB565 values (65536 bits = 8192 bytes)
    uint8_t seen[8192];
    memset(seen, 0, sizeof(seen));

    for (int i = 0; i < total; i++) {
        uint16_t px = fb[i];
        if (px != 0) {
            nonzero++;
            if (first_nz_x < 0) {
                first_nz_x = i % 320;
                first_nz_y = i / 320;
                first_nz_val = px;
            }
        }
        seen[px >> 3] |= (1 << (px & 7));
    }

    int unique = 0;
    for (int i = 0; i < 8192; i++) {
        uint8_t b = seen[i];
        while (b) { unique += b & 1; b >>= 1; }
    }

    // Optional pixel_at query
    int qx = -1, qy = -1;
    json_get_int(params, "x", &qx);
    json_get_int(params, "y", &qy);

    char pixel_at[128] = "";
    if (qx >= 0 && qx < 320 && qy >= 0 && qy < 320) {
        uint16_t px = fb[qy * 320 + qx];
        int r = ((px >> 11) & 0x1F) << 3;
        int g = ((px >> 5) & 0x3F) << 2;
        int b = (px & 0x1F) << 3;
        snprintf(pixel_at, sizeof(pixel_at),
            ",\"pixel_at\":{\"x\":%d,\"y\":%d,\"rgb565\":%u,\"r\":%d,\"g\":%d,\"b\":%d}",
            qx, qy, px, r, g, b);
    }

    char first_nz[128] = "null";
    if (first_nz_x >= 0) {
        snprintf(first_nz, sizeof(first_nz),
            "{\"x\":%d,\"y\":%d,\"rgb565\":%u}", first_nz_x, first_nz_y, first_nz_val);
    }

    char *resp = malloc(512);
    if (!resp) return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"oom\"}}");
    snprintf(resp, 512,
        "{\"jsonrpc\":\"2.0\",\"result\":{\"nonzero_pixels\":%d,\"total_pixels\":%d,"
        "\"unique_colors\":%d,\"first_nonzero\":%s%s,\"present_count\":%u}}",
        nonzero, total, unique, first_nz, pixel_at, hal_display_get_present_count());
    return resp;
}

static char *h_display_diff(const char *params) {
    uint16_t *fb = hal_display_get_framebuffer();
    if (!fb) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"no framebuffer\"}}");
    }

    char action[16] = "";
    json_get_str(params, "action", action, sizeof(action));

    if (strcmp(action, "capture") == 0) {
        memcpy(s_diff_reference, fb, 320 * 320 * sizeof(uint16_t));
        s_diff_has_reference = true;
        return strdup("{\"jsonrpc\":\"2.0\",\"result\":{\"captured\":true}}");
    }

    if (strcmp(action, "compare") == 0) {
        if (!s_diff_has_reference) {
            return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"no reference captured\"}}");
        }

        const int total = 320 * 320;
        int changed = 0;
        int min_x = 320, min_y = 320, max_x = -1, max_y = -1;

        for (int i = 0; i < total; i++) {
            if (fb[i] != s_diff_reference[i]) {
                changed++;
                int x = i % 320;
                int y = i / 320;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }

        double pct = (changed * 100.0) / total;
        char bbox[128] = "null";
        if (changed > 0) {
            snprintf(bbox, sizeof(bbox), "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
        }

        char *resp = malloc(256);
        if (!resp) return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"oom\"}}");
        snprintf(resp, 256,
            "{\"jsonrpc\":\"2.0\",\"result\":{\"changed_pixels\":%d,\"total_pixels\":%d,"
            "\"change_pct\":%.2f,\"bbox\":%s}}",
            changed, total, pct, bbox);
        return resp;
    }

    return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"action must be capture or compare\"}}");
}

static char *h_get_pixel(const char *params) {
    uint16_t *fb = hal_display_get_framebuffer();
    if (!fb) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"no framebuffer\"}}");
    }

    int x = -1, y = -1, w = 1, h = 1;
    json_get_int(params, "x", &x);
    json_get_int(params, "y", &y);
    json_get_int(params, "w", &w);
    json_get_int(params, "h", &h);

    if (x < 0 || y < 0 || x >= 320 || y >= 320) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"x,y required and must be 0-319\"}}");
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 16) w = 16;
    if (h > 16) h = 16;
    if (x + w > 320) w = 320 - x;
    if (y + h > 320) h = 320 - y;

    if (w == 1 && h == 1) {
        uint16_t px = fb[y * 320 + x];
        int r = ((px >> 11) & 0x1F) << 3;
        int g = ((px >> 5) & 0x3F) << 2;
        int b = (px & 0x1F) << 3;

        char *resp = malloc(256);
        if (!resp) return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"oom\"}}");
        snprintf(resp, 256,
            "{\"jsonrpc\":\"2.0\",\"result\":{\"x\":%d,\"y\":%d,\"rgb565\":%u,\"r\":%d,\"g\":%d,\"b\":%d}}",
            x, y, px, r, g, b);
        return resp;
    }

    // Region mode — return array of pixels
    size_t buf_size = 64 + (size_t)(w * h) * 8;
    char *pixels = malloc(buf_size);
    if (!pixels) return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"oom\"}}");
    pixels[0] = '\0';
    size_t off = 0;

    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            uint16_t px = fb[row * 320 + col];
            off += (size_t)snprintf(pixels + off, buf_size - off, "%s%u",
                off > 0 ? "," : "", px);
        }
    }

    size_t resp_size = off + 256;
    char *resp = malloc(resp_size);
    if (!resp) { free(pixels); return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"oom\"}}"); }
    snprintf(resp, resp_size,
        "{\"jsonrpc\":\"2.0\",\"result\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"pixels\":[%s]}}",
        x, y, w, h, pixels);
    free(pixels);
    return resp;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

typedef char *(*handler_fn)(const char *params);

static struct {
    const char *name;
    handler_fn fn;
} s_handlers[] = {
    { "ping",                h_ping },
    { "launch_app",         h_launch_app },
    { "exit_app",           h_exit_app },
    { "dev_command",        h_dev_command },
    { "get_running_app",    h_get_running_app },
    { "inject_button",      h_inject_button },
    { "inject_char",        h_inject_char },
    { "screenshot",        h_screenshot },
    { "read_file",          h_read_file },
    { "write_file",         h_write_file },
    { "delete_file",        h_delete_file },
    { "list_dir",           h_list_dir },
    { "disk_info",          h_disk_info },
    { "play_tone",          h_play_tone },
    { "stop_audio",         h_stop_audio },
    { "get_display_buffer", h_get_display_buffer },
    { "get_button_state",   h_get_button_state },
    { "get_heap_info",      h_get_heap_info },
    { "get_audio_state",     h_get_audio_state },
    { "get_wifi_state",     h_get_wifi_state },
    { "set_wifi_state",     h_set_wifi_state },
    { "set_battery",        h_set_battery },
    { "get_log_buffer",     h_get_log_buffer },
    { "set_time_multiplier",h_set_time_multiplier },
    { "step_time",          h_step_time },
    { "get_terminal_buffer", h_get_terminal_buffer },
    { "shutdown",           h_shutdown },
    { "clear_log_buffer",   h_clear_log_buffer },
    { "get_crash_log",      h_get_crash_log },
    { "wait_for_idle",      h_wait_for_idle },
    { "display_stats",      h_display_stats },
    { "display_diff",       h_display_diff },
    { "get_pixel",          h_get_pixel },
    { "rescan_apps",        h_rescan_apps },
    { "get_last_outcome",   h_get_last_outcome },
    { "subscribe",          h_subscribe },
    { "get_input_state",    h_get_input_state },
    { "sanitizer_selftest", h_sanitizer_selftest },
    { "get_mp3_stats",      h_get_mp3_stats },
    { NULL, NULL },
};

// Adds "id" to a handler's result. Handlers return either a full envelope
// {"jsonrpc":"2.0",...} (whose jsonrpc key is dropped to avoid a duplicate)
// or a bare object {...} that becomes the rest of the envelope. Heap-sized:
// responses (screenshots, files, logs) have no fixed upper bound.
static char *wrap_response(int id, const char *result) {
    const char *inner = result + 1;  // skip opening '{'
    if (result[0] == '{') {
        const char *skip = strstr(inner, "\"jsonrpc\"");
        const char *after = skip ? strchr(skip, ',') : NULL;
        if (after) {
            after++;
            while (*after == ' ' || *after == '\t') after++;
            inner = after;
        }
    }
    size_t n = strlen(result) + 64;
    char *out = malloc(n);
    if (!out) return NULL;
    if (result[0] == '{')
        snprintf(out, n, "{\"jsonrpc\":\"2.0\",\"id\":%d,%s\n", id, inner);
    else
        snprintf(out, n, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n", id, result);
    return out;
}

char *sim_handler_dispatch(const char *request, const char *end) {
    int id = 0;
    char method[128] = {0};
    char *params = NULL;

    if ((size_t)(end - request) < 9 || strncmp(request, "{\"jsonrpc\"", 9) != 0) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Invalid request\"}}");
    }

    const char *id_start = strstr(request, "\"id\"");
    if (id_start && id_start < end) {
        const char *colon = strchr(id_start, ':');
        if (colon && ++colon < end) {
            while (*colon == ' ' || *colon == '\t') colon++;
            id = atoi(colon);
        }
    }

    const char *method_start = strstr(request, "\"method\"");
    if (!method_start || method_start >= end) {
        char err[128];
        snprintf(err, sizeof(err),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32600,\"message\":\"method missing\"}}", id);
        return strdup(err);
    }
    const char *colon = strchr(method_start, ':');
    if (!colon || colon >= end) {
        return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"colon missing after method\"}}");
    }
    const char *m = strchr(colon, '"');
    if (m) m++;
    else return strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"quote missing for method\"}}");
    size_t mi = 0;
    while (*m && *m != '"' && mi < sizeof(method) - 1) method[mi++] = *m++;
    method[mi] = '\0';

    const char *params_start = strstr(request, "\"params\"");
    if (params_start && params_start < end) {
        const char *colon = strchr(params_start, ':');
        if (colon) {
            const char *p = colon + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '{') {
                int depth = 0;
                const char *end_brace = p;
                while (end_brace < end && (*end_brace != '}' || depth > 0)) {
                    if (*end_brace == '{') depth++;
                    else if (*end_brace == '}') depth--;
                    end_brace++;
                }
                size_t len = (size_t)(end_brace - p);
                params = malloc(len + 1);
                if (params) {
                    memcpy(params, p, len);
                    params[len] = '\0';
                }
            }
        }
    }
    if (!params) {
        params = malloc(4);
        if (params) { params[0] = '\0'; }
    }

    char *ret = NULL;
    for (int i = 0; s_handlers[i].name; i++) {
        if (strcmp(s_handlers[i].name, method) == 0) {
            char *result = s_handlers[i].fn(params ? params : "");
            if (!result) {
                ret = strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"handler returned null\"}}");
            } else {
                ret = wrap_response(id, result);
                free(result);
            }
            free(params);
            return ret;
        }
    }

    free(params);
    static char err[256];
    snprintf(err, sizeof(err),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32601,\"message\":\"method not found: %s\"}}",
             id, method);
    return strdup(err);
}
