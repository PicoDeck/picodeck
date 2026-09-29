#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

#include "terminal.h"

// =============================================================================
// PicoOS API
//
// This is the central interface between the OS and apps. The OS owns all hardware;
// apps borrow it through here.
//
// In Lua, this is exposed as the `picocalc` global module.
// In C, a pointer to this struct is passed to picodeck_main() by the native loader.
// =============================================================================

// --- Input ------------------------------------------------------------------

// Button bitmask values (keyboard + d-pad)
#define BTN_UP        (1 << 0)
#define BTN_DOWN      (1 << 1)
#define BTN_LEFT      (1 << 2)
#define BTN_RIGHT     (1 << 3)
#define BTN_ENTER     (1 << 4)    // Enter key
#define BTN_ESC       (1 << 5)    // Escape key
#define BTN_MENU      (1 << 6)    // System menu trigger (F10 key)
#define BTN_F1        (1 << 7)
#define BTN_F2        (1 << 8)
#define BTN_F3        (1 << 9)
#define BTN_F4        (1 << 10)
#define BTN_F5        (1 << 11)
#define BTN_F6        (1 << 12)
#define BTN_F7        (1 << 13)
#define BTN_F8        (1 << 14)
#define BTN_F9        (1 << 15)
#define BTN_BACKSPACE (1 << 16)   // Backspace key
#define BTN_TAB       (1 << 17)   // Tab key
#define BTN_DEL       (1 << 18)   // Delete key (its own key; Shift+Delete is End)
#define BTN_SHIFT     (1 << 19)   // Shift modifier
#define BTN_CTRL      (1 << 20)   // Ctrl modifier
#define BTN_ALT       (1 << 21)   // Alt modifier
#define BTN_FN        (1 << 22)   // Fn/Symbol modifier

// Gamepad button bitmask values (picocalc_gamepad_t, API version 9). Each is
// an alias for keys (primary and alternate slot), which still report as
// themselves through the BTN_* masks. Defaults: arrows, A=F4, B=F5, X=Delete,
// Y=Backspace, L=F2, R=F3, Start=F1, Select=Tab.
#define PAD_UP        (1 << 0)
#define PAD_DOWN      (1 << 1)
#define PAD_LEFT      (1 << 2)
#define PAD_RIGHT     (1 << 3)
#define PAD_A         (1 << 4)
#define PAD_B         (1 << 5)
#define PAD_X         (1 << 6)
#define PAD_Y         (1 << 7)
#define PAD_L         (1 << 8)
#define PAD_R         (1 << 9)
#define PAD_START     (1 << 10)
#define PAD_SELECT    (1 << 11)

// Built-in font ids for display->setFont (API version 6)
#define PC_FONT_6X8               0
#define PC_FONT_8X12              1
#define PC_FONT_SCIENTIFICA       2
#define PC_FONT_SCIENTIFICA_BOLD  3

typedef struct {
    // Returns current bitmask of held buttons (BTN_* flags)
    uint32_t (*getButtons)(void);
    // Returns bitmask of buttons pressed THIS frame (edge detect, not held)
    uint32_t (*getButtonsPressed)(void);
    // Returns bitmask of buttons released THIS frame
    uint32_t (*getButtonsReleased)(void);
    // Returns the last ASCII character typed (0 if none this frame)
    // Includes full keyboard; use this for text input
    char (*getChar)(void);
} picocalc_input_t;

// --- Gamepad (API version 9) -------------------------------------------------
// A logical gamepad read like the input masks (after sys->poll()). Players
// rebind it; the keys stay readable through picocalc_input_t. Only present
// when api->version >= 9: the pointer sits after `version` in PicoCalcAPI.

typedef struct {
    uint32_t (*getButtons)(void);          // PAD_* held
    uint32_t (*getButtonsPressed)(void);   // PAD_* pressed this frame
    uint32_t (*getButtonsReleased)(void);  // PAD_* released this frame
    // Name of the key bound to one PAD_* button ("F4", "Del", "W"), for
    // on-screen hints. slot 0 = primary, 1 = alternate. NULL if unbound.
    const char *(*getLabel)(uint32_t pad_button, int slot);
} picocalc_gamepad_t;

// --- Display ----------------------------------------------------------------

