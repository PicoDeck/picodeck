#pragma once

#include <stdbool.h>

// =============================================================================
// App Config — /data/<APP_ID>/config.json
//
// Per-app configuration storage. Each app gets its own isolated config file
// stored in /data/<APP_ID>/config.json.
//
// Auto-creates the /data/<APP_ID>/ directory on first save if it doesn't exist.
//
// The store is one umm block made when the first key is stored and freed when
// the app ends (appconfig_unbind), so an app that uses no config pays nothing.
// It never moves: a pointer from appconfig_get stays good while other keys are
// set, as the native API has always allowed (a delete shifts the later keys).
//
// load + save never deletes what the store could not load: what is in the file
// beyond the limits (a key past APPCONFIG_MAX_ENTRIES, an over-long key or
// value) or is not a string (a number, true, a nested value) is kept verbatim
// and written back by appconfig_save. appconfig_get does not see it; setting or
// deleting that key replaces it; appconfig_clear and appconfig_reset drop it.
//
// API (C):
//   appconfig_load(app_id)      — load from /data/<APP_ID>/config.json
//   appconfig_save()            — save to file
//   appconfig_get(key, default) — get value, returns default if not found
//   appconfig_put(key, value)   — set value; says why when it stored nothing
//   appconfig_set(key, value)   — appconfig_put with no result (the native API)
//   appconfig_clear()           — clear in-memory store
//   appconfig_reset()           — delete config file
//   appconfig_unbind()          — forget the app id and entries (app switch)
// =============================================================================

#define APPCONFIG_MAX_ENTRIES  32
#define APPCONFIG_KEY_MAX      32  // sizes: keys are at most 31 bytes
#define APPCONFIG_VAL_MAX      256 // values at most 255

typedef enum {
    APPCONFIG_OK = 0,
    APPCONFIG_FULL,          // a new key past APPCONFIG_MAX_ENTRIES
    APPCONFIG_KEY_TOO_LONG,
    APPCONFIG_VALUE_TOO_LONG,
    APPCONFIG_BAD_KEY,       // NULL or empty
    APPCONFIG_NO_MEMORY,
} appconfig_status_t;

bool        appconfig_load(const char *app_id);
bool        appconfig_save(void);
const char *appconfig_get(const char *key, const char *fallback);

// A NULL or empty value deletes the key. Anything but APPCONFIG_OK stored
// nothing: an existing key keeps its old value, never a truncated one.
appconfig_status_t appconfig_put(const char *key, const char *value);
const char *appconfig_status_text(appconfig_status_t status);

// The native API's set (void in PicoCalcAPI): appconfig_put, logging a refusal.
void        appconfig_set(const char *key, const char *value);

void        appconfig_clear(void);
bool        appconfig_reset(void);
const char *appconfig_get_app_id(void);

// Forget the bound app id and in-memory entries (not the file), and free the
// store.  Called by app_identity at every app start and exit so no app sees the
// previous app's store.  Not part of the native API.
void        appconfig_unbind(void);
