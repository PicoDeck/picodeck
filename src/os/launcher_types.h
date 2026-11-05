#pragma once
#include <stdbool.h>
#include <stdint.h>

// ── App types ─────────────────────────────────────────────────────────────────

typedef enum {
    APP_TYPE_LUA    = 0,  // main.lua present
    APP_TYPE_NATIVE = 1,  // main.elf present (PIE binary)
} app_type_t;

// ── App discovery record ──────────────────────────────────────────────────────

typedef struct {
    char id[80];           // Reverse-DNS app ID (e.g. "net.picodeck.editor")
    char name[256];        // Display name from app.json
    char path[128];        // Full path to app directory ("/apps/foo")
    char description[128]; // Short description from app.json
    char version[16];
    char category[24];     // "games", "tools", "system", "demos", "emulators", "network"
    app_type_t type;               // Detected app type
    bool has_root_filesystem;      // "root-filesystem" requirement
    bool has_http;                 // "http" requirement
    bool has_audio;                // "audio" requirement
    char requirements[128];        // every requirement name, folded and
                                   // space-separated ("http audio ...")
    uint32_t system_clock_khz;     // Optional overclock (0 = use system default)
    uint32_t min_psram_kb;         // Optional: largest contiguous PSRAM block the
                                   // app needs to start (0 = no check)
    void *icon;                    // pc_image_t*, loaded from icon.png/bmp, NULL if absent
} app_entry_t;