typedef struct {
    void (*clear)(uint16_t color_rgb565);
    void (*setPixel)(int x, int y, uint16_t color);
    void (*fillRect)(int x, int y, int w, int h, uint16_t color);
    void (*drawRect)(int x, int y, int w, int h, uint16_t color);
    void (*drawLine)(int x0, int y0, int x1, int y1, uint16_t color);
    void (*drawCircle)(int cx, int cy, int r, uint16_t color);
    void (*fillCircle)(int cx, int cy, int r, uint16_t color);
    // Draw a null-terminated string. Returns pixel width of drawn text.
    int  (*drawText)(int x, int y, const char *text, uint16_t fg, uint16_t bg);
    // Flush the internal framebuffer to the LCD (call once per frame).
    // All three presents (flush, flushRows, flushRegion) first draw the OS
    // overlays into the rows they send: a system toast and, when the Show
    // FPS setting is on, the OS FPS counter (see perf below).
    void (*flush)(void);
    // Returns display width/height
    int  (*getWidth)(void);
    int  (*getHeight)(void);
    // Set display brightness 0-255 (controls backlight PWM)
    void (*setBrightness)(uint8_t brightness);
    // Draw an integer-scaled image (nearest-neighbor, no transparency).
    // Input data in host byte order (RGB565). Ideal for emulator blits.
    void (*drawImageNN)(int x, int y, const uint16_t *data,
                        int src_w, int src_h, int scale);
    // Flush only rows y0..y1 (inclusive, full width) from the back buffer.
    // Does NOT swap buffers. Useful for partial screen updates. An OS overlay
    // outside the rows is put on the panel directly, not into the buffer.
    void (*flushRows)(int y0, int y1);
    // Flush rows y0..y1 (inclusive) with buffer swap. Like flush() but only
    // transfers the specified row range. Useful for emulators with letterboxing.
    void (*flushRegion)(int y0, int y1);
    // Get writable pointer to the current back buffer (320x320 RGB565,
    // big-endian / byte-swapped). For direct framebuffer writes.
    uint16_t* (*getBackBuffer)(void);
    // Framebuffer post-processing effects (shaders)
    void (*effectInvert)(void);
    void (*effectDarken)(uint8_t factor);      // 0=black, 255=no change
    void (*effectBrighten)(uint8_t factor);    // 0=no change, 255=white
    void (*effectTint)(uint8_t r, uint8_t g, uint8_t b, uint8_t strength);
    void (*effectGrayscale)(void);
    void (*effectBlend)(const uint16_t *src, int w, int h, uint8_t alpha);
    void (*effectPalette)(const uint16_t *lut, int lut_size);
    void (*effectDither)(uint8_t levels);      // quantization levels
    void (*effectScanline)(uint8_t intensity); // 0=none, 255=black lines
    void (*effectPosterize)(uint8_t levels);   // 2-32 levels per channel
    // Raycasting primitives
    void (*fillVLine)(int x, int y0, int y1, uint16_t color);
    void (*drawTexturedColumn)(int x, int y0, int y1,
                               const uint16_t *tex, int tex_w, int tex_h,
                               int tex_x, int tex_y0, int tex_y1);
    void (*fillVLineGradient)(int x, int y0, int y1,
                              uint16_t color_top, uint16_t color_bottom);
    // --- API version 4 additions ---
    // Clip rect: all pixel-writing primitives respect it except clear(),
    // flush*(), and the whole-buffer effects. Default is full screen.
    void (*setClipRect)(int x, int y, int w, int h);
    void (*getClipRect)(int *x, int *y, int *w, int *h);
    void (*clearClipRect)(void);
    // Extra primitives (previously Lua-only)
    void (*fillHLine)(int y, int x0, int x1, uint16_t color);
    void (*fillTriangle)(int x0, int y0, int x1, int y1,
                         int x2, int y2, uint16_t color);
    // Hardware vertical scroll (ST7365P VSCRDEF/VSCRSADD).
    // top_fixed + scroll_height + bottom_fixed must equal 320.
    void (*setScrollArea)(int top_fixed, int scroll_height, int bottom_fixed);
    void (*setScrollOffset)(int offset);
    // Mode 7 perspective ground-plane render. Power-of-two texture dims wrap
    // seamlessly; other sizes clamp. Respects the clip rect.
    void (*drawPlane)(const uint16_t *tex, int tex_w, int tex_h,
                      float cam_x, float cam_y, float cam_z,
                      float angle, int horizon_y, float scale);
    // --- API version 6 additions: fonts ---------------------------------
    // Font ids: PC_FONT_6X8 .. PC_FONT_SCIENTIFICA_BOLD are built in;
    // loadFont returns ids >= 4. Every loaded font is freed when the app
    // exits. setFont ignores ids that are not live.
    void (*setFont)(int font_id);
    int  (*getFont)(void);
    int  (*getFontWidth)(void);            // max advance of the active font
    int  (*getFontHeight)(void);
    int  (*textWidth)(const char *text);   // real width, proportional-aware
    // slot id, or -1 on failure (bad file, or all 8 slots in use; the reason
    // is logged). Lua's display.loadFont returns nil, errstr instead.
    int  (*loadFont)(const char *path);
    void (*unloadFont)(int font_id);
    // Draw text leaving background pixels untouched.
    int  (*drawTextTransparent)(int x, int y, const char *text, uint16_t fg);
} picocalc_display_t;

// --- Filesystem (SD card) ---------------------------------------------------

typedef void* pcfile_t;   // opaque file handle

