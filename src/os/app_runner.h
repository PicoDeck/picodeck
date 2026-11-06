#pragma once
#include <stdbool.h>
#include "launcher_types.h"

// =============================================================================
// AppRunner — vtable interface for per-runtime app loaders
//
// Each runtime (Lua, native, …) implements one AppRunner.
// launcher.c iterates the table of known runners and dispatches to the first
// one whose can_handle() returns true for a given app_entry_t.
// =============================================================================

typedef struct {
    const char *name;
    // Returns true if this runner handles the given app type.
    bool (*can_handle)(const app_entry_t *app);
    // Load and execute the app.  Blocks until the app exits.  Returns true
    // on clean exit, false if the app failed to load or crashed.
    bool (*run)(const app_entry_t *app);
} AppRunner;
