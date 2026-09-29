#pragma once
// Font slot table: 0..3 are the compiled-in fonts, 4..11 hold fonts loaded
// from .pfn files on the SD card. The launcher calls font_registry_unload_all
// around every app so loaded fonts never outlive their app.
#include "font.h"

#define FONT_REGISTRY_BUILTIN 4
#define FONT_REGISTRY_SLOTS   12

// Returns NULL for ids that are out of range or not currently loaded.
const pc_font_t *font_registry_get(int id);

// Loaded-font slots (ids FONT_REGISTRY_BUILTIN..FONT_REGISTRY_SLOTS-1). The
// Lua display.loadFont, graphics.font.new and a native app's loadFont all draw
// from this one pool.
#define FONT_REGISTRY_LOADED (FONT_REGISTRY_SLOTS - FONT_REGISTRY_BUILTIN)

// Read `path` (absolute SD path) into PSRAM and validate it. Returns the
// new slot id (>= FONT_REGISTRY_BUILTIN) or -1 (missing file, bad image,
// no free slot); the reason is printed to the serial log. -1 is the native
// API's failure value (`loadFont` in os.h); the Lua bridge turns it into
// `nil, errstr` via font_registry_load_ex.
int font_registry_load(const char *path);

// As font_registry_load; on failure also sets *why (if non-NULL) to a static
// string naming the cause: "no free font slot", "cannot read file" or "not a
// valid .pfn font". *why is left untouched on success.
int font_registry_load_ex(const char *path, const char **why);
#define FONT_REGISTRY_WHY_FULL "no free font slot"

// Free one loaded slot. No-op for built-ins and empty slots.
void font_registry_unload(int id);

// Free every loaded slot.
void font_registry_unload_all(void);