typedef struct {
    // Open a file. mode: "r", "w", "a", "rb", "wb" etc.
    pcfile_t (*open)(const char *path, const char *mode);
    int      (*read)(pcfile_t f, void *buf, int len);
    int      (*write)(pcfile_t f, const void *buf, int len);
    void     (*close)(pcfile_t f);
    bool     (*exists)(const char *path);
    int      (*size)(const char *path);
    int      (*fsize)(pcfile_t f);
    bool     (*seek)(pcfile_t f, uint32_t offset);
    uint32_t (*tell)(pcfile_t f);
    // List directory. Calls callback for each entry. Returns entry count.
    int      (*listDir)(const char *path,
                        void (*callback)(const char *name, bool is_dir,
                                         uint32_t size, void *user),
                        void *user);
    // Create a directory. Returns true on success or if already exists.
    bool     (*mkdir)(const char *path);
    // Delete a file or empty directory. Returns true on success.
    bool     (*deleteFile)(const char *path);
    // Rename/move a file. Returns true on success.
    bool     (*renameFile)(const char *src, const char *dst);
    // Check if path is a directory. Returns true if it exists and is a directory.
    bool     (*isDir)(const char *path);
    // Modal file-browser overlay (system-menu styling). Blocks until the
    // user picks a file or cancels. start_path: directory shown first.
    // root_path: topmost directory reachable via Esc (NULL = start_path);
    // Esc at root cancels. On success returns true with the full file
    // path in out_path. Requires api->version >= 3.
    bool     (*browse)(const char *start_path, const char *root_path,
                       char *out_path, int out_len);
} picocalc_fs_t;

// --- System -----------------------------------------------------------------

typedef struct {
    // Milliseconds since boot
    uint32_t (*getTimeMs)(void);
    // Microseconds since boot (64-bit, for high-precision timing)
    uint64_t (*getTimeUs)(void);
    // Trigger a system reboot
    void     (*reboot)(void);
    // Battery level 0-100 (from STM32 via I2C). -1 only before the first
    // reading (a few seconds after boot); on USB power it keeps the last
    // level.
    int      (*getBatteryPercent)(void);
    // True if connected to USB power (reads GP24, the Pico 2's VBUS sense).
    // Not reliable on Pico W-family boards, including the PicoCalc's
    // Pimoroni Pico Plus 2 W: GP24 belongs to the wireless chip there, so
    // this currently reads false even on USB power. A fix (reading VBUS
    // through the CYW43) is pending.
    bool     (*isUSBPowered)(void);
    // Add an item to the system menu overlay (max 4 items per app)
    // callback is called when the item is selected in the menu, after the
    // menu has closed and, when memory for the copy was available, given the
    // app its screen (both framebuffers) back; what it draws shows at the
    // app's next flush
    void     (*addMenuItem)(const char *label, void (*callback)(void *user), void *user);
    // Clear all app-registered menu items (called automatically on app exit)
    void     (*clearMenuItems)(void);
    // Log a message to UART serial debug output
    void     (*log)(const char *fmt, ...);
    // Single OS tick for native apps: polls the keyboard, feeds the watchdog,
    // reclaims released HTTP/TCP slots and serves dev commands.  Also checks
    // for the Sym (Menu) key and shows the system menu overlay automatically.
    // It fires no HTTP callbacks (native apps poll http->isComplete()).
    // Call in your main loop.
    void     (*poll)(void);
    // Returns true (once) after the user selects "Exit App" from the system
    // menu.  Native apps should check this each frame and return from
    // picodeck_main() when it fires.
    bool     (*shouldExit)(void);
    // Register a callback to be called on Core 1 every 5ms, alongside
    // audio updates.  Used by native apps (e.g. DOOM) to offload audio
    // mixing from Core 0.  Pass NULL to deregister.
    void     (*setAudioCallback)(void (*cb)(void));
} picocalc_sys_t;

// --- Audio ------------------------------------------------------------------

typedef struct {
    // Play a square wave tone at freq Hz for duration_ms milliseconds.
    // duration_ms = 0 plays indefinitely until stopTone() is called.
    void (*playTone)(uint32_t freq_hz, uint32_t duration_ms);
    void (*stopTone)(void);
    // Master volume 0-100; it resets to 100 when the app exits
    void (*setVolume)(uint8_t volume);
    // PCM sample streaming. Samples are stereo interleaved int16_t (L,R,L,R).
    // count = number of stereo frames (each frame = 2 int16_t values).
    void (*startStream)(uint32_t sample_rate);
    void (*stopStream)(void);
    void (*pushSamples)(const int16_t *samples, int count);
} picocalc_audio_t;

// --- WiFi (Pico 2W only) -----------------------------------------------------
// CYW43 uses hardware SPI1; LCD uses PIO0 — independent buses, no arbitration
// needed. All WiFi calls are cross-core IPC (Core 0 → Core 1).

typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,      // IP assigned, internet NOT verified
    WIFI_STATUS_FAILED,
    WIFI_STATUS_ONLINE,         // Internet connectivity confirmed
} wifi_status_t;

typedef struct {
    // Connect to a WiFi network. Non-blocking: check status with getStatus().
    void (*connect)(const char *ssid, const char *password);
    void (*disconnect)(void);
    wifi_status_t (*getStatus)(void);
    // Returns current IP as a string, or NULL if not connected
    const char *  (*getIP)(void);
    // Returns SSID of current connection, or NULL
    const char *  (*getSSID)(void);
    // True if WiFi hardware is present (Pico 2W vs standard Pico 2)
    bool          (*isAvailable)(void);
} picocalc_wifi_t;

// --- TCP Sockets ------------------------------------------------------------

typedef void* pctcp_t;   // opaque TCP connection handle

typedef enum {
    TCP_CB_CONNECT  = (1 << 0),
    TCP_CB_READ     = (1 << 1),
    TCP_CB_WRITE    = (1 << 2),
    TCP_CB_CLOSED   = (1 << 3),
    TCP_CB_FAILED   = (1 << 4),
} tcp_event_t;

// connectEx() flags (API version 8).
#define PCTCP_TLS          (1u << 0)  // TLS; the server certificate is verified
                                      // against the OS root bundle + host name
#define PCTCP_TLS_INSECURE (1u << 1)  // with PCTCP_TLS: skip verification
                                      // (self-signed dev servers only)

typedef struct {
    // Open a TCP connection to host:port. Non-blocking.
    pctcp_t (*connect)(const char *host, uint16_t port, bool use_ssl);
    // Write data to the connection. Returns bytes sent or <0 on error.
    int     (*write)(pctcp_t c, const void *buf, int len);
    // Read data from the connection. Returns bytes read or 0 if none available.
    int     (*read)(pctcp_t c, void *buf, int len);
    // Close the connection and release it; the handle is invalid afterwards
    // (the slot returns to the pool once Core 1 has let go of it).
    void    (*close)(pctcp_t c);
    // Returns number of bytes available for reading.
    int     (*available)(pctcp_t c);
    // Returns the last error string, or NULL.
    const char * (*getError)(pctcp_t c);
    // Returns bitmask of pending events (TCP_CB_*).
    uint32_t (*getEvents)(pctcp_t c);
    // --- API version 8 ---
    // connect() with PCTCP_* flags. A TLS socket reports TCP_CB_CONNECT only
    // once the handshake (and certificate check) has succeeded; a TLS
    // connect before SNTP has set the clock fails with "clock not set".
    pctcp_t (*connectEx)(const char *host, uint16_t port, uint32_t flags);
} picocalc_tcp_t;

// --- UI Widgets -------------------------------------------------------------

typedef struct {
    // Modal text input with title bar. Returns true on Enter, false on Esc.
    bool (*textInput)(const char *title, const char *prompt,
                      const char *initial, char *out, int out_len);
    // Simpler text input (no title bar). Returns true on Enter, false on Esc.
    bool (*textInputSimple)(const char *prompt, const char *default_val,
                            char *out_buf, int out_len);
    // Yes/no confirmation dialog. Returns true on Enter/Y, false on Esc/N.
    bool (*confirm)(const char *message);
} picocalc_ui_t;

// --- PSRAM ------------------------------------------------------------------

typedef struct {
    bool (*pioAvailable)(void);
    bool (*pioBulkAvailable)(void);
    void (*pioRead)(uint32_t addr, uint8_t *dst, uint32_t len);
    void (*pioWrite)(uint32_t addr, const uint8_t *src, uint32_t len);
    void (*pioBulkRead)(uint32_t addr, uint8_t *dst, uint32_t len);
    void (*pioBulkWrite)(uint32_t addr, const uint8_t *src, uint32_t len);
    void *(*qmiAlloc)(uint32_t size);
    void (*qmiFree)(void *ptr);
} picocalc_psram_t;

// --- Performance -----------------------------------------------------------

typedef struct {
    void (*beginFrame)(void);
    // One frame of the app's own loop. While an app calls it, the OS FPS
    // counter (Show FPS) shows endFrame calls per second, not presents.
    void (*endFrame)(void);
    int  (*getFPS)(void);
    uint32_t (*getFrameTime)(void);
    void (*drawFPS)(int x, int y);
    void (*setTargetFPS)(uint32_t fps);
} picocalc_perf_t;

// --- HTTP Client ------------------------------------------------------------
// Mongoose-based HTTP/1.1+HTTPS client. Non-blocking, cross-core safe.
// Connections are acquired from a pool of HTTP_MAX_CONNECTIONS (8).
// All calls are Core 0 → Core 1 IPC — do NOT call mg_* functions directly.

typedef void* pchttp_t;  // opaque HTTP connection handle

typedef struct {
    // Allocate a connection. server = hostname/IP, port = 0 → 80/443 default.
    // use_ssl = true for HTTPS. Returns NULL if pool exhausted.
    pchttp_t (*newConn)(const char *server, uint16_t port, bool use_ssl);
    // Initiate GET request. extra_hdrs may be NULL. Non-blocking.
    void  (*get)(pchttp_t c, const char *path, const char *extra_hdrs);
    // Initiate POST request. body/body_len may be 0/NULL. Non-blocking.
    void  (*post)(pchttp_t c, const char *path, const char *extra_hdrs,
                  const char *body, uint32_t body_len);
    // Read up to len bytes of response body. Returns bytes read (0 if none).
    int      (*read)(pchttp_t c, uint8_t *buf, uint32_t len);
    // Returns bytes available in the receive buffer.
    uint32_t (*available)(pchttp_t c);
    // Close the connection and return it to the pool.
    void  (*close)(pchttp_t c);
    // HTTP response status code (e.g. 200, 404). 0 if not yet received.
    int   (*getStatus)(pchttp_t c);
    // Last error string, or NULL if no error.
    const char* (*getError)(pchttp_t c);
    // Progress: sets *received and *total (-1 if unknown). Returns total.
    // (Lua conn:getProgress() returns received, total instead.)
    int   (*getProgress)(pchttp_t c, int *received, int *total);
    // Configuration — call before get()/post().
    void  (*setKeepAlive)(pchttp_t c, bool keep_alive);
    void  (*setByteRange)(pchttp_t c, int from, int to);
    void  (*setConnectTimeout)(pchttp_t c, int seconds);
    void  (*setReadTimeout)(pchttp_t c, int seconds);
    bool  (*setReadBufferSize)(pchttp_t c, int bytes);
    // Returns true when the request has completed (success or failure).
    // Use getStatus()/getError() to determine outcome.
    bool  (*isComplete)(pchttp_t c);
    // --- API version 8 ---
    // HTTPS verifies the server certificate (OS root bundle + host name) and
    // refuses to connect until SNTP has set the clock ("clock not set").
    // setInsecure(c, true) before get()/post() skips both — for self-signed
    // development servers only.
    void  (*setInsecure)(pchttp_t c, bool insecure);
} picocalc_http_t;

// --- Sound Player -----------------------------------------------------------
// High-level audio: sample playback, streaming file playback, MP3 decoding.
// Objects allocated via umm_malloc (QMI PSRAM). Free when done.

typedef void* pcsound_sample_t; // opaque sound sample handle
typedef void* pcsound_player_t; // opaque sample player handle
typedef void* pcfileplayer_t;   // opaque file player handle
typedef void* pcmp3player_t;    // opaque MP3 player handle

typedef struct {
    // --- Sample (WAV-like in-memory playback) ---
    // Load a sample from a file. Returns NULL on failure.
    pcsound_sample_t (*sampleLoad)(const char *path);
    // Free a loaded sample.
    void  (*sampleFree)(pcsound_sample_t s);   // stops + detaches any player using it

    // --- Sample player ---
    // Create a new player instance. Returns NULL on OOM.
    pcsound_player_t (*playerNew)(void);
    void  (*playerSetSample)(pcsound_player_t p, pcsound_sample_t s);
    void     (*playerPlay)(pcsound_player_t p, uint8_t repeat_count);  // 0 = loop until stopped, n = play n times (as filePlayerPlay; mp3PlayerPlay: 0 loops, else once)
    void     (*playerStop)(pcsound_player_t p);
    bool     (*playerIsPlaying)(pcsound_player_t p);
    uint8_t  (*playerGetVolume)(pcsound_player_t p);
    void     (*playerSetVolume)(pcsound_player_t p, uint8_t vol);   // 0–100
    void     (*playerSetLoop)(pcsound_player_t p, bool loop);
    void     (*playerFree)(pcsound_player_t p);   // never frees the player's sample

    // --- File player (streaming from SD card) ---
    pcfileplayer_t (*filePlayerNew)(void);
    void     (*filePlayerLoad)(pcfileplayer_t fp, const char *path);
    void     (*filePlayerPlay)(pcfileplayer_t fp, uint8_t repeat_count);  // 0 = loop until stopped, n = play n times (as playerPlay); setLoopRange loops regardless
    void     (*filePlayerStop)(pcfileplayer_t fp);
    void     (*filePlayerPause)(pcfileplayer_t fp);
    void     (*filePlayerResume)(pcfileplayer_t fp);
    bool     (*filePlayerIsPlaying)(pcfileplayer_t fp);
    void     (*filePlayerSetVolume)(pcfileplayer_t fp, uint8_t vol);  // 0-100 (clamped), both L/R channels
    uint8_t  (*filePlayerGetVolume)(pcfileplayer_t fp);
    uint32_t (*filePlayerGetOffset)(pcfileplayer_t fp);
    void     (*filePlayerSetOffset)(pcfileplayer_t fp, uint32_t pos);
    bool     (*filePlayerDidUnderrun)(pcfileplayer_t fp);  // this player's stream starved since the last call or play(); cleared by the call
    void     (*filePlayerFree)(pcfileplayer_t fp);

    // --- MP3 player (Core 1 decoding, PIO PSRAM ring buffer) ---
    pcmp3player_t (*mp3PlayerNew)(void);
    void     (*mp3PlayerLoad)(pcmp3player_t mp, const char *path);
    void     (*mp3PlayerPlay)(pcmp3player_t mp, uint8_t repeat_count);  // 0 = loop until stopped, any other count plays once (the count is not honoured); as Lua play()
    void     (*mp3PlayerStop)(pcmp3player_t mp);
    void     (*mp3PlayerPause)(pcmp3player_t mp);
    void     (*mp3PlayerResume)(pcmp3player_t mp);
    bool     (*mp3PlayerIsPlaying)(pcmp3player_t mp);
    void     (*mp3PlayerSetVolume)(pcmp3player_t mp, uint8_t vol);  // 0-100 (clamped)
    uint8_t  (*mp3PlayerGetVolume)(pcmp3player_t mp);
    void     (*mp3PlayerSetLoop)(pcmp3player_t mp, bool loop);
    void     (*mp3PlayerFree)(pcmp3player_t mp);
} picocalc_soundplayer_t;

// --- App Config -------------------------------------------------------------
// Per-app key/value config persisted at /data/<APP_ID>/config.json.
// Nothing loads it for a native app: the store is unbound at every app start
// and exit, so call load() with your own id first. Max 4 keys, 32-char keys,
// 256-char values.

typedef struct {
    // Bind and load the running app's own config. Only the running app's id
    // is accepted (case-insensitive); another app's id returns false and
    // leaves the binding unchanged. Not called for you by the launcher.
    bool        (*load)(const char *app_id);
    // Save in-memory config to /data/<APP_ID>/config.json.
    bool        (*save)(void);
    // Get value for key. Returns fallback (may be NULL) if key not found.
    const char* (*get)(const char *key, const char *fallback);
    // Set value for key.
    void        (*set)(const char *key, const char *value);
    // Clear all in-memory entries (does not delete the file).
    void        (*clear)(void);
    // Delete the config file and clear memory.
    bool        (*reset)(void);
    // Returns the currently loaded app_id, or NULL.
    const char* (*getAppId)(void);
} picocalc_appconfig_t;

// --- Crypto -----------------------------------------------------------------
// mbedTLS wrappers. All digest/HMAC functions are synchronous.
// AES and ECDH use opaque context handles; free them when done.

typedef void* pccrypto_aes_t;   // opaque AES-CTR context
typedef void* pccrypto_ecdh_t;  // opaque ECDH context

typedef struct {
    // --- Hash / MAC ---
    void (*sha256)(const uint8_t *data, uint32_t len, uint8_t out[32]);
    void (*sha1)(const uint8_t *data, uint32_t len, uint8_t out[20]);
    void (*hmacSha256)(const uint8_t *key, uint32_t klen,
                       const uint8_t *data, uint32_t dlen, uint8_t out[32]);
    void (*hmacSha1)(const uint8_t *key, uint32_t klen,
                     const uint8_t *data, uint32_t dlen, uint8_t out[20]);
    // Fill buf with cryptographically random bytes.  False (buf zeroed) when
    // there is no seeded, working DRBG: never use the bytes then.  (Returned
    // since 2026-09; the slot and calling convention are unchanged, so older
    // apps that ignore the result still link and run.)
    bool (*randomBytes)(uint8_t *buf, uint32_t len);
    // SSH session-key derivation (RFC 4253 §7.2). letter = 'A'–'F'.
    // K = shared secret mpint, H = exchange hash, session_id = initial H.
    void (*deriveKey)(char letter,
                      const uint8_t *K, uint32_t k_len,
                      const uint8_t *H, uint32_t h_len,
                      const uint8_t *session_id, uint32_t sid_len,
                      uint8_t *out, uint32_t out_len);
    // --- AES-CTR ---
    // Create AES-CTR context. klen = 16/24/32. nonce = 16 bytes.
    pccrypto_aes_t (*aesNew)(const uint8_t *key, uint32_t klen,
                              const uint8_t *nonce);
    // Encrypt/decrypt in place (or in→out). Returns 0 on success.
    int  (*aesUpdate)(pccrypto_aes_t ctx,
                      const uint8_t *in, uint8_t *out, uint32_t len);
    void (*aesFree)(pccrypto_aes_t ctx);
    // --- ECDH ---
    // Create an X25519 or P-256 keypair.
    pccrypto_ecdh_t (*ecdhX25519)(void);
    pccrypto_ecdh_t (*ecdhP256)(void);
    // Write public key bytes into out; sets *out_len.
    void (*ecdhGetPublicKey)(pccrypto_ecdh_t ctx, uint8_t *out, uint32_t *out_len);
    // Compute shared secret from remote public key. Returns 0 on success.
    int  (*ecdhComputeShared)(pccrypto_ecdh_t ctx,
                              const uint8_t *remote, uint32_t rlen,
                              uint8_t *out, uint32_t *out_len);
    void (*ecdhFree)(pccrypto_ecdh_t ctx);
    // --- Signature verification ---
    bool (*rsaVerify)(const uint8_t *pubkey, uint32_t pklen,
                      const uint8_t *sig, uint32_t slen,
                      const uint8_t *hash, uint32_t hlen);
    bool (*ecdsaP256Verify)(const uint8_t *pubkey, uint32_t pklen,
                             const uint8_t *sig, uint32_t slen,
                             const uint8_t *hash, uint32_t hlen);
} picocalc_crypto_t;

// --- Graphics / Image -------------------------------------------------------
// Image loading and drawing. Images stored in PSRAM via umm_malloc.

typedef void* pcimage_t;  // opaque image handle

typedef struct {
    // Load image from path (BMP/JPEG/PNG/GIF). Returns NULL on failure.
    pcimage_t (*load)(const char *path);
    // Allocate blank zeroed image. Returns NULL on OOM.
    pcimage_t (*newBlank)(int width, int height);
    // Free image (pixels + struct). Safe to call with NULL.
    void      (*free)(pcimage_t img);
    // Image dimensions.
    int       (*width)(pcimage_t img);
    int       (*height)(pcimage_t img);
    // Raw pixel data pointer (PSRAM): w*h row-major RGB565 in host byte
    // order, the same values colour arguments take. Not byte-swapped like
    // display->getBackBuffer(): drawing the image does the swap.
    uint16_t* (*pixels)(pcimage_t img);
    // Per-image transparent color for color-key blending (0 = disabled).
    void      (*setTransparentColor)(pcimage_t img, uint16_t color);
    // Draw operations.
    void      (*draw)(pcimage_t img, int x, int y);
    void      (*drawRegion)(pcimage_t img, int sx, int sy, int sw, int sh, int dx, int dy);
    // dst_w/dst_h are a destination SIZE; Lua's img:drawScaled takes a scale
    // multiplier instead.
    void      (*drawScaled)(pcimage_t img, int x, int y, int dst_w, int dst_h);
} picocalc_graphics_t;

// --- Video ------------------------------------------------------------------
// MJPEG video playback. video_player_t* is used as the opaque handle.

typedef void* pcvideo_t;  // opaque video player handle

typedef struct {
    pcvideo_t (*newPlayer)(void);
    void      (*free)(pcvideo_t vp);
    bool      (*load)(pcvideo_t vp, const char *path);
    void      (*play)(pcvideo_t vp);
    void      (*pause)(pcvideo_t vp);
    void      (*resume)(pcvideo_t vp);
    void      (*stop)(pcvideo_t vp);
    bool      (*update)(pcvideo_t vp);
    void      (*seek)(pcvideo_t vp, uint32_t frame);
    float     (*getFPS)(pcvideo_t vp);
    void      (*getSize)(pcvideo_t vp, uint32_t *w, uint32_t *h);
    bool      (*isPlaying)(pcvideo_t vp);
    bool      (*isPaused)(pcvideo_t vp);
    void      (*setLoop)(pcvideo_t vp, bool loop);
    void      (*setAutoFlush)(pcvideo_t vp, bool af);
    bool      (*hasAudio)(pcvideo_t vp);
    void      (*setVolume)(pcvideo_t vp, uint8_t vol);
    uint8_t   (*getVolume)(pcvideo_t vp);
    void      (*setMuted)(pcvideo_t vp, bool muted);
    bool      (*getMuted)(pcvideo_t vp);
    uint32_t  (*getDroppedFrames)(pcvideo_t vp);
    void      (*resetStats)(pcvideo_t vp);

    // --- API version 7 additions -------------------------------------------
    // Time-based position/seek (ms).  Seeks clamp to the file and never wrap;
    // a seek while paused presents the target frame; a seek after the end
    // restarts playback.  With loop off the player holds the last frame and
    // hasEnded() turns true (isPlaying() false); play() or resume() replays.
    uint32_t  (*getFrameCount)(pcvideo_t vp);
    uint32_t  (*getDurationMs)(pcvideo_t vp);
    uint32_t  (*getPositionMs)(pcvideo_t vp);
    void      (*seekMs)(pcvideo_t vp, uint32_t ms);
    void      (*seekRelativeMs)(pcvideo_t vp, int32_t delta_ms);
    bool      (*hasEnded)(pcvideo_t vp);
    // Built-in progress OSD (bar + elapsed/total) drawn over the bottom of
    // the video.  On by default; shown on play/pause/seek, hides after the
    // timeout (default 3000 ms) while playing, stays while paused/ended.
    void      (*setOSD)(pcvideo_t vp, bool enabled);
    void      (*showOSD)(pcvideo_t vp);
    void      (*setOSDTimeout)(pcvideo_t vp, uint32_t ms);
} picocalc_video_t;

// --- MOD Music Player -------------------------------------------------------
// Tracker music playback via pocketmod. Single static instance.

typedef void* pcmodplayer_t;  // opaque MOD player handle

typedef struct {
    pcmodplayer_t (*create)(void);
    void     (*destroy)(pcmodplayer_t mp);
    bool     (*load)(pcmodplayer_t mp, const char *path);
    void     (*play)(pcmodplayer_t mp, bool loop);
    void     (*stop)(pcmodplayer_t mp);
    void     (*pause)(pcmodplayer_t mp);
    void     (*resume)(pcmodplayer_t mp);
    bool     (*isPlaying)(pcmodplayer_t mp);
    void     (*setVolume)(pcmodplayer_t mp, uint8_t vol);  // 0-100
    uint8_t  (*getVolume)(pcmodplayer_t mp);
    void     (*setLoop)(pcmodplayer_t mp, bool loop);
} picocalc_modplayer_t;

// --- ZIP Archive Extraction ------------------------------------------------

// Opaque read-in-place archive handle (API version 5).
typedef void *pczip_t;

typedef struct {
    char     name[256];   // entry name, NUL-terminated
    uint32_t size;        // uncompressed bytes
    uint32_t comp_size;   // compressed bytes
    bool     is_dir;
} pczip_stat_t;

// zip->read: the entry does not fit in buf_cap (see picocalc_zip_t::read).
#define PCZIP_ERR_TOO_SMALL (-2)

typedef struct {
    bool (*extract)(const char *zip_path, const char *dest_dir);
    // Returns number of files in archive, -1 on error.
    int  (*list)(const char *zip_path);
    // --- API version 5 additions: read-in-place archive handles ------------
    // Open an archive for random access without extracting it. At most 4
    // archives may be open at once; handles are force-closed when the app
    // exits. Returns NULL on error.
    pczip_t (*open)(const char *zip_path);
    void    (*close)(pczip_t z);
    int     (*numEntries)(pczip_t z);                 // files + dirs, -1 on error
    int     (*locate)(pczip_t z, const char *name);   // entry index, -1 if absent
    bool    (*statIndex)(pczip_t z, int idx, pczip_stat_t *out);
    // Decompress entry idx into a caller-supplied buffer. Returns the bytes
    // written, or a negative code: PCZIP_ERR_TOO_SMALL (-2) when the entry
    // is larger than buf_cap (nothing is written; allocate pczip_stat_t.size
    // bytes, from statIndex, and retry), -1 for any other error. Test the
    // result with `< 0`, never `== -1`. Older firmware returns -1 for both.
    int     (*read)(pczip_t z, int idx, void *buf, uint32_t buf_cap);
    // Stream entry idx to dest_path on the SD card (constant memory).
    bool    (*extractEntry)(pczip_t z, int idx, const char *dest_path);
} picocalc_zip_t;

// --- The complete OS API struct ---------------------------------------------
// This is what gets passed to every Lua environment and future C app loaders.

typedef struct PicoCalcAPI {
    const picocalc_input_t   *input;
    const picocalc_display_t *display;
    const picocalc_fs_t      *fs;
    const picocalc_sys_t     *sys;
    const picocalc_audio_t   *audio;
    const picocalc_wifi_t    *wifi;
    const picocalc_tcp_t     *tcp;
    const picocalc_ui_t      *ui;
    const picocalc_psram_t   *psram;
    const picocalc_perf_t    *perf;
    const picocalc_terminal_t *terminal;
    // --- Phase 1 additions (appended for ABI compatibility) ---
    const picocalc_http_t        *http;        // HTTP/HTTPS client
    const picocalc_soundplayer_t *soundplayer; // sample/file/MP3 player
    const picocalc_appconfig_t   *appconfig;   // per-app config store
    const picocalc_crypto_t      *crypto;      // crypto primitives
    // --- Phase 2 additions ---
    const picocalc_graphics_t    *graphics;    // image loading/drawing
    const picocalc_video_t       *video;       // MJPEG video playback
    const picocalc_modplayer_t   *modplayer;   // MOD tracker music
    const picocalc_zip_t         *zip;         // ZIP extraction
    uint32_t                      version;     // 1=Phase1, 2=Phase2, 3=fs->browse,
                                             // 4=clip rect + mode-7 plane + display parity
                                             // 5=zip read-in-place handles
                                             // 6=fonts: display->setFont/getFont/...
                                             // 7=video time seek/position, OSD, hasEnded
                                             // 8=TLS verification: http->setInsecure,
                                             //   tcp->connectEx
                                             // 9=gamepad
    // --- API version 9: fields after `version` (check it first) ---
    const picocalc_gamepad_t     *gamepad;     // logical gamepad (PAD_*)
} PicoCalcAPI;

// The global API instance, populated during os_init()
extern PicoCalcAPI g_api;

// Optional audio callback for native apps that need Core 1 mixing.
// Set by the native app at startup, cleared on exit. Called every 5ms
// from core1_entry() alongside mp3_player_update()/fileplayer_update().
extern _Atomic(void (*)(void)) g_native_audio_callback;
